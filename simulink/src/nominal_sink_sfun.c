/*
 * nominal_sink_sfun.c — a Simulink sink block that streams signals to Nominal.
 *
 * The model's step function is not a place to do network I/O, so this block does
 * almost nothing: it copies one row into libnominalstream's ring and returns. Encoding,
 * TLS and HTTP all happen on that library's sender thread. nm_push neither allocates
 * nor blocks, so the cost per step is bounded and independent of the network — which is
 * what makes the block safe on a real-time target rather than only on the desktop.
 *
 * Under saturation the library discards rows and counts them. That is the right trade
 * for hardware-in-the-loop: stalling a model driving real hardware is far worse than
 * losing telemetry. The discarded count is an output port so the loss is visible in the
 * model instead of silent.
 *
 * Signals arrive as a flat vector with channel names given as a parameter, rather than
 * as a bus. Introspecting bus element names from inside a C S-function is both fiddly
 * and version-sensitive, and it is exactly the sort of thing that fails to survive code
 * generation — which this block must do, since Speedgoat targets are code-generated.
 * The bus ergonomics belong in a mask callback that fills in the names at edit time,
 * where MATLAB can inspect the bus properly.
 *
 * The token is read from the NOMINAL_TOKEN environment variable and is deliberately not
 * a block parameter: parameters are saved inside the .slx, and a credential committed
 * to a model file is a credential leaked.
 */

#define S_FUNCTION_NAME  nominal_sink_sfun
#define S_FUNCTION_LEVEL 2

#include "simstruc.h"

#include "nominal_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build with -DNM_SFUN_TRACE=1 to log callback entry and exit. A fault inside an
 * S-function surfaces as a bare access violation that takes MATLAB down with it, with
 * no indication of which callback was running, so having this switchable is worth the
 * few lines. */
#if defined(NM_SFUN_TRACE) && NM_SFUN_TRACE
/* mexPrintf only. Calling mexEvalString here would re-enter the MATLAB interpreter from
 * inside a callback declared SS_OPTION_EXCEPTION_FREE_CODE, which is exactly what that
 * option promises not to do. */
#define NM_TRACE(...) mexPrintf("[nm] " __VA_ARGS__)
#else
#define NM_TRACE(...) ((void)0)
#endif

/* Block parameters, in mask order. */
enum {
    P_CHANNELS = 0,   /* comma-separated channel names */
    P_DATASET,        /* dataset RID */
    P_BASE_URL,       /* API root */
    P_SAMPLE_TIME,    /* seconds between writes (decimation, not the solver step) */
    P_CAPACITY,       /* ring capacity in rows */
    P_TIME_MODE,      /* 1 = simulation time from an epoch, 2 = host wall clock */
    P_SHOW_DROPPED,   /* non-zero adds an output port carrying the discarded-row count */
    P_MODE,           /* 1 = direct HTTPS, 2 = frames to a relay */
    P_PROXY_HOST,
    P_PROXY_PORT,
    P_COUNT
};

/* One-based, matching the mask popup's evaluated option index. */
enum { MODE_DIRECT = 1, MODE_PROXY = 2 };

/* One-based to match a Simulink mask popup, whose evaluated value is the option
 * index. Keeping the C side aligned with the mask avoids an off-by-one that would
 * silently pick the wrong clock. */
enum { TIME_MODE_SIM = 1, TIME_MODE_WALL = 2 };

/* Per-block state. Kept in PWork because the epoch is an int64 nanosecond count and
 * RWork would store it as a double -- which holds integers exactly only to 2^53, about
 * 104 days in nanoseconds, so every real timestamp would be silently rounded. */
typedef struct {
    nm_stream *stream;
    int64_t    epoch_nanos;
    int        time_mode;
    int        mode;            /* MODE_DIRECT or MODE_PROXY, for the end-of-run report */
    size_t     channel_count;
} sink_state;

