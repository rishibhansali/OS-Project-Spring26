#ifndef INDEX_H
#define INDEX_H

/*
 * index.h — in-memory inverted index and document map for the indexer.
 *
 * The indexer builds the entire index in memory as pages arrive via IPC,
 * then flushes everything to disk atomically when the sentinel is received.
 *
 * On startup, if existing index files are found they are loaded into memory
 * first so that new data is merged with (appended to) the previous run.
 *
 * On-disk layout (relative to out_dir):
 *   index/docs.tsv      — docid TAB url TAB filepath TAB depth
 *   index/dict.tsv      — term TAB offset TAB df
 *   index/postings.bin  — [uint32 count][uint32 docid × count] per term
 */

#include <stdint.h>
#include <stddef.h>
#include "indexer/tokenizer.h"

/* --- Postings list for one term. --- */
typedef struct {
    uint32_t *docids;   /* sorted ascending (after flush-time qsort) */
    uint32_t  count;
    uint32_t  capacity;
} postings_t;

/* --- One slot in the inverted-index hash map. --- */
typedef struct {
    char      *term;     /* heap-allocated; NULL = empty slot */
    postings_t postings;
} index_entry_t;

#define INDEX_INIT_CAPACITY  (1u << 17)  /* 131072, power of 2 */
#define INDEX_LOAD_THRESHOLD 0.65

/* --- In-memory inverted index (open-addressing hash map). --- */
typedef struct {
    index_entry_t *table;
    size_t         capacity;
    size_t         count;
} inverted_index_t;

/* --- One document in the document map. --- */
typedef struct {
    uint32_t  docid;
    char     *url;
    char     *filepath;
    uint16_t  depth;
} doc_entry_t;

typedef struct {
    doc_entry_t *entries;
    size_t       count;
    size_t       capacity;
} doc_map_t;

/* --- Top-level indexer context. --- */
typedef struct {
    inverted_index_t inv;
    doc_map_t        docs;
    char            *out_dir;  /* heap-allocated; NOT freed by indexer_destroy */
} indexer_ctx_t;

/*
 * Initialize the indexer context.
 * If out_dir/index/dict.tsv exists, loads the previous index into memory.
 * Returns 0 on success, -1 on error.
 */
int  indexer_init(indexer_ctx_t *ctx, const char *out_dir);

/*
 * Add one document to the in-memory index.
 * tokens is the list of words extracted from the document.
 */
void indexer_add_document(indexer_ctx_t *ctx,
                           uint32_t docid, uint16_t depth,
                           const char *url, const char *filepath,
                           token_list_t *tokens);

/*
 * Flush the complete in-memory index to disk:
 *   index/docs.tsv, index/dict.tsv, index/postings.bin
 * Also creates the index/ directory if needed.
 * Returns 0 on success, -1 on error.
 */
int  indexer_flush(indexer_ctx_t *ctx);

/* Free all in-memory resources. Does NOT free ctx itself. */
void indexer_destroy(indexer_ctx_t *ctx);

#endif /* INDEX_H */
