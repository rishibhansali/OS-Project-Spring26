#ifndef PARSE_H
#define PARSE_H

/*
 * parse.h — HTML link extraction and URL normalization via libxml2.
 *
 * xmlInitParser() should be called in main() before threads start.
 * Each call to parse_extract_links() creates and frees its own xmlDoc,
 * so concurrent calls from different threads are safe.
 */

#include <stddef.h>

/* A growable list of heap-allocated URL strings. */
typedef struct {
    char **urls;
    int    count;
    int    capacity;
} link_list_t;

/*
 * Parse html (html_len bytes) and extract all <a href> links.
 * base_url is used to resolve relative references.
 *
 * Returns a heap-allocated link_list_t whose urls[] are individually
 * heap-allocated normalized strings.  Caller must call link_list_free().
 * Returns NULL on severe failure (OOM, etc.).
 */
link_list_t *parse_extract_links(const char *html, size_t html_len,
                                  const char *base_url);

/* Free a link_list_t and all contained URL strings. */
void link_list_free(link_list_t *ll);

/*
 * Normalize raw_url relative to base_url:
 *   - Resolve relative references (RFC 3986)
 *   - Lowercase scheme and host
 *   - Strip fragment (#...)
 *   - Remove default ports (:80 for http, :443 for https)
 *   - Reject non-HTTP/HTTPS schemes (mailto:, javascript:, data:, ...)
 *
 * Returns a heap-allocated normalized URL string, or NULL if the URL
 * is invalid or uses an unsupported scheme. Caller must free().
 */
char *url_normalize(const char *raw_url, const char *base_url);

/* Returns 1 if the URL scheme is http or https (case-insensitive). */
int url_is_http(const char *url);

#endif /* PARSE_H */
