#include "query/index_reader.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>

/* -----------------------------------------------------------------------
 * Dictionary
 * ----------------------------------------------------------------------- */

static int cmp_dict_entry(const void *a, const void *b)
{
    return strcmp(((const dict_entry_t *)a)->term,
                  ((const dict_entry_t *)b)->term);
}

int dict_load(dict_t *d, const char *dict_path)
{
    d->entries = NULL;
    d->count   = 0;

    FILE *f = fopen(dict_path, "r");
    if (!f) {
        fprintf(stderr, "[QUERY] Cannot open %s: %s\n",
                dict_path, strerror(errno));
        return -1;
    }

    size_t capacity = 256;
    d->entries = malloc(capacity * sizeof(dict_entry_t));
    if (!d->entries) { fclose(f); return -1; }

    char   *line  = NULL;
    size_t  llen  = 0;
    ssize_t n;

    while ((n = getline(&line, &llen, f)) > 0) {
        if (line[n-1] == '\n') line[n-1] = '\0';
        if (line[0] == '\0')   continue;

        char *saveptr = NULL;
        char *s_term = strtok_r(line,  "\t", &saveptr);
        char *s_off  = strtok_r(NULL,  "\t", &saveptr);
        char *s_df   = strtok_r(NULL,  "\t", &saveptr);
        if (!s_term || !s_off || !s_df) continue;

        if (d->count == capacity) {
            capacity *= 2;
            dict_entry_t *tmp = realloc(d->entries,
                                         capacity * sizeof(dict_entry_t));
            if (!tmp) { free(line); fclose(f); dict_free(d); return -1; }
            d->entries = tmp;
        }

        dict_entry_t *e = &d->entries[d->count];
        e->term   = strdup(s_term);
        e->offset = strtoull(s_off, NULL, 10);
        e->df     = (uint32_t)strtoul(s_df, NULL, 10);
        if (!e->term) continue;
        d->count++;
    }

    free(line);
    fclose(f);

    /* Sort by term for binary search. */
    if (d->count > 0)
        qsort(d->entries, d->count, sizeof(dict_entry_t), cmp_dict_entry);

    return 0;
}

void dict_free(dict_t *d)
{
    if (!d->entries) return;
    for (size_t i = 0; i < d->count; i++) free(d->entries[i].term);
    free(d->entries);
    d->entries = NULL;
    d->count   = 0;
}

int dict_lookup(const dict_t *d, const char *term)
{
    if (d->count == 0) return -1;

    /* Use a dummy entry for bsearch key. */
    dict_entry_t key;
    key.term = (char *)term; /* safe: cmp only reads it */

    const dict_entry_t *found =
        bsearch(&key, d->entries, d->count,
                sizeof(dict_entry_t), cmp_dict_entry);

    if (!found) return -1;
    return (int)(found - d->entries);
}

/* -----------------------------------------------------------------------
 * Document map
 * ----------------------------------------------------------------------- */

static int cmp_doc_by_docid(const void *a, const void *b)
{
    uint32_t x = ((const doc_info_t *)a)->docid;
    uint32_t y = ((const doc_info_t *)b)->docid;
    return (x > y) - (x < y);
}

