CC      = clang
STD     = -std=c11
WARN    = -Wall -Wextra
OPT     = -O2
DEFS    = -D_POSIX_C_SOURCE=200809L
INCS    = -Isrc

# Detect header/library locations in priority order:
#   1. pkg-config (works after: brew install pkg-config curl libxml2)
#   2. Homebrew keg-only (Apple Silicon: /opt/homebrew/opt/...)
#   3. macOS system SDK (always available via Command Line Tools)

SDK         := $(shell xcrun --show-sdk-path 2>/dev/null)
BREW_CURL   := $(shell brew --prefix curl    2>/dev/null)
BREW_XML2   := $(shell brew --prefix libxml2 2>/dev/null)

PKGCFG_CF   := $(shell pkg-config --cflags libcurl libxml-2.0 2>/dev/null)
PKGCFG_LIBS := $(shell pkg-config --libs   libcurl libxml-2.0 2>/dev/null)
PKGCFG_LIBS_XML := $(shell pkg-config --libs libxml-2.0 2>/dev/null)

ifneq ($(strip $(PKGCFG_CF)),)
  # pkg-config available and found both libraries
  EXT_CFLAGS      := $(PKGCFG_CF)
  EXT_LIBS        := $(PKGCFG_LIBS)
  EXT_LIBS_XML    := $(PKGCFG_LIBS_XML)
else ifneq ($(wildcard $(BREW_CURL)/lib/libcurl.dylib $(BREW_CURL)/lib/libcurl.a),)
  # Homebrew curl + xml2 libraries are present
  EXT_CFLAGS      := -I$(BREW_CURL)/include -I$(BREW_XML2)/include/libxml2
  EXT_LIBS        := -L$(BREW_CURL)/lib -lcurl -L$(BREW_XML2)/lib -lxml2
  EXT_LIBS_XML    := -L$(BREW_XML2)/lib -lxml2
else
  # Fall back to macOS system SDK (curl and libxml2 are part of macOS)
  EXT_CFLAGS      := $(if $(SDK),-I$(SDK)/usr/include/libxml2)
  EXT_LIBS        := -lcurl -lxml2
  EXT_LIBS_XML    := -lxml2
endif

CFLAGS       = $(STD) $(WARN) $(OPT) $(DEFS) $(INCS) $(EXT_CFLAGS)
LIBS_CRAWLER = $(EXT_LIBS) -lpthread
LIBS_INDEXER = $(EXT_LIBS_XML) -lpthread
LIBS_QUERY   =

BUILD = build

SRC_CRW = src/crawler
SRC_IDX = src/indexer
SRC_QRY = src/query

CRAWLER_SRCS = $(SRC_CRW)/main.c      \
               $(SRC_CRW)/queue.c     \
               $(SRC_CRW)/visited.c   \
               $(SRC_CRW)/fetch.c     \
               $(SRC_CRW)/parse.c     \
               $(SRC_CRW)/ipc_client.c

INDEXER_SRCS = $(SRC_IDX)/main.c       \
               $(SRC_IDX)/ipc_server.c \
               $(SRC_IDX)/tokenizer.c  \
               $(SRC_IDX)/index.c

QUERY_SRCS   = $(SRC_QRY)/main.c         \
               $(SRC_QRY)/index_reader.c

.PHONY: all clean dirs run help cpp_crawler cpp_indexer cpp_query run_cpp

all: dirs crawler indexer query cpp_crawler cpp_indexer cpp_query

dirs:
	@mkdir -p $(BUILD) data/pages index

crawler: $(CRAWLER_SRCS) src/common/ipc_proto.h $(SRC_CRW)/queue.h \
         $(SRC_CRW)/visited.h $(SRC_CRW)/fetch.h $(SRC_CRW)/parse.h \
         $(SRC_CRW)/ipc_client.h
	$(CC) $(CFLAGS) -o $(BUILD)/crawler $(CRAWLER_SRCS) $(LIBS_CRAWLER)
	@echo "Built: $(BUILD)/crawler"

indexer: $(INDEXER_SRCS) src/common/ipc_proto.h $(SRC_IDX)/ipc_server.h \
         $(SRC_IDX)/tokenizer.h $(SRC_IDX)/index.h
	$(CC) $(CFLAGS) -o $(BUILD)/indexer $(INDEXER_SRCS) $(LIBS_INDEXER)
	@echo "Built: $(BUILD)/indexer"

