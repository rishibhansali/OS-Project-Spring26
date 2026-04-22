#!/bin/bash
# test_webcrawler.sh - Comprehensive test suite for web crawler project
# Usage: ./test_webcrawler.sh

set -e  # Exit on error

echo "=========================================="
echo "Web Crawler Project - Test Suite"
echo "=========================================="
echo ""

# Configuration
IPC_SOCK="/tmp/webcrawler_test_$$.sock"
SEED_URL="https://example.com"
MAX_DEPTH=2
MAX_PAGES=15
THREADS=4

# Cleanup function
cleanup() {
    echo ""
    echo "[CLEANUP] Removing test socket..."
    rm -f "$IPC_SOCK"
}
trap cleanup EXIT

# Step 1: Clean and build
echo "[TEST 1] Building project..."
make clean > /dev/null 2>&1
if ! make all > /dev/null 2>&1; then
    echo "❌ FAILED: Build errors detected"
    exit 1
fi
echo "✅ PASS: Project builds without errors"
echo ""

# Step 2: Verify binaries
echo "[TEST 2] Verifying binaries exist..."
for bin in build/crawler build/indexer build/query; do
    if [ ! -x "$bin" ]; then
        echo "❌ FAILED: $bin not found or not executable"
        exit 1
    fi
done
echo "✅ PASS: All binaries present"
echo ""

# Step 3: Test help flags
echo "[TEST 3] Testing help flags..."
for bin in build/crawler build/indexer build/query; do
    if ! "$bin" -h > /dev/null 2>&1; then
        echo "❌ FAILED: $bin -h failed"
        exit 1
    fi
done
echo "✅ PASS: Help flags work"
echo ""

# Step 4: Start indexer in background
echo "[TEST 4] Starting indexer..."
./build/indexer --ipc "$IPC_SOCK" --out . > /tmp/indexer_output_$$.txt 2>&1 &
INDEXER_PID=$!
sleep 1

# Check if indexer is running
if ! kill -0 "$INDEXER_PID" 2>/dev/null; then
    echo "❌ FAILED: Indexer crashed on startup"
    cat /tmp/indexer_output_$$.txt
    exit 1
fi
echo "✅ PASS: Indexer started (PID: $INDEXER_PID)"
echo ""

# Step 5: Run crawler
echo "[TEST 5] Running crawler..."
echo "  Seed: $SEED_URL"
echo "  Max depth: $MAX_DEPTH"
echo "  Max pages: $MAX_PAGES"
echo "  Threads: $THREADS"
echo ""

if ! ./build/crawler --seed "$SEED_URL" --max-depth "$MAX_DEPTH" \
     --max-pages "$MAX_PAGES" -t "$THREADS" --out . --ipc "$IPC_SOCK" 2>&1 | tee /tmp/crawler_output_$$.txt; then
    echo "❌ FAILED: Crawler execution failed"
    exit 1
fi

# Check crawler output
if ! grep -q "Pages fetched:" /tmp/crawler_output_$$.txt; then
    echo "❌ FAILED: No summary statistics in crawler output"
    exit 1
fi
echo "✅ PASS: Crawler completed successfully"
echo ""

# Step 6: Wait for indexer to finish
echo "[TEST 6] Waiting for indexer to flush..."
sleep 2

# Check if indexer finished
if kill -0 "$INDEXER_PID" 2>/dev/null; then
    echo "⚠️  WARNING: Indexer still running, terminating..."
    kill "$INDEXER_PID" 2>/dev/null || true
fi

# Check indexer output
if ! grep -q "Index written" /tmp/indexer_output_$$.txt; then
    echo "❌ FAILED: Indexer did not flush index"
    cat /tmp/indexer_output_$$.txt
    exit 1
fi
echo "✅ PASS: Indexer flushed index successfully"
echo ""

# Step 7: Verify output files
echo "[TEST 7] Verifying output files..."

# Check data/pages
if [ ! -d data/pages ]; then
    echo "❌ FAILED: data/pages directory not created"
    exit 1
fi

