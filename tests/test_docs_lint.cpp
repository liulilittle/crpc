// CRPC -- machine checks for the documentation set.
//
// docs/ holds the normative policy and user-facing guides, but until now only
// docs/abi.md had an automated gate (tests/test_abi_docs.cpp). The rest could
// drift silently: a guide could be added without its Chinese pair, a file could
// be saved with the wrong encoding, or a "this is still being worked on" phrase
// could be left behind long after the work landed.
//
// This test is deliberately generic. It walks docs/ and asserts four properties:
//
//   1. every Markdown file exists, is non-empty and is strict UTF-8 (no BOM, no
//      U+FFFD, no overlong/surrogate/truncated sequence);
//   2. every non-historical English document has a Chinese sibling, and every
//      paired document links to its sibling;
//   3. no non-historical document carries a known stale phrase;
//   4. every qualified API name used in a fenced C++ block resolves against the
//      public headers.
//
// The checks are text scans, not a Markdown parser. That keeps them stable when
// prose is rewrapped while still failing when a fact changes. The historical
// exemption is explicit: docs/CHANGELOG* (dated release notes) and any restored
// docs/superpowers/** archive are records of a past moment rather than living
// documents, so a stale phrase there is the point rather than a defect.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "test_framework.hpp"

namespace {

namespace fs = std::filesystem;

// Resolve the repository root from this source file. CMake compiles tests with
// absolute paths, so `__FILE__` already points at <root>/tests/test_docs_lint.cpp
// in every supported generator. The fallback walks up from the working
// directory so a manual compile still finds docs/.
fs::path repo_root() {
    fs::path from_source = fs::path(__FILE__).parent_path().parent_path();
    if (fs::exists(from_source / "docs" / "README.md")) {
        return from_source;
    }
    fs::path probe = fs::current_path();
    for (int depth = 0; depth < 8 && !probe.empty(); ++depth) {
        if (fs::exists(probe / "docs" / "README.md")) {
            return probe;
        }
        if (!probe.has_parent_path() || probe.parent_path() == probe) {
            break;
        }
        probe = probe.parent_path();
    }
    return {};
}

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

// --- Strict UTF-8 -----------------------------------------------------------
//
// A byte string is accepted only if it decodes as UTF-8 with no overlong form,
// no UTF-16 surrogate, no code point above U+10FFFF, no truncated sequence, no
// leading BOM and no U+FFFD. A BOM is rejected explicitly because the source
// tree is UTF-8 without BOM; a stray BOM makes some tools see an invisible
// character at the start of the first heading.
bool utf8_strict(const std::string& text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        return false;
    }
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        int len = 0;
        std::uint32_t cp = 0;
        if ((lead & 0xE0) == 0xC0) {
            len = 2;
            cp = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
            cp = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
            cp = lead & 0x07u;
        } else {
            return false;
        }
        if (i + static_cast<std::size_t>(len) > text.size()) {
            return false;
        }
        for (int k = 1; k < len; ++k) {
            const unsigned char cont =
                static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            if ((cont & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cont & 0x3Fu);
        }
        if (len == 2 && cp < 0x80) {
            return false;
        }
        if (len == 3 && cp < 0x800) {
            return false;
        }
        if (len == 4 && cp < 0x10000) {
            return false;
        }
        if (cp > 0x10FFFF) {
            return false;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return false;
        }
        if (cp == 0xFFFD) {
            return false;
        }
        i += static_cast<std::size_t>(len);
    }
    return true;
}

// --- Document inventory -----------------------------------------------------

struct DocFile {
    std::string rel;
    fs::path path;
};

std::vector<DocFile> collect_docs(const fs::path& root) {
    std::vector<DocFile> out;
    const fs::path docs_dir = root / "docs";
    std::error_code ec;
    fs::recursive_directory_iterator it(
        docs_dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return out;
    }
    for (const fs::directory_entry& entry : it) {
        std::error_code type_ec;
        if (!entry.is_regular_file(type_ec)) {
            continue;
        }
        if (entry.path().extension() != ".md") {
            continue;
        }
        DocFile doc;
        doc.rel = fs::relative(entry.path(), docs_dir).generic_string();
        doc.path = entry.path();
        out.push_back(std::move(doc));
    }
    std::sort(out.begin(), out.end(),
              [](const DocFile& a, const DocFile& b) { return a.rel < b.rel; });
    return out;
}

bool has_doc(const std::vector<DocFile>& docs, const std::string& rel) {
    for (const DocFile& doc : docs) {
        if (doc.rel == rel) {
            return true;
        }
    }
    return false;
}

bool is_chinese(const std::string& rel) {
    return rel.size() >= 9 && rel.compare(rel.size() - 9, 9, ".zh-CN.md") == 0;
}

std::string english_of(const std::string& zh_rel) {
    return zh_rel.substr(0, zh_rel.size() - 9) + ".md";
}

std::string chinese_of(const std::string& en_rel) {
    return en_rel.substr(0, en_rel.size() - 3) + ".zh-CN.md";
}

// Dated records and working plans are not living documents. A "being finalised"
// note in a plan from last month is history, not a defect.
bool is_historical(const std::string& rel) {
    return rel.rfind("superpowers/", 0) == 0 ||
           rel.rfind("CHANGELOG", 0) == 0;
}

// --- Stale-phrase matching --------------------------------------------------
//
// Prose is wrapped at 80 columns, so a phrase can be split across a line break.
// Both the document and the needle are normalised to single spaces before the
// substring test, so "being\nfinalised" still matches "being finalised" while a
// phrase inside a longer word ("finalised" inside "finalised-ish") is not
// silently missed. The needle list is the known-stale set; extending it is a
// one-line change here.

std::string normalize_ws(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
            c == '\v') {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(raw);
    }
    return out;
}

// --- API-name resolution ----------------------------------------------------

bool ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

struct HeaderIndex {
    std::set<std::string> ids;
    std::set<std::string> quals;
};

// Collect every identifier and every `a::b::...` spelling that appears in the
// public headers. A qualified name is only kept when it is well formed (it never
// ends in `::`), so a doc reference can be matched exactly.
void scan_quals(const std::string& text, std::set<std::string>& out) {
    std::size_t i = 0;
    while (i < text.size()) {
        if (!ident_char(text[i])) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        std::size_t j = i;
        for (;;) {
            while (j < text.size() && ident_char(text[j])) {
                ++j;
            }
            if (j + 2 < text.size() && text[j] == ':' && text[j + 1] == ':' &&
                ident_char(text[j + 2])) {
                j += 2;
                continue;
            }
            break;
        }
        if (j > start) {
            const std::string qual = text.substr(start, j - start);
            if (qual.find("::") != std::string::npos) {
                out.insert(qual);
            }
            i = j;
        } else {
            ++i;
        }
    }
}

HeaderIndex scan_headers(const fs::path& root) {
    HeaderIndex index;
    const fs::path include_dir = root / "include" / "crpc";
    std::error_code ec;
    fs::recursive_directory_iterator it(
        include_dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return index;
    }
    for (const fs::directory_entry& entry : it) {
        std::error_code type_ec;
        if (!entry.is_regular_file(type_ec) ||
            entry.path().extension() != ".hpp") {
            continue;
        }
        const std::string text = read_text(entry.path());
        std::size_t i = 0;
        while (i < text.size()) {
            if (ident_char(text[i])) {
                const std::size_t start = i;
                while (i < text.size() && ident_char(text[i])) {
                    ++i;
                }
                index.ids.insert(text.substr(start, i - start));
            } else {
                ++i;
            }
        }
        scan_quals(text, index.quals);
    }
    return index;
}

