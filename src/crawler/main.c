/*
 * crawler/main.c — Multithreaded web crawler.
 *
 * Usage:
 *   crawler --seed <url> --max-depth <D> --max-pages <N>
 *           -t <threads> --out <dir> --ipc <socket-path>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <stdatomic.h>
#include <getopt.h>
#include <pthread.h>
#include <curl/curl.h>
#include <libxml/parser.h>

#include "crawler/queue.h"
#include "crawler/visited.h"
#include "crawler/fetch.h"
#include "crawler/parse.h"
#include "crawler/ipc_client.h"

/* -----------------------------------------------------------------------
 * Shared crawler context (created in main, passed to all workers)
 * ----------------------------------------------------------------------- */
typedef struct {
    /* Configuration (read-only after init). */
    char  *seed_url;
    char  *out_dir;
    char  *ipc_path;
    int    max_depth;
    int    max_pages;
    int    num_threads;

    /* Shared mutable state. */
    url_queue_t   queue;
    visited_set_t visited;
    ipc_client_t  ipc;

    /* Atomic stats counters. */
    _Atomic uint32_t next_docid;
    _Atomic int      pages_fetched;
    _Atomic int      pages_failed;
    _Atomic int      pages_skipped;

    /* Number of workers currently processing a URL (not blocked on pop).
     * Used for frontier-exhaustion detection: when this drops to 0 and
     * the queue is also empty, there is no more work to do. */
    _Atomic int active_workers;

    /* Set to 1 by the first worker that detects stop condition. */
    volatile int shutdown;
} crawler_ctx_t;

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Create directory path (including parents), ignoring EEXIST. */
static void mkdirp(const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        fprintf(stderr, "[CRAWLER] mkdirp(%s): %s\n", tmp, strerror(errno));
}

/* Write data of len bytes to filepath.  Returns 0 on success, -1 on error. */
static int write_file(const char *filepath, const char *data, size_t len)
{
    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    size_t written = 0;
    while (written < len) {
        ssize_t r = write(fd, data + written, len - written);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        written += (size_t)r;
    }
    close(fd);
    return 0;
}

/* -----------------------------------------------------------------------
 * Worker thread
 * ----------------------------------------------------------------------- */
