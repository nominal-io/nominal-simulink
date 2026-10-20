/*
 * nm_runs.c — create a run in Nominal and set its end, over the REST API.
 *
 * Request shapes are from the conjure service definition (RunService):
 *
 *   POST /scout/v1/run              createRun      CreateRunRequest -> Run
 *   PUT  /scout/v1/run/{rid}        updateRun      UpdateRunRequest -> Run
 *   POST /scout/v1/search-runs      searchRuns     SearchRunsRequest -> SearchRunsResponse
 *
 * The search has no exact-title query. Its exactMatch is a case-insensitive substring
 * match on the title, so every page of results is read and filtered to the exact title
 * here. The more runs have similar titles, the longer that takes.
 *
 * A fresh libcurl handle per request: a run makes two or three requests in its life, so
 * reusing a connection would buy nothing worth the state.
 */

#include "nominal_stream.h"
#include "nm_encode.h"   /* nm_split_timestamp */
#include "nm_http.h"     /* nm_http_global_init */

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A search response lists whole runs, a few kilobytes each. This holds a page of them. */
#define MAX_RESPONSE_BYTES (4u * 1024u * 1024u)
#define SEARCH_PAGE_SIZE   100

/* ------------------------------------------------------------------ buffers */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    int    failed;   /* allocation failed or the limit was reached */
} buf;

static void buf_add(buf *b, const char *s, size_t n)
{
    if (b->failed) {
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap > 0 ? b->cap : 1024;
        char  *grown;

        while (b->len + n + 1 > cap) {
            cap *= 2;
        }
        if (cap > MAX_RESPONSE_BYTES) {
            b->failed = 1;
            return;
        }
        grown = realloc(b->data, cap);
        if (grown == NULL) {
            b->failed = 1;
            return;
        }
        b->data = grown;
        b->cap  = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_str(buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

/* A JSON string, quoted and escaped. Bytes above 0x7f pass through, as UTF-8. */
static void buf_json(buf *b, const char *s)
{
    buf_add(b, "\"", 1);
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        char          esc[8];

        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            buf_add(b, esc, 2);
        } else if (c < 0x20) {
            snprintf(esc, sizeof esc, "\\u%04x", c);
            buf_add(b, esc, 6);
        } else {
            buf_add(b, (const char *)&c, 1);
        }
    }
    buf_add(b, "\"", 1);
}

/* A UtcTimestamp: {"secondsSinceEpoch":..,"offsetNanoseconds":..} */
static void buf_timestamp(buf *b, int64_t nanos)
{
    int64_t seconds;
    int32_t frac;
    char    text[96];

    nm_split_timestamp(nanos, &seconds, &frac);
    snprintf(text, sizeof text, "{\"secondsSinceEpoch\":%lld,\"offsetNanoseconds\":%ld}",
             (long long)seconds, (long)frac);
    buf_str(b, text);
}

/* ------------------------------------------------------------- JSON reading
 *
 * Just enough to pull a few fields out of a response: skip any value, find a key in an
 * object, read a string. Anything malformed comes back NULL.
 */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

static const char *skip_string(const char *p)
{
    for (p++; *p != '\0'; p++) {
        if (*p == '\\') {
            if (p[1] == '\0') {
                return NULL;
            }
            p++;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

static const char *skip_value(const char *p)
{
    int depth = 0;

    p = skip_ws(p);
    do {
        if (*p == '\0') {
            return NULL;
        }
        if (*p == '"') {
            p = skip_string(p);
            if (p == NULL) {
                return NULL;
            }
            continue;
        }
        if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            depth--;
        } else if (depth == 0) {
            /* A number, true, false or null: runs to the next delimiter. */
            while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' &&
                   *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                p++;
            }
            return p;
        }
        p++;
    } while (depth > 0);
    return p;
}

/* Four hex digits at p, or -1 if they aren't all there. */
static long hex4(const char *p)
{
    long v = 0;
    int  i;

    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v |= c - 'A' + 10;
        } else {
            return -1;
        }
    }
    return v;
}

/* Copy a JSON string at p into out, decoding escapes. Returns 0, or -1 if malformed or
 * too long. */
