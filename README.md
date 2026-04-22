# Multithreaded Web Crawler Pipeline

A concurrent web crawler (C11, pthreads, libcurl, libxml2) that crawls URLs in parallel, feeds fetched pages through a UNIX socket IPC pipeline to an indexer process, and builds a persistent on-disk inverted index queryable via a command-line tool.

---

## Project Structure

```
webcrawler/
├── Makefile
├── README.md
├── src/
│   ├── common/
│   │   └── ipc_proto.h          shared IPC wire protocol
│   ├── crawler/
│   │   ├── main.c               CLI, thread pool, shutdown, summary
│   │   ├── queue.h/queue.c      bounded thread-safe URL frontier
│   │   ├── visited.h/visited.c  thread-safe hash set (check+insert)
│   │   ├── fetch.h/fetch.c      libcurl HTTP GET, per-thread handle
│   │   ├── parse.h/parse.c      libxml2 link extraction + URL normalize
│   │   └── ipc_client.h/ipc_client.c  UNIX socket sender
│   ├── indexer/
│   │   ├── main.c               CLI, IPC recv loop, flush on sentinel
│   │   ├── ipc_server.h/ipc_server.c  UNIX socket server
│   │   ├── tokenizer.h/tokenizer.c    HTML tag-strip + word tokenize
│   │   └── index.h/index.c      in-memory inverted index + disk flush
│   └── query/
│       ├── main.c               CLI, load index, AND intersect, print
│       └── index_reader.h/index_reader.c  dict/postings/docs readers
├── data/
│   └── pages/                   HTML files saved by crawler (<docid>.html)
└── index/
    ├── docs.tsv                 document map
    ├── dict.tsv                 term dictionary
    └── postings.bin             postings lists (binary)
```

---

## Build Requirements