static void *worker_thread(void *arg)
{
    crawler_ctx_t *ctx = (crawler_ctx_t *)arg;

    CURL *curl = fetch_init_handle();
    if (!curl) {
        fprintf(stderr, "[WORKER] curl_easy_init failed\n");
        return NULL;
    }

    char *url  = NULL;
    int   depth = 0;

    while (!ctx->shutdown) {
        /* Block until a URL is available or shutdown. */
        if (url_queue_pop(&ctx->queue, &url, &depth) < 0)
            break; /* shutdown + empty */

        /* This worker is now actively processing a URL. */
        atomic_fetch_add(&ctx->active_workers, 1);

        /* Atomically claim a page slot; bail if already at the limit.
         * Claiming before the fetch closes the TOCTOU race: without this,
         * T threads could all read pages_fetched < max_pages simultaneously
         * and overshoot the limit by up to T-1 pages. */
        int slot = atomic_fetch_add(&ctx->pages_fetched, 1);
        if (slot >= ctx->max_pages) {
            atomic_fetch_sub(&ctx->pages_fetched, 1);
            free(url);
            atomic_fetch_sub(&ctx->active_workers, 1);
            ctx->shutdown = 1;
            url_queue_shutdown(&ctx->queue);
            break;
        }

        /* Fetch the page. */
        fetch_result_t *res = fetch_url(curl, url);
        if (!res) {
            fprintf(stderr, "[CRAWLER] OOM fetching %s\n", url);
            atomic_fetch_sub(&ctx->pages_fetched, 1);
            atomic_fetch_add(&ctx->pages_failed, 1);
            free(url);
            goto end_iter;
        }

        if (res->curl_code != CURLE_OK) {
            fprintf(stderr, "[CRAWLER] CURL error for %s: %s\n",
                    url, res->error_buf);
            atomic_fetch_sub(&ctx->pages_fetched, 1);
            atomic_fetch_add(&ctx->pages_failed, 1);
            fetch_result_free(res);
            free(url);
            goto end_iter;
        }

        if (res->http_code != 200) {
            fprintf(stderr, "[CRAWLER] HTTP %ld for %s\n", res->http_code, url);
            atomic_fetch_sub(&ctx->pages_fetched, 1);
            atomic_fetch_add(&ctx->pages_failed, 1);
            fetch_result_free(res);
            free(url);
            goto end_iter;
        }

        if (!res->data || res->size == 0) {
            atomic_fetch_sub(&ctx->pages_fetched, 1);
            atomic_fetch_add(&ctx->pages_failed, 1);
            fetch_result_free(res);
            free(url);
            goto end_iter;
        }

        /* Assign a unique document ID. */
        uint32_t docid = atomic_fetch_add(&ctx->next_docid, 1u);

        /* Save HTML to <out_dir>/data/pages/<docid>.html */
        char filepath[4096];
        snprintf(filepath, sizeof(filepath),
                 "%s/data/pages/%u.html", ctx->out_dir, docid);

        if (write_file(filepath, res->data, res->size) < 0) {
            fprintf(stderr, "[CRAWLER] write_file failed for docid %u: %s\n",
                    docid, strerror(errno));
            atomic_fetch_sub(&ctx->pages_fetched, 1);
            atomic_fetch_add(&ctx->pages_failed, 1);
            fetch_result_free(res);
            free(url);
            goto end_iter;
        }

        /* Send metadata to indexer. */
        ipc_client_send(&ctx->ipc, docid, (uint16_t)depth, url, filepath);

        /* Slot was claimed above; log progress. */
        int now = slot + 1;
        fprintf(stderr, "[CRAWLER] [%d/%d] depth=%d %s\n",
                now, ctx->max_pages, depth, url);

        /* Trigger shutdown if we've reached the page limit. */
        if (now >= ctx->max_pages) {
            fetch_result_free(res);
            free(url);
            atomic_fetch_sub(&ctx->active_workers, 1);
            ctx->shutdown = 1;
            url_queue_shutdown(&ctx->queue);
            break;
        }

        /* Parse and enqueue new links (only if below max depth). */
        if (depth < ctx->max_depth) {
            link_list_t *links = parse_extract_links(res->data, res->size, url);
            if (links) {
                for (int i = 0; i < links->count; i++) {
                    char *link = links->urls[i];
                    links->urls[i] = NULL; /* take ownership */

                    if (visited_check_and_insert(&ctx->visited, link) == 1) {
                        /* Newly seen URL — push to queue. */
                        if (url_queue_push(&ctx->queue, link, depth + 1) < 0) {
                            /* Shutdown happened while we were pushing. */
                            free(link);
                        } else {
                            free(link); /* queue has its own strdup'd copy */
                        }
                    } else {
                        atomic_fetch_add(&ctx->pages_skipped, 1);
                        free(link);
                    }
                }
                link_list_free(links);
            }
        }

        fetch_result_free(res);
        free(url);
        url = NULL;

end_iter:
        /* Decrement active workers and check for frontier exhaustion.
         * If we are the last active worker AND the queue is empty, there
         * is no more work to do, so trigger shutdown. */
        {
            int remaining = atomic_fetch_sub(&ctx->active_workers, 1) - 1;
            if (remaining == 0 && url_queue_size(&ctx->queue) == 0 &&
                !ctx->shutdown) {
                ctx->shutdown = 1;
                url_queue_shutdown(&ctx->queue);
            }
        }
    }

    fetch_cleanup_handle(curl);
    return NULL;
}

