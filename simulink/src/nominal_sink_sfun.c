/*
 * nominal_sink_sfun.c — a Simulink sink block that streams signals to Nominal.
 *
 * The model's step function is not a place to do network I/O, so in each step this block
 * only copies one row into libnominalstream's ring and returns. Encoding and all network
 * I/O, whether HTTPS or datagrams to a relay, happen on that library's sender thread.
 * nm_push neither allocates nor blocks, so the cost per step is bounded and independent
 * of the network, which is what makes the block safe on a real-time target rather than
 * only on the desktop.
 *
 * Under saturation the library discards rows and counts them. That is the right trade
 * for hardware-in-the-loop: stalling a model driving real hardware is far worse than
 * losing telemetry. The discarded count is available on an optional output port, and
 * mdlTerminate warns at the end of a run if rows were lost, so loss is never silent.
 *
 * Signals arrive as a flat vector with channel names given as a parameter, rather than
 * as a bus. Introspecting bus element names from inside a C S-function is both fiddly
 * and version-sensitive, and it is exactly the sort of thing that fails to survive code
 * generation — which this block must do, since Speedgoat targets are code-generated.
 * Instead, nominal_sink_autonames, behind the mask's Detect button, fills in the names
 * at edit time, where MATLAB can inspect the bus properly.
 *
 * The token is read from the NOMINAL_TOKEN environment variable and is deliberately not
 * a block parameter: parameters are saved inside the .slx, and a credential committed
 * to a model file is a credential leaked.
 */

#define S_FUNCTION_NAME  nominal_sink_sfun
#define S_FUNCTION_LEVEL 2

#include "simstruc.h"

#include "nominal_stream.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Configure with -DNM_SFUN_TRACE=ON to log entry to each callback. A fault inside an
 * S-function surfaces as a bare access violation that takes MATLAB down with it, with
 * no indication of which callback was running. */
#if defined(NM_SFUN_TRACE) && NM_SFUN_TRACE
/* mexPrintf only. mexEvalString would re-enter the MATLAB interpreter from inside a
 * Simulink callback. */
#define NM_TRACE(...) mexPrintf("[nm] " __VA_ARGS__)
#else
#define NM_TRACE(...) ((void)0)
#endif

/* Block parameters, in the order of the Parameters string that make_library sets.
 * Keep the two in step. The dialog lists the fields in a different order. */
enum {
    P_CHANNELS = 0,   /* comma-separated channel names */
    P_DATASET,        /* dataset RID */
    P_BASE_URL,       /* API root */
    P_SAMPLE_TIME,    /* seconds between writes (decimation, not the solver step) */
    P_CAPACITY,       /* ring capacity in rows */
    P_TIME_MODE,      /* one of the TIME_MODE_ values below */
    P_SHOW_DROPPED,   /* non-zero adds an output port carrying the discarded-row count */
    P_MODE,           /* 1 = direct HTTPS, 2 = frames to a relay */
    P_PROXY_HOST,
    P_PROXY_PORT,
    P_TAGS,           /* key US value RS key US value ..., from nominal_sink_tags */
    P_EXEC_ID,        /* "auto", an ID, or empty for none, from nominal_sink_execid */
    P_COUNT
};

/* One-based to match a Simulink mask popup, whose evaluated value is the option
 * index. Keeping the C side aligned with the mask avoids an off-by-one that would
 * silently pick the wrong transport or clock. */
enum { MODE_DIRECT = 1, MODE_PROXY = 2 };
enum {
    TIME_MODE_SIM      = 1,   /* the run's start time plus simulation time */
    TIME_MODE_SIM_ZERO = 2,   /* simulation time alone, so every run starts at 0 */
    TIME_MODE_WALL     = 3    /* the host's clock when the row is written */
};

/* The separators nominal_sink_tags joins the Tags field with. */
#define TAG_KEY_SEP '\x1f'
#define TAG_SEP     '\x1e'