std::vector<std::string> split_qual(const std::string& qual) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = qual.find("::", start);
        if (pos == std::string::npos) {
            parts.push_back(qual.substr(start));
            break;
        }
        parts.push_back(qual.substr(start, pos - start));
        start = pos + 2;
    }
    return parts;
}

// Type names a document declares itself (a tutorial's `class greeter`, a
// recipe's `struct profile`). A qualified name rooted at one of these is user
// code, not a CRPC API, and must not be checked against the headers.
std::set<std::string> declared_types(const std::string& text) {
    std::set<std::string> out;
    static const char* const keywords[] = {"class", "struct", "namespace",
                                           "using"};
    std::size_t i = 0;
    while (i < text.size()) {
        if (!ident_char(text[i])) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < text.size() && ident_char(text[i])) {
            ++i;
        }
        const std::string word = text.substr(start, i - start);
        bool is_keyword = false;
        for (const char* kw : keywords) {
            if (word == kw) {
                is_keyword = true;
                break;
            }
        }
        if (!is_keyword) {
            continue;
        }
        std::size_t k = i;
        while (k < text.size() && (text[k] == ' ' || text[k] == '\t' ||
                                   text[k] == '\n' || text[k] == '\r')) {
            ++k;
        }
        const std::size_t name_start = k;
        while (k < text.size() && ident_char(text[k])) {
            ++k;
        }
        if (k > name_start) {
            out.insert(text.substr(name_start, k - name_start));
        }
    }
    return out;
}

// Fenced ```cpp blocks only. A doc that shows an API in a ```text diagram is
// not making a compilable claim, and shell/powershell blocks are not C++.
std::vector<std::string> cpp_blocks(const std::string& text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t open = text.find("```cpp", pos);
        if (open == std::string::npos) {
            break;
        }
        const std::size_t line_end = text.find('\n', open + 6);
        if (line_end == std::string::npos) {
            break;
        }
        const std::size_t close = text.find("```", line_end + 1);
        if (close == std::string::npos) {
            break;
        }
        out.push_back(text.substr(line_end + 1, close - (line_end + 1)));
        pos = close + 3;
    }
    return out;
}

} // namespace

// --- 1. Presence, size and encoding -----------------------------------------

CRPC_TEST(docs_lint_all_markdown_present_nonempty_and_strict_utf8) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::vector<DocFile> docs = collect_docs(root);
    CRPC_CHECK(!docs.empty());

    // Meta-assertion: without this, an emptied or moved docs/ tree would leave
    // every loop below vacuous and the test would pass on nothing.
    CRPC_CHECK(has_doc(docs, "README.md"));
    CRPC_CHECK(has_doc(docs, "abi.md"));
    CRPC_CHECK(has_doc(docs, "protocol.md"));
    CRPC_CHECK(has_doc(docs, "redis-client.md"));

    for (const DocFile& doc : docs) {
        const std::string text = read_text(doc.path);
        if (text.empty()) {
            std::printf("  empty document: %s\n", doc.rel.c_str());
        }
        CRPC_CHECK_FALSE(text.empty());
        if (!utf8_strict(text)) {
            std::printf("  not strict UTF-8 (BOM/U+FFFD/bad sequence): %s\n",
                        doc.rel.c_str());
        }
        CRPC_CHECK(utf8_strict(text));
    }
}

// --- 2. Cross-language index ------------------------------------------------

CRPC_TEST(docs_lint_every_english_doc_has_a_chinese_pair) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::vector<DocFile> docs = collect_docs(root);
    std::size_t checked = 0;
    for (const DocFile& doc : docs) {
        if (is_chinese(doc.rel) || is_historical(doc.rel)) {
            continue;
        }
        const std::string zh = chinese_of(doc.rel);
        if (!has_doc(docs, zh)) {
            std::printf("  missing Chinese pair: %s (expected %s)\n",
                        doc.rel.c_str(), zh.c_str());
        }
        CRPC_CHECK(has_doc(docs, zh));
        ++checked;
    }
    CRPC_CHECK(checked > 0);
}

CRPC_TEST(docs_lint_pairs_link_to_each_other) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::vector<DocFile> docs = collect_docs(root);
    std::size_t checked = 0;
    for (const DocFile& doc : docs) {
        const std::string peer =
            is_chinese(doc.rel) ? english_of(doc.rel) : chinese_of(doc.rel);
        if (!has_doc(docs, peer)) {
            continue;
        }
        const std::string self = fs::path(doc.rel).filename().generic_string();
        const std::string peer_name =
            fs::path(peer).filename().generic_string();
        const std::string text = read_text(doc.path);
        const std::string peer_text = read_text(root / "docs" / peer);
        if (text.find(peer_name) == std::string::npos) {
            std::printf("  %s does not link to %s\n", doc.rel.c_str(),
                        peer_name.c_str());
        }
        CRPC_CHECK(text.find(peer_name) != std::string::npos);
        if (peer_text.find(self) == std::string::npos) {
            std::printf("  %s does not link back to %s\n", peer.c_str(),
                        self.c_str());
        }
        CRPC_CHECK(peer_text.find(self) != std::string::npos);
        ++checked;
    }
    CRPC_CHECK(checked > 0);
}

// --- 3. Known stale phrases -------------------------------------------------

CRPC_TEST(docs_lint_no_stale_phrases) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    static const char* const kStale[] = {
        "being finalised",
        "being finalized",
        "正在定案",
        "待复核",
        "正在补齐",
        "CF-1 completion race",
    };
    const std::vector<DocFile> docs = collect_docs(root);
    std::size_t checked = 0;
    for (const DocFile& doc : docs) {
        if (is_historical(doc.rel)) {
            continue;
        }
        const std::string normalized = normalize_ws(read_text(doc.path));
        for (const char* phrase : kStale) {
            if (normalized.find(normalize_ws(phrase)) == std::string::npos) {
                continue;
            }
            std::printf("  stale phrase \"%s\" in %s\n", phrase,
                        doc.rel.c_str());
            CRPC_CHECK(false);
        }
        ++checked;
    }
    CRPC_CHECK(checked > 0);
}

// --- 4. API names in fenced C++ blocks --------------------------------------

