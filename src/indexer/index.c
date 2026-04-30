#include "indexer/index.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <inttypes.h>

/* -----------------------------------------------------------------------
 * FNV-1a hash (duplicated from visited.c to keep modules self-contained)
 * ----------------------------------------------------------------------- */
static uint32_t fnv1a_32(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        h = (h ^ (uint32_t)*p) * 16777619u;
    return h;
}

/* -----------------------------------------------------------------------
 * Postings helpers
 * ----------------------------------------------------------------------- */

static int postings_init(postings_t *p)
{
    p->capacity = 8;
    p->count    = 0;
    p->docids   = malloc(p->capacity * sizeof(uint32_t));
    return p->docids ? 0 : -1;
}

static void postings_append(postings_t *p, uint32_t docid)
{
    /* Avoid duplicate docids (same document may have same word many times). */
    if (p->count > 0 && p->docids[p->count - 1] == docid) return;

    if (p->count == p->capacity) {
        uint32_t nc  = p->capacity * 2;
        uint32_t *tmp = realloc(p->docids, nc * sizeof(uint32_t));
        if (!tmp) return;
        p->docids   = tmp;
        p->capacity = nc;
    }
    p->docids[p->count++] = docid;
}

/* -----------------------------------------------------------------------
 * Inverted index (open-addressing hash map)
 * ----------------------------------------------------------------------- */

static int inv_init(inverted_index_t *inv, size_t capacity)
{
    inv->table    = calloc(capacity, sizeof(index_entry_t));
    if (!inv->table) return -1;
    inv->capacity = capacity;
    inv->count    = 0;
    return 0;
}

static void inv_destroy(inverted_index_t *inv)
{
    for (size_t i = 0; i < inv->capacity; i++) {
        if (inv->table[i].term) {
            free(inv->table[i].term);
            free(inv->table[i].postings.docids);
        }
    }
    free(inv->table);
    inv->table = NULL;
}

static int inv_resize(inverted_index_t *inv, size_t new_cap);

/* Find or create a slot for term.  Returns pointer to slot, or NULL on OOM. */
static index_entry_t *inv_find_or_create(inverted_index_t *inv, const char *term)
{
    /* Resize if needed before lookup. */
    if ((double)inv->count / (double)inv->capacity > INDEX_LOAD_THRESHOLD) {
        if (inv_resize(inv, inv->capacity * 2) < 0) return NULL;
    }

    uint32_t h    = fnv1a_32(term);
    size_t   mask = inv->capacity - 1;
    size_t   slot = h & mask;

    for (size_t i = 0; i < inv->capacity; i++) {
        size_t idx = (slot + i) & mask;
        index_entry_t *e = &inv->table[idx];

        if (!e->term) {
            /* Empty slot — create new entry. */
            e->term = strdup(term);
            if (!e->term) return NULL;
            if (postings_init(&e->postings) < 0) {
                free(e->term); e->term = NULL;
                return NULL;
            }
            inv->count++;
            return e;
        }
        if (strcmp(e->term, term) == 0) return e;
    }
    return NULL; /* should never reach here after resize */
}

static int inv_resize(inverted_index_t *inv, size_t new_cap)
{
    index_entry_t *new_table = calloc(new_cap, sizeof(index_entry_t));
    if (!new_table) return -1;

    size_t mask = new_cap - 1;
    for (size_t i = 0; i < inv->capacity; i++) {
        index_entry_t *e = &inv->table[i];
        if (!e->term) continue;
        uint32_t h    = fnv1a_32(e->term);
        size_t   slot = h & mask;
        while (new_table[slot].term)
            slot = (slot + 1) & mask;
        new_table[slot] = *e; /* copy struct */
    }
    free(inv->table);
    inv->table    = new_table;
    inv->capacity = new_cap;
    return 0;
}

/* -----------------------------------------------------------------------
 * Document map
 * ----------------------------------------------------------------------- */

static int docmap_init(doc_map_t *dm)
{
    dm->count    = 0;
    dm->capacity = 256;
    dm->entries  = malloc(dm->capacity * sizeof(doc_entry_t));
    return dm->entries ? 0 : -1;
}