/* -----------------------------------------------------------------------
 * Usage / help
 * ----------------------------------------------------------------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
        "USAGE: %s --seed <url> --max-depth <D> --max-pages <N>"
        " -t <threads> --out <dir> --ipc <path>\n"
        "\n"
        "  --seed       Seed URL to start crawling from\n"
        "  --max-depth  Maximum link-follow depth (0 = seed only)\n"
        "  --max-pages  Stop after fetching this many pages\n"
        "  -t           Number of worker threads\n"
        "  --out        Output directory (saves pages/ and index/ inside)\n"
        "  --ipc        UNIX socket path for indexer communication\n"
        "  -h           Print this help\n",
        prog);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    crawler_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Defaults. */
    ctx.max_depth   = 3;
    ctx.max_pages   = 100;
    ctx.num_threads = 4;

    /* --- Parse CLI ---------------------------------------------------- */
    static struct option long_opts[] = {
        { "seed",      required_argument, NULL, 's' },
        { "max-depth", required_argument, NULL, 'd' },
        { "max-pages", required_argument, NULL, 'n' },
        { "out",       required_argument, NULL, 'o' },
        { "ipc",       required_argument, NULL, 'i' },
        { "help",      no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:d:n:t:o:i:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': ctx.seed_url     = optarg; break;
        case 'd': ctx.max_depth    = atoi(optarg); break;
        case 'n': ctx.max_pages    = atoi(optarg); break;
        case 't': ctx.num_threads  = atoi(optarg); break;
        case 'o': ctx.out_dir      = optarg; break;
        case 'i': ctx.ipc_path     = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (!ctx.seed_url || !ctx.out_dir || !ctx.ipc_path) {
        fprintf(stderr, "Error: --seed, --out, and --ipc are required.\n");
        usage(argv[0]);
        return 1;
    }
    if (ctx.num_threads < 1)  ctx.num_threads = 1;
    if (ctx.max_pages   < 1)  ctx.max_pages   = 1;
    if (ctx.max_depth   < 0)  ctx.max_depth   = 0;

    /* --- One-time library init (before threads) ----------------------- */
    curl_global_init(CURL_GLOBAL_ALL);
    xmlInitParser();
    /* Suppress libxml2 warnings/errors on stderr. */
    xmlSetGenericErrorFunc(NULL, NULL);

    /* --- Create output directories ------------------------------------ */
    char pages_dir[4096];
    snprintf(pages_dir, sizeof(pages_dir), "%s/data/pages", ctx.out_dir);
    mkdirp(pages_dir);
    char index_dir[4096];
    snprintf(index_dir, sizeof(index_dir), "%s/index", ctx.out_dir);
    mkdirp(index_dir);

    /* --- Initialize shared state -------------------------------------- */
    if (url_queue_init(&ctx.queue, 2048) < 0) {
        fprintf(stderr, "Failed to init URL queue\n"); return 1;
    }
    if (visited_init(&ctx.visited, VISITED_INIT_CAPACITY) < 0) {
        fprintf(stderr, "Failed to init visited set\n"); return 1;
    }

    /* --- Connect to indexer ------------------------------------------- */
    fprintf(stderr, "[CRAWLER] Connecting to indexer at %s ...\n", ctx.ipc_path);
    if (ipc_client_connect(&ctx.ipc, ctx.ipc_path) < 0) {
        fprintf(stderr, "[CRAWLER] Failed to connect to indexer.\n"
                        "         Is the indexer running?\n");
        return 1;
    }
    fprintf(stderr, "[CRAWLER] Connected.\n");

    /* --- Seed the queue ----------------------------------------------- */
    atomic_store(&ctx.next_docid,    0);
    atomic_store(&ctx.pages_fetched, 0);
    atomic_store(&ctx.pages_failed,  0);
    atomic_store(&ctx.pages_skipped, 0);
    atomic_store(&ctx.active_workers, 0);
    ctx.shutdown = 0;

    visited_check_and_insert(&ctx.visited, ctx.seed_url);
    url_queue_push(&ctx.queue, ctx.seed_url, 0);

    /* --- Start timer -------------------------------------------------- */
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* --- Launch worker threads ---------------------------------------- */
    pthread_t *tids = malloc((size_t)ctx.num_threads * sizeof(pthread_t));
    if (!tids) { perror("malloc tids"); return 1; }

    fprintf(stderr, "[CRAWLER] Starting %d worker threads. "
            "max-depth=%d max-pages=%d\n",
            ctx.num_threads, ctx.max_depth, ctx.max_pages);

    for (int i = 0; i < ctx.num_threads; i++) {
        if (pthread_create(&tids[i], NULL, worker_thread, &ctx) != 0) {
            fprintf(stderr, "[CRAWLER] pthread_create[%d] failed: %s\n",
                    i, strerror(errno));
            ctx.shutdown = 1;
            url_queue_shutdown(&ctx.queue);
        }
    }

    /* --- Wait for all workers to finish ------------------------------- */
    for (int i = 0; i < ctx.num_threads; i++)
        pthread_join(tids[i], NULL);
    free(tids);

    /* --- Send sentinel to indexer ------------------------------------- */
    ipc_client_send_sentinel(&ctx.ipc);
    ipc_client_close(&ctx.ipc);

    /* --- Stop timer --------------------------------------------------- */
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed = (t_end.tv_sec  - t_start.tv_sec) +
                     (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    /* --- Print summary ------------------------------------------------ */
    fprintf(stdout,
        "\n=== Crawler Summary ===\n"
        "Pages fetched:   %d\n"
        "Pages failed:    %d\n"
        "Pages skipped:   %d\n"
        "Max queue depth: %d\n"
        "Total runtime:   %.2fs\n",
        atomic_load(&ctx.pages_fetched),
        atomic_load(&ctx.pages_failed),
        atomic_load(&ctx.pages_skipped),
        url_queue_max_depth(&ctx.queue),
        elapsed);

    /* --- Cleanup ------------------------------------------------------ */
    url_queue_destroy(&ctx.queue);
    visited_destroy(&ctx.visited);
    xmlCleanupParser();
    curl_global_cleanup();

    return 0;
}