/* The tag Execution ID is sent as, and the value that asks for a new ID each run. */
#define EXEC_ID_TAG  "execution-id"
#define EXEC_ID_AUTO "auto"

/* Used both when sizing the ports and when opening the stream. */
#define MSG_NO_CHANNELS "Nominal Stream Sink: the channel list is empty. Enter channel " \
                        "names in quotes, separated by commas, or click Detect from "   \
                        "connected signal."

/* Per-block state. Kept in PWork because the epoch is an int64 nanosecond count and
 * RWork would store it as a double -- which holds integers exactly only to 2^53, about
 * 104 days in nanoseconds, so every real timestamp would be silently rounded. */
typedef struct {
    nm_stream *stream;
    int64_t    epoch_nanos;
    int        time_mode;
    int        mode;            /* MODE_DIRECT or MODE_PROXY, for the end-of-run report */
    size_t     channel_count;
    char      *execution_id;    /* NULL when the block sends none */
} sink_state;

static void free_state(sink_state *st)
{
    if (st != NULL) {
        free(st->execution_id);
        free(st);
    }
}

/* ------------------------------------------------------------------ helpers */

/*
 * Check a text parameter really is text, reporting it by its dialog label if not.
 *
 * The mask converts strings to char on the way in, so single and double quotes both
 * arrive as char. Anything still not char -- a number, say -- used to be read as empty,
 * which for the URL meant silently writing to the default instance instead of the one
 * the user named.
 */
static int require_text(SimStruct *S, int index, const char *label, const char *example)
{
    static char    msg[192];
    const mxArray *a = ssGetSFcnParam(S, index);

    if (a != NULL && mxIsChar(a)) {
        return 1;
    }
    snprintf(msg, sizeof msg, "Nominal Stream Sink: %s must be text in quotes, for "
                              "example %s.", label, example);
    ssSetErrorStatus(S, msg);
    return 0;
}

/* Copy a string parameter out of its mxArray. Caller frees. */
static char *param_string(SimStruct *S, int index)
{
    const mxArray *a = ssGetSFcnParam(S, index);
    size_t         len;
    char          *out;

    if (a == NULL || !mxIsChar(a)) {
        return NULL;
    }
    len = mxGetNumberOfElements(a) + 1;
    out = malloc(len);
    if (out == NULL) {
        return NULL;
    }
    if (mxGetString(a, out, (int)len) != 0) {
        free(out);
        return NULL;
    }
    return out;
}

/*
 * Split "a, b, c" into an array of trimmed names, sharing one buffer. Returns the
 * count, or 0 if there are no names or allocation fails. The caller frees *storage and
 * *names separately.
 */
static size_t split_names(const char *csv, char **storage, char ***names)
{
    char   *buf, *p;
    char  **list;
    size_t  count = 0, capacity = 8;

    *storage = NULL;
    *names   = NULL;

    if (csv == NULL) {
        return 0;
    }

    buf = malloc(strlen(csv) + 1);
    if (buf == NULL) {
        return 0;
    }
    strcpy(buf, csv);

    list = malloc(capacity * sizeof *list);
    if (list == NULL) {
        free(buf);
        return 0;
    }

    p = buf;
    while (*p != '\0') {
        char *start, *end;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        start = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        end = p;
        if (*p == ',') {
            p++;
        }
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        *end = '\0';

        if (*start == '\0') {
            continue;   /* tolerate a trailing comma or a stray blank entry */
        }

        if (count == capacity) {
            char **grown;
            capacity *= 2;
            grown = realloc(list, capacity * sizeof *list);
            if (grown == NULL) {
                free(list);
                free(buf);
                return 0;
            }
            list = grown;
        }
        list[count++] = start;
    }

    if (count == 0) {
        free(list);
        free(buf);
        return 0;
    }

    *storage = buf;
    *names   = list;
    return count;
}

