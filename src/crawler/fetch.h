#ifndef FETCH_H
#define FETCH_H

/*
 * fetch.h — HTTP fetching via libcurl.
 *
 * Each worker thread creates one CURL handle via fetch_init_handle(),
 * reuses it for every request (keep-alive), and destroys it on exit.
 *
 * curl_global_init() must be called in main() before any threads start.
 */

#include <stddef.h>
#include <curl/curl.h>

#define FETCH_TIMEOUT_CONNECT_SECS  10L
#define FETCH_TIMEOUT_TOTAL_SECS    30L
#define FETCH_MAX_REDIRECTS          5L
#define FETCH_MAX_FILESIZE_BYTES    (10L * 1024L * 1024L)  /* 10 MB */
#define FETCH_USER_AGENT            "webcrawler/1.0"

typedef struct {
    char    *data;                      /* response body (null-terminated) */
    size_t   size;                      /* bytes in data (without null)    */
    long     http_code;                 /* e.g. 200, 404, 0 on libcurl err */
    CURLcode curl_code;                 /* CURLE_OK on success             */
    char     error_buf[CURL_ERROR_SIZE];/* human-readable error string     */
} fetch_result_t;

/*
 * Create and configure a reusable CURL easy handle for one worker thread.
 * Returns NULL on failure.
 */
CURL *fetch_init_handle(void);

/*
 * Perform an HTTP GET of url using the given (thread-local) handle.
 * Returns a heap-allocated fetch_result_t; caller must call fetch_result_free().
 *
 * On libcurl failure: result->curl_code != CURLE_OK.
 * On HTTP error (4xx/5xx): result->http_code reflects it; data still present.
 * Returns NULL only on severe allocation failure.
 */
fetch_result_t *fetch_url(CURL *handle, const char *url);

/* Free a fetch_result_t and its data buffer. */
void fetch_result_free(fetch_result_t *r);

/* Clean up a CURL easy handle obtained from fetch_init_handle(). */
void fetch_cleanup_handle(CURL *handle);

#endif /* FETCH_H */
