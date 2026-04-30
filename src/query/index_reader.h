#ifndef INDEX_READER_H
#define INDEX_READER_H

/*
 * index_reader.h — read the on-disk inverted index for the query tool.
 *
 * Expected file layout (relative to the index directory):
 *   docs.tsv      — docid TAB url TAB filepath TAB depth
 *   dict.tsv      — term TAB offset TAB df
 *   postings.bin  — [uint32 count][uint32 docid × count] per term
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Dictionary
 * ----------------------------------------------------------------------- */

typedef struct {
    char    *term;     /* heap-allocated; sorted for binary search */
    uint64_t offset;   /* byte offset into postings.bin            */
    uint32_t df;       /* document frequency                       */
} dict_entry_t;

typedef struct {
    dict_entry_t *entries;
    size_t        count;
} dict_t;

/*
 * Load all entries from dict.tsv and sort them by term.
 * Returns 0 on success, -1 on error.
 */
int  dict_load(dict_t *d, const char *dict_path);
void dict_free(dict_t *d);

/*
 * Binary search for a lowercase term in the sorted dict.
 * Returns the index into d->entries, or -1 if not found.
 */
int  dict_lookup(const dict_t *d, const char *term);

/* -----------------------------------------------------------------------
 * Document map
 * ----------------------------------------------------------------------- */

typedef struct {
    uint32_t  docid;
    char     *url;       /* heap-allocated */
    char     *filepath;  /* heap-allocated */
    uint16_t  depth;
} doc_info_t;

typedef struct {
    doc_info_t *entries;
    size_t      count;
} docmap_t;

/*
 * Load all entries from docs.tsv.
 * Entries are sorted by docid for O(log n) lookup.
 * Returns 0 on success, -1 on error.
 */
int  docmap_load(docmap_t *dm, const char *docs_path);
void docmap_free(docmap_t *dm);

/*
 * Look up the URL for a given docid.
 * Returns a pointer into dm->entries[i].url, or NULL if not found.
 */
const char *docmap_get_url(const docmap_t *dm, uint32_t docid);

/* -----------------------------------------------------------------------
 * Postings
 * ----------------------------------------------------------------------- */

/*
 * Read the postings list for one term from postings.bin.
 * Seeks pf to the given byte offset, reads the count header, then
 * allocates and fills a docid array.
 *
 * Returns a heap-allocated array of *count_out docids (sorted ascending),
 * or NULL on error.  Caller must free().
 */
uint32_t *postings_read(FILE *pf, uint64_t offset, uint32_t *count_out);

/* -----------------------------------------------------------------------
 * Sorted-intersection (AND query)
 * ----------------------------------------------------------------------- */

/*
 * Compute the AND intersection of n_lists sorted uint32_t arrays.
 * lists[i]   — pointer to sorted docid array for term i
 * lengths[i] — number of elements in lists[i]
 *
 * Returns a heap-allocated sorted array of docids present in ALL lists.
 * *result_count is set to the number of results (0 if empty).
 * Returns NULL (with *result_count = 0) if no common docids.
 * Caller must free() the returned array.
 */
uint32_t *intersect_sorted(uint32_t **lists, uint32_t *lengths,
                            int n_lists, uint32_t *result_count);

#endif /* INDEX_READER_H */
