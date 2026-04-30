/*
 * indexer/main.c — Inverted-index builder process.
 *
 * Usage:
 *   indexer --ipc <socket-path> --out <dir>
 *
 * Lifecycle:
 *   1. Bind + listen on the UNIX socket.
 *   2. Accept the crawler connection.
 *   3. Loop: recv metadata → read HTML → tokenize → add to in-memory index.
 *   4. On sentinel: flush to disk and exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <getopt.h>
#include <limits.h>

#include "indexer/ipc_server.h"
#include "indexer/tokenizer.h"
#include "indexer/index.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* -----------------------------------------------------------------------
 * Read entire file into a heap buffer.  *out_len set to byte count.
 * Returns NULL on error (errno set).
 * ----------------------------------------------------------------------- */
static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0)                      { fclose(f); return NULL; }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *out_len  = got;
    return buf;
}

/* -----------------------------------------------------------------------
 * Usage
 * ----------------------------------------------------------------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
        "USAGE: %s --ipc <socket-path> --out <dir>\n"
        "\n"
        "  --ipc   UNIX socket path to listen on (must match crawler)\n"
        "  --out   Output directory (index/ will be created inside it)\n"
        "  -h      Print this help\n",
        prog);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    char *ipc_path = NULL;
    char *out_dir  = NULL;

    static struct option long_opts[] = {
        { "ipc",  required_argument, NULL, 'i' },
        { "out",  required_argument, NULL, 'o' },
        { "help", no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:o:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i': ipc_path = optarg; break;
        case 'o': out_dir  = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!ipc_path || !out_dir) {
        fprintf(stderr, "Error: --ipc and --out are required.\n");
        usage(argv[0]);
        return 1;
    }

    /* --- Initialize IPC server ---------------------------------------- */
    ipc_server_t server;
    if (ipc_server_init(&server, ipc_path) < 0) {
        fprintf(stderr, "[INDEXER] Failed to init IPC server at %s\n", ipc_path);
        return 1;
    }
    fprintf(stderr, "[INDEXER] Listening on %s — waiting for crawler...\n", ipc_path);

    /* --- Initialize in-memory index ----------------------------------- */
    indexer_ctx_t idx;
    if (indexer_init(&idx, out_dir) < 0) {
        fprintf(stderr, "[INDEXER] Failed to init index context\n");
        ipc_server_close(&server);
        return 1;
    }

    /* --- Accept crawler connection ------------------------------------ */
    if (ipc_server_accept(&server) < 0) {
        fprintf(stderr, "[INDEXER] Accept failed\n");
        ipc_server_close(&server);
        indexer_destroy(&idx);
        return 1;
    }
    fprintf(stderr, "[INDEXER] Crawler connected. Indexing...\n");

    /* --- Main receive loop -------------------------------------------- */
    char url_buf[8192];
    char path_buf[PATH_MAX + 1];
    uint32_t docid;
    uint16_t depth;
    int docs_indexed = 0;

    for (;;) {
        int rc = ipc_server_recv(&server, &docid, &depth,
                                 url_buf,  sizeof(url_buf),
                                 path_buf, sizeof(path_buf));
        if (rc == 0) {
            /* Sentinel — crawler is done. */
            fprintf(stderr, "[INDEXER] Sentinel received. Flushing...\n");
            break;
        }
        if (rc < 0) {
            fprintf(stderr, "[INDEXER] IPC receive error: %s\n", strerror(errno));
            break;
        }

        /* Read saved HTML. */
        size_t html_len;
        char  *html = read_file(path_buf, &html_len);
        if (!html) {
            fprintf(stderr, "[INDEXER] Cannot read %s: %s\n",
                    path_buf, strerror(errno));
            continue;
        }

        /* Tokenize and index. */
        token_list_t *tokens = tokenize_html(html, html_len);
        free(html);

        if (tokens) {
            indexer_add_document(&idx, docid, depth, url_buf, path_buf, tokens);
            token_list_free(tokens);
            docs_indexed++;
        }

        if (docs_indexed % 50 == 0)
            fprintf(stderr, "[INDEXER] Indexed %d documents so far...\n",
                    docs_indexed);
    }

    /* --- Flush to disk ------------------------------------------------ */
    fprintf(stderr, "[INDEXER] Indexed %d documents total. Writing index...\n",
            docs_indexed);

    if (indexer_flush(&idx) < 0)
        fprintf(stderr, "[INDEXER] WARNING: flush returned error\n");
    else
        fprintf(stderr, "[INDEXER] Index written to %s/index/\n", out_dir);

    /* --- Cleanup ------------------------------------------------------ */
    ipc_server_close(&server);
    indexer_destroy(&idx);
    return 0;
}
