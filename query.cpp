/*
 * query.cpp — Command-line index search tool (C++17)
 *
 * Usage:
 *   ./query --index <dir> <term1> [term2 ...]
 *
 * Reads the on-disk inverted index produced by indexer.cpp and answers
 * multi-term AND queries: only documents that contain ALL search terms
 * are returned.
 *
 * On-disk format expected in <dir>/:
 *   docs.tsv      — docid  url  filepath  depth  (tab-separated)
 *   dict.tsv      — term   offset  df             (tab-separated)
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

// -------------------------------------------------------------------------
// Load docs.tsv into a map: docid → url
// Each line: docid<TAB>url<TAB>filepath<TAB>depth
// -------------------------------------------------------------------------
static std::unordered_map<int, std::string>
load_docs(const std::string &path)
{
    std::unordered_map<int, std::string> docid_to_url;
    std::ifstream f(path);
    if (!f) {
        std::cerr << "[QUERY] Cannot open " << path << ": " << strerror(errno) << "\n";
        return docid_to_url;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        int         docid;
        std::string url;
        if (ss >> docid >> url)
            docid_to_url[docid] = url;
    }
    return docid_to_url;
}

// -------------------------------------------------------------------------
// Load dict.tsv into a map: term → {byte_offset, document_frequency}
// Each line: term<TAB>offset<TAB>df
// -------------------------------------------------------------------------
static std::unordered_map<std::string, std::pair<long, int>>
load_dict(const std::string &path)
{
    std::unordered_map<std::string, std::pair<long, int>> dict;
    std::ifstream f(path);
    if (!f) {
        std::cerr << "[QUERY] Cannot open " << path << ": " << strerror(errno) << "\n";
        return dict;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string term;
        long        offset;
        int         df;
        if (ss >> term >> offset >> df)
            dict[term] = {offset, df};
    }
    return dict;
}

// -------------------------------------------------------------------------
// Read the postings list for one term from postings.bin.
// Seeks the file pointer to 'offset', reads the uint32_t count header,
// then reads that many uint32_t docids into a vector.
// Returns an empty vector on any error.
// -------------------------------------------------------------------------
static std::vector<int>
read_postings(FILE *pf, long offset)
{
    if (fseek(pf, offset, SEEK_SET) != 0) return {};

    uint32_t count = 0;
    if (fread(&count, sizeof(uint32_t), 1, pf) != 1) return {};
    if (count == 0) return {};

    std::vector<int> ids(count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t uid = 0;
        if (fread(&uid, sizeof(uint32_t), 1, pf) != 1) return {};
        ids[i] = (int)uid;
    }
    return ids;   // already sorted (indexer wrote them sorted)
}

// -------------------------------------------------------------------------
// AND intersection of N sorted integer lists.
// Uses a simple nested loop: starts with the first list and intersects
// each subsequent list into the running result.
// Returns the set of docids that appear in ALL lists.
// -------------------------------------------------------------------------
static std::vector<int>
intersect_all(const std::vector<std::vector<int>> &lists)
{
    if (lists.empty()) return {};

    std::vector<int> result = lists[0];

    for (size_t i = 1; i < lists.size() && !result.empty(); i++) {
        std::vector<int> merged;
        const std::vector<int> &other = lists[i];

        // Both lists are sorted — merge in O(m + n)
        size_t j = 0, k = 0;
        while (j < result.size() && k < other.size()) {
            if (result[j] == other[k]) {
                merged.push_back(result[j]);
                j++; k++;
            } else if (result[j] < other[k]) {
                j++;
            } else {
                k++;
            }
        }
        result = std::move(merged);
    }

    return result;
}

// -------------------------------------------------------------------------
// main
// -------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    std::string index_dir;

    // Parse --index flag; remaining args are query terms
    int term_start = argc;   // index of first query term in argv
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--index" && i + 1 < argc) {
            index_dir  = argv[++i];
            term_start = i + 1;
        }
    }

    if (index_dir.empty()) {
        std::cerr << "Usage: query --index <dir> <term1> [term2 ...]\n";
        return 1;
    }

    int n_terms = argc - term_start;
    if (n_terms <= 0) {
        std::cerr << "Error: at least one search term required.\n";
        return 1;
    }

    // Collect and lowercase the query terms
    std::vector<std::string> terms;
    for (int i = term_start; i < argc; i++) {
        std::string t = argv[i];
        std::transform(t.begin(), t.end(), t.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        terms.push_back(t);
    }

    // --- Load index files --------------------------------------------------
    auto dict        = load_dict(index_dir + "/dict.tsv");
    auto docid_to_url= load_docs(index_dir + "/docs.tsv");

    if (dict.empty()) {
        std::cerr << "[QUERY] Dictionary is empty or failed to load.\n";
        return 1;
    }

    FILE *pf = fopen((index_dir + "/postings.bin").c_str(), "rb");
    if (!pf) {
        std::cerr << "[QUERY] Cannot open postings.bin: " << strerror(errno) << "\n";
        return 1;
    }

    // --- Look up each query term ------------------------------------------
    std::vector<std::vector<int>> posting_lists;

    for (const std::string &term : terms) {
        auto it = dict.find(term);
        if (it == dict.end()) {
            std::cout << "No documents matched all query terms.\n";
            fclose(pf);
            return 0;
        }
        long offset = it->second.first;
        std::vector<int> ids = read_postings(pf, offset);
        if (ids.empty()) {
            std::cout << "No documents matched all query terms.\n";
            fclose(pf);
            return 0;
        }
        posting_lists.push_back(std::move(ids));
    }

    fclose(pf);

    // --- AND intersection -------------------------------------------------
    std::vector<int> result = intersect_all(posting_lists);

    if (result.empty()) {
        std::cout << "No documents matched all query terms.\n";
        return 0;
    }

    // --- Print results ----------------------------------------------------
    std::cout << "Found " << result.size() << " matching document"
              << (result.size() == 1 ? "" : "s")
              << " (AND across terms):\n";

    for (int docid : result) {
        auto it = docid_to_url.find(docid);
        std::string url = (it != docid_to_url.end()) ? it->second : "(unknown)";
        std::cout << "  " << docid << "  " << url << "\n";
    }

    return 0;
}