/* ------------------------------------------------------------------ helpers */

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
 * Split "a, b, c" into a NULL-terminated array of trimmed names, sharing one buffer.
 * Returns the count, or 0 on failure. *storage must be freed by the caller; *names is
 * an array that must be freed separately.
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

    csv   = param_string(S, P_CHANNELS);
    count = split_names(csv, &storage, &names);
    free(csv);
    free(names);
    free(storage);

    if (count == 0) {
        ssSetErrorStatus(S, "Nominal Stream Sink: the channel list is empty. "
                            "Give a comma-separated list of channel names.");
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

    /* No zero-crossings, and the block is safe to leave out of a model that does not
     * need it. Exception-free code lets Simulink skip its own try/catch wrapper. */
    /* Deliberately NOT SS_OPTION_EXCEPTION_FREE_CODE. That option promises the callbacks
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
    int         mode;

    ssSetPWorkValue(S, 0, NULL);

    mode = (int)mxGetScalar(ssGetSFcnParam(S, P_MODE));

    /* Only direct mode needs a credential. In proxy mode the dataset RID travels in
     * the frames and the token lives on the relay, so this machine holds nothing worth
     * stealing — demanding one here would be asking for a secret we will not use. */
    token = getenv("NOMINAL_TOKEN");
    if (mode != MODE_PROXY && (token == NULL || token[0] == '\0')) {
        ssSetErrorStatus(S, "Nominal Stream Sink: NOMINAL_TOKEN is not set. "
                            "The token is read from the environment rather than stored "
                            "in the model. (Proxy mode does not need one.)");
        return;
    }

    csv        = param_string(S, P_CHANNELS);
    rid        = param_string(S, P_DATASET);
    url        = param_string(S, P_BASE_URL);
    proxy_host = param_string(S, P_PROXY_HOST);
    count      = split_names(csv, &storage, &names);

    if (count == 0 || rid == NULL || rid[0] == '\0') {
        ssSetErrorStatus(S, "Nominal Stream Sink: a dataset RID and at least one "
                            "channel name are required.");
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

    if (mode == MODE_PROXY) {
        double port = mxGetScalar(ssGetSFcnParam(S, P_PROXY_PORT));

        if (proxy_host == NULL || proxy_host[0] == '\0' ||
            port < 1.0 || port > 65535.0) {
            ssSetErrorStatus(S, "Nominal Stream Sink: proxy mode needs a host and a "
                                "port between 1 and 65535.");
            free(st);
            goto cleanup;
        }
        cfg.mode       = NM_MODE_PROXY;
        cfg.proxy_host = proxy_host;
        cfg.proxy_port = (uint16_t)port;
    } else {
        cfg.mode     = NM_MODE_DIRECT;
        cfg.base_url = (url != NULL && url[0] != '\0')
                     ? url : "https://api.gov.nominal.io/api";
        cfg.token    = token;
    }
    cfg.capacity_rows = (size_t)mxGetScalar(ssGetSFcnParam(S, P_CAPACITY));
    cfg.max_batch_delay_ms = 100;
    /* An RTOS has no OS trust store, so a target needs an explicit bundle. On the
     * desktop NULL uses the platform's own, which is what Windows Schannel provides. */
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
        static char msg[256];
        snprintf(msg, sizeof msg, "Nominal Stream Sink: could not open the stream (%s).",
                 nm_strerror(status));
        ssSetErrorStatus(S, msg);
        free(st);
        goto cleanup;
    }

    ssSetPWorkValue(S, 0, st);
    NM_TRACE("mdlStart: stream open, %u channels\n", (unsigned)st->channel_count);

cleanup:
    free(csv);
    free(rid);
    free(url);
    free(proxy_host);
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

    if (st->time_mode == TIME_MODE_WALL) {
        ts = nm_now_nanos();
    } else {
        /* Simulation time against the epoch captured at start. Faster-than-real-time
         * runs therefore produce a time axis that matches the model, not the clock. */
        ts = st->epoch_nanos + (int64_t)(ssGetT(S) * 1.0e9);
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

    /* Blocks until buffered rows have been sent or have definitively failed, so the
     * data has landed before the simulation is considered over. A deadlock here would
     * hang MATLAB silently, which is why the library's sender thread takes no lock the
     * producer can hold. */
    /*
     * nm_close_stats rather than nm_close, because the counts worth reporting are the
     * ones after the final flush — and nm_close frees the stream, so there is no later
     * moment at which to read them.
     */
    nm_stats_t stats;
    nm_close_stats(st->stream, &stats);

    /*
     * Report only when something went wrong. This is what keeps loss from being silent
     * now that the output port is off by default: a run that dropped rows says so,
     * whether or not anyone wired up the diagnostic.
     *
     * The total is every row the model produced, not only those that found room in the
     * buffer. Rows discarded for want of room are part of the loss, so dividing by the
     * accepted count could report more lost than written.
     */
    if (stats.rows_dropped > 0 || stats.requests_failed > 0) {
        static char warn[512];
        char        why[96];

        /* Say which of the three ways rows are lost happened, since each has a
         * different remedy. With no failed sends, the loss can only be the buffer
         * refusing rows. Status 0 means a request got no response at all, which
         * "HTTP status 0" would only obscure. */
        if (stats.requests_failed == 0) {
            snprintf(why, sizeof why,
                     "the buffer filled faster than the network drained it");
        } else if (st->mode == MODE_PROXY) {
            snprintf(why, sizeof why, "%llu datagrams could not be sent to the relay",
                     (unsigned long long)stats.requests_failed);
        } else if (stats.last_http_status > 0) {
            snprintf(why, sizeof why, "%llu requests failed, the last with HTTP status %ld",
                     (unsigned long long)stats.requests_failed, stats.last_http_status);
        } else {
            snprintf(why, sizeof why, "%llu requests failed, the last with no response",
                     (unsigned long long)stats.requests_failed);
        }
        snprintf(warn, sizeof warn,
                 "Nominal Stream Sink: %llu of %llu rows did not reach Nominal (%s).%s%s",
                 (unsigned long long)stats.rows_dropped,
                 (unsigned long long)stats.rows_pushed,
                 why,
                 stats.last_error[0] != '\0' ? " Last error: " : "",
                 stats.last_error);
        ssWarning(S, warn);
    }
    free(st);
    ssSetPWorkValue(S, 0, NULL);
}

#ifdef MATLAB_MEX_FILE
#include "simulink.c"
#else
#include "cg_sfun.h"
#endif