/*
 * Split the Tags parameter into tags that point into `text`, which this modifies.
 * nominal_sink_tags has already checked every key and value, so this only has to undo
 * the joining. Leaves `spare` empty entries after them for the caller to add to. Returns
 * the count split, which is 0 for no tags. *tags is NULL if allocation failed or there
 * is nothing to hold. The caller frees *tags.
 */
static size_t split_tags(char *text, size_t spare, nm_tag **tags)
{
    size_t count = 0, i = 0;
    char  *p;

    *tags = NULL;
    if (text != NULL && text[0] != '\0') {
        count = 1;
        for (p = text; *p != '\0'; p++) {
            if (*p == TAG_SEP) {
                count++;
            }
        }
    }
    if (count + spare == 0) {
        return 0;
    }

    *tags = calloc(count + spare, sizeof **tags);
    if (*tags == NULL) {
        return 0;
    }

    p = count > 0 ? text : NULL;
    while (p != NULL && i < count) {
        char *end = strchr(p, TAG_SEP);
        char *sep;

        if (end != NULL) {
            *end = '\0';
        }
        sep = strchr(p, TAG_KEY_SEP);
        if (sep != NULL) {
            *sep = '\0';
            (*tags)[i].key   = p;
            (*tags)[i].value = sep + 1;
            i++;
        }
        p = end != NULL ? end + 1 : NULL;
    }
    return i;
}

/*
 * A new execution ID: the UTC time the run started, then six hex digits that differ
 * between runs starting in the same second, such as parsim workers. The digits hash the
 * clock's nanoseconds, this block's state address and a counter, so two processes
 * starting together still differ.
 */
static void make_execution_id(char *out, size_t len, int64_t now_nanos, const void *salt)
{
    static uint32_t counter = 0;
    uint64_t        h = 0xcbf29ce484222325ULL;   /* FNV-1a, as in nm_frame.c */
    uint64_t        parts[3];
    time_t          seconds = (time_t)(now_nanos / 1000000000LL);
    struct tm      *utc = gmtime(&seconds);
    size_t          i;

    parts[0] = (uint64_t)now_nanos;
    parts[1] = (uint64_t)(uintptr_t)salt;
    parts[2] = ++counter;
    for (i = 0; i < sizeof parts; i++) {
        h ^= ((const uint8_t *)parts)[i];
        h *= 0x100000001b3ULL;
    }

    if (utc == NULL) {
        snprintf(out, len, "%06x", (unsigned)(h & 0xffffffu));
        return;
    }
    snprintf(out, len, "%04d%02d%02d-%02d%02d%02d-%06x",
             utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
             utc->tm_hour, utc->tm_min, utc->tm_sec, (unsigned)(h & 0xffffffu));
}

/* ------------------------------------------------------------------ S-function */

