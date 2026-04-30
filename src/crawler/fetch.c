#include "crawler/fetch.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Internal write callback for libcurl.
 * Accumulates response body into a dynamically grown heap buffer.
 * ----------------------------------------------------------------------- */
typedef struct {
    char  *buf;
    size_t size;
} write_ctx_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t bytes = size * nmemb;
    write_ctx_t *ctx = (write_ctx_t *)userdata;

    char *tmp = realloc(ctx->buf, ctx->size + bytes + 1);
    if (!tmp) return 0; /* returning 0 causes CURLE_WRITE_ERROR */

    ctx->buf = tmp;
    memcpy(ctx->buf + ctx->size, ptr, bytes);
    ctx->size += bytes;
    ctx->buf[ctx->size] = '\0';
    return bytes;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

CURL *fetch_init_handle(void)
{
    return curl_easy_init();
}

fetch_result_t *fetch_url(CURL *handle, const char *url)
{
    fetch_result_t *result = calloc(1, sizeof(*result));
    if (!result) return NULL;

    write_ctx_t wctx = { NULL, 0 };

    /* Reset handle to defaults so per-request options start clean. */
    curl_easy_reset(handle);

    curl_easy_setopt(handle, CURLOPT_URL,             url);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION,   write_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA,       &wctx);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER,     result->error_buf);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS,       FETCH_MAX_REDIRECTS);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT,  FETCH_TIMEOUT_CONNECT_SECS);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT,         FETCH_TIMEOUT_TOTAL_SECS);
    curl_easy_setopt(handle, CURLOPT_MAXFILESIZE_LARGE,
                     (curl_off_t)FETCH_MAX_FILESIZE_BYTES);
    curl_easy_setopt(handle, CURLOPT_USERAGENT,       FETCH_USER_AGENT);
    /* Accept compressed responses and decompress automatically. */
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    /* Don't verify SSL certificates (simplifies crawling; log errors). */
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER,  0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST,  0L);

    result->curl_code = curl_easy_perform(handle);

    if (result->curl_code == CURLE_OK) {
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &result->http_code);
    } else {
        /* Ensure error_buf is printable even if curl didn't set it. */
        if (result->error_buf[0] == '\0')
            strncpy(result->error_buf,
                    curl_easy_strerror(result->curl_code),
                    CURL_ERROR_SIZE - 1);
    }

    result->data = wctx.buf; /* may be NULL if nothing was written */
    result->size = wctx.size;
    return result;
}

void fetch_result_free(fetch_result_t *r)
{
    if (!r) return;
    free(r->data);
    free(r);
}

void fetch_cleanup_handle(CURL *handle)
{
    if (handle) curl_easy_cleanup(handle);
}
