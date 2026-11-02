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
CRPC_TEST_MAIN()