- macOS (Apple Clang / clang with C11 support)
- [Homebrew](https://brew.sh) for dependencies

### Install dependencies

```bash
brew install pkg-config curl libxml2
```

### Build

```bash
make all
```

This produces `build/crawler`, `build/indexer`, and `build/query`.

### Clean

```bash
make clean    # removes build/, data/, index/
```

---

## Usage

### Step 1 — Start the indexer (it must be running before the crawler)

```bash
./build/indexer --ipc /tmp/crawl.sock --out .
```

The indexer listens on the UNIX socket and waits for the crawler to connect.

### Step 2 — Run the crawler

```bash
./build/crawler \
    --seed https://en.wikipedia.org/wiki/Linux \
    --max-depth 2 \
    --max-pages 100 \
    -t 8 \
    --out . \
    --ipc /tmp/crawl.sock
```

When the crawler finishes (page limit or frontier exhaustion), it sends a sentinel to the indexer. The indexer then flushes the index to disk and exits.

### Step 3 — Query the index

```bash
./build/query --index . operating systems threads
./build/query --index . linux kernel
./build/query --index . thistermdoesnotexist
```

**Sample output:**
```
Found 3 matching documents (AND across terms):
  42  https://en.wikipedia.org/wiki/Operating_system
  87  https://en.wikipedia.org/wiki/Thread_(computing)
  105 https://en.wikipedia.org/wiki/POSIX_Threads
```

```
No documents matched all query terms.
```

### Makefile `run` target (automated demo)

```bash
make run    # crawls https://example.com depth=2 pages=50 threads=4
```

### Help

```bash
./build/crawler -h
./build/indexer -h
./build/query   -h
```

---

## CLI Reference

```
crawler --seed <url> --max-depth <D> --max-pages <N> -t <threads> --out <dir> --ipc <path>
indexer --ipc <path> --out <dir>
query   --index <dir> <term1> [term2 ...]
```

| Flag | Description |
|---|---|
| `--seed` | Seed URL (required for crawler) |
| `--max-depth` | Max link-follow depth; 0 = only the seed page |
| `--max-pages` | Stop after this many successful fetches |
| `-t` | Number of worker threads in the pool |
| `--out` | Root output directory; `data/pages/` and `index/` live inside |
| `--ipc` | UNIX domain socket path; must match between crawler and indexer |
| `--index` | Directory containing `index/` subdirectory (for query) |

---

## Design Details

### URL Queue (Bounded Frontier)

Implemented in `src/crawler/queue.c` as a circular ring buffer protected by `pthread_mutex_t` and two `pthread_cond_t` variables (`not_full`, `not_empty`).

- **Backpressure**: Producer (`url_queue_push`) blocks via `pthread_cond_wait` when the queue is full.
- **Consumer blocking**: Worker threads block in `url_queue_pop` when the queue is empty.
- **Shutdown**: `url_queue_shutdown()` sets a flag and broadcasts both condition variables, waking all blocked threads. Push returns `-1` (caller must free URL); pop returns `-1` when shutdown and empty.
- **High-water mark** tracked for summary statistics.

### Visited Set

Implemented in `src/crawler/visited.c` as an open-addressing hash table (power-of-2 capacity, FNV-1a hash, linear probing) protected by a single `pthread_mutex_t`.

- `visited_check_and_insert()` is **atomic**: under the mutex it checks for the URL and inserts it in one operation, preventing any race between the check and the insert.
- Automatic resize at 70% load factor (capacity doubles, entries rehashed).

### Thread Pool

Fixed-size pool of N `pthread` worker threads, all sharing:
- The URL queue (blocking pop)
- The visited set (atomic check+insert)
- The IPC client (mutex-serialized sends)
- Atomic counters: `pages_fetched`, `pages_failed`, `pages_skipped`, `next_docid`

No thread-per-URL. Each thread loops (pop → fetch → save → IPC send → parse → enqueue). Threads exit when `url_queue_pop` returns `-1` (shutdown + empty).

### Stop Conditions (Race-Free)

- `_Atomic int pages_fetched` is incremented with `atomic_fetch_add`.
- Worker checks `atomic_load(&ctx->pages_fetched) >= ctx->max_pages` before fetching.
- The first worker to exceed the limit sets `ctx->shutdown = 1` and calls `url_queue_shutdown()`, which broadcasts to all waiting threads.
- `url_queue_shutdown()` is idempotent: calling it multiple times is safe.

### IPC Protocol

UNIX domain socket (`AF_UNIX`, `SOCK_STREAM`) at the path given by `--ipc`.

**Wire format** (defined in `src/common/ipc_proto.h`):

```
[ipc_msg_header_t: 10 bytes, packed]
  uint32_t docid
  uint16_t depth
  uint16_t url_len
  uint16_t path_len
[url_len bytes of URL (no null terminator)]
[path_len bytes of filepath (no null terminator)]
```

**Sentinel**: header with `docid = UINT32_MAX`, all lengths = 0, no body. Signals end-of-crawl to indexer.

All multi-byte fields are in host byte order (both processes run on the same machine).

The IPC client uses an internal `pthread_mutex_t` to serialize concurrent sends from multiple worker threads.

### On-Disk Index Format

All files live inside `<out_dir>/index/`.

| File | Format | Description |
|---|---|---|
| `docs.tsv` | `docid\turl\tfilepath\tdepth\n` | Document map |
| `dict.tsv` | `term\toffset\tdf\n` | Dictionary: term → byte offset in postings.bin + doc frequency |
| `postings.bin` | `[uint32 count][uint32 docid × count]` per term | Binary postings lists, sorted ascending |

`offset` in `dict.tsv` is a `uint64_t` byte offset into `postings.bin` written as a decimal integer.

The index supports **append/merge across runs**: on startup the indexer loads any existing index into memory, merges new documents, and rewrites all three files on flush.

### Tokenization

1. Strip HTML tags: simple state machine skipping `<...>` content; entire `<script>` and `<style>` element bodies are discarded.
2. Decode common HTML entities (`&amp;`, `&lt;`, `&gt;`, `&nbsp;`, `&quot;`).
3. Split on whitespace + punctuation via `strtok_r`.
4. Lowercase each token; discard tokens shorter than 2 or longer than 128 characters.
5. Filter ~90 common English stopwords (sorted array + `bsearch`).

---

## macOS Notes

- Uses `clang` explicitly (avoids `gcc` → Apple Clang alias ambiguity).
- Makefile auto-detects Homebrew keg-only libxml2 via `brew --prefix`.
- Uses `pthread_mutex_t + pthread_cond_t` throughout (avoids deprecated `sem_init`).
- UNIX socket path limited to 103 characters (`sockaddr_un.sun_path` is 104 bytes on macOS).
- `clock_gettime(CLOCK_MONOTONIC)` used for runtime measurement (available since macOS 10.12).

---

## Summary Statistics (printed by crawler on exit)

```
=== Crawler Summary ===
Pages fetched:   N
Pages failed:    N
Pages skipped:   N
Max queue depth: N
Total runtime:   X.XXs
```