CRPC_TEST(docs_lint_api_names_exist_in_public_headers) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const HeaderIndex index = scan_headers(root);
    CRPC_CHECK(!index.ids.empty());
    CRPC_CHECK(!index.quals.empty());

    const std::vector<DocFile> docs = collect_docs(root);
    std::size_t candidates = 0;
    bool bad = false;
    for (const DocFile& doc : docs) {
        if (is_historical(doc.rel)) {
            continue;
        }
        const std::string text = read_text(doc.path);
        const std::set<std::string> declared = declared_types(text);
        for (const std::string& block : cpp_blocks(text)) {
            std::set<std::string> quals;
            scan_quals(block, quals);
            for (const std::string& qual : quals) {
                const std::vector<std::string> parts = split_qual(qual);
                const std::string& ns = parts.front();
                if (ns == "std") {
                    continue;
                }
                if (declared.count(ns) != 0) {
                    continue;
                }
                ++candidates;
                if (index.quals.count(qual) != 0) {
                    continue;
                }
                if (index.ids.count(ns) != 0 &&
                    index.ids.count(parts.back()) != 0) {
                    continue;
                }
                std::printf("  unknown API name \"%s\" in %s\n", qual.c_str(),
                            doc.rel.c_str());
                bad = true;
            }
        }
    }
    // Meta-assertion: redis-client.md must contribute candidates, so a broken
    // extractor cannot make this test vacuous.
    CRPC_CHECK(candidates > 0);
    CRPC_CHECK_FALSE(bad);
}


// --- 5. The API index must not silently fall behind ---------------------------
//
// docs/api/README.md calls itself "a map of the public headers under
// include/crpc". For a long time it was a map of *some* of them: 115 of the 183
// headers were absent, including whole subsystems added later (db/, orm/,
// mq/, game/, ipc/, fs/, ws/). Nothing noticed, because every other docs check
// asks "is what is written true", never "is anything missing".
//
// This is the symmetric check: every public header has to be pointed at from
// both language versions. Listing a header is cheap (a link), so the bar is
// set at the complete set rather than a curated subset -- an index that omits
// two thirds of the surface is worse than no index, because a reader concludes
// the missing modules do not exist.

CRPC_TEST(docs_api_index_lists_every_public_header) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const fs::path headers_dir = root / "include" / "crpc";
    CRPC_CHECK(fs::exists(headers_dir));
    if (!fs::exists(headers_dir)) {
        return;
    }

    std::set<std::string> headers;
    for (const auto& entry : fs::recursive_directory_iterator(headers_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".hpp") {
            continue;
        }
        headers.insert(
            fs::relative(entry.path(), headers_dir).generic_string());
    }
    // A scan that found nothing would make every assertion below vacuous.
    CRPC_CHECK(headers.size() > 100);

    const char* const indexes[] = {"docs/api/README.md",
                                   "docs/api/README.zh-CN.md"};
    for (const char* rel : indexes) {
        const fs::path path = root / rel;
        CRPC_CHECK(fs::exists(path));
        if (!fs::exists(path)) {
            continue;
        }
        const std::string text = read_text(path);
        std::size_t missing = 0;
        std::vector<std::string> reported;
        for (const std::string& header : headers) {
            if (text.find("include/crpc/" + header) != std::string::npos) {
                continue;
            }
            ++missing;
            if (reported.size() < 10) {
                reported.push_back(header);
            }
        }
        for (const std::string& header : reported) {
            std::printf("  %s does not list include/crpc/%s\n", rel,
                        header.c_str());
        }
        if (missing > reported.size()) {
            std::printf("  ... and %zu more in %s\n",
                        missing - reported.size(), rel);
        }
        CRPC_CHECK_EQ(std::size_t{0}, missing);
    }
}


// --- 6. The docs index must link every document -------------------------------
//
// docs/README.md calls itself "the entry point for the English documentation",
// and README.zh-CN.md is the same for Chinese. Neither is checked against the
// directory, so a document can exist, be linked from the top-level README and
// from its own language pair, and still be missing from the index a reader
// actually starts at. That is what happened to docs/ipc.md: written, reviewed,
// linked from ../../README.md, absent from the index for two rounds.
//
// The requirement is one-directional and precise: every English document is
// linked from the English index, every Chinese document from the Chinese index.
// The index deliberately links one language per file (each page links its own
// pair at the top), so requiring both languages everywhere would be wrong.

CRPC_TEST(docs_lint_index_lists_every_document) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::vector<DocFile> docs = collect_docs(root);
    CRPC_CHECK(!docs.empty());

    struct IndexCase {
        const char* index;
        bool chinese;
    };
    const IndexCase cases[] = {{"docs/README.md", false},
                               {"docs/README.zh-CN.md", true}};

    std::size_t checked = 0;
    bool bad = false;
    for (const IndexCase& c : cases) {
        const fs::path index_path = root / c.index;
        CRPC_CHECK(fs::exists(index_path));
        if (!fs::exists(index_path)) {
            continue;
        }
        const std::string index_text = read_text(index_path);
        for (const DocFile& doc : docs) {
            if (is_historical(doc.rel)) {
                continue;
            }
            if (is_chinese(doc.rel) != c.chinese) {
                continue;
            }
            // The index itself and its language sibling are not entries in it.
            if (doc.rel == "README.md" || doc.rel == "README.zh-CN.md") {
                continue;
            }
            ++checked;
            const std::string needle = "](" + doc.rel + ")";
            if (index_text.find(needle) != std::string::npos) {
                continue;
            }
            std::printf("  %s does not link %s\n", c.index, doc.rel.c_str());
            bad = true;
        }
    }
    // Both indexes together hold ~36 entries; a collapsed scan must not pass.
    CRPC_CHECK(checked > 20);
    CRPC_CHECK_FALSE(bad);
}

// --- 7. The examples index must mention every example ------------------------
//
// Same gap, third place: docs/api/README.md was missing 115 headers,
// docs/README.md was missing ipc.md, and examples/README.md was missing
// quickstart_ipc.cpp. The examples index is the page the root README sends a
// reader to, so an example absent from it is an example nobody discovers.
//
// "Mentioned" is the right bar here rather than a link: the page is prose about
// what each example demonstrates, not a table of links, so the file name in the
// text is what a reader searches for.

CRPC_TEST(docs_lint_examples_index_lists_every_example) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const fs::path dir = root / "examples";
    CRPC_CHECK(fs::exists(dir));
    if (!fs::exists(dir)) {
        return;
    }

    std::vector<std::string> examples;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".cpp") {
            continue;
        }
        examples.push_back(entry.path().filename().generic_string());
    }
    std::sort(examples.begin(), examples.end());
    CRPC_CHECK(examples.size() >= 4);

    const char* const indexes[] = {"examples/README.md",
                                   "examples/README.zh-CN.md"};
    bool bad = false;
    for (const char* rel : indexes) {
        const fs::path path = root / rel;
        CRPC_CHECK(fs::exists(path));
        if (!fs::exists(path)) {
            continue;
        }
        const std::string text = read_text(path);
        for (const std::string& example : examples) {
            if (text.find(example) != std::string::npos) {
                continue;
            }
            std::printf("  %s does not mention %s\n", rel, example.c_str());
            bad = true;
        }
    }
    CRPC_CHECK_FALSE(bad);
}

// --- 8. design.md's build/CI section must match the tree ---------------------
//
// Section 20 is the normative description of how this project is built. It had
// drifted into fiction: it named seven library targets (crpc_core,
// crpc_transport, crpc_rpc, crpc_http, crpc_cluster, crpc_tests, crpc_fuzz,
// crpc_bench) when the tree builds one library plus an INTERFACE shim, it
// listed CRPC_BUILD_FUZZ / CRPC_WITH_LZ4 / CRPC_WITH_ZSTD /
// CRPC_32BIT_FRAME_CAP which never existed, and it advertised CI jobs for x86,
// fuzz smoke, golden bytes and a benchmark report when ci.yml had none of them
// (x86 is now real; the rest were removed from the document).
//
// Both language versions are checked, because only one of them being right is
// the same defect in a different language.