static void docmap_destroy(doc_map_t *dm)
{
    for (size_t i = 0; i < dm->count; i++) {
        free(dm->entries[i].url);
        free(dm->entries[i].filepath);
    }
    free(dm->entries);
    dm->entries = NULL;
}

static void docmap_append(doc_map_t *dm,
                           uint32_t docid, uint16_t depth,
                           const char *url, const char *filepath)
{
    if (dm->count == dm->capacity) {
        size_t nc = dm->capacity * 2;
        doc_entry_t *tmp = realloc(dm->entries, nc * sizeof(doc_entry_t));
        if (!tmp) return;
        dm->entries  = tmp;
        dm->capacity = nc;
    }
    doc_entry_t *e = &dm->entries[dm->count++];
    e->docid    = docid;
    e->depth    = depth;
    e->url      = strdup(url);
    e->filepath = strdup(filepath);
}

/* -----------------------------------------------------------------------
 * Load existing index from disk (for append/merge across runs)
 * ----------------------------------------------------------------------- */

static int load_existing_index(indexer_ctx_t *ctx)
{
    char dict_path[4096], docs_path[4096], post_path[4096];
    snprintf(dict_path, sizeof(dict_path), "%s/index/dict.tsv",     ctx->out_dir);
    snprintf(docs_path, sizeof(docs_path), "%s/index/docs.tsv",     ctx->out_dir);
    snprintf(post_path, sizeof(post_path), "%s/index/postings.bin", ctx->out_dir);

    /* Load docs.tsv. */
    FILE *fd = fopen(docs_path, "r");
    if (fd) {
        char *line  = NULL;
        size_t llen = 0;
        ssize_t n;
        while ((n = getline(&line, &llen, fd)) > 0) {
            if (line[n-1] == '\n') line[n-1] = '\0';
            char *saveptr = NULL;
            char *s_docid = strtok_r(line,    "\t", &saveptr);
            char *s_url   = strtok_r(NULL,    "\t", &saveptr);
            char *s_fpath = strtok_r(NULL,    "\t", &saveptr);
            char *s_depth = strtok_r(NULL,    "\t", &saveptr);
            if (!s_docid || !s_url || !s_fpath || !s_depth) continue;
            uint32_t docid = (uint32_t)strtoul(s_docid, NULL, 10);
            uint16_t depth = (uint16_t)strtoul(s_depth, NULL, 10);
            docmap_append(&ctx->docs, docid, depth, s_url, s_fpath);
        }
        free(line);
        fclose(fd);
    }

    /* Load dict.tsv + postings.bin together. */
    FILE *fd_dict = fopen(dict_path, "r");
    FILE *fd_post = fopen(post_path, "rb");
    if (fd_dict && fd_post) {
        char *line  = NULL;
        size_t llen = 0;
        ssize_t n;
        while ((n = getline(&line, &llen, fd_dict)) > 0) {
            if (line[n-1] == '\n') line[n-1] = '\0';
            char *saveptr = NULL;
            char *s_term = strtok_r(line, "\t", &saveptr);
            char *s_off  = strtok_r(NULL, "\t", &saveptr);
            if (!s_term || !s_off) continue;
            uint64_t offset = strtoull(s_off, NULL, 10);

            if (fseeko(fd_post, (off_t)offset, SEEK_SET) != 0) continue;
            uint32_t cnt = 0;
            if (fread(&cnt, sizeof(uint32_t), 1, fd_post) != 1) continue;
            if (cnt == 0) continue;

            uint32_t *docids = malloc(cnt * sizeof(uint32_t));
            if (!docids) continue;
            if (fread(docids, sizeof(uint32_t), cnt, fd_post) != cnt) {
                free(docids); continue;
            }

            index_entry_t *e = inv_find_or_create(&ctx->inv, s_term);
            if (!e) { free(docids); continue; }

            /* Merge: append all existing docids (they are already sorted). */
            for (uint32_t k = 0; k < cnt; k++)
                postings_append(&e->postings, docids[k]);
            free(docids);
        }
        free(line);
        fclose(fd_dict);
        fclose(fd_post);
    } else {
        if (fd_dict) fclose(fd_dict);
        if (fd_post) fclose(fd_post);
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * mkdir helper (creates directory even if it exists)
 * ----------------------------------------------------------------------- */
static void ensure_dir(const char *path)
{
    /* Try to create; ignore EEXIST. */
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        fprintf(stderr, "[INDEX] mkdir(%s): %s\n", path, strerror(errno));
}

/* -----------------------------------------------------------------------
 * Comparator for qsort on uint32_t arrays (for postings sorting)
 * ----------------------------------------------------------------------- */
static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

int indexer_init(indexer_ctx_t *ctx, const char *out_dir)
{
    ctx->out_dir = strdup(out_dir);

    if (inv_init(&ctx->inv, INDEX_INIT_CAPACITY) < 0)  return -1;
    if (docmap_init(&ctx->docs) < 0) { inv_destroy(&ctx->inv); return -1; }

    /* Load existing index if present (append mode). */
    load_existing_index(ctx);
    return 0;
}

void indexer_add_document(indexer_ctx_t *ctx,
                           uint32_t docid, uint16_t depth,
                           const char *url, const char *filepath,
                           token_list_t *tokens)
{
    docmap_append(&ctx->docs, docid, depth, url, filepath);

    for (size_t i = 0; i < tokens->count; i++) {
        index_entry_t *e = inv_find_or_create(&ctx->inv, tokens->words[i]);
        if (e) postings_append(&e->postings, docid);
    }
}

int indexer_flush(indexer_ctx_t *ctx)
{
    char dir[4096], docs_path[4096], dict_path[4096], post_path[4096];
    snprintf(dir,       sizeof(dir),       "%s/index",              ctx->out_dir);
    snprintf(docs_path, sizeof(docs_path), "%s/index/docs.tsv",     ctx->out_dir);
    snprintf(dict_path, sizeof(dict_path), "%s/index/dict.tsv",     ctx->out_dir);
    snprintf(post_path, sizeof(post_path), "%s/index/postings.bin", ctx->out_dir);

    ensure_dir(dir);

    /* ------ docs.tsv ------ */
    FILE *fd = fopen(docs_path, "w");
    if (!fd) { perror(docs_path); return -1; }
    for (size_t i = 0; i < ctx->docs.count; i++) {
        doc_entry_t *d = &ctx->docs.entries[i];
        fprintf(fd, "%" PRIu32 "\t%s\t%s\t%" PRIu16 "\n",
                d->docid, d->url ? d->url : "",
                d->filepath ? d->filepath : "", d->depth);
    }
    fclose(fd);

    /* ------ postings.bin + dict.tsv ------ */
    FILE *fp = fopen(post_path, "wb");
    if (!fp) { perror(post_path); return -1; }
    FILE *fd2 = fopen(dict_path, "w");
    if (!fd2) { perror(dict_path); fclose(fp); return -1; }

    for (size_t i = 0; i < ctx->inv.capacity; i++) {
        index_entry_t *e = &ctx->inv.table[i];
        if (!e->term || e->postings.count == 0) continue;

        /* Sort postings (deduplicate then sort). */
        qsort(e->postings.docids, e->postings.count,
              sizeof(uint32_t), cmp_u32);

        /* Deduplicate after sort. */
        uint32_t uniq = 1;
        for (uint32_t k = 1; k < e->postings.count; k++) {
            if (e->postings.docids[k] != e->postings.docids[k-1])
                e->postings.docids[uniq++] = e->postings.docids[k];
        }
        e->postings.count = uniq;

        long offset = ftell(fp);

        fwrite(&e->postings.count, sizeof(uint32_t), 1, fp);
        fwrite(e->postings.docids, sizeof(uint32_t), e->postings.count, fp);

        fprintf(fd2, "%s\t%ld\t%" PRIu32 "\n",
                e->term, offset, e->postings.count);
    }

    fclose(fp);
    fclose(fd2);
    fprintf(stderr, "[INDEXER] Flushed %zu terms, %zu documents.\n",
            ctx->inv.count, ctx->docs.count);
    return 0;
}

void indexer_destroy(indexer_ctx_t *ctx)
{
    inv_destroy(&ctx->inv);
    docmap_destroy(&ctx->docs);
    free(ctx->out_dir);
    ctx->out_dir = NULL;
}
