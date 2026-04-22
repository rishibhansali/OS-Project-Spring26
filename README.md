# Multithreaded Web Crawler Pipeline

A concurrent web crawler implemented in two separate stacks:

- **C11 version** (`src/`) — uses pthreads, libcurl, libxml2, binary IPC protocol
- **C++17 version** (project root) — uses `std::thread`, `std::mutex`, `std::condition_variable`, STL containers, libcurl, text IPC protocol

Both versions crawl URLs in parallel, stream fetched-page metadata to an indexer over a UNIX domain socket, and build a persistent on-disk inverted index queryable via a command-line tool.

---

## Project Structure

```
├── Makefile              builds both C and C++ versions
├── crawler.cpp           C++17 multithreaded crawler
├── indexer.cpp           C++17 inverted-index builder
├── query.cpp             C++17 AND query tool
├── src/
│   ├── common/
│   │   └── ipc_proto.h   shared binary IPC wire protocol (C)
│   ├── crawler/          C11 crawler sources
│   ├── indexer/          C11 indexer sources
│   └── query/            C11 query sources
├── build/                compiled C binaries (build/crawler, etc.)
├── data/
│   └── pages/            HTML files saved by crawler (<docid>.html)
└── index/                on-disk index (docs.tsv, dict.tsv, postings.bin)
```

---

## Build

### Install dependencies (macOS)

```bash
brew install pkg-config curl libxml2
```

### Build all (C and C++ versions)

```bash
make all
```

Produces:
- `build/crawler`, `build/indexer`, `build/query`  (C11)
- `./crawler`, `./indexer`, `./query`               (C++17)

### Build only C++ version

```bash
make cpp_crawler cpp_indexer cpp_query
```

### Clean

```bash
make clean
```

---

## Running the C++ version

### Step 1 — Start the indexer (must run before the crawler connects)

```bash
./indexer --ipc /tmp/crawl.sock --out data/index
```

The indexer binds a UNIX socket and blocks in `accept()` waiting for the crawler.

### Step 2 — Run the crawler

```bash
./crawler \
    --seed https://en.wikipedia.org/wiki/Linux \
    --max-depth 2 \
    --max-pages 50 \
    -t 4 \
    --out data \
    --ipc /tmp/crawl.sock
```

When the crawler finishes it closes the socket. The indexer sees EOF, flushes to disk, and exits.

### Step 3 — Query the index

```bash
./query --index data/index operating systems
./query --index data/index linux kernel threads
./query --index data/index termthatdoesnotexist
```

**Sample output:**
```
Found 3 matching documents (AND across terms):
  0  https://en.wikipedia.org/wiki/Linux
  12 https://en.wikipedia.org/wiki/Operating_system
  31 https://en.wikipedia.org/wiki/POSIX_Threads
```

### Automated demo (Makefile target)

```bash
make run_cpp
```

---

## CLI Reference

| Binary   | Flags                                                                 |
|----------|-----------------------------------------------------------------------|
| indexer  | `--ipc <path>` `--out <dir>`                                         |
| crawler  | `--seed <url>` `--max-depth <D>` `--max-pages <N>` `-t <T>` `--out <dir>` `--ipc <path>` |
| query    | `--index <dir>` `<term1>` `[term2 ...]`                              |

---

## Design Details

### Bounded URL Queue (crawler.cpp)

The URL frontier is a `std::queue<UrlEntry>` wrapped by a `std::mutex` and **two `std::condition_variable`s**:

- **`not_empty`** — worker threads (consumers) wait here when the queue is empty. `push()` calls `notify_one()` after adding a URL to wake one waiting consumer.
- **`not_full`** — worker threads (producers pushing extracted links) wait here when the queue is at capacity (capped at 1000). `pop()` calls `notify_one()` after removing a URL to wake one waiting producer. This provides **backpressure**: if fetching outpaces consumption the producers slow down instead of growing memory unbounded.

`shutdown()` sets a `done` flag and calls `notify_all()` on both CVs, so every blocked thread wakes up and returns `false` to exit its loop cleanly.

### Visited Set (crawler.cpp)

`VisitedSet` wraps `std::unordered_set<std::string>` with a single `std::mutex`.

`check_and_insert()` is **atomic**: the lookup and the insert are both done under the same lock. This prevents two threads from both seeing a URL as "new" at the same moment and crawling it twice.

### Thread Pool (crawler.cpp)

A fixed pool of `N` `std::thread` workers all share:
- The bounded URL queue (blocking pop)
- The visited set (atomic check+insert)
- The IPC client (mutex-serialized sends)
- `std::atomic<int>` counters: `pages_fetched`, `pages_failed`, `pages_skipped`, `next_docid`

Each worker loops: **pop → fetch (libcurl) → save HTML → IPC send → extract links → push new URLs**.

### Stop Conditions (crawler.cpp)

Two conditions trigger `queue.shutdown()`, which wakes all blocked workers:

1. **Page limit**: `pages_fetched >= max_pages` checked before each fetch.
2. **Frontier exhaustion**: `active_workers == 0 && queue.size() == 0`. The `active_workers` atomic is incremented after a successful pop and decremented at the end of each iteration. When it reaches 0 with an empty queue, no thread is doing anything and no new URLs will arrive — crawl is done.

### IPC Protocol (text, between C++ crawler and C++ indexer)

Transport: UNIX domain socket (`AF_UNIX`, `SOCK_STREAM`).

Each message is a **newline-terminated text line**:
```
docid url filepath depth\n
```

Example:
```
42 https://en.wikipedia.org/wiki/Linux data/pages/42.html 1
```

The crawler closes the socket when done; the indexer detects EOF and flushes.

The IPC client has an internal `std::mutex` so concurrent worker threads never interleave their messages.

### On-Disk Index Format

All three files live in the directory given to `./indexer --out <dir>`.

| File | Format | Description |
|---|---|---|
| `docs.tsv` | `docid\turl\tfilepath\tdepth\n` | Document map |
| `dict.tsv` | `term\toffset\tdf\n` | Term → byte offset in postings.bin + document frequency |
| `postings.bin` | `[uint32_t count][uint32_t docid × count]` per term | Binary postings lists (sorted ascending) |

`offset` in `dict.tsv` is the byte position in `postings.bin` where that term's block starts. The query tool uses `fseek()` to jump directly to it without scanning the whole file.

### Tokenization (indexer.cpp)

1. Strip HTML tags: simple state machine, skips everything between `<` and `>`.
2. Split on any non-alphabetic character.
3. Lowercase each token; discard tokens shorter than 2 characters.

### AND Intersection (query.cpp)

Loads postings lists for all query terms, then intersects them pairwise using a linear merge (both lists are already sorted). Result is the set of docids present in **all** lists.

---

## macOS Notes

- C build uses `clang` with `-std=c11`; C++ build uses `g++` with `-std=c++17`
- Makefile auto-detects libcurl via `pkg-config` then Homebrew keg-only, falling back to the macOS system SDK
- UNIX socket path is limited to 103 characters on macOS
- `std::chrono::steady_clock` used for runtime measurement (monotonic, not wall-clock)