CRPC_TEST(docs_lint_design_build_facts_match_the_tree) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const fs::path cmake_path = root / "CMakeLists.txt";
    const fs::path ci_path = root / ".github" / "workflows" / "ci.yml";
    CRPC_CHECK(fs::exists(cmake_path));
    CRPC_CHECK(fs::exists(ci_path));
    if (!fs::exists(cmake_path) || !fs::exists(ci_path)) {
        return;
    }
    const std::string cmake = read_text(cmake_path);
    const std::string ci = read_text(ci_path);

    // Every option the document says exists must be a real CMake variable.
    const char* const claimed_options[] = {
        "CRPC_BUILD_TESTS", "CRPC_BUILD_BENCH", "CRPC_BUILD_EXAMPLES",
        "CRPC_BUILD_SHARED", "CRPC_WITH_OPENSSL", "CRPC_WITH_ODBC",
        "CRPC_WARNINGS_AS_ERRORS", "CRPC_ENABLE_LTO", "CRPC_SANITIZER",
        "CRPC_RTTI_DIR", "CRPC_COROCC_DIR"};
    // Options that were removed from the document; naming them again is the
    // regression this half guards.
    const char* const phantom_options[] = {
        "CRPC_BUILD_FUZZ", "CRPC_WITH_LZ4", "CRPC_WITH_ZSTD",
        "CRPC_32BIT_FRAME_CAP"};
    // Targets the tree actually creates. The needle includes the table cell
    // separator: the claim is "this row of the target table is true", so a row
    // renamed to `crpc_all` must fail even though "`crpc`" still appears in the
    // surrounding prose.
    const char* const real_targets[] = {"| `crpc` |", "| `crpc_mpmc` |"};
    const char* const phantom_targets[] = {
        "`crpc_transport`", "`crpc_rpc`", "`crpc_http`", "`crpc_cluster`",
        "`crpc_tests`", "`crpc_fuzz`", "`crpc_bench`"};

    const char* const docs[] = {"docs/design.md", "docs/design.zh-CN.md"};
    bool bad = false;
    for (const char* rel : docs) {
        const fs::path path = root / rel;
        CRPC_CHECK(fs::exists(path));
        if (!fs::exists(path)) {
            continue;
        }
        const std::string text = read_text(path);

        for (const char* option : claimed_options) {
            if (cmake.find(option) == std::string::npos) {
                std::printf("  %s claims option %s, absent from CMakeLists\n",
                            rel, option);
                bad = true;
            }
        }
        for (const char* option : phantom_options) {
            if (text.find(option) != std::string::npos) {
                std::printf("  %s still names non-existent option %s\n", rel,
                            option);
                bad = true;
            }
        }
        for (const char* target : real_targets) {
            if (text.find(target) == std::string::npos) {
                std::printf("  %s no longer names real target %s\n", rel,
                            target);
                bad = true;
            }
        }
        for (const char* target : phantom_targets) {
            if (text.find(target) != std::string::npos) {
                std::printf("  %s still names non-existent target %s\n", rel,
                            target);
                bad = true;
            }
        }

        // CI facts: what the document may claim must appear in the workflow.
        struct CiClaim {
            const char* phrase_in_doc;
            const char* must_appear_in_ci;
        };
        const CiClaim claims[] = {
            {"linux-sanitizers", "linux-sanitizers"},
            {"linux-bench-compile", "linux-bench-compile"},
            {"bench-compile", "bench-compile"},
            {"examples", "examples"}
        };
        for (const CiClaim& claim : claims) {
            if (text.find(claim.phrase_in_doc) == std::string::npos) {
                continue;
            }
            if (ci.find(claim.must_appear_in_ci) == std::string::npos) {
                std::printf("  %s claims CI job %s, absent from ci.yml\n", rel,
                            claim.must_appear_in_ci);
                bad = true;
            }
        }
        // The x86 claim is the one this round made true: if the document lists
        // 32-bit Windows jobs, the workflow must actually pass the matrix arch to
        // BOTH the build and the test step. Counting occurrences is deliberate:
        // pinning only one of the two still means half the job builds x64, which
        // is not coverage. (Mutation M1 did exactly that and the first version of
        // this check passed it.)
        if (text.find("x86") != std::string::npos) {
            std::size_t uses = 0;
            const std::string needle = "-Arch ${{ matrix.arch }}";
            for (std::size_t pos = ci.find(needle); pos != std::string::npos;
                 pos = ci.find(needle, pos + needle.size())) {
                ++uses;
            }
            if (uses < 2) {
                std::printf("  %s claims x86 CI coverage, ci.yml passes the "
                            "matrix arch %zu time(s), need >= 2\n",
                            rel, uses);
                bad = true;
            }
        }
    }
    CRPC_CHECK_FALSE(bad);
}
// --- 9. The one-click scripts must accept the same switches ----------------
//
// "One-click build" is a per-platform promise: a contributor on Linux must be
// able to run the same job CI runs on Windows. That promise had already been
// broken once for the dependency paths (`build.sh` required CRPC_RTTI_DIR in
// the environment while `build.ps1` fell back to the submodule), and it broke
// again when `-Examples` and `-NoTests` were added to build.ps1 for the
// examples/bench CI jobs: build.sh and test.sh had neither, so the exact
// configuration those jobs test could not be reproduced on Linux at all.
//
// The check is deliberately one-directional: every switch the PowerShell
// script exposes must exist in its POSIX counterpart. The POSIX scripts also
// take value options (--config, --arch, ...) that the PowerShell ones expose
// as parameters of the same name, so comparing the full sets would produce
// noise without catching anything.

namespace {

// `[switch]$Name` in PowerShell; `--name)` in the POSIX case statement.
std::vector<std::string> ps_switches(const std::string& text) {
    std::vector<std::string> out;
    const std::string needle = "[switch]$";
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        pos += needle.size();
        std::size_t end = pos;
        while (end < text.size()) {
            const char c = text[end];
            const bool ident = (c >= 'A' && c <= 'Z') ||
                               (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9') || c == '_';
            if (!ident) {
                break;
            }
            ++end;
        }
        std::string name = text.substr(pos, end - pos);
        for (char& ch : name) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        out.push_back(name);
        pos = end;
    }
    return out;
}

std::vector<std::string> sh_switches(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        std::size_t pos = line.find_first_not_of(" \t");
        if (pos == std::string::npos || line[pos] != '-') {
            continue;
        }
        const std::size_t close = line.find(')', pos);
        if (close == std::string::npos) {
            continue;
        }
        std::string name = line.substr(pos, close - pos);
        // `-h|--help)`: take the long form.
        const std::size_t bars = name.rfind('|');
        if (bars != std::string::npos) {
            name = name.substr(bars + 1);
        }
        while (!name.empty() && name.front() == '-') {
            name.erase(name.begin());
        }
        if (name.empty() || name.find(' ') != std::string::npos) {
            continue;
        }
        // `-NoTests` has no separator while `--no-tests` does, so drop both
        // separators rather than mapping one to the other: mapping '-' to '_'
        // produced `no_tests` vs `notests` and failed on an equivalent pair.
        std::string compact;
        for (char ch : name) {
            if (ch != '-' && ch != '_') {
                compact.push_back(ch);
            }
        }
        out.push_back(compact);
    }
    return out;
}

} // namespace