static void mdlInitializeSizes(SimStruct *S)
{
    NM_TRACE("mdlInitializeSizes enter\n");
    char  *csv;
    char  *storage = NULL;
    char **names   = NULL;
    size_t count;

    ssSetNumSFcnParams(S, P_COUNT);
    if (ssGetNumSFcnParams(S) != ssGetSFcnParamsCount(S)) {
        return;   /* Simulink reports the mismatch itself */
    }
    /* None of these are tunable: the channel set, dataset and buffer sizing are all
     * fixed when the stream opens. */
    {
        int i;
        for (i = 0; i < P_COUNT; i++) {
            ssSetSFcnParamTunable(S, i, SS_PRM_NOT_TUNABLE);
        }
    }

    if (!require_text(S, P_CHANNELS, "Channel names", "'engine.rpm, engine.egt'")) {
        return;
    }
    csv   = param_string(S, P_CHANNELS);
    count = split_names(csv, &storage, &names);
    free(csv);
    free(names);
    free(storage);

    if (count == 0) {
        ssSetErrorStatus(S, MSG_NO_CHANNELS);
        return;
    }

    NM_TRACE("mdlInitializeSizes: %u channels parsed\n", (unsigned)count);

    if (!ssSetNumInputPorts(S, 1)) {
        return;
    }
    ssSetInputPortWidth(S, 0, (int)count);
    ssSetInputPortDataType(S, 0, SS_DOUBLE);
    ssSetInputPortDirectFeedThrough(S, 0, 1);
    /* Contiguous, so the port's signal is a plain const double* and one row reaches
     * the ring with no gather step. */
    ssSetInputPortRequiredContiguous(S, 0, 1);

    /*
     * The discarded-row count, as an optional output port.
     *
     * Optional because Simulink treats an unconnected output as a block error, so a
     * port that is always present forces every user of a *sink* to wire up or
     * terminate a diagnostic most of them do not want. Off by default therefore, and
     * mdlTerminate warns if anything was actually dropped — loss stays impossible to
     * miss without making the common case awkward.
     */
    if (mxGetScalar(ssGetSFcnParam(S, P_SHOW_DROPPED)) != 0.0) {
        if (!ssSetNumOutputPorts(S, 1)) {
            return;
        }
        ssSetOutputPortWidth(S, 0, 1);
        ssSetOutputPortDataType(S, 0, SS_DOUBLE);
    } else if (!ssSetNumOutputPorts(S, 0)) {
        return;
    }

    NM_TRACE("mdlInitializeSizes: ports declared\n");

    ssSetNumSampleTimes(S, 1);
    ssSetNumPWork(S, 1);
    NM_TRACE("mdlInitializeSizes: work vectors declared\n");

    /* SS_OPTION_CALL_TERMINATE_ON_EXIT makes Simulink call mdlTerminate even when
     * mdlStart never ran, which is why mdlTerminate checks for PWork first.
     *
     * Deliberately NOT SS_OPTION_EXCEPTION_FREE_CODE. That option promises the callbacks
     * never invoke anything that can raise a MATLAB exception, and this file calls
     * mxGetString and (when tracing) mexPrintf, which can. The promise buys a little
     * speed and costs Simulink's protective wrapper, so breaking it turns any fault
     * into an access violation that takes MATLAB down with no diagnostic. */
    ssSetOptions(S, SS_OPTION_CALL_TERMINATE_ON_EXIT);
    NM_TRACE("mdlInitializeSizes leave\n");
}

static void mdlInitializeSampleTimes(SimStruct *S)
{
    /* An explicit discrete rate, not an inherited one: this is how often telemetry is
     * written, which is a separate decision from the solver step. */
    real_T ts;

    NM_TRACE("mdlInitializeSampleTimes enter\n");
    ts = mxGetScalar(ssGetSFcnParam(S, P_SAMPLE_TIME));
    NM_TRACE("mdlInitializeSampleTimes: ts=%g\n", ts);

    ssSetSampleTime(S, 0, ts > 0.0 ? ts : 0.001);
    ssSetOffsetTime(S, 0, 0.0);
    ssSetModelReferenceSampleTimeDefaultInheritance(S);
}

