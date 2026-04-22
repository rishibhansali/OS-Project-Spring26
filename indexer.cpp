/*
 * indexer.cpp — Inverted index builder (C++17)
 *
 * Usage:
 *   ./indexer --ipc <socket-path> --out <dir>
 *
 * Flow:
 *   1. Create a UNIX socket, bind, listen, accept one crawler connection.
 *   2. Read newline-terminated "docid url filepath depth\n" messages.
 *   3. For each message: open the HTML file, strip tags, tokenize words,
 *      add every word → docid mapping to the in-memory inverted index.
 *   4. When the crawler closes the connection, flush to disk and exit.
 *
 * On-disk format (all files written to <out>/):
 *   docs.tsv      — docid  url  filepath  depth  (tab-separated)
 *   dict.tsv      — term   offset  df        (tab-separated)
 *   postings.bin  — [uint32 count][uint32 docid × count] per term
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cerrno>

// POSIX headers for socket and filesystem
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

// -------------------------------------------------------------------------
// Data structures
// -------------------------------------------------------------------------

struct DocInfo {
    int         docid;
    std::string url;
    std::string filepath;
    int         depth;
};

// -------------------------------------------------------------------------
// HTML tag stripping
// Skip everything between < and > (simple state machine).
// -------------------------------------------------------------------------
static std::string strip_tags(const std::string &html)
{
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    for (char c : html) {
        if      (c == '<') { in_tag = true;  out += ' '; }
        else if (c == '>') { in_tag = false; out += ' '; }
        else if (!in_tag)   out += c;
    }
    return out;
}

// -------------------------------------------------------------------------
// Tokenizer: extract lowercase alphabetic words of length >= 2.
// Splits on any non-alpha character.
// -------------------------------------------------------------------------
static std::vector<std::string> tokenize(const std::string &text)
{
    std::vector<std::string> tokens;
    std::string word;
    for (unsigned char c : text) {
        if (std::isalpha(c)) {
            word += (char)std::tolower(c);
        } else if (!word.empty()) {
            if (word.size() >= 2)
                tokens.push_back(word);
            word.clear();
        }
    }
    if (word.size() >= 2)
        tokens.push_back(word);
    return tokens;
}

// -------------------------------------------------------------------------
// Read a whole file into a string.  Returns empty string on error.
// -------------------------------------------------------------------------
static std::string read_file(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

// -------------------------------------------------------------------------
// Receive bytes from a socket one at a time until '\n' or EOF.
// Returns the accumulated line (without the newline).
// Returns an empty string on connection close.
// -------------------------------------------------------------------------
static std::string recv_line(int fd)
{
    std::string line;
    char c;
    while (true) {
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) break;        // 0 = EOF, -1 = error
        if (c == '\n') break;
        line += c;
    }
    return line;
}

static void make_dir(const std::string &path)
{
    mkdir(path.c_str(), 0755);
}

// -------------------------------------------------------------------------
// main
// -------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    std::string ipc_path, out_dir;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "--ipc" && i + 1 < argc) ipc_path = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_dir  = argv[++i];
    }

    if (ipc_path.empty() || out_dir.empty()) {
        std::cerr << "Usage: indexer --ipc <path> --out <dir>\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // UNIX socket setup block
    //
    // We create an AF_UNIX SOCK_STREAM socket (like a local TCP pipe).
    // bind() names it at ipc_path in the filesystem.
    // listen() marks it as a passive socket ready to accept connections.
    // accept() blocks until the crawler connects, then returns a new fd for
    // that specific connection.  The original server_fd is no longer needed
    // after accept() and is closed immediately.
    // -----------------------------------------------------------------------
    unlink(ipc_path.c_str());   // remove stale socket from a previous run

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { std::cerr << "socket: " << strerror(errno) << "\n"; return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ipc_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind: " << strerror(errno) << "\n"; return 1;
    }
    if (listen(server_fd, 1) < 0) {
        std::cerr << "listen: " << strerror(errno) << "\n"; return 1;
    }

    std::cerr << "[INDEXER] Listening on " << ipc_path << " — waiting for crawler...\n";

    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) { std::cerr << "accept: " << strerror(errno) << "\n"; return 1; }
    close(server_fd);   // no more connections needed
    // -----------------------------------------------------------------------
    // End socket setup block
    // -----------------------------------------------------------------------

    std::cerr << "[INDEXER] Crawler connected. Indexing...\n";

    // In-memory index structures
    std::vector<DocInfo>                             docs;
    std::unordered_map<std::string, std::vector<int>> inv_index; // word → [docid, ...]

    // Receive loop: read one message per crawled page until the crawler closes
    int docs_indexed = 0;
    while (true) {
        std::string line = recv_line(client_fd);
        if (line.empty()) break;   // EOF — crawler done

        // Parse "docid url filepath depth"
        std::istringstream ss(line);
        DocInfo doc;
        if (!(ss >> doc.docid >> doc.url >> doc.filepath >> doc.depth))
            continue;

        docs.push_back(doc);

        // Open the saved HTML file and index its words
        std::string html = read_file(doc.filepath);
        if (html.empty()) {
            std::cerr << "[INDEXER] Cannot read " << doc.filepath << "\n";
            continue;
        }

        std::string          text  = strip_tags(html);
        std::vector<std::string> words = tokenize(text);

        for (const std::string &w : words)
            inv_index[w].push_back(doc.docid);

        docs_indexed++;
        if (docs_indexed % 10 == 0)
            std::cerr << "[INDEXER] Indexed " << docs_indexed << " documents...\n";
    }

    close(client_fd);
    unlink(ipc_path.c_str());

    std::cerr << "[INDEXER] Indexed " << docs_indexed << " docs, "
              << inv_index.size() << " unique terms. Writing index to "
              << out_dir << " ...\n";

    make_dir(out_dir);

    // -----------------------------------------------------------------------
    // File-writing section
    //
    // We write three files to <out_dir>/:
    //
    //   docs.tsv     — one tab-separated line per document:
    //                  docid  url  filepath  depth
    //
    //   postings.bin — binary file.  For each term we write:
    //                  [uint32_t count][uint32_t docid_0]...[uint32_t docid_n]
    //                  Terms are written in hash-map iteration order.
    //
    //   dict.tsv     — one tab-separated line per term:
    //                  term  offset  df
    //                  'offset' is the byte position inside postings.bin where
    //                  that term's block begins.  The query tool uses ftell()
    //                  before each write here, stores it in dict.tsv, and later
    //                  fseeks directly to that position — avoiding a full scan
    //                  of the binary file for every query.
    // -----------------------------------------------------------------------

    // 1. Write docs.tsv
    std::ofstream docs_f(out_dir + "/docs.tsv");
    if (!docs_f) { std::cerr << "Cannot open docs.tsv\n"; return 1; }
    for (const DocInfo &d : docs)
        docs_f << d.docid << "\t" << d.url << "\t"
               << d.filepath << "\t" << d.depth << "\n";
    docs_f.close();

    // 2. Write postings.bin + dict.tsv together
    FILE *pf = fopen((out_dir + "/postings.bin").c_str(), "wb");
    if (!pf) { std::cerr << "Cannot open postings.bin\n"; return 1; }

    std::ofstream dict_f(out_dir + "/dict.tsv");
    if (!dict_f) { std::cerr << "Cannot open dict.tsv\n"; fclose(pf); return 1; }

    for (auto &kv : inv_index) {
        const std::string &term = kv.first;
        std::vector<int>  &ids  = kv.second;

        // Sort and deduplicate docids for this term
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

        // Record the byte offset just before we write this term's postings.
        // This is what goes into dict.tsv so the query tool can fseek here.
        long offset = ftell(pf);

        // Write binary block: [count][docid0][docid1]...
        uint32_t count = (uint32_t)ids.size();
        fwrite(&count, sizeof(uint32_t), 1, pf);
        for (int id : ids) {
            uint32_t uid = (uint32_t)id;
            fwrite(&uid, sizeof(uint32_t), 1, pf);
        }

        // dict.tsv line: term  byte-offset  document-frequency
        dict_f << term << "\t" << offset << "\t" << count << "\n";
    }

    fclose(pf);
    dict_f.close();
    // -----------------------------------------------------------------------
    // End file-writing section
    // -----------------------------------------------------------------------

    std::cerr << "[INDEXER] Done.\n";
    return 0;
}