CRPC_TEST(docs_lint_one_click_scripts_accept_the_same_switches) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    struct Pair {
        const char* ps;
        const char* sh;
    };
    const Pair pairs[] = {{"scripts/build.ps1", "scripts/build.sh"},
                          {"scripts/test.ps1", "scripts/test.sh"}};
    std::size_t compared = 0;
    bool bad = false;
    for (const Pair& pair : pairs) {
        const fs::path ps_path = root / pair.ps;
        const fs::path sh_path = root / pair.sh;
        CRPC_CHECK(fs::exists(ps_path));
        CRPC_CHECK(fs::exists(sh_path));
        if (!fs::exists(ps_path) || !fs::exists(sh_path)) {
            continue;
        }
        const std::vector<std::string> ps = ps_switches(read_text(ps_path));
        const std::vector<std::string> sh = sh_switches(read_text(sh_path));
        // Both sides must contribute, or the parser is broken and every
        // comparison below is vacuous.
        CRPC_CHECK(!ps.empty());
        CRPC_CHECK(!sh.empty());
        for (const std::string& name : ps) {
            ++compared;
            bool found = false;
            for (const std::string& candidate : sh) {
                if (candidate == name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::printf("  %s accepts --%s, %s does not\n", pair.ps,
                            name.c_str(), pair.sh);
                bad = true;
            }
        }
    }
    CRPC_CHECK(compared >= 6);
    CRPC_CHECK_FALSE(bad);
}
// --- 10. A shell script's --help must list every flag it accepts -----------
//
// The usage block is the only documentation these scripts have (there is no
// man page), and it drifted twice: `--examples` and `--no-tests` were added to
// the case statement while the usage text still listed seven flags, so
// `--help` was actively misleading about the two shapes CI depends on.
//
// The check is textual on purpose. It compares the flags parsed out of the case
// statement against those named in the leading comment block; both come from the
// same file, so it cannot pass by inspecting a different copy.

namespace {

// Flags in the `while [[ $# ... case "$1" in` table, e.g. `--examples)`.
std::vector<std::string> sh_flag_table(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        std::size_t pos = line.find_first_not_of(" \t");
        if (pos == std::string::npos || line[pos] != '-') {
            continue;
        }
        const std::size_t close = line.find(')', pos);
        // A case arm, not a stray dash in prose: `--flag) ... ;;`.
        if (close == std::string::npos ||
            line.find(";;", close) == std::string::npos) {
            continue;
        }
        std::string name = line.substr(pos, close - pos);
        const std::size_t bars = name.rfind('|');
        if (bars != std::string::npos) {
            name = name.substr(bars + 1);
        }
        while (!name.empty() && name.front() == '-') {
            name.erase(name.begin());
        }
        // Flags whose value is a whole word ("--arch) ARCH=$2") are skipped:
        // only the documentation of boolean switches is being checked here,
        // because those are the ones that were missing.
        if (name.empty() || name.find(' ') != std::string::npos ||
            name.find('{') != std::string::npos) {
            continue;
        }
        // `--help` documents itself; every usage block would have to name it
        // and none do, which says nothing about whether a real flag is missing.
        if (name == "help" || name == "h") {
            continue;
        }
        out.push_back(name);
    }
    return out;
}

// The leading block of `#` lines, i.e. what --help prints.
std::string leading_comment_block(const std::string& text) {
    std::istringstream stream(text);
    std::string line;
    std::string block;
    bool started = false;
    while (std::getline(stream, line)) {
        std::size_t pos = line.find_first_not_of(" \t");
        if (pos != std::string::npos && line[pos] == '#') {
            started = true;
            block += line;
            block += '\n';
            continue;
        }
        if (started) {
            break;
        }
    }
    return block;
}

} // namespace

CRPC_TEST(docs_lint_shell_usage_lists_every_flag) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const char* const scripts[] = {"scripts/build.sh", "scripts/test.sh"};
    std::size_t compared = 0;
    bool bad = false;
    for (const char* rel : scripts) {
        const fs::path path = root / rel;
        CRPC_CHECK(fs::exists(path));
        if (!fs::exists(path)) {
            continue;
        }
        const std::string text = read_text(path);
        const std::vector<std::string> flags = sh_flag_table(text);
        const std::string usage = leading_comment_block(text);
        CRPC_CHECK(!flags.empty());
        CRPC_CHECK(!usage.empty());
        for (const std::string& flag : flags) {
            ++compared;
            if (usage.find("--" + flag) == std::string::npos) {
                std::printf("  %s accepts --%s but its usage block does not "
                            "list it\n", rel, flag.c_str());
                bad = true;
            }
        }
    }
    // Two scripts x (clean, shared, bench, examples, no-tests) at least.
    CRPC_CHECK(compared >= 10);
    CRPC_CHECK_FALSE(bad);
}
// --- 11. The usage block in the docs must match the scripts -----------------
//
// "One-click scripts" lists the full command line for both script families, in
// both languages. That copy lagged the scripts twice: `-Examples`/`-NoTests`
// were added to the scripts and never appeared here, and `-Bench` was never
// listed at all. A reader following section 5 would not learn that the
// examples-only configuration -- the one CI uses -- exists.
//
// The check is scoped to the fenced block that contains `<script> [`, i.e. the
// usage line itself. Matching anywhere in the document would pass on a prose
// mention elsewhere, which is the failure mode the last two rounds were about.

namespace {

// Every fenced ``` block in `text`, in order.
std::vector<std::string> fenced_blocks(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    std::string line;
    std::string current;
    bool inside = false;
    while (std::getline(stream, line)) {
        if (line.rfind("```", 0) == 0) {
            if (inside) {
                out.push_back(current);
                current.clear();
            }
            inside = !inside;
            continue;
        }
        if (inside) {
            current += line;
            current += '\n';
        }
    }
    return out;
}

// The usage *lines* for `script`, joined. A bare mention is not enough: the
// document also contains example invocations in earlier blocks, so the needle is
// a line where the script name is followed by a '[' bracket.
//
// The result is the script's own lines only, not the whole block. Both scripts
// of one family share a block, so a block-wide search would let `test.sh`'s
// `[--examples]` satisfy the check for `build.sh` -- exactly the false green the
// mutation test produced on the first version of this check.
std::string usage_lines_for(const std::string& doc, const std::string& script) {
    std::string out;
    for (const std::string& block : fenced_blocks(doc)) {
        std::istringstream stream(block);
        std::string line;
        bool collecting = false;
        while (std::getline(stream, line)) {
            const std::size_t at = line.find(script);
            bool is_start = false;
            if (at != std::string::npos) {
                std::size_t pos = at + script.size();
                while (pos < line.size() &&
                       (line[pos] == ' ' || line[pos] == '\t')) {
                    ++pos;
                }
                is_start = pos < line.size() && line[pos] == '[';
            }
            if (is_start) {
                collecting = true;
            } else if (collecting) {
                // A continuation line is indented and starts with a bracket; the
                // next script name or the closing fence ends this entry.
                std::size_t pos = line.find_first_not_of(" \t");
                if (pos == std::string::npos) {
                    break;
                }
                if (line[pos] != '[' && line.find("./scripts/") == std::string::npos) {
                    break;
                }
                if (line.find("./scripts/") != std::string::npos) {
                    break;
                }
            }
            if (collecting) {
                out += line;
                out += '\n';
            }
        }
    }
    return out;
}

} // namespace

