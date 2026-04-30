#include "crawler/parse.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>

#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <libxml/uri.h>

/* -----------------------------------------------------------------------
 * link_list helpers
 * ----------------------------------------------------------------------- */

static link_list_t *link_list_new(void)
{
    link_list_t *ll = malloc(sizeof(*ll));
    if (!ll) return NULL;
    ll->count    = 0;
    ll->capacity = 16;
    ll->urls     = malloc((size_t)ll->capacity * sizeof(char *));
    if (!ll->urls) { free(ll); return NULL; }
    return ll;
}

static void link_list_append(link_list_t *ll, char *url)
{
    if (ll->count == ll->capacity) {
        int new_cap = ll->capacity * 2;
        char **tmp  = realloc(ll->urls, (size_t)new_cap * sizeof(char *));
        if (!tmp) { free(url); return; }
        ll->urls     = tmp;
        ll->capacity = new_cap;
    }
    ll->urls[ll->count++] = url;
}

void link_list_free(link_list_t *ll)
{
    if (!ll) return;
    for (int i = 0; i < ll->count; i++) free(ll->urls[i]);
    free(ll->urls);
    free(ll);
}

/* -----------------------------------------------------------------------
 * URL normalization
 * ----------------------------------------------------------------------- */

int url_is_http(const char *url)
{
    if (!url) return 0;
    /* Case-insensitive prefix check. */
    if (strncasecmp(url, "http://",  7) == 0) return 1;
    if (strncasecmp(url, "https://", 8) == 0) return 1;
    return 0;
}

char *url_normalize(const char *raw_url, const char *base_url)
{
    if (!raw_url || raw_url[0] == '\0') return NULL;

    /* Skip javascript:, mailto:, data:, tel:, etc. */
    const char *colon = strchr(raw_url, ':');
    const char *slash = strchr(raw_url, '/');
    if (colon && (!slash || colon < slash)) {
        /* Has a scheme before the first slash — only accept http/https. */
        if (!url_is_http(raw_url)) return NULL;
    }

    /* Resolve relative reference against base. */
    xmlChar *base = xmlCharStrdup(base_url ? base_url : "");
    xmlChar *href = xmlCharStrdup(raw_url);
    xmlChar *abs  = xmlBuildURI(href, base);
    xmlFree(href);
    xmlFree(base);

    if (!abs) return NULL;

    /* Only keep http/https. */
    if (!url_is_http((char *)abs)) {
        xmlFree(abs);
        return NULL;
    }

    /* Parse the absolute URI to normalize its components. */
    xmlURIPtr uri = xmlParseURI((char *)abs);
    xmlFree(abs);
    if (!uri) return NULL;

    /* Lowercase scheme. */
    if (uri->scheme) {
        for (char *p = uri->scheme; *p; p++) *p = (char)tolower((unsigned char)*p);
    }

    /* Lowercase host. */
    if (uri->server) {
        for (char *p = uri->server; *p; p++) *p = (char)tolower((unsigned char)*p);
    }

    /* Strip fragment. */
    if (uri->fragment) {
        xmlFree(uri->fragment);
        uri->fragment = NULL;
    }

    /* Remove default ports. */
    if (uri->port != 0) {
        if ((strcmp(uri->scheme, "http")  == 0 && uri->port == 80)  ||
            (strcmp(uri->scheme, "https") == 0 && uri->port == 443))
            uri->port = 0;
    }

    /* Reconstruct the URI string. */
    char *result_str = (char *)xmlSaveUri(uri);
    xmlFreeURI(uri);

    if (!result_str) return NULL;

    /* xmlSaveUri returns xmlChar* (unsigned char*); dup as plain char*. */
    char *out = strdup(result_str);
    xmlFree(result_str);
    return out;
}

/* -----------------------------------------------------------------------
 * DOM walker: collect all <a href> links
 * ----------------------------------------------------------------------- */

static void walk_nodes(xmlNode *node, const char *base_url, link_list_t *out)
{
    for (xmlNode *cur = node; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE &&
            xmlStrcasecmp(cur->name, (const xmlChar *)"a") == 0) {

            xmlChar *href = xmlGetProp(cur, (const xmlChar *)"href");
            if (href) {
                char *norm = url_normalize((char *)href, base_url);
                if (norm) link_list_append(out, norm);
                xmlFree(href);
            }
        }
        /* Recurse into children. */
        if (cur->children)
            walk_nodes(cur->children, base_url, out);
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

link_list_t *parse_extract_links(const char *html, size_t html_len,
                                   const char *base_url)
{
    link_list_t *ll = link_list_new();
    if (!ll) return NULL;

    if (!html || html_len == 0) return ll;

    int options = HTML_PARSE_NOWARNING | HTML_PARSE_NOERROR |
                  HTML_PARSE_RECOVER   | HTML_PARSE_NONET;

    htmlDocPtr doc = htmlReadMemory(html, (int)html_len,
                                    base_url, "UTF-8", options);
    if (!doc) return ll;

    xmlNode *root = xmlDocGetRootElement(doc);
    if (root) walk_nodes(root, base_url, ll);

    xmlFreeDoc(doc);
    return ll;
}
