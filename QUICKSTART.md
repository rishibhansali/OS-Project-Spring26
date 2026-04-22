# 🚀 Quick Start Guide

## Prerequisites

✅ macOS with Command Line Tools installed  
✅ libcurl (system default)  
✅ libxml2 (system default)  

Optional: `brew install pkg-config curl libxml2` for latest versions

## 30-Second Demo

```bash
# 1. Build everything
cd /Users/gagandeepsingh/Desktop/webcrawler
make all

# 2. Run automated demo
make run

# That's it! The demo will:
#   - Start indexer in background
#   - Crawl https://example.com (depth 2, 50 pages, 4 threads)
#   - Build inverted index
#   - Run sample queries
```

## Manual Usage (Full Control)

### Terminal 1: Start Indexer

```bash
cd /Users/gagandeepsingh/Desktop/webcrawler
./build/indexer --ipc /tmp/crawl.sock --out .
```

Wait for: `[INDEXER] Listening on /tmp/crawl.sock — waiting for crawler...`

### Terminal 2: Run Crawler

```bash
cd /Users/gagandeepsingh/Desktop/webcrawler

# Small crawl (good for testing)
./build/crawler \
    --seed https://example.com \
    --max-depth 1 \
    --max-pages 5 \
    -t 2 \
    --out . \
    --ipc /tmp/crawl.sock

# Medium crawl
./build/crawler \
    --seed https://en.wikipedia.org/wiki/Operating_system \
    --max-depth 2 \
    --max-pages 100 \
    -t 8 \
    --out . \
    --ipc /tmp/crawl.sock

# Large crawl
./build/crawler \
    --seed https://news.ycombinator.com \
    --max-depth 3 \
    --max-pages 500 \
    -t 16 \
    --out . \
    --ipc /tmp/crawl.sock
```

Indexer will automatically flush and exit when crawler finishes.

### Terminal 2: Query the Index

```bash
# Single term query
./build/query --index . operating

# Multi-term AND query
./build/query --index . operating system threads

# Another example
./build/query --index . internet protocol
```

## Command Reference

### Crawler Options

```
--seed <url>        Starting URL (required)
--max-depth <D>     Max link depth (0 = seed only)
--max-pages <N>     Stop after N pages
-t <threads>        Worker thread count
--out <dir>         Output directory
--ipc <path>        Socket path (must match indexer)
-h                  Help
```

### Indexer Options

```
--ipc <path>        Socket path (must match crawler)
--out <dir>         Output directory
-h                  Help
```

### Query Options

```
--index <dir>       Index directory
<term1> [term2...]  Query terms (AND semantics)
-h                  Help
```

## Expected Output

### Crawler Summary
```
=== Crawler Summary ===
Pages fetched:   18
Pages failed:    0
Pages skipped:   5
Max queue depth: 20
Total runtime:   2.67s
```

### Query Results
```
Found 8 matching documents (AND across terms):
  0  https://example.com
  1  https://iana.org/domains/example
  ...
```

## File Locations

After running:

```
data/
  pages/
    0.html          ← Crawled page #0
    1.html          ← Crawled page #1
    ...

index/
  docs.tsv          ← Document map
  dict.tsv          ← Term dictionary
  postings.bin      ← Binary postings lists
```

## Troubleshooting

### Error: "Failed to connect to indexer"

**Cause:** Indexer not running or wrong socket path  
**Fix:** Start indexer first in separate terminal

### Error: "Address already in use"

**Cause:** Previous socket file exists  
**Fix:** `rm /tmp/crawl.sock` before starting indexer

### Build errors

**Cause:** Missing libraries  
**Fix:** `brew install pkg-config curl libxml2`

### No results from query

**Cause:** Terms not in index  
**Fix:** Try simpler/common terms, check docs.tsv for actual URLs

## Clean Up

```bash
# Remove all generated files
make clean

# Remove specific socket
rm /tmp/crawl.sock
```

## Testing

```bash
# Run full test suite (10 tests, ~30 seconds)
./test_webcrawler.sh

# Expected: ✅ ALL TESTS PASSED
```

## Performance Tips

1. **More threads = faster crawling**
   - Start with `-t 4`, increase to `-t 16` for more CPU cores
   - Diminishing returns beyond 16 threads (network bottleneck)

2. **Larger queue capacity**
   - Edit `QUEUE_DEFAULT_CAPACITY` in `queue.c` if crawling very broad sites

3. **Depth vs. pages trade-off**
   - `--max-depth 1 --max-pages 100` = broad crawl (follows many links from seed)
   - `--max-depth 5 --max-pages 100` = deep crawl (follows link chains)

## Next Steps

1. ✅ Read `README.md` for architecture details
2. ✅ Read `VERIFICATION.md` for compliance report  
3. ✅ Read `PROJECT_SUMMARY.md` for implementation overview
4. ✅ Run `./test_webcrawler.sh` to verify everything works
5. ✅ Experiment with different seed URLs and parameters

## Example Workflows

### Crawl Wikipedia

```bash
# Terminal 1
./build/indexer --ipc /tmp/wiki.sock --out .

# Terminal 2
./build/crawler \
    --seed https://en.wikipedia.org/wiki/Computer_science \
    --max-depth 2 \
    --max-pages 200 \
    -t 12 \
    --out . \
    --ipc /tmp/wiki.sock

./build/query --index . algorithm data structure
./build/query --index . programming language
```

### Crawl News Site

```bash
# Terminal 1
./build/indexer --ipc /tmp/news.sock --out .

# Terminal 2
./build/crawler \
    --seed https://news.ycombinator.com \
    --max-depth 1 \
    --max-pages 50 \
    -t 4 \
    --out . \
    --ipc /tmp/news.sock

./build/query --index . technology startup
```

## Documentation

| File | Description |
|------|-------------|
| `README.md` | 249-line comprehensive documentation |
| `PROJECT_SUMMARY.md` | Implementation overview |
| `VERIFICATION.md` | Requirements compliance report |
| `test_webcrawler.sh` | Automated test suite |
| `src/*/*.c` | Source code with inline comments |

## Getting Help

```bash
make help                # Build system help
./build/crawler -h       # Crawler usage
./build/indexer -h       # Indexer usage
./build/query -h         # Query usage
```

---

**Ready to explore? Start with `make run` or `./test_webcrawler.sh`!**