CRPC_TEST(docs_lint_documented_usage_matches_the_scripts) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    struct Case {
        const char* doc;
        const char* script;
        bool posix;
    };
    const Case cases[] = {
        {"docs/building.md", "build.ps1", false},
        {"docs/building.md", "test.ps1", false},
        {"docs/building.md", "build.sh", true},
        {"docs/building.md", "test.sh", true},
        {"docs/building.zh-CN.md", "build.ps1", false},
        {"docs/building.zh-CN.md", "test.ps1", false},
        {"docs/building.zh-CN.md", "build.sh", true},
        {"docs/building.zh-CN.md", "test.sh", true},
    };
    std::size_t compared = 0;
    bool bad = false;
    for (const Case& c : cases) {
        const fs::path doc_path = root / c.doc;
        const fs::path script_path = root / "scripts" / c.script;
        CRPC_CHECK(fs::exists(doc_path));
        CRPC_CHECK(fs::exists(script_path));
        if (!fs::exists(doc_path) || !fs::exists(script_path)) {
            continue;
        }
        const std::string block = usage_lines_for(read_text(doc_path), c.script);
        if (block.empty()) {
            std::printf("  %s has no usage block for %s\n", c.doc, c.script);
            bad = true;
            continue;
        }
        const std::string script_text = read_text(script_path);
        const std::vector<std::string> names =
            c.posix ? sh_switches(script_text) : ps_switches(script_text);
        CRPC_CHECK(!names.empty());
        for (const std::string& name : names) {
            // The names are compared compacted (no '-' or '_'), so the documented
            // spelling has to be recovered from the script text: `--build-root`
            // compacts to `buildroot` and cannot be re-spelled from it.
            if (name == "help" || name == "h") {
                continue;  // documents itself; no usage block names it
            }
            const std::string documented =
                c.posix ? ("--" + name) : ("-" + name);
            std::string haystack = block;
            for (char& ch : haystack) {
                if (ch == '-' || ch == '_') {
                    ch = ' ';
                }
            }
            std::string needle = documented;
            for (char& ch : needle) {
                if (ch == '-' || ch == '_') {
                    ch = ' ';
                }
            }
            // Compare on the compacted forms on both sides.
            std::string compact_block;
            for (char ch : block) {
                if (ch != '-' && ch != '_' && ch != ' ') {
                    compact_block.push_back(
                        static_cast<char>(std::tolower(
                            static_cast<unsigned char>(ch))));
                }
            }
            ++compared;
            if (compact_block.find(name) == std::string::npos) {
                std::printf("  %s: %s usage block does not list %s\n", c.doc,
                            c.script, documented.c_str());
                bad = true;
            }
        }
    }
    // 8 case pairs x at least 4 switches each.
    CRPC_CHECK(compared >= 30);
    CRPC_CHECK_FALSE(bad);
}
// --- 12. A documented benchmark file must exist ----------------------------
//
// docs/performance.md sections 2 and 4 describe the benchmark plan file by
// file (`bench/bench_redis.cpp`, `bench/bench_rpc_matrix.cpp`, ...). Two of
// those entries, `bench/bench_http.cpp` and `bench/bench_codec.cpp`, were
// never written, so the document promised coverage that did not exist -- and
// the codec numbers quoted elsewhere in the same document come from
// `bench/bench_core.cpp`, not from either of them.
//
// A planned-but-absent benchmark may legitimately be discussed, so the rule is
// narrow: naming a `bench/*.cpp` file is only allowed if the file exists,
// unless the sentence also says it is not implemented. That keeps the gap
// visible (which is the point) while forbidding the "listed as if it exists"
// form the document had.

CRPC_TEST(docs_lint_bench_claims_match_the_bench_directory) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const fs::path bench_dir = root / "bench";
    CRPC_CHECK(fs::exists(bench_dir));
    if (!fs::exists(bench_dir)) {
        return;
    }

    std::set<std::string> present;
    for (const auto& entry : fs::directory_iterator(bench_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        present.insert(entry.path().filename().generic_string());
    }
    CRPC_CHECK(present.size() >= 5);

    const char* const docs[] = {"docs/performance.md",
                                "docs/performance.zh-CN.md"};
    std::size_t checked = 0;
    bool bad = false;
    for (const char* rel : docs) {
        const fs::path path = root / rel;
        CRPC_CHECK(fs::exists(path));
        if (!fs::exists(path)) {
            continue;
        }
        const std::string text = read_text(path);
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            std::size_t pos = 0;
            while ((pos = line.find("bench/", pos)) != std::string::npos) {
                const std::size_t end = line.find(".cpp", pos);
                if (end == std::string::npos) {
                    break;
                }
                const std::string file =
                    line.substr(pos + 6, end - pos - 6) + ".cpp";
                pos = end + 4;
                if (present.count(file) != 0) {
                    ++checked;
                    continue;
                }
                // Absent: only acceptable when the line admits it.
                const bool admitted =
                    line.find("not implemented") != std::string::npos ||
                    line.find("尚未实现") != std::string::npos ||
                    line.find("未实现") != std::string::npos;
                if (!admitted) {
                    std::printf("  %s names bench/%s, which does not exist, "
                                "without saying so\n", rel, file.c_str());
                    bad = true;
                }
            }
        }
    }
    // The two documents name well over a dozen real files between them.
    CRPC_CHECK(checked >= 8);
    CRPC_CHECK_FALSE(bad);
}

// --- 13. Sample output in examples/README must match what the program prints
//
// Both READMEs quote a `soak:` transcript. It went stale twice without anyone
// noticing: the sample kept `connect p90 = 14.71 ms` after that defect was
// fixed (a6d92fd), and it kept the pre-fix `connect-avg=` after the summary
// gained `connect_failures=` / `worst_connect_ms=` (0f80ace) and the STATS line
// gained `active=` (c98ec20). A sample nobody can reproduce is worse than no
// sample: it teaches the reader that a 15 ms connect tail is normal.
//
// The check is deliberately narrow -- it looks only at lines that start with
// `soak: requests=` -- so prose may still discuss the old numbers, which it
// does on purpose to explain why they changed.
//
// It cannot compare values (machine dependent), only that the fields the
// program actually prints are present, and that the one value known to be a
// fixed defect is not presented as current.
CRPC_TEST(docs_lint_example_output_matches_the_program) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const char* const docs[] = {"examples/README.md",
                                "examples/README.zh-CN.md"};
    std::size_t checked = 0;
    bool bad = false;
    for (const char* rel : docs) {
        const fs::path path = root / rel;
        CRPC_CHECK(fs::exists(path));
        if (!fs::exists(path)) {
            continue;
        }
        std::istringstream stream(read_text(path));
        std::string line;
        while (std::getline(stream, line)) {
            if (line.rfind("soak: requests=", 0) != 0) {
                continue;
            }
            ++checked;
            // Fields the program prints today (examples/deploy_check.cpp).
            for (const char* field : {"failures=", "connect_failures=",
                                      "worst_connect_ms=", "handles ",
                                      "connect-ms p50=", "p99="}) {
                if (line.find(field) == std::string::npos) {
                    std::printf("  %s: soak sample is missing %s\n", rel,
                                field);
                    bad = true;
                }
            }
            // A value that only existed while a fixed defect was present.
            // It is fine in prose, not in the line presented as output.
            if (line.find("14.71") != std::string::npos) {
                std::printf("  %s: soak sample still shows the fixed "
                            "connect tail (14.71 ms)\n", rel);
                bad = true;
            }
        }
    }
    CRPC_CHECK(checked >= 2);
    CRPC_CHECK_FALSE(bad);
}

