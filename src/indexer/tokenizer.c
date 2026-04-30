#include "indexer/tokenizer.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Stopword list (sorted for bsearch)
 * ----------------------------------------------------------------------- */
static const char *stopwords[] = {
    "a", "about", "above", "after", "again", "all", "also", "am", "an",
    "and", "any", "are", "as", "at", "be", "been", "being", "between",
    "both", "but", "by", "can", "could", "did", "do", "does", "doing",
    "down", "during", "each", "few", "for", "from", "further", "get",
    "got", "had", "has", "have", "having", "he", "her", "here", "hers",
    "herself", "him", "himself", "his", "how", "i", "if", "in", "into",
    "is", "it", "its", "itself", "just", "me", "more", "most", "my",
    "myself", "no", "not", "now", "of", "off", "on", "once", "only",
    "or", "other", "our", "out", "own", "re", "same", "she", "should",
    "so", "some", "such", "than", "that", "the", "their", "them",
    "themselves", "then", "there", "these", "they", "this", "those",
    "through", "to", "too", "under", "until", "up", "us", "very",
    "was", "we", "were", "what", "when", "where", "which", "while",
    "who", "whom", "why", "will", "with", "would", "you", "your",
    "yours", "yourself"
};
#define N_STOPWORDS ((int)(sizeof(stopwords) / sizeof(stopwords[0])))

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, *(const char **)b);
}

int is_stopword(const char *word)
{
    return bsearch(word, stopwords, (size_t)N_STOPWORDS,
                   sizeof(char *), cmp_str) != NULL;
}

/* -----------------------------------------------------------------------
 * token_list helpers
 * ----------------------------------------------------------------------- */

static token_list_t *tlist_new(void)
{
    token_list_t *tl = malloc(sizeof(*tl));
    if (!tl) return NULL;
    tl->count    = 0;
    tl->capacity = 64;
    tl->words    = malloc(tl->capacity * sizeof(char *));
    if (!tl->words) { free(tl); return NULL; }
    return tl;
}

static void tlist_append(token_list_t *tl, char *word)
{
    if (tl->count == tl->capacity) {
        size_t nc   = tl->capacity * 2;
        char **tmp  = realloc(tl->words, nc * sizeof(char *));
        if (!tmp) { free(word); return; }
        tl->words    = tmp;
        tl->capacity = nc;
    }
    tl->words[tl->count++] = word;
}

void token_list_free(token_list_t *tl)
{
    if (!tl) return;
    for (size_t i = 0; i < tl->count; i++) free(tl->words[i]);
    free(tl->words);
    free(tl);
}

/* -----------------------------------------------------------------------
 * Tag stripping: returns heap-allocated plain text (caller frees).
 * Replaces every tag with a space; skips <script> and <style> bodies.
 * ----------------------------------------------------------------------- */
static char *strip_tags(const char *html, size_t len, size_t *out_len)
{
    char  *out    = malloc(len + 2);
    if (!out) return NULL;
    size_t j      = 0;
    int    in_tag = 0;

    for (size_t i = 0; i < len; ) {
        if (html[i] == '<') {
            /* Check for <script or <style to skip their content. */
            const char *skip_end = NULL;
            if (i + 7 <= len &&
                strncasecmp(html + i + 1, "script", 6) == 0 &&
                (isspace((unsigned char)html[i+7]) || html[i+7] == '>'))
                skip_end = "</script>";
            else if (i + 6 <= len &&
                     strncasecmp(html + i + 1, "style", 5) == 0 &&
                     (isspace((unsigned char)html[i+6]) || html[i+6] == '>'))
                skip_end = "</style>";

            if (skip_end) {
                /* Scan forward to closing tag. */
                size_t end_len = strlen(skip_end);
                i++;
                while (i + end_len <= len) {
                    if (strncasecmp(html + i, skip_end, end_len) == 0) {
                        i += end_len;
                        break;
                    }
                    i++;
                }
                out[j++] = ' ';
                continue;
            }

            in_tag = 1;
            i++;
            continue;
        }

        if (html[i] == '>') {
            in_tag = 0;
            out[j++] = ' ';
            i++;
            continue;
        }

        if (!in_tag) {
            out[j++] = html[i];
        }
        i++;
    }
    out[j] = '\0';
    *out_len = j;
    return out;
}

/* Decode a handful of common HTML entities in-place (dst may equal src). */
static size_t decode_entities(char *dst, const char *src, size_t len)
{
    size_t j = 0;
    for (size_t i = 0; i < len; ) {
        if (src[i] == '&') {
            if (strncmp(src+i, "&amp;",  5) == 0) { dst[j++]=' '; i+=5; }
            else if (strncmp(src+i, "&lt;",  4) == 0) { dst[j++]=' '; i+=4; }
            else if (strncmp(src+i, "&gt;",  4) == 0) { dst[j++]=' '; i+=4; }
            else if (strncmp(src+i, "&nbsp;",6) == 0) { dst[j++]=' '; i+=6; }
            else if (strncmp(src+i, "&quot;",6) == 0) { dst[j++]=' '; i+=6; }
            else { dst[j++] = src[i++]; }
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
    return j;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

token_list_t *tokenize_html(const char *html, size_t len)
{
    token_list_t *tl = tlist_new();
    if (!tl) return NULL;
    if (!html || len == 0) return tl;

    /* Step 1: strip tags. */
    size_t text_len;
    char *text = strip_tags(html, len, &text_len);
    if (!text) { token_list_free(tl); return NULL; }

    /* Step 2: decode entities in-place. */
    text_len = decode_entities(text, text, text_len);

    /* Step 3: tokenize. */
    /* Delimiters: anything that is not alphanumeric. */
    char *saveptr = NULL;
    char *tok     = strtok_r(text, " \t\n\r\f\v"
                                   ".,;:!?\"'()-[]{}/<>@#$%^&*+=|\\~`_", &saveptr);
    while (tok) {
        size_t tlen = strlen(tok);

        /* Step 4: filter by length. */
        if (tlen >= TOKEN_MIN_LEN && tlen <= TOKEN_MAX_LEN) {
            /* Lowercase. */
            char lower[TOKEN_MAX_LEN + 1];
            for (size_t k = 0; k < tlen; k++)
                lower[k] = (char)tolower((unsigned char)tok[k]);
            lower[tlen] = '\0';

            /* Step 5: filter stopwords. */
            if (!is_stopword(lower)) {
                char *copy = strdup(lower);
                if (copy) tlist_append(tl, copy);
            }
        }

        tok = strtok_r(NULL, " \t\n\r\f\v"
                             ".,;:!?\"'()-[]{}/<>@#$%^&*+=|\\~`_", &saveptr);
    }

    free(text);
    return tl;
}
