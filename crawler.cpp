/*
 * crawler.cpp — Multithreaded web crawler (C++17)
 *
 * Usage:
 *   ./crawler --seed <url> --max-depth <D> --max-pages <N>
 *             -t <threads> --out <dir> --ipc <socket-path>
 *
 * Architecture:
 *   - A bounded URL queue shared by all worker threads.
 *   - A thread-safe visited set prevents re-crawling the same URL.
 *   - A fixed-size thread pool (std::thread) pops URLs, fetches with libcurl,
 *     saves HTML, extracts links, and reports each page to the indexer.
 *   - The indexer is notified over a UNIX socket; when the crawler is done
 *     it closes the socket (EOF signals the indexer to flush and exit).
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <queue>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cerrno>

// POSIX / system headers
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

// libcurl for HTTP
#include <curl/curl.h>

// -------------------------------------------------------------------------
// URL entry stored in the work queue
// -------------------------------------------------------------------------
struct UrlEntry {
    std::string url;
    int         depth;
};

// -------------------------------------------------------------------------
// Bounded thread-safe URL queue
//
// This wraps std::queue with a mutex and two condition variables:
//
//   not_empty — consumers (worker threads) wait here when the queue is empty.
//               Signaled by push() when a new URL is added.
//
//   not_full  — producers (worker threads pushing newly found links) wait
//               here when the queue is at capacity.  Provides backpressure:
//               if crawling is very fast the queue fills up and extra link
//               extraction blocks until workers consume some entries.
//               Signaled by pop() when a slot is freed.
//
// shutdown() sets a flag and broadcasts both CVs so every blocked caller
// wakes up and sees the done flag, returning false to exit cleanly.
// -------------------------------------------------------------------------
struct BoundedQueue {
    std::queue<UrlEntry>    q;
    std::mutex              mtx;
    std::condition_variable not_empty;  // wait here when queue is empty
    std::condition_variable not_full;   // wait here when queue is full
    int                     cap;
    bool                    done;

    explicit BoundedQueue(int capacity) : cap(capacity), done(false) {}

    // push: blocks if queue is at cap; returns false if shutdown was called
    bool push(const std::string &url, int depth) {
        std::unique_lock<std::mutex> lk(mtx);
        // Block while full unless shutting down (done avoids deadlock)
        not_full.wait(lk, [&]{ return (int)q.size() < cap || done; });
        if (done) return false;
        q.push({url, depth});
        not_empty.notify_one();  // wake one waiting consumer
        return true;
    }

    // pop: blocks if queue is empty; returns false when shutdown + empty
    bool pop(UrlEntry &out) {
        std::unique_lock<std::mutex> lk(mtx);
        // Block while empty unless shutting down
        not_empty.wait(lk, [&]{ return !q.empty() || done; });
        if (q.empty()) return false;   // shutdown + empty = no more work
        out = std::move(q.front());
        q.pop();
        not_full.notify_one();  // wake one waiting producer
        return true;
    }

    // shutdown: wake all blocked callers so they can exit
    void shutdown() {
        std::lock_guard<std::mutex> lk(mtx);
        done = true;
        not_empty.notify_all();
        not_full.notify_all();
    }

    int size() {
        std::lock_guard<std::mutex> lk(mtx);
        return (int)q.size();
    }
};

// -------------------------------------------------------------------------
// Thread-safe visited set
//
// check_and_insert is atomic: the check and the insert happen under the
// same lock, so two threads cannot both see a URL as "new" at the same time.
// -------------------------------------------------------------------------
struct VisitedSet {
    std::unordered_set<std::string> set;
    std::mutex                      mtx;

    // Returns true if url was not in the set and was just inserted.
    bool check_and_insert(const std::string &url) {
        std::lock_guard<std::mutex> lk(mtx);
        return set.insert(url).second;
    }
};

// -------------------------------------------------------------------------
// IPC client: sends "docid url filepath depth\n" to the indexer socket.
// A mutex serializes sends so multiple threads never interleave messages.
// -------------------------------------------------------------------------
struct IpcClient {
    int        fd  = -1;
    std::mutex mtx; // protects fd across concurrent worker threads

    bool connect(const std::string &path) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd); fd = -1; return false;
        }
        return true;
    }

    // Send one metadata line to the indexer.  Thread-safe.
    void send_msg(int docid, const std::string &url,
                  const std::string &filepath, int depth) {
        std::lock_guard<std::mutex> lk(mtx);
        if (fd < 0) return;
        std::string msg = std::to_string(docid) + " " + url + " "
                        + filepath + " " + std::to_string(depth) + "\n";
        send(fd, msg.c_str(), msg.size(), 0);
    }

    void close_connection() {
        if (fd >= 0) { close(fd); fd = -1; }
    }
};

// -------------------------------------------------------------------------
// libcurl write callback — appends received bytes to a std::string
// -------------------------------------------------------------------------
static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    auto *body = static_cast<std::string *>(ud);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

// Fetch url using the given (per-thread) curl handle.
// Returns the response body, or empty string on any error.
static std::string fetch_url(CURL *curl, const std::string &url)
{
    std::string body;
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,       5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "cpp-crawler/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) return {};
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (code != 200) return {};
    return body;
}

// -------------------------------------------------------------------------
// Link extraction using std::string::find
// Scans for href=" and href=' patterns and returns raw href values.
// -------------------------------------------------------------------------
static std::vector<std::string> extract_hrefs(const std::string &html)
{
    std::vector<std::string> hrefs;
    size_t pos = 0;
    while (pos < html.size()) {
        // Find next href= (case-sensitive; lowercase covers most real pages)
        size_t found = html.find("href=", pos);
        if (found == std::string::npos) break;
        pos = found + 5;
        if (pos >= html.size()) break;

        char quote = html[pos];
        if (quote != '"' && quote != '\'') continue;
        pos++;

        size_t end = html.find(quote, pos);
        if (end == std::string::npos) break;

        std::string href = html.substr(pos, end - pos);
        pos = end + 1;

        if (!href.empty())
            hrefs.push_back(std::move(href));
    }
    return hrefs;
}

// Resolve href to an absolute http/https URL given the page's base URL.
// Handles: absolute URLs, root-relative (/path), protocol-relative (//host).
// Returns empty string for anything we can't resolve (relative paths, mailto:, etc.).
static std::string resolve_url(const std::string &href, const std::string &base)
{
    if (href.empty() || href[0] == '#') return {};

    // Already absolute http(s)
    if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0)
        return href;

    // Protocol-relative: //host/path  →  https://host/path
    if (href.rfind("//", 0) == 0)
        return "https:" + href;

    // Root-relative: /path  →  scheme://host/path
    if (href[0] == '/') {
        size_t sep = base.find("://");
        if (sep == std::string::npos) return {};
        size_t host_start = sep + 3;
        size_t host_end   = base.find('/', host_start);
        if (host_end == std::string::npos) host_end = base.size();
        return base.substr(0, host_end) + href;
    }

    // Relative paths (../../foo) are hard without a full parser; skip them.
    return {};
}

static void make_dir(const std::string &path)
{
    mkdir(path.c_str(), 0755);
}

static bool write_file(const std::string &path, const std::string &data)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return f.good();
}

// -------------------------------------------------------------------------
// main
// -------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    // --- Parse CLI flags ---------------------------------------------------
    std::string seed, out_dir, ipc_path;
    int max_depth = 2, max_pages = 50, num_threads = 4;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "--seed"      && i+1 < argc) seed        = argv[++i];
        else if (a == "--max-depth" && i+1 < argc) max_depth   = std::stoi(argv[++i]);
        else if (a == "--max-pages" && i+1 < argc) max_pages   = std::stoi(argv[++i]);
        else if (a == "-t"          && i+1 < argc) num_threads = std::stoi(argv[++i]);
        else if (a == "--out"       && i+1 < argc) out_dir     = argv[++i];
        else if (a == "--ipc"       && i+1 < argc) ipc_path    = argv[++i];
    }

    if (seed.empty() || out_dir.empty() || ipc_path.empty()) {
        std::cerr << "Usage: crawler --seed <url> --max-depth <D> --max-pages <N>"
                     " -t <threads> --out <dir> --ipc <path>\n";
        return 1;
    }
    if (num_threads < 1) num_threads = 1;
    if (max_pages   < 1) max_pages   = 1;

    // --- Create output directory for HTML pages ---------------------------
    std::string pages_dir = out_dir + "/pages";
    make_dir(out_dir);
    make_dir(pages_dir);

    // --- Initialize shared state ------------------------------------------
    BoundedQueue queue(1000);    // cap: at most 1000 pending URLs in flight
    VisitedSet   visited;
    IpcClient    ipc;

    std::atomic<int> pages_fetched{0};
    std::atomic<int> pages_failed {0};
    std::atomic<int> pages_skipped{0};
    std::atomic<int> next_docid   {0};

    // active_workers counts threads currently fetching/processing (not in pop).
    // When active_workers == 0 AND queue is empty, the frontier is exhausted.
    std::atomic<int> active_workers{0};

    curl_global_init(CURL_GLOBAL_ALL);

    // --- Connect to indexer -----------------------------------------------
    std::cerr << "[CRAWLER] Connecting to indexer at " << ipc_path << "...\n";
    if (!ipc.connect(ipc_path)) {
        std::cerr << "[CRAWLER] Failed to connect. Is the indexer running?\n";
        return 1;
    }
    std::cerr << "[CRAWLER] Connected.\n";

    // --- Seed the queue ---------------------------------------------------
    visited.check_and_insert(seed);
    queue.push(seed, 0);

    auto t_start = std::chrono::steady_clock::now();

    // --- Worker thread lambda ---------------------------------------------
    //
    // Stop conditions (both trigger queue.shutdown() which wakes all threads):
    //   1. pages_fetched >= max_pages — we've crawled enough.
    //   2. active_workers == 0 AND queue is empty — frontier exhausted:
    //      no thread is doing anything and there are no queued URLs, so
    //      no new URLs will ever arrive.
    //
    // Checking these two conditions non-atomically is safe in practice:
    // if we miss a transition, the next iteration will catch it.
    // -----------------------------------------------------------------------
    auto worker = [&]() {
        CURL *curl = curl_easy_init();
        if (!curl) { std::cerr << "[WORKER] curl_easy_init failed\n"; return; }

        while (true) {
            // Check page limit before blocking on pop
            if (pages_fetched.load() >= max_pages) {
                queue.shutdown();
                break;
            }

            UrlEntry entry;
            if (!queue.pop(entry)) break;  // shutdown + empty

            // We now hold a URL and are actively working
            active_workers.fetch_add(1);

            // Re-check page limit (another thread may have tripped it)
            if (pages_fetched.load() >= max_pages) {
                active_workers.fetch_sub(1);
                queue.shutdown();
                break;
            }

            // Fetch the page via libcurl
            std::string html = fetch_url(curl, entry.url);
            if (html.empty()) {
                pages_failed.fetch_add(1);
                // Fall through to frontier-exhaustion check below
            } else {
                // Save HTML to disk: <out>/pages/<docid>.html
                int         docid    = next_docid.fetch_add(1);
                std::string filepath = pages_dir + "/" +
                                       std::to_string(docid) + ".html";

                if (!write_file(filepath, html)) {
                    pages_failed.fetch_add(1);
                } else {
                    // Send metadata line to indexer over the UNIX socket
                    ipc.send_msg(docid, entry.url, filepath, entry.depth);

                    int n = pages_fetched.fetch_add(1) + 1;
                    std::cerr << "[CRAWLER] [" << n << "/" << max_pages
                              << "] depth=" << entry.depth
                              << " " << entry.url << "\n";

                    if (n >= max_pages) {
                        active_workers.fetch_sub(1);
                        queue.shutdown();
                        break;
                    }

                    // Extract and enqueue links (only if below max_depth)
                    if (entry.depth < max_depth) {
                        std::vector<std::string> hrefs = extract_hrefs(html);
                        for (const std::string &href : hrefs) {
                            std::string abs = resolve_url(href, entry.url);
                            if (abs.empty()) {
                                pages_skipped.fetch_add(1);
                                continue;
                            }
                            // Check-and-insert is atomic so no two threads
                            // can both believe they discovered the same URL.
                            if (!visited.check_and_insert(abs)) {
                                pages_skipped.fetch_add(1);
                                continue;
                            }
                            queue.push(abs, entry.depth + 1);
                        }
                    }
                }
            }

            // Done with this URL — decrement active count.
            // If we are the last active thread AND the queue is empty,
            // no more work will ever arrive: trigger shutdown.
            int remaining = active_workers.fetch_sub(1) - 1;
            if (remaining == 0 && queue.size() == 0)
                queue.shutdown();
        }

        curl_easy_cleanup(curl);
    };

    // --- Launch thread pool -----------------------------------------------
    std::cerr << "[CRAWLER] Starting " << num_threads << " threads."
              << " max-depth=" << max_depth
              << " max-pages=" << max_pages << "\n";

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; i++)
        threads.emplace_back(worker);

    // --- Wait for all workers to finish -----------------------------------
    for (auto &t : threads) t.join();

    // Close socket — the indexer sees EOF and flushes the index to disk
    ipc.close_connection();

    auto t_end   = std::chrono::steady_clock::now();
    double secs  = std::chrono::duration<double>(t_end - t_start).count();

    std::cout << "\n=== Crawler Summary ===\n"
              << "Pages fetched:  " << pages_fetched.load() << "\n"
              << "Pages failed:   " << pages_failed.load()  << "\n"
              << "Pages skipped:  " << pages_skipped.load() << "\n"
              << "Total runtime:  " << secs << "s\n";

    curl_global_cleanup();
    return 0;
}