// --- 17. A test that executes nothing must say so ---------------------------
//
// A translation unit whose only main is `int main() { return 0; }` still
// registers with CTest and reports "Passed 0.00 sec". Nothing distinguishes
// that from a real pass, so a sanitizer job or a platform matrix looks like it
// covers code it never executed. This repository has already recorded that
// mistake once, and it was live again: `test_rpc_response_cpp20` was a no-op
// in *every* configuration that existed, because no build script or CI job
// passed `-DCMAKE_CXX_STANDARD=20`, and its four cases had never run.
//
// The rule: the no-op path must print something. It stays green (the test list
// is meant to be identical across platforms and standards), but a reader -- or
// a log -- can tell that zero work happened.
namespace {

// True when `pos` sits inside a comment or a string/char literal.
//
// The first version of this guard matched raw text, so it counted *its own
// explanatory comment* -- which quotes `int main() { return 0; }` -- as a
// seventh no-op. That inflated `noops` and, worse, showed the announcement
// check was file-wide: any file that merely mentioned another test's SKIP line
// would have been accepted even if its own main printed nothing. Both are the
// same mistake, so both are fixed here: skip matches that are not code, and
// require the announcement inside the no-op body itself.
bool in_comment_or_literal(const std::string& text, std::size_t pos) {
    bool line_comment = false;
    bool block_comment = false;
    bool in_string = false;
    bool in_char = false;
    for (std::size_t i = 0; i < pos && i < text.size(); ++i) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        if (line_comment) {
            if (c == '\n') {
                line_comment = false;
            }
        } else if (block_comment) {
            if (c == '*' && next == '/') {
                block_comment = false;
                ++i;
            }
        } else if (in_string) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (in_char) {
            if (c == '\\') {
                ++i;
            } else if (c == '\'') {
                in_char = false;
            }
        } else if (c == '/' && next == '/') {
            line_comment = true;
            ++i;
        } else if (c == '/' && next == '*') {
            block_comment = true;
            ++i;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '\'') {
            in_char = true;
        }
    }
    return line_comment || block_comment || in_string || in_char;
}

} // namespace

CRPC_TEST(docs_lint_noop_tests_announce_themselves) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const fs::path tests_dir = root / "tests";
    CRPC_CHECK(fs::exists(tests_dir));
    if (!fs::exists(tests_dir)) {
        return;
    }

    std::size_t noops = 0;
    bool bad = false;
    for (const auto& entry : fs::directory_iterator(tests_dir)) {
        if (!entry.is_regular_file() ||
            entry.path().extension() != ".cpp") {
            continue;
        }
        const std::string text = read_text(entry.path());
        // The trivial main, allowing for any amount of whitespace.
        std::size_t pos = 0;
        while ((pos = text.find("int main()", pos)) != std::string::npos) {
            // This file documents the mistake by quoting the shape it forbids;
            // a match inside a comment is prose, not a main.
            if (in_comment_or_literal(text, pos)) {
                pos += 1;
                continue;
            }
            const std::size_t open = text.find('{', pos);
            if (open == std::string::npos) {
                break;
            }
            const std::size_t close = text.find('}', open);
            if (close == std::string::npos) {
                break;
            }
            std::string body = text.substr(open + 1, close - open - 1);
            body.erase(std::remove_if(body.begin(), body.end(),
                                     [](unsigned char c) {
                                         return std::isspace(c) != 0;
                                     }),
                       body.end());
            pos = close + 1;
            // A main that runs no test case: it never calls the harness
            // (`CRPC_`) and just returns success. Announcing itself with a
            // printf is allowed and expected; running cases is not.
            if (body.find("CRPC_") != std::string::npos ||
                body.find("return0") == std::string::npos) {
                continue;
            }
            ++noops;
            // The explanation must be this main's own output, not any
            // "SKIP crpc_test_" somewhere else in the file: a file-wide search
            // would accept a silent main as long as a sibling printed.
            if (body.find("SKIPcrpc_test_") == std::string::npos) {
                std::printf(
                    "  %s has a no-op main that prints no reason\n",
                    entry.path().filename().generic_string().c_str());
                bad = true;
            }
        }
    }
    // The suite does have platform/standard-gated binaries; if this drops to
    // zero the scan stopped working and the check below would be vacuous.
    CRPC_CHECK(noops >= 4);
    CRPC_CHECK_FALSE(bad);
}

namespace {

// The text of the section that starts at `heading`, up to the next "## ".
std::string matrix_section(const std::string& text, const std::string& heading) {
    const std::size_t start = text.find(heading);
    if (start == std::string::npos) {
        return std::string();
    }
    const std::size_t end = text.find("\n## ", start + 1);
    return text.substr(start, end == std::string::npos ? std::string::npos
                                                       : end - start);
}

// Whole-word search: `lock` must not match inside "blocking", nor `net` inside
// "network". Backticks are not required, so the existing rows keep working.
bool contains_word(const std::string& haystack, const std::string& word) {
    const auto is_word_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    for (std::size_t pos = haystack.find(word); pos != std::string::npos;
         pos = haystack.find(word, pos + 1)) {
        const bool left_ok = pos == 0 || !is_word_char(haystack[pos - 1]);
        const std::size_t after = pos + word.size();
        const bool right_ok =
            after >= haystack.size() || !is_word_char(haystack[after]);
        if (left_ok && right_ok) {
            return true;
        }
    }
    return false;
}

} // namespace