int docmap_load(docmap_t *dm, const char *docs_path)
{
    dm->entries = NULL;
    dm->count   = 0;

    FILE *f = fopen(docs_path, "r");
    if (!f) {
        fprintf(stderr, "[QUERY] Cannot open %s: %s\n",
                docs_path, strerror(errno));
        return -1;
    }

    size_t capacity = 256;
    dm->entries = malloc(capacity * sizeof(doc_info_t));
    if (!dm->entries) { fclose(f); return -1; }

    char   *line = NULL;
    size_t  llen = 0;
    ssize_t n;

    while ((n = getline(&line, &llen, f)) > 0) {
        if (line[n-1] == '\n') line[n-1] = '\0';
        if (line[0] == '\0')   continue;

        char *saveptr = NULL;
        char *s_docid = strtok_r(line, "\t", &saveptr);
        char *s_url   = strtok_r(NULL, "\t", &saveptr);
        char *s_fpath = strtok_r(NULL, "\t", &saveptr);
        char *s_depth = strtok_r(NULL, "\t", &saveptr);
        if (!s_docid || !s_url) continue;

        if (dm->count == capacity) {
            capacity *= 2;
            doc_info_t *tmp = realloc(dm->entries,
                                       capacity * sizeof(doc_info_t));
            if (!tmp) { free(line); fclose(f); docmap_free(dm); return -1; }
            dm->entries = tmp;
        }

        doc_info_t *e = &dm->entries[dm->count];
        e->docid    = (uint32_t)strtoul(s_docid, NULL, 10);
        e->url      = strdup(s_url);
        e->filepath = s_fpath ? strdup(s_fpath) : strdup("");
        e->depth    = s_depth ? (uint16_t)strtoul(s_depth, NULL, 10) : 0;
        if (!e->url || !e->filepath) {
            free(e->url);
            free(e->filepath);
            continue;
        }
        dm->count++;
    }

    free(line);
    fclose(f);

    if (dm->count > 0)
        qsort(dm->entries, dm->count, sizeof(doc_info_t), cmp_doc_by_docid);

    return 0;
}

void docmap_free(docmap_t *dm)
{
    if (!dm->entries) return;
    for (size_t i = 0; i < dm->count; i++) {
        free(dm->entries[i].url);
        free(dm->entries[i].filepath);
    }
    free(dm->entries);
    dm->entries = NULL;
    dm->count   = 0;
}

const char *docmap_get_url(const docmap_t *dm, uint32_t docid)
{
    if (dm->count == 0) return NULL;
    doc_info_t key;
    key.docid = docid;
    const doc_info_t *found =
        bsearch(&key, dm->entries, dm->count,
                sizeof(doc_info_t), cmp_doc_by_docid);
    return found ? found->url : NULL;
}

/* -----------------------------------------------------------------------
 * Postings
 * ----------------------------------------------------------------------- */

uint32_t *postings_read(FILE *pf, uint64_t offset, uint32_t *count_out)
{
    if (fseeko(pf, (off_t)offset, SEEK_SET) != 0) return NULL;

    uint32_t count = 0;
    if (fread(&count, sizeof(uint32_t), 1, pf) != 1) return NULL;
    if (count == 0) { *count_out = 0; return NULL; }

    uint32_t *docids = malloc(count * sizeof(uint32_t));
    if (!docids) return NULL;

    if (fread(docids, sizeof(uint32_t), count, pf) != count) {
        free(docids);
        return NULL;
    }

    *count_out = count;
    return docids;
}

/* -----------------------------------------------------------------------
 * AND intersection of sorted arrays
 * ----------------------------------------------------------------------- */

uint32_t *intersect_sorted(uint32_t **lists, uint32_t *lengths,
                             int n_lists, uint32_t *result_count)
{
    *result_count = 0;
    if (n_lists <= 0) return NULL;

    /* Copy the first list as the working result. */
    uint32_t *result = malloc(lengths[0] * sizeof(uint32_t));
    if (!result) return NULL;
    memcpy(result, lists[0], lengths[0] * sizeof(uint32_t));
    uint32_t rcount = lengths[0];

    /* Successively intersect with each remaining list. */
    for (int i = 1; i < n_lists; i++) {
        uint32_t *merged = malloc(rcount * sizeof(uint32_t));
        if (!merged) { free(result); return NULL; }

        uint32_t j = 0, k = 0, m = 0;
        while (j < rcount && k < lengths[i]) {
            if (result[j] == lists[i][k]) {
                merged[m++] = result[j];
                j++; k++;
            } else if (result[j] < lists[i][k]) {
                j++;
            } else {
                k++;
            }
        }
        free(result);
        result = merged;
        rcount = m;
        if (rcount == 0) break;
    }

    *result_count = rcount;
    if (rcount == 0) { free(result); return NULL; }
    return result;
}