#define MDL_START
static void mdlStart(SimStruct *S)
{
    NM_TRACE("mdlStart enter\n");
    sink_state *st;
    nm_config   cfg;
    nm_status   status = NM_OK;
    char       *csv = NULL, *rid = NULL, *url = NULL;
    char       *storage = NULL;
    char      **names = NULL;
    size_t      count = 0;
    const char *token;
    char       *proxy_host = NULL;
    char       *tag_text = NULL, *exec_id = NULL;
    nm_tag     *tags = NULL;
    int         mode;

    ssSetPWorkValue(S, 0, NULL);

    mode = (int)mxGetScalar(ssGetSFcnParam(S, P_MODE));

    /* Only direct mode needs a credential. In proxy mode the dataset RID travels in
     * the frames and the token lives on the relay, so this machine holds nothing worth
     * stealing — demanding one here would be asking for a secret we will not use. */
    token = getenv("NOMINAL_TOKEN");
    if (mode != MODE_PROXY && (token == NULL || token[0] == '\0')) {
        ssSetErrorStatus(S, "Nominal Stream Sink: NOMINAL_TOKEN is not set. Set it to "
                            "your Nominal API key, for example with "
                            "setenv('NOMINAL_TOKEN', '<API key>'), then run the model "
                            "again.");
        return;
    }

    if (!require_text(S, P_DATASET, "Dataset RID", "'ri.catalog.<...>'")) {
        return;
    }
    if (mode == MODE_PROXY
            ? !require_text(S, P_PROXY_HOST, "Proxy host", "'127.0.0.1'")
            : !require_text(S, P_BASE_URL, "API base URL", "'https://api.gov.nominal.io/api'")) {
        return;
    }

    csv        = param_string(S, P_CHANNELS);
    rid        = param_string(S, P_DATASET);
    url        = param_string(S, P_BASE_URL);
    proxy_host = param_string(S, P_PROXY_HOST);
    count      = split_names(csv, &storage, &names);

    if (count == 0) {
        ssSetErrorStatus(S, MSG_NO_CHANNELS);
        goto cleanup;
    }
    if (rid == NULL || rid[0] == '\0') {
        ssSetErrorStatus(S, "Nominal Stream Sink: Dataset RID is empty. Enter the RID of "
                            "the dataset to write to, in quotes.");
        goto cleanup;
    }

    st = calloc(1, sizeof *st);
    if (st == NULL) {
        ssSetErrorStatus(S, "Nominal Stream Sink: out of memory.");
        goto cleanup;
    }

    st->time_mode     = (int)mxGetScalar(ssGetSFcnParam(S, P_TIME_MODE));
    st->mode          = mode;
    st->channel_count = count;
    /* Captured once, so simulation time maps onto a fixed instant for the whole run. */
    st->epoch_nanos   = nm_now_nanos();

    memset(&cfg, 0, sizeof cfg);
    cfg.dataset_rid   = rid;
    cfg.channel_names = (const char *const *)names;
    cfg.channel_count = count;

    /* The execution ID goes on as one more tag, so it reaches Nominal, and the relay,
     * exactly as the others do. split_tags leaves room for it. */
    tag_text      = param_string(S, P_TAGS);
    exec_id       = param_string(S, P_EXEC_ID);
    cfg.tag_count = split_tags(tag_text, 1, &tags);
    cfg.tags      = tags;

    if (exec_id != NULL && exec_id[0] != '\0') {
        if (strcmp(exec_id, EXEC_ID_AUTO) == 0) {
            st->execution_id = malloc(32);
            if (st->execution_id != NULL) {
                make_execution_id(st->execution_id, 32, st->epoch_nanos, st);
            }
        } else {
            st->execution_id = exec_id;
            exec_id = NULL;   /* st owns it now */
        }
        if (st->execution_id == NULL || tags == NULL) {
            ssSetErrorStatus(S, "Nominal Stream Sink: out of memory.");
            free_state(st);
            goto cleanup;
        }
        tags[cfg.tag_count].key   = EXEC_ID_TAG;
        tags[cfg.tag_count].value = st->execution_id;
        cfg.tag_count++;
    }

    if (mode == MODE_PROXY) {
        double port = mxGetScalar(ssGetSFcnParam(S, P_PROXY_PORT));

        if (proxy_host == NULL || proxy_host[0] == '\0' ||
            port < 1.0 || port > 65535.0) {
            ssSetErrorStatus(S, "Nominal Stream Sink: in proxy mode, Proxy host can't be "
                                "empty and Proxy port must be from 1 to 65535.");
            free_state(st);
            goto cleanup;
        }
        cfg.mode       = NM_MODE_PROXY;
        cfg.proxy_host = proxy_host;
        cfg.proxy_port = (uint16_t)port;
    } else {
        /* No default to fall back on: a blank URL is a mistake, and quietly
         * substituting one instance's URL would write to the wrong place. */
        if (url == NULL || url[0] == '\0') {
            ssSetErrorStatus(S, "Nominal Stream Sink: API base URL is empty. Enter the "
                                "API URL of the Nominal instance the dataset is in.");
            free_state(st);
            goto cleanup;
        }
        cfg.mode     = NM_MODE_DIRECT;
        cfg.base_url = url;
        cfg.token    = token;
    }
    cfg.capacity_rows = (size_t)mxGetScalar(ssGetSFcnParam(S, P_CAPACITY));
    cfg.max_batch_delay_ms = 100;
    /* An RTOS has no OS trust store, so a target needs an explicit bundle. On the
     * desktop, NULL leaves libcurl to use its default trust store (the Windows
     * certificate store, through Schannel, on Windows). */
    cfg.ca_bundle_path = getenv("NOMINAL_CA_BUNDLE");
    if (cfg.ca_bundle_path != NULL && cfg.ca_bundle_path[0] == '\0') {
        cfg.ca_bundle_path = NULL;
    }
    if (cfg.capacity_rows == 0) {
        cfg.capacity_rows = 4096;
    }

    /* nm_open copies the channel names, so their storage can be released below. */
    st->stream = nm_open(&cfg, &status);
    if (st->stream == NULL) {
        static char msg[384];
        if (status == NM_ERR_TOO_LARGE) {
            snprintf(msg, sizeof msg,
                     "Nominal Stream Sink: in proxy mode, the dataset RID, channel names and "
                     "tags have to fit in one 1400-byte packet to the relay, and these don't. "
                     "Use fewer or shorter channel names or tags.");
        } else if (mode == MODE_PROXY && status == NM_ERR_TRANSPORT) {
            snprintf(msg, sizeof msg,
                     "Nominal Stream Sink: could not set up sending to Proxy host '%s'. "
                     "Check that the name or address is right.", proxy_host);
        } else {
            snprintf(msg, sizeof msg, "Nominal Stream Sink: could not open the stream (%s).",
                     nm_strerror(status));
        }
        ssSetErrorStatus(S, msg);
        free_state(st);
        goto cleanup;
    }

    ssSetPWorkValue(S, 0, st);
    NM_TRACE("mdlStart: stream open, %u channels\n", (unsigned)st->channel_count);

cleanup:
    free(csv);
    free(rid);
    free(url);
    free(proxy_host);
    free(tag_text);
    free(exec_id);
    free(tags);
    free(names);
    free(storage);
}