static int read_string(const char *p, char *out, size_t len)
{
    size_t n = 0;

    p = skip_ws(p);
    if (*p != '"') {
        return -1;
    }
    for (p++; *p != '"'; p++) {
        char c = *p;

        if (c == '\0') {
            return -1;
        }
        if (c == '\\') {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                /* Titles are compared byte for byte, so decode to UTF-8. Characters
                 * outside the Basic Multilingual Plane come as surrogate pairs. */
                long          cp = hex4(p + 1);
                unsigned char u[4];
                size_t        k, m;

                if (cp < 0) {
                    return -1;
                }
                p += 4;
                if (cp >= 0xd800 && cp < 0xdc00 && p[1] == '\\' && p[2] == 'u') {
                    long lo = hex4(p + 3);
                    if (lo < 0xdc00 || lo > 0xdfff) {
                        return -1;
                    }
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    p += 6;
                }
                if (cp < 0x80) {
                    u[0] = (unsigned char)cp; m = 1;
                } else if (cp < 0x800) {
                    u[0] = (unsigned char)(0xc0 | (cp >> 6));
                    u[1] = (unsigned char)(0x80 | (cp & 0x3f)); m = 2;
                } else if (cp < 0x10000) {
                    u[0] = (unsigned char)(0xe0 | (cp >> 12));
                    u[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
                    u[2] = (unsigned char)(0x80 | (cp & 0x3f)); m = 3;
                } else {
                    u[0] = (unsigned char)(0xf0 | (cp >> 18));
                    u[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
                    u[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
                    u[3] = (unsigned char)(0x80 | (cp & 0x3f)); m = 4;
                }
                for (k = 0; k < m; k++) {
                    if (n + 1 >= len) {
                        return -1;
                    }
                    out[n++] = (char)u[k];
                }
                continue;
            }
            case '\0':
                return -1;
            default:
                c = *p;   /* \" \\ \/ */
            }
        }
        if (n + 1 >= len) {
            return -1;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return 0;
}

/* The value of `key` in the object at obj, or NULL if it has none. */
static const char *object_get(const char *obj, const char *key)
{
    const char *p = skip_ws(obj);
    size_t      klen = strlen(key);

    if (*p != '{') {
        return NULL;
    }
    p = skip_ws(p + 1);
    while (*p == '"') {
        const char *name = p + 1;
        const char *after = skip_string(p);

        if (after == NULL) {
            return NULL;
        }
        p = skip_ws(after);
        if (*p != ':') {
            return NULL;
        }
        p = skip_ws(p + 1);
        if ((size_t)(after - 1 - name) == klen && memcmp(name, key, klen) == 0) {
            return p;
        }
        p = skip_value(p);
        if (p == NULL) {
            return NULL;
        }
        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- requests */

static size_t collect(char *data, size_t size, size_t nmemb, void *userdata)
{
    buf_add(userdata, data, size * nmemb);
    return size * nmemb;
}

/* Make one request. Returns 0 for a 2xx answer, with the body in *response, or -1 with
 * the reason in err. */
static int request(const nm_run_config *cfg, const char *method, const char *path,
                   const buf *body, buf *response, char *err, size_t err_len)
{
    CURL              *curl;
    struct curl_slist *headers = NULL;
    buf                url = { 0 }, auth = { 0 };
    CURLcode           rc;
    long               status = 0;
    int                result = -1;

    if (nm_http_global_init() != 0) {
        snprintf(err, err_len, "could not start the HTTPS client");
        return -1;
    }

    buf_str(&url, cfg->base_url);
    buf_str(&url, path);
    buf_str(&auth, "Authorization: Bearer ");
    buf_str(&auth, cfg->token);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Expect:");
    if (!auth.failed) {
        headers = curl_slist_append(headers, auth.data);
    }

    curl = curl_easy_init();
    if (curl == NULL || headers == NULL || url.failed || auth.failed || body->failed) {
        snprintf(err, err_len, "could not set up the request");
        goto done;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.data);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body->len);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    /* Trust as for writes: see nm_http_open. */
    if (cfg->ca_bundle_path != NULL) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, cfg->ca_bundle_path);
    }
#ifdef CURLSSLOPT_NATIVE_CA
    else {
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
    }
#endif

    rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE) {
            snprintf(err, err_len, "%s. Set NOMINAL_CA_BUNDLE to a PEM file of the CA "
                                   "certificates to trust", curl_easy_strerror(rc));
        } else {
            snprintf(err, err_len, "%s", curl_easy_strerror(rc));
        }
        goto done;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        /* The body is a conjure error, whose errorInstanceId is what support needs. */
        snprintf(err, err_len, "HTTP status %ld: %.200s", status,
                 response->data != NULL ? response->data : "(no response body)");
        goto done;
    }
    buf_add(response, "", 0);   /* an empty body still reads as "" */
    if (response->failed) {
        snprintf(err, err_len, "could not read the response");
        goto done;
    }
    result = 0;

done:
    if (curl != NULL) {
        curl_easy_cleanup(curl);
    }
    curl_slist_free_all(headers);
    free(url.data);
    if (auth.data != NULL) {
        memset(auth.data, 0, auth.len);
        free(auth.data);
    }
    return result;
}

