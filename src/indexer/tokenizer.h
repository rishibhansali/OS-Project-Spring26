#ifndef TOKENIZER_H
#define TOKENIZER_H

/*
 * tokenizer.h — HTML tag stripping and word tokenization.
 *
 * Used by the indexer to convert saved HTML into a list of
 * lowercase alphabetic tokens suitable for indexing.
 */

#include <stddef.h>

#define TOKEN_MIN_LEN  2
#define TOKEN_MAX_LEN 128

typedef struct {
    char  **words;    /* array of heap-allocated lowercase token strings */
    size_t  count;
    size_t  capacity;
} token_list_t;

/*
 * Tokenize an HTML document:
 *   1. Strip all HTML tags (simple state machine; skips <script>/<style> blocks)
 *   2. Decode basic HTML entities (&amp; &lt; &gt; &nbsp;)
 *   3. Split on whitespace and punctuation
 *   4. Lowercase each token; discard tokens outside [TOKEN_MIN_LEN, TOKEN_MAX_LEN]
 *   5. Filter common English stopwords
 *
 * Returns a heap-allocated token_list_t; caller must call token_list_free().
 * Returns NULL on severe allocation failure.
 */
token_list_t *tokenize_html(const char *html, size_t len);

/* Returns 1 if word is a common English stopword (lowercase input). */
int is_stopword(const char *word);

/* Free a token_list_t and all contained strings. */
void token_list_free(token_list_t *tl);

#endif /* TOKENIZER_H */