static void mdlOutputs(SimStruct *S, int_T tid)
{
    sink_state    *st = ssGetPWorkValue(S, 0);
    const real_T  *u;
    int64_t        ts;

    (void)tid;   /* plain cast rather than UNUSED_ARG, which is not in every release */

    NM_TRACE("mdlOutputs enter (st=%p)\n", (void *)st);

    if (st == NULL || st->stream == NULL) {
        return;
    }

    /* Minor steps of a variable-step solver are provisional; writing them would put
     * points on the wire that the solver may yet discard. */
    if (!ssIsMajorTimeStep(S)) {
        return;
    }

    u = ssGetInputPortRealSignal(S, 0);

    /* Rounded, not truncated: 0.03 s is a hair under 3e7 ns as a double, and truncating
     * it would stamp the row 1 ns early. */
    if (st->time_mode == TIME_MODE_WALL) {
        ts = nm_now_nanos();
    } else if (st->time_mode == TIME_MODE_SIM_ZERO) {
        /* Simulation time alone, so every run of a model lines up from 0. */
        ts = (int64_t)llround(ssGetT(S) * 1.0e9);
    } else {
        /* Simulation time against the epoch captured at start. Faster-than-real-time
         * runs therefore produce a time axis that matches the model, not the clock. */
        ts = st->epoch_nanos + (int64_t)llround(ssGetT(S) * 1.0e9);
    }

    nm_push(st->stream, ts, u, st->channel_count);

    /* nm_dropped_rows, not nm_stats: the latter copies a 256-byte error string to
     * hand back one number, which is not something to do every step. */
    if (ssGetNumOutputPorts(S) > 0) {
        real_T *y = ssGetOutputPortRealSignal(S, 0);
        y[0] = (real_T)nm_dropped_rows(st->stream);
    }
}