/* ------------------------------------------------------------------- public */

static int missing(const char *s)
{
    return s == NULL || s[0] == '\0';
}

static int has_workspace(const nm_run_config *cfg)
{
    return !missing(cfg->workspace_rid);
}

/* One page of the search for cfg->title, continuing from `token` if it isn't empty.
 * Writes the next page's token to `token`, or "" on the last page. Returns 1 if a run
 * titled exactly cfg->title was on the page, 0 if not, -1 on failure. */
static int search_page(const nm_run_config *cfg, char *token, size_t token_len,
                       char *rid, size_t rid_len, char *err, size_t err_len)
{
    buf         body = { 0 }, response = { 0 };
    const char *results, *p;
    char        title[1024];
    char        size[32];
    int         found = -1;

    buf_str(&body, "{\"sort\":{\"isDescending\":true,"
                   "\"sortKey\":{\"type\":\"field\",\"field\":\"CREATED_AT\"}},");
    snprintf(size, sizeof size, "\"pageSize\":%d,", SEARCH_PAGE_SIZE);
    buf_str(&body, size);
    if (token[0] != '\0') {
        buf_str(&body, "\"nextPageToken\":");
        buf_json(&body, token);
        buf_str(&body, ",");
    }
    buf_str(&body, "\"query\":");
    if (has_workspace(cfg)) {
        buf_str(&body, "{\"type\":\"and\",\"and\":[");
    }
    buf_str(&body, "{\"type\":\"exactMatch\",\"exactMatch\":");
    buf_json(&body, cfg->title);
    buf_str(&body, "}");
    if (has_workspace(cfg)) {
        buf_str(&body, ",{\"type\":\"workspace\",\"workspace\":");
        buf_json(&body, cfg->workspace_rid);
        buf_str(&body, "}]}");
    }
    buf_str(&body, "}");

    if (request(cfg, "POST", "/scout/v1/search-runs", &body, &response, err, err_len) != 0) {
        goto done;
    }

    found = 0;
    results = object_get(response.data, "results");
    p = results != NULL ? skip_ws(results) : NULL;
    if (p == NULL || *p != '[') {
        goto done;
    }
    for (p = skip_ws(p + 1); *p == '{'; ) {
        const char *t = object_get(p, "title");
        const char *r = object_get(p, "rid");

        if (t != NULL && r != NULL && read_string(t, title, sizeof title) == 0 &&
            strcmp(title, cfg->title) == 0 && read_string(r, rid, rid_len) == 0) {
            found = 1;
            break;
        }
        p = skip_value(p);
        if (p == NULL) {
            break;
        }
        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
        }
    }

    /* The next page's token: a string, or null or absent on the last page. A token too
     * long to keep ends the search as the last page would. */
    token[0] = '\0';
    p = object_get(response.data, "nextPageToken");
    if (p != NULL && *skip_ws(p) == '"' && read_string(p, token, token_len) != 0) {
        token[0] = '\0';
    }

done:
    free(body.data);
    free(response.data);
    return found;
}

/* With shared set: the newest run titled exactly cfg->title, searching every page. 1 if
 * found, 0 if not, -1 on failure. */