PAGE_COUNT=$(ls -1 data/pages/*.html 2>/dev/null | wc -l)
if [ "$PAGE_COUNT" -lt 1 ]; then
    echo "❌ FAILED: No HTML files in data/pages"
    exit 1
fi
echo "  ✓ Found $PAGE_COUNT HTML files in data/pages/"

# Check index files
for file in index/docs.tsv index/dict.tsv index/postings.bin; do
    if [ ! -f "$file" ]; then
        echo "❌ FAILED: $file not created"
        exit 1
    fi
    echo "  ✓ $file exists"
done

# Verify index content
DOCS_COUNT=$(wc -l < index/docs.tsv)
TERMS_COUNT=$(wc -l < index/dict.tsv)
echo "  ✓ Index contains $DOCS_COUNT documents, $TERMS_COUNT terms"

if [ "$DOCS_COUNT" -lt 1 ]; then
    echo "❌ FAILED: No documents in index"
    exit 1
fi
if [ "$TERMS_COUNT" -lt 10 ]; then
    echo "❌ FAILED: Too few terms in index"
    exit 1
fi
echo "✅ PASS: All output files verified"
echo ""

# Step 8: Test queries
echo "[TEST 8] Testing query functionality..."

# Query 1: Common terms (should find results)
echo -n "  Query 'domain example': "
RESULTS=$(./build/query --index . domain example 2>&1)
if echo "$RESULTS" | grep -q "Found .* matching documents"; then
    COUNT=$(echo "$RESULTS" | grep "Found" | grep -oE '[0-9]+' | head -1)
    echo "✅ Found $COUNT documents"
else
    echo "❌ FAILED"
    echo "$RESULTS"
    exit 1
fi

# Query 2: Single term
echo -n "  Query 'example': "
RESULTS=$(./build/query --index . example 2>&1)
if echo "$RESULTS" | grep -q -E "(Found .* matching documents|No documents matched)"; then
    echo "✅ PASS"
else
    echo "❌ FAILED"
    exit 1
fi

# Query 3: Non-existent term
echo -n "  Query 'xyznonexistent': "
RESULTS=$(./build/query --index . xyznonexistent 2>&1)
if echo "$RESULTS" | grep -q "No documents matched all query terms"; then
    echo "✅ PASS (correctly returns no results)"
else
    echo "❌ FAILED"
    exit 1
fi

echo "✅ PASS: All queries executed successfully"
echo ""

# Step 9: Verify index persistence
echo "[TEST 9] Testing index persistence..."
echo "  Running second query (index should be loaded from disk)..."
if ! ./build/query --index . domain 2>&1 | grep -q -E "(Found|No documents)"; then
    echo "❌ FAILED: Cannot reload index from disk"
    exit 1
fi
echo "✅ PASS: Index persists across runs"
echo ""

# Step 10: Performance check
echo "[TEST 10] Performance metrics..."
if [ -f /tmp/crawler_output_$$.txt ]; then
    FETCHED=$(grep "Pages fetched:" /tmp/crawler_output_$$.txt | grep -oE '[0-9]+' | head -1)
    FAILED=$(grep "Pages failed:" /tmp/crawler_output_$$.txt | grep -oE '[0-9]+' | head -1)
    SKIPPED=$(grep "Pages skipped:" /tmp/crawler_output_$$.txt | grep -oE '[0-9]+' | head -1)
    RUNTIME=$(grep "Total runtime:" /tmp/crawler_output_$$.txt | grep -oE '[0-9]+\.[0-9]+' | head -1)
    
    echo "  Pages fetched:  $FETCHED"
    echo "  Pages failed:   $FAILED"
    echo "  Pages skipped:  $SKIPPED"
    echo "  Total runtime:  ${RUNTIME}s"
    
    if [ "$FETCHED" -gt 0 ] && [ "$FAILED" -eq 0 ]; then
        echo "✅ PASS: Performance metrics look good"
    else
        echo "⚠️  WARNING: Check performance metrics"
    fi
fi
echo ""

# Cleanup temp files
rm -f /tmp/crawler_output_$$.txt /tmp/indexer_output_$$.txt

echo "=========================================="
echo "✅ ALL TESTS PASSED"
echo "=========================================="
echo ""
echo "Summary:"
echo "  - Build system: ✅ WORKING"
echo "  - Crawler: ✅ WORKING"
echo "  - Indexer: ✅ WORKING"
echo "  - Query tool: ✅ WORKING"
echo "  - IPC pipeline: ✅ WORKING"
echo "  - Index persistence: ✅ WORKING"
echo ""
echo "Project is fully functional and ready for submission!"