query: $(QUERY_SRCS) $(SRC_QRY)/index_reader.h
	$(CC) $(CFLAGS) -o $(BUILD)/query $(QUERY_SRCS) $(LIBS_QUERY)
	@echo "Built: $(BUILD)/query"

clean:
	rm -rf $(BUILD)/ data/ index/
	rm -f crawler indexer query
	@echo "Cleaned."

# End-to-end demo run:
#   1. Start indexer in background  (listens for crawler)
#   2. Run crawler on example.com   (depth 2, 50 pages, 4 threads)
#   3. After crawler exits indexer flushes and exits
#   4. Query the resulting index
run: all
	@echo "=== Starting indexer in background ==="
	$(BUILD)/indexer --ipc /tmp/webcrawler.sock --out . &
	@sleep 0.5
	@echo "=== Starting crawler ==="
	$(BUILD)/crawler --seed https://example.com --max-depth 2 \
	    --max-pages 50 -t 4 --out . --ipc /tmp/webcrawler.sock
	@sleep 1
	@echo "=== Querying index ==="
	$(BUILD)/query --index . web example domain

help:
	@echo "Targets: all  clean  run  run_cpp  help"
	@echo ""
	@echo "C binaries (build/):"
	@echo "  $(BUILD)/indexer --ipc /tmp/crawl.sock --out <dir>"
	@echo "  $(BUILD)/crawler --seed <url> --max-depth <D> --max-pages <N> -t <T> --out <dir> --ipc /tmp/crawl.sock"
	@echo "  $(BUILD)/query   --index <dir> <term1> [term2 ...]"
	@echo ""
	@echo "C++ binaries (project root):"
	@echo "  ./indexer --ipc /tmp/crawl.sock --out data/index"
	@echo "  ./crawler --seed <url> --max-depth <D> --max-pages <N> -t <T> --out data --ipc /tmp/crawl.sock"
	@echo "  ./query   --index data/index <term1> [term2 ...]"

# =============================================================================
# C++17 targets
# Compiled separately from the C sources; produce binaries in the project root.
# Uses g++ -std=c++17 -Wall -pthread and links libcurl.
# =============================================================================

CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread -O2

# Detect libcurl for C++ build the same way as for C
ifneq ($(strip $(PKGCFG_LIBS)),)
  CXXLIBS = $(PKGCFG_LIBS)
else ifneq ($(wildcard $(BREW_CURL)/lib/libcurl.dylib $(BREW_CURL)/lib/libcurl.a),)
  CXXLIBS = -L$(BREW_CURL)/lib -lcurl
else
  CXXLIBS = -lcurl
endif

cpp_indexer: indexer.cpp
	$(CXX) $(CXXFLAGS) -o indexer indexer.cpp
	@echo "Built: ./indexer (C++)"

cpp_crawler: crawler.cpp
	$(CXX) $(CXXFLAGS) -o crawler crawler.cpp $(CXXLIBS)
	@echo "Built: ./crawler (C++)"

cpp_query: query.cpp
	$(CXX) $(CXXFLAGS) -o query query.cpp
	@echo "Built: ./query (C++)"

# End-to-end demo using the C++ binaries:
#   1. Start indexer in background (listens on UNIX socket)
#   2. Wait 1 second for indexer to be ready
#   3. Run crawler (fetches up to 50 pages, 4 threads, depth 2)
#   4. Crawler closes socket → indexer flushes → indexer exits
#   5. Query the resulting index
run_cpp: cpp_indexer cpp_crawler cpp_query
	@mkdir -p data/index data/pages
	@echo "=== Starting C++ indexer in background ==="
	./indexer --ipc /tmp/crawl.sock --out data/index &
	@sleep 1
	@echo "=== Starting C++ crawler ==="
	./crawler --seed https://en.wikipedia.org/wiki/Linux \
	    --max-depth 2 --max-pages 50 -t 4 \
	    --out data --ipc /tmp/crawl.sock
	@echo "=== Querying index ==="
	./query --index data/index operating systems threads
