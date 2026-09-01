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

/* Block parameters, in mask order. */
enum {
    P_CHANNELS = 0,   /* comma-separated channel names */
    P_DATASET,        /* dataset RID */
    P_BASE_URL,       /* API root */
    P_SAMPLE_TIME,    /* seconds between writes (decimation, not the solver step) */
    P_CAPACITY,       /* ring capacity in rows */
    P_TIME_MODE,      /* 1 = simulation time from an epoch, 2 = host wall clock */
    P_COUNT
};

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

    if (!ssSetNumInputPorts(S, 1)) {
        return;
    }
    ssSetInputPortWidth(S, 0, (int)count);
    ssSetInputPortDataType(S, 0, SS_DOUBLE);
    ssSetInputPortDirectFeedThrough(S, 0, 1);
    /* Contiguous, so the port's signal is a plain const double* and one row reaches
     * the ring with no gather step. */
    ssSetInputPortRequiredContiguous(S, 0, 1);

    /* Discarded rows, so saturation is observable from inside the model. */
    if (!ssSetNumOutputPorts(S, 1)) {
        return;
    }
    ssSetOutputPortWidth(S, 0, 1);
    ssSetOutputPortDataType(S, 0, SS_DOUBLE);

    ssSetNumSampleTimes(S, 1);
    ssSetNumPWork(S, 1);

    /* No zero-crossings, and the block is safe to leave out of a model that does not
     * need it. Exception-free code lets Simulink skip its own try/catch wrapper. */
    ssSetOptions(S, SS_OPTION_EXCEPTION_FREE_CODE |
                    SS_OPTION_CALL_TERMINATE_ON_EXIT);
}

static void mdlInitializeSampleTimes(SimStruct *S)
{
    /* An explicit discrete rate, not an inherited one: this is how often telemetry is
     * written, which is a separate decision from the solver step. */
    real_T ts = mxGetScalar(ssGetSFcnParam(S, P_SAMPLE_TIME));

    ssSetSampleTime(S, 0, ts > 0.0 ? ts : 0.001);
    ssSetOffsetTime(S, 0, 0.0);
    ssSetModelReferenceSampleTimeDefaultInheritance(S);
}

#define MDL_START
static void mdlStart(SimStruct *S)
{
    sink_state *st;
    nm_config   cfg;
    nm_status   status = NM_OK;
    char       *csv = NULL, *rid = NULL, *url = NULL;
    char       *storage = NULL;
    char      **names = NULL;
    size_t      count = 0;
    const char *token;

    ssSetPWorkValue(S, 0, NULL);

    token = getenv("NOMINAL_TOKEN");
    if (token == NULL || token[0] == '\0') {
        ssSetErrorStatus(S, "Nominal Stream Sink: NOMINAL_TOKEN is not set. "
                            "The token is read from the environment rather than stored "
                            "in the model.");
        return;
    }

    csv = param_string(S, P_CHANNELS);
    rid = param_string(S, P_DATASET);
    url = param_string(S, P_BASE_URL);
    count = split_names(csv, &storage, &names);

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
    st->channel_count = count;
    /* Captured once, so simulation time maps onto a fixed instant for the whole run. */
    st->epoch_nanos   = nm_now_nanos();

    memset(&cfg, 0, sizeof cfg);
    cfg.base_url      = (url != NULL && url[0] != '\0')
                      ? url : "https://api.gov.nominal.io/api";
    cfg.token         = token;
    cfg.dataset_rid   = rid;
    cfg.channel_names = (const char *const *)names;
    cfg.channel_count = count;
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

cleanup:
    free(csv);
    free(rid);
    free(url);
    free(names);
    free(storage);
}

static void mdlOutputs(SimStruct *S, int_T tid)
{
    sink_state    *st = ssGetPWorkValue(S, 0);
    const real_T  *u;
    real_T        *y = ssGetOutputPortRealSignal(S, 0);
    int64_t        ts;

    (void)tid;   /* plain cast rather than UNUSED_ARG, which is not in every release */

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
    y[0] = (real_T)nm_dropped_rows(st->stream);
}

static void mdlTerminate(SimStruct *S)
{
    sink_state *st = ssGetPWorkValue(S, 0);

    if (st == NULL) {
        return;
    }

    /* Blocks until buffered rows have been sent or have definitively failed, so the
     * data has landed before the simulation is considered over. A deadlock here would
     * hang MATLAB silently, which is why the library's sender thread takes no lock the
     * producer can hold. */
    nm_close(st->stream);
    free(st);
    ssSetPWorkValue(S, 0, NULL);
}

#ifdef MATLAB_MEX_FILE
#include "simulink.c"
#else
#include "cg_sfun.h"
#endif
