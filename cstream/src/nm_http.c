#include "nm_http.h"
#include "nm_thread.h"   /* nm_sleep_ms, for backoff */

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Endpoint path, from the conjure service definition:
 *   NominalChannelWriterService::writeNominalBatches
 *   /storage/writer/v1/nominal/{dataSourceRid}
 */
#define NM_WRITE_PATH "/storage/writer/v1/nominal/"

struct nm_http {
    CURL              *curl;
    struct curl_slist *headers;
    char              *url;
    char               response[256];
    size_t             response_len;
};

/* Collect a bounded prefix of the response body. Errors arrive as a conjure envelope
 * carrying an errorInstanceId, which is the handle support needs -- so keep enough to
 * include it, and drop the rest rather than growing a buffer on the sender thread. */
static size_t collect_response(char *data, size_t size, size_t nmemb, void *userdata)
{
    nm_http *h = userdata;
    size_t   incoming = size * nmemb;
    size_t   room = sizeof h->response - h->response_len - 1;
    size_t   take = incoming < room ? incoming : room;

    if (take > 0) {
        memcpy(h->response + h->response_len, data, take);
        h->response_len += take;
        h->response[h->response_len] = '\0';
    }
    return incoming;   /* claim it all, or libcurl treats the short write as an error */
}

nm_http *nm_http_open(const char *base_url, const char *token,
                      const char *dataset_rid, const char *ca_bundle_path)
{
    nm_http *h;
    size_t   url_len;
    char    *auth = NULL;
    size_t   auth_len;

    if (base_url == NULL || token == NULL || dataset_rid == NULL) {
        return NULL;
    }

    h = calloc(1, sizeof *h);
    if (h == NULL) {
        return NULL;
    }

    url_len = strlen(base_url) + strlen(NM_WRITE_PATH) + strlen(dataset_rid) + 1;
    h->url = malloc(url_len);
    if (h->url == NULL) {
        goto fail;
    }
    snprintf(h->url, url_len, "%s%s%s", base_url, NM_WRITE_PATH, dataset_rid);

    auth_len = strlen("Authorization: Bearer ") + strlen(token) + 1;
    auth = malloc(auth_len);
    if (auth == NULL) {
        goto fail;
    }
    snprintf(auth, auth_len, "Authorization: Bearer %s", token);

    h->headers = curl_slist_append(h->headers, "Content-Type: application/x-protobuf");
    /* The server accepts identity, zstd, snappy, or no header at all; we send
     * uncompressed so that libzstd never has to cross-compile for QNX. */
    h->headers = curl_slist_append(h->headers, "Content-Encoding: identity");
    h->headers = curl_slist_append(h->headers, auth);
    /* libcurl otherwise adds "Expect: 100-continue" for larger bodies, which costs a
     * round trip per request for no benefit here. */
    h->headers = curl_slist_append(h->headers, "Expect:");
    free(auth);
    auth = NULL;
    if (h->headers == NULL) {
        goto fail;
    }

    h->curl = curl_easy_init();
    if (h->curl == NULL) {
        goto fail;
    }

    curl_easy_setopt(h->curl, CURLOPT_URL, h->url);
    curl_easy_setopt(h->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(h->curl, CURLOPT_HTTPHEADER, h->headers);
    curl_easy_setopt(h->curl, CURLOPT_WRITEFUNCTION, collect_response);
    curl_easy_setopt(h->curl, CURLOPT_WRITEDATA, h);
    curl_easy_setopt(h->curl, CURLOPT_NOSIGNAL, 1L);
    /* Matches the Rust client's posture: fail fast rather than hold rows hostage to a
     * stalled connection, since the ring is filling behind us either way. */
    curl_easy_setopt(h->curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(h->curl, CURLOPT_TIMEOUT, 30L);

    /*
     * TLS trust. An explicit bundle is the portable answer and the one a target will
     * use -- an RTOS has no OS trust store to fall back on, which is precisely what
     * made the Rust path's platform verifier a dead end for QNX.
     *
     * With no bundle given we ask libcurl for the operating system's store. That is
     * what makes desktop "just work", and without it an OpenSSL-backed libcurl built
     * without a baked-in CA path fails every request with
     * CURLE_PEER_FAILED_VERIFICATION.
     */
    if (ca_bundle_path != NULL) {
        curl_easy_setopt(h->curl, CURLOPT_CAINFO, ca_bundle_path);
    }
#ifdef CURLSSLOPT_NATIVE_CA
    else {
        curl_easy_setopt(h->curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
    }
#endif

    return h;

fail:
    free(auth);
    nm_http_close(h);
    return NULL;
}

void nm_http_post(nm_http *h, const uint8_t *body, size_t len,
                  int max_attempts, nm_http_result *out)
{
    int attempt;

    memset(out, 0, sizeof *out);
    if (h == NULL || body == NULL || len == 0) {
        snprintf(out->error, sizeof out->error, "invalid request");
        return;
    }

    curl_easy_setopt(h->curl, CURLOPT_POSTFIELDS, (const char *)body);
    curl_easy_setopt(h->curl, CURLOPT_POSTFIELDSIZE, (long)len);

    for (attempt = 1; attempt <= max_attempts; attempt++) {
        CURLcode rc;

        h->response[0]  = '\0';
        h->response_len = 0;

        rc = curl_easy_perform(h->curl);
        if (rc != CURLE_OK) {
            /* Certificate failures are the one transport error with an obvious
             * remedy, and the bare libcurl string does not suggest it. */
            if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE) {
                snprintf(out->error, sizeof out->error,
                         "%s -- set ca_bundle_path to a CA bundle (libcurl: %s)",
                         curl_easy_strerror(rc), curl_version());
            } else {
                snprintf(out->error, sizeof out->error, "%s", curl_easy_strerror(rc));
            }
            out->status = 0;

            /* No amount of retrying fixes a trust problem. */
            if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE) {
                return;
            }
        } else {
            curl_easy_getinfo(h->curl, CURLINFO_RESPONSE_CODE, &out->status);

            if (out->status >= 200 && out->status < 300) {
                out->ok = 1;
                out->error[0] = '\0';
                return;
            }

            snprintf(out->error, sizeof out->error, "%s",
                     h->response_len > 0 ? h->response : "(no response body)");

            /* 4xx will not become 2xx by repeating it. 5xx might -- but a malformed
             * body also reports as 500 here, so retrying is capped rather than
             * persistent. */
            if (out->status < 500) {
                return;
            }
        }

        if (attempt < max_attempts) {
            nm_sleep_ms(50u * (uint32_t)attempt);   /* brief linear backoff */
        }
    }
}

void nm_http_close(nm_http *h)
{
    if (h == NULL) {
        return;
    }
    if (h->curl != NULL) {
        curl_easy_cleanup(h->curl);
    }
    if (h->headers != NULL) {
        curl_slist_free_all(h->headers);
    }
    free(h->url);
    free(h);
}