static void mdlTerminate(SimStruct *S)
{
    sink_state *st;

    NM_TRACE("mdlTerminate enter\n");

    /*
     * Simulink runs a query pass before simulating: mdlInitializeSizes, then
     * mdlTerminate, and only on a second pass does mdlStart run. So mdlTerminate is
     * reached with the PWork vector not yet allocated, and ssGetPWorkValue then reads
     * through a null base pointer — yielding garbage rather than NULL, so an
     * `st == NULL` check does not save you. Dereferencing it is an access violation
     * that takes MATLAB down with nothing but "segmentation violation in the
     * S-function" to go on.
     *
     * Checking the vector exists before reading an element is what makes this safe.
     */
    if (ssGetPWork(S) == NULL) {
        NM_TRACE("mdlTerminate: no PWork yet (query pass), nothing to release\n");
        return;
    }

    st = ssGetPWorkValue(S, 0);
    if (st == NULL) {
        return;
    }

    /*
     * nm_close_stats blocks until buffered rows are sent or have definitively failed, or
     * until the 15 s allowed at close runs out, so nothing is in flight when the run ends.
     * A deadlock here would hang MATLAB silently,
     * which is why the library's sender thread takes no lock the producer can hold.
     *
     * nm_close_stats rather than nm_close, because the counts worth reporting are the
     * ones after the final flush, and nm_close frees the stream, so there is no later
     * moment at which to read them.
     */
    nm_stats_t stats;
    nm_close_stats(st->stream, &stats);

    /*
     * Report only when something went wrong. This is what keeps loss from being silent
     * now that the output port is off by default: a run that dropped rows says so,
     * whether or not anyone wired up the diagnostic.
     *
     * The "of" total is rows_pushed, every row the model produced, including rows the
     * full buffer refused. Using rows_accepted instead could report more rows lost than
     * written.
     */
    if (stats.rows_dropped > 0 || stats.requests_failed > 0) {
        static char warn[1024];
        char        why[96];
        char        who[300] = "";

        /* In a campaign of runs, which one lost rows. */
        if (st->execution_id != NULL) {
            snprintf(who, sizeof who, "execution %s: ", st->execution_id);
        }

        /* Say which of the three ways rows are lost happened, since each has a
         * different remedy. With no failed sends, the loss can only be the buffer
         * refusing rows. Status 0 means a request got no response at all, which
         * "HTTP status 0" would only obscure. */
        if (stats.requests_failed == 0) {
            snprintf(why, sizeof why,
                     "the buffer filled faster than the network drained it");
        } else if (st->mode == MODE_PROXY) {
            snprintf(why, sizeof why, "%llu packets could not be sent to the relay",
                     (unsigned long long)stats.requests_failed);
        } else if (stats.last_http_status > 0) {
            snprintf(why, sizeof why, "%llu requests failed, the last with HTTP status %ld",
                     (unsigned long long)stats.requests_failed, stats.last_http_status);
        } else {
            snprintf(why, sizeof why, "%llu requests failed, the last with no response",
                     (unsigned long long)stats.requests_failed);
        }
        snprintf(warn, sizeof warn,
                 "Nominal Stream Sink: %s%llu of %llu rows did not reach Nominal (%s).%s%s",
                 who,
                 (unsigned long long)stats.rows_dropped,
                 (unsigned long long)stats.rows_pushed,
                 why,
                 stats.last_error[0] != '\0' ? " Last error: " : "",
                 stats.last_error);
        ssWarning(S, warn);
    }
    free_state(st);
    ssSetPWorkValue(S, 0, NULL);
}

#ifdef MATLAB_MEX_FILE
#include "simulink.c"
#else
#include "cg_sfun.h"
#endif
