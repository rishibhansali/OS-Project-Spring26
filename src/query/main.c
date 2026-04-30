/*
 * query/main.c — Command-line index search tool.
 *
 * Usage:
 *   query --index <dir> <term1> [term2 ...]
 *
 * Performs an AND query across the given terms and prints matching documents.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <getopt.h>

#include "query/index_reader.h"

#define MAX_TERMS 64

static void usage(const char *prog)
{
    fprintf(stderr,
        "USAGE: %s --index <dir> <term1> [term2 ...]\n"
        "\n"
        "  --index  Directory containing index/docs.tsv, index/dict.tsv,\n"
        "           and index/postings.bin\n"
        "  -h       Print this help\n"
        "\nExample:\n"
        "  %s --index . operating systems threads\n",
        prog, prog);
}

/* Lowercase a string in-place. */
static void str_tolower(char *s)
{
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

int main(int argc, char *argv[])
{
    char *index_dir = NULL;

    static struct option long_opts[] = {
        { "index", required_argument, NULL, 'x' },
        { "help",  no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "x:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'x': index_dir = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!index_dir) {
        fprintf(stderr, "Error: --index is required.\n");
        usage(argv[0]);
        return 1;
    }

    /* Remaining arguments are query terms. */
    int    n_terms = argc - optind;
    char **terms   = argv + optind;

    if (n_terms == 0) {
        fprintf(stderr, "Error: at least one search term is required.\n");
        usage(argv[0]);
        return 1;
    }
    if (n_terms > MAX_TERMS) {
        fprintf(stderr, "Error: too many query terms (max %d).\n", MAX_TERMS);
        return 1;
    }

    /* Lowercase all query terms. */
    for (int i = 0; i < n_terms; i++)
        str_tolower(terms[i]);

    /* --- Build file paths --------------------------------------------- */
    char dict_path[4096], docs_path[4096], post_path[4096];
    snprintf(dict_path, sizeof(dict_path), "%s/index/dict.tsv",     index_dir);
    snprintf(docs_path, sizeof(docs_path), "%s/index/docs.tsv",     index_dir);
    snprintf(post_path, sizeof(post_path), "%s/index/postings.bin", index_dir);

    /* --- Load index files --------------------------------------------- */
    dict_t   dict;
    docmap_t docs;

    if (dict_load(&dict, dict_path) < 0) {
        fprintf(stderr, "Failed to load dictionary from %s\n", dict_path);
        return 1;
    }
    if (docmap_load(&docs, docs_path) < 0) {
        fprintf(stderr, "Failed to load document map from %s\n", docs_path);
        dict_free(&dict);
        return 1;
    }

    FILE *pf = fopen(post_path, "rb");
    if (!pf) {
        fprintf(stderr, "Cannot open postings file %s\n", post_path);
        dict_free(&dict);
        docmap_free(&docs);
        return 1;
    }

    /* --- Look up every query term ------------------------------------- */
    uint32_t  *lists[MAX_TERMS];
    uint32_t   lengths[MAX_TERMS];
    int        n_found = 0;

    for (int i = 0; i < n_terms; i++) {
        int idx = dict_lookup(&dict, terms[i]);
        if (idx < 0) {
            printf("No documents matched all query terms.\n");
            /* Clean up already-allocated lists. */
            for (int j = 0; j < n_found; j++) free(lists[j]);
            fclose(pf);
            dict_free(&dict);
            docmap_free(&docs);
            return 0;
        }

        uint32_t cnt = 0;
        uint32_t *docids = postings_read(pf, dict.entries[idx].offset, &cnt);
        if (!docids || cnt == 0) {
            printf("No documents matched all query terms.\n");
            for (int j = 0; j < n_found; j++) free(lists[j]);
            free(docids);
            fclose(pf);
            dict_free(&dict);
            docmap_free(&docs);
            return 0;
        }

        lists[n_found]   = docids;
        lengths[n_found] = cnt;
        n_found++;
    }

    /* --- AND intersection --------------------------------------------- */
    uint32_t  result_count = 0;
    uint32_t *result = intersect_sorted(lists, lengths, n_found, &result_count);

    /* Free per-term postings. */
    for (int i = 0; i < n_found; i++) free(lists[i]);
    fclose(pf);

    /* --- Print results ------------------------------------------------ */
    if (result_count == 0 || !result) {
        printf("No documents matched all query terms.\n");
    } else {
        printf("Found %" PRIu32 " matching document%s (AND across terms):\n",
               result_count, result_count == 1 ? "" : "s");
        for (uint32_t i = 0; i < result_count; i++) {
            uint32_t    docid = result[i];
            const char *url   = docmap_get_url(&docs, docid);
            printf("  %" PRIu32 "  %s\n", docid, url ? url : "(unknown)");
        }
    }

    free(result);
    dict_free(&dict);
    docmap_free(&docs);
    return 0;
}