// --- 13. The architecture status matrix must cover every module --------------
//
// docs/README.md says: "The status matrix in document 2 (architecture) is the
// single authoritative per-module status record." It was not. Section 19 listed
// 16 areas -- all of the framework core -- and omitted every adapter subsystem:
// db (MySQL, MongoDB, TDS, ODBC, HBase, TiDB, Oracle), fs, ipc, mq (AMQP, MSMQ),
// memcached, crypto, algorithm, game, memory, pattern, async and ws. The Chinese
// version was shorter still.
//
// The check is scoped to section 19 rather than the whole file: a module named
// once in passing prose is not "in the status record", and the point of the
// claim is that a reader can look up any module and find its status plus the
// test that backs it.
CRPC_TEST(docs_lint_architecture_matrix_covers_every_module) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const fs::path include_dir = root / "include" / "crpc";
    CRPC_CHECK(fs::exists(include_dir));
    if (!fs::exists(include_dir)) {
        return;
    }

    std::vector<std::string> modules;
    for (const auto& entry : fs::directory_iterator(include_dir)) {
        if (!entry.is_directory()) {
            continue;
        }
        modules.push_back(entry.path().filename().generic_string());
    }
    std::sort(modules.begin(), modules.end());
    // A scan that found nothing would make every assertion below vacuous.
    CRPC_CHECK(modules.size() >= 20);

    std::size_t compared = 0;
    bool bad = false;

    // English: every module directory must be named in the status matrix. The
    // English rows are labelled with the module names, so this is exact.
    {
        const fs::path path = root / "docs" / "architecture.md";
        CRPC_CHECK(fs::exists(path));
        if (fs::exists(path)) {
            const std::string text = read_text(path);
            const std::string section = matrix_section(text, "## 19. Status matrix");
            CRPC_CHECK(!section.empty());
            for (const std::string& module : modules) {
                ++compared;
                if (!contains_word(section, module)) {
                    std::printf("  architecture.md section 19 does not cover "
                                "module %s\n", module.c_str());
                    bad = true;
                }
            }
            // Evidence parity: every suite the English matrix cites must be cited
            // by the Chinese one too. The Chinese matrix labels areas in Chinese
            // ("可观测性", "韧性原语"), so requiring the English module names there
            // would need a translation table; requiring the same *evidence* is
            // both checkable and the thing that actually matters. It is also what
            // the previous drift broke: the Chinese matrix was a strict subset.
            const fs::path zh_path = root / "docs" / "architecture.zh-CN.md";
            CRPC_CHECK(fs::exists(zh_path));
            if (fs::exists(zh_path)) {
                const std::string zh_text = read_text(zh_path);
                const std::string zh_section =
                    matrix_section(zh_text, "## 19. ");
                CRPC_CHECK(!zh_section.empty());
                const std::string needle = "`test_";
                for (std::size_t pos = section.find(needle);
                     pos != std::string::npos; pos = section.find(needle, pos + 1)) {
                    const std::size_t close = section.find('`', pos + 1);
                    if (close == std::string::npos) {
                        break;
                    }
                    const std::string evidence = section.substr(pos, close - pos + 1);
                    ++compared;
                    if (zh_section.find(evidence) == std::string::npos) {
                        std::printf("  architecture.zh-CN.md section 19 does not "
                                    "cite %s (present in the English matrix)\n",
                                    evidence.c_str());
                        bad = true;
                    }
                }
            }
        }
    }
    // Two documents x >= 20 modules.
    CRPC_CHECK(compared >= 40);
    CRPC_CHECK_FALSE(bad);
}

// --- 14. A fixture must destroy its connections before their sources --------
//
// This is a source-level gate, and it exists because the defect it catches is
// invisible to every other check: a test fixture that owns both a
// `unique_ptr<connection>` and the `io::source` bound to it must declare the
// SOURCE first, because members destruct in reverse declaration order and a
// connection's destructor waits for an in-flight delivery that runs into the
// source. Declaring the connection first frees the source while the pump can
// still call into it.
//
// That is not theoretical: `crpc_test_io_iocp_uaf` crashed with 0xC0000005 at
// roughly 1 in 30 x86 Debug runs, faulting inside the pump's `s->on_data(...)`
// with the vtable pointer replaced by the MSVC debug heap's freed-block fill
// (0xDDDDDDDD), while the main thread sat in `~uaf_server` destroying the
// connection vector. A cdb stack from the minidump pinned it in one look --
// but only after the fixture had been crashing intermittently for a day.
//
// `test_io_iocp_uaf_nested_push.cpp` already carried a comment describing this
// exact trap; a comment does not stop the next fixture from repeating it.
CRPC_TEST(tests_owning_connections_destroy_before_their_sources) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const fs::path tests_dir = root / "tests";
    CRPC_CHECK(fs::exists(tests_dir));
    if (!fs::exists(tests_dir)) {
        return;
    }

    // Leading whitespace of a line, so nesting can be judged without a parser.
    const auto indent_of = [](const std::string& line) {
        std::size_t n = 0;
        while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) {
            ++n;
        }
        return n;
    };
    const auto trimmed = [](const std::string& line) {
        const std::size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) {
            return std::string();
        }
        const std::size_t e = line.find_last_not_of(" \t\r");
        return line.substr(b, e - b + 1);
    };

    std::size_t checked = 0;
    bool bad = false;
    for (const auto& entry : fs::directory_iterator(tests_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().generic_string();
        if (name.rfind("test_", 0) != 0 || entry.path().extension() != ".cpp") {
            continue;
        }
        const std::string rel = "tests/" + name;
        const std::string text = read_text(entry.path());
        std::vector<std::string> lines;
        {
            std::size_t start = 0;
            while (start <= text.size()) {
                const std::size_t end = text.find('\n', start);
                if (end == std::string::npos) {
                    lines.push_back(text.substr(start));
                    break;
                }
                lines.push_back(text.substr(start, end - start));
                start = end + 1;
            }
        }

        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string head = trimmed(lines[i]);
            if (head.rfind("struct ", 0) != 0 && head.rfind("class ", 0) != 0) {
                continue;
            }
            if (head.find('{') == std::string::npos) {
                continue;
            }
            const std::size_t struct_indent = indent_of(lines[i]);
            // Body ends at the first line whose leading '}' sits at or left of
            // the struct's own indentation.
            std::size_t conn_at = std::string::npos;
            std::size_t source_at = std::string::npos;
            for (std::size_t j = i + 1; j < lines.size(); ++j) {
                const std::string t = trimmed(lines[j]);
                if (t.empty() || t.rfind("//", 0) == 0) {
                    continue;
                }
                if (t[0] == '}' && indent_of(lines[j]) <= struct_indent) {
                    break;
                }
                // Only direct members: same indentation as the struct's braces.
                if (indent_of(lines[j]) != struct_indent + 4) {
                    continue;
                }
                if (t.back() != ';' || t.find('(') != std::string::npos) {
                    continue;
                }
                const bool owns_connection =
                    t.find("unique_ptr<connection") != std::string::npos ||
                    t.find("unique_ptr<io::connection") != std::string::npos;
                // A source member is either a `unique_ptr<...source>` or the
                // source object itself (`echo_source src;`). Both are destroyed
                // by this fixture, so both must outlive the connection.
                const bool owns_source =
                    t.find("source") != std::string::npos;
                if (owns_connection && conn_at == std::string::npos) {
                    conn_at = j;
                }
                if (owns_source && source_at == std::string::npos) {
                    source_at = j;
                }
            }
            if (conn_at == std::string::npos || source_at == std::string::npos) {
                continue;
            }
            ++checked;
            if (source_at > conn_at) {
                std::printf("  %s:%zu declares the connection before the source "
                            "it is bound to; the source must be declared first "
                            "(members destruct in reverse order)\n",
                            rel.c_str(), i + 1);
                bad = true;
            }
        }
    }
    // Four fixtures own both right now (this file's set includes the two IOCP
    // bundles, the UAF server and the nested-push server). Fewer means the scan
    // stopped matching and the assertions above became vacuous.
    CRPC_CHECK(checked >= 4);
    CRPC_CHECK_FALSE(bad);
}

CRPC_TEST_MAIN()