static int find_run(const nm_run_config *cfg, char *rid, size_t rid_len,
                    char *err, size_t err_len)
{
    char token[4096] = "";
    char last[4096];
    int  found;

    do {
        memcpy(last, token, sizeof last);
        found = search_page(cfg, token, sizeof token, rid, rid_len, err, err_len);
        /* A repeated token would loop forever, so it ends the search like no token. */
    } while (found == 0 && token[0] != '\0' && strcmp(token, last) != 0);

    return found;
}

nm_status nm_run_begin(const nm_run_config *cfg, char *rid, size_t rid_len,
                       char *err, size_t err_len)
{
    buf         body = { 0 }, response = { 0 };
    const char *r;
    size_t      i;
    nm_status   st = NM_ERR_TRANSPORT;
    char        scratch[8];

    if (err == NULL || err_len == 0) {
        err     = scratch;
        err_len = sizeof scratch;
    }
    err[0] = '\0';
    if (cfg == NULL || missing(cfg->base_url) || missing(cfg->token) ||
        missing(cfg->dataset_rid) || missing(cfg->ref_name) || missing(cfg->title) ||
        rid == NULL || rid_len == 0 || (cfg->tag_count > 0 && cfg->tags == NULL)) {
        return NM_ERR_ARG;
    }
    rid[0] = '\0';

    if (cfg->shared) {
        int found = find_run(cfg, rid, rid_len, err, err_len);
        if (found != 0) {
            return found > 0 ? NM_OK : NM_ERR_TRANSPORT;
        }
    }

    buf_str(&body, "{\"title\":");
    buf_json(&body, cfg->title);
    buf_str(&body, ",\"description\":\"\",\"startTime\":");
    buf_timestamp(&body, cfg->start_nanos);
    buf_str(&body, ",\"dataSources\":{");
    buf_json(&body, cfg->ref_name);
    buf_str(&body, ":{\"dataSourceRid\":");
    buf_json(&body, cfg->dataset_rid);
    buf_str(&body, ",\"seriesTags\":{");
    for (i = 0; i < cfg->tag_count; i++) {
        if (i > 0) {
            buf_str(&body, ",");
        }
        buf_json(&body, cfg->tags[i].key);
        buf_str(&body, ":");
        buf_json(&body, cfg->tags[i].value);
    }
    buf_str(&body, "}}}");
    if (has_workspace(cfg)) {
        buf_str(&body, ",\"workspace\":");
        buf_json(&body, cfg->workspace_rid);
    }
    buf_str(&body, "}");

    if (request(cfg, "POST", "/scout/v1/run", &body, &response, err, err_len) == 0) {
        r = object_get(response.data, "rid");
        if (r != NULL && read_string(r, rid, rid_len) == 0) {
            st = NM_OK;
        } else {
            snprintf(err, err_len, "the response named no run");
        }
    }

    free(body.data);
    free(response.data);
    return st;
}

nm_status nm_run_end(const nm_run_config *cfg, const char *rid, int64_t end_nanos,
                     char *err, size_t err_len)
{
    buf       body = { 0 }, response = { 0 }, path = { 0 };
    nm_status st = NM_ERR_TRANSPORT;
    char      scratch[8];

    if (err == NULL || err_len == 0) {
        err     = scratch;
        err_len = sizeof scratch;
    }
    err[0] = '\0';
    if (cfg == NULL || missing(cfg->base_url) || missing(cfg->token) || missing(rid)) {
        return NM_ERR_ARG;
    }

    /* RIDs are lowercase letters, digits, dots and hyphens, so the path needs no
     * escaping. */
    buf_str(&path, "/scout/v1/run/");
    buf_str(&path, rid);

    buf_str(&body, "{\"endTime\":");
    buf_timestamp(&body, end_nanos);
    if (cfg->shared) {
        /* Only move the end later, so a shorter execution can't cut a shared run short. */
        buf_str(&body, ",\"strictOverwrite\":false");
    }
    buf_str(&body, "}");

    if (!path.failed &&
        request(cfg, "PUT", path.data, &body, &response, err, err_len) == 0) {
        st = NM_OK;
    }

    free(path.data);
    free(body.data);
    free(response.data);
    return st;
}
