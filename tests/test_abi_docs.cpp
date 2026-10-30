// CRPC -- machine checks for the ABI policy documents.
//
// docs/abi.md and docs/abi.zh-CN.md are normative: they state what the library
// guarantees at the binary level. They had drifted from the code in ways that
// only a reader would notice (a wrong namespace for `connect_options`, a
// Chinese text claiming layout assertions that do not exist, a CI job that was
// never written). This test turns those facts into assertions so the drift
// cannot come back.
//
// The test reads the documents as text. It does not parse Markdown: each check
// is a substring assertion on a sentence the policy must (or must not) contain.
// That keeps the check stable when prose is edited, while still failing when a
// claim flips from "not implemented" to "implemented".
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <crpc/rpc/client.hpp>

#include "test_framework.hpp"

namespace {

namespace fs = std::filesystem;

// Resolve the repository root from this source file. CMake compiles tests with
// absolute paths, so `__FILE__` already points at <root>/tests/test_abi_docs.cpp
// in every supported generator. If that ever changes, fall back to walking up
// from the working directory looking for docs/abi.md.
fs::path repo_root() {
    fs::path from_source = fs::path(__FILE__).parent_path().parent_path();
    if (fs::exists(from_source / "docs" / "abi.md")) {
        return from_source;
    }
    fs::path probe = fs::current_path();
    for (int depth = 0; depth < 8 && !probe.empty(); ++depth) {
        if (fs::exists(probe / "docs" / "abi.md")) {
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
    CRPC_CHECK(input.good());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool has(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

// --- Phrase matching --------------------------------------------------------
//
// The documents are prose wrapped at 80 columns, so a phrase can be split
// across a line break, and the break can fall inside a word. Two matchers are
// needed because the two kinds of assertion fail differently:
//
//   * a required phrase must not be missed. It is compiled to a pattern where
//     every run of whitespace in the needle becomes `\s*`, so `mem\nber`
//     matches `member`. The last character gets a `(?![\w])` guard when it is a
//     word character, so `member count` does not match `member counter`.
//   * a forbidden phrase must not be missed either, but a false alarm only
//     makes the test fail loudly, while a miss lets the wrong claim back in.
//     For that one the comparison strips all whitespace, which is the strictest
//     form: it catches the phrase however it is wrapped.

std::string strip_space(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Compile a needle into a regex. Whitespace is allowed *anywhere* between two
// needle characters (`\s*`), because the document wraps at 80 columns and the
// break can fall inside a word the needle has no space in. Every needle
// character is matched literally, and a trailing word character gets a
// `(?![\w])` guard so `member count` does not match `member counter`.
std::string phrase_pattern(const char* needle) {
    const std::string n(needle);
    std::string re;
    re.reserve(n.size() * 4);
    bool first = true;
    for (char c : n) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        if (!first) {
            re += "\\s*";
        }
        first = false;
        if (is_word_char(c)) {
            re.push_back(c);
        } else {
            static const char* special = "\\^$.|?*+()[]{}";
            if (std::strchr(special, c) != nullptr) {
                re.push_back('\\');
            }
            re.push_back(c);
        }
    }
    if (!n.empty() && is_word_char(n.back())) {
        re += "(?![\\w])";
    }
    return re;
}

bool has_phrase(const std::string& text, const char* needle) {
    try {
        return std::regex_search(text, std::regex(phrase_pattern(needle)));
    } catch (const std::regex_error&) {
        CRPC_CHECK(false);
        return false;
    }
}

bool forbidden_phrase(const std::string& text, const char* needle) {
    return strip_space(text).find(strip_space(needle)) != std::string::npos;
}

} // namespace

CRPC_TEST(abi_docs_english_names_the_real_namespace) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.md");
    CRPC_CHECK(!text.empty());

    // `connect_options` lives in `crpc`, not in `crpc::io` (include/crpc/io.hpp).
    CRPC_CHECK_FALSE(has(text, "io::connect_options"));
    CRPC_CHECK(has(text, "crpc::connect_options"));
}

CRPC_TEST(abi_docs_english_says_assertions_are_not_implemented) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.md");

    // The headers carry no per-struct sizeof/alignof assertions; src/abi_check.cpp
    // is a separate translation unit, not a header assertion.
    CRPC_CHECK(has(text, "do **not** currently enforce"));
    CRPC_CHECK(has(text, "planned, not implemented"));

    // No size-tag pattern exists.
    CRPC_CHECK(has(text, "There is no size-tag pattern today"));
}

CRPC_TEST(abi_docs_english_describes_the_export_gate) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.md");

    // The gate is real: the script exists and both test runners invoke it.
    CRPC_CHECK_FALSE(read_text(root / "scripts" / "export_gate.py").empty());
    const std::string ps1 = read_text(root / "scripts" / "test.ps1");
    CRPC_CHECK(has(ps1, "export_gate.py"));
    const std::string sh = read_text(root / "scripts" / "test.sh");
    CRPC_CHECK(has(sh, "export_gate.py"));

    // The document names the gate and its baseline file.
    CRPC_CHECK(has(text, "scripts/export_gate.py"));
    CRPC_CHECK(has(text, "scripts/export_baseline.txt"));
    CRPC_CHECK(has_phrase(text, "is run by"));

    // The stale "no checker" claim must not return.
    CRPC_CHECK_FALSE(forbidden_phrase(text, "No ABI checker is implemented"));
    CRPC_CHECK_FALSE(forbidden_phrase(text, "CI does not compare exported symbols"));
}

CRPC_TEST(abi_docs_chinese_does_not_claim_header_assertions) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.zh-CN.md");
    CRPC_CHECK(!text.empty());

    // The old text claimed the header already had sizeof/alignof static_asserts.
    CRPC_CHECK_FALSE(has(text, "在头文件中有 `sizeof`"));
    CRPC_CHECK_FALSE(has(text, "头文件中有 `sizeof`"));
    // ...and the replacement must say the enforcement is still missing.
    CRPC_CHECK(has(text, "尚未"));
    CRPC_CHECK(has(text, "布局断言"));
}

CRPC_TEST(abi_docs_chinese_does_not_claim_size_tag_pattern) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.zh-CN.md");

    // The old text showed an adopted `struct_size` field as if it existed.
    CRPC_CHECK_FALSE(has(text, "struct_size;"));
    CRPC_CHECK_FALSE(has(text, "选项结构体采用尺寸标记模式"));
    CRPC_CHECK(has(text, "当前没有 size-tag 模式"));
}

CRPC_TEST(abi_docs_chinese_describes_the_export_gate) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.zh-CN.md");

    // The gate is real: the script exists and both test runners invoke it.
    CRPC_CHECK_FALSE(read_text(root / "scripts" / "export_gate.py").empty());
    const std::string ps1 = read_text(root / "scripts" / "test.ps1");
    CRPC_CHECK(has(ps1, "export_gate.py"));
    const std::string sh = read_text(root / "scripts" / "test.sh");
    CRPC_CHECK(has(sh, "export_gate.py"));

    // The document names the gate and its baseline file.
    CRPC_CHECK(has(text, "scripts/export_gate.py"));
    CRPC_CHECK(has(text, "scripts/export_baseline.txt"));
    CRPC_CHECK(has(text, "scripts/test.ps1"));

    // The stale claims must not return.
    CRPC_CHECK_FALSE(forbidden_phrase(text, "未实现 ABI 检查器"));
    CRPC_CHECK_FALSE(forbidden_phrase(text, "均不存在"));
}

CRPC_TEST(abi_docs_chinese_is_clean_utf8) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.zh-CN.md");
    CRPC_CHECK(!text.empty());
    // U+FFFD means the file was decoded with the wrong encoding and re-encoded.
    CRPC_CHECK(text.find("\xEF\xBF\xBD") == std::string::npos);
    // Both language versions link to each other at the top.
    CRPC_CHECK(has(text, "abi.md"));
    const std::string english = read_text(root / "docs" / "abi.md");
    CRPC_CHECK(has(english, "abi.zh-CN.md"));
}

CRPC_TEST(abi_docs_english_describes_the_member_count_guard) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.md");

    // The member-count guard is the mechanism that catches a tail append on a
    // target where tail padding absorbs it (x86), so the document must name it.
    CRPC_CHECK(has_phrase(text, "member count"));
    // The x86 tail-padding blind spot of an absolute sizeof anchor must stay
    // documented; without it the member count looks redundant.
    CRPC_CHECK(has_phrase(text, "tail padding"));
    CRPC_CHECK(has_phrase(text, "blind to a tail append on x86"));

    // The positive claim that a tail append IS caught must be present, so that
    // flipping it to "is NOT caught" fails. A negative-only check cannot see
    // that rewrite, because the negated sentence no longer contains the
    // forbidden phrase.
    CRPC_CHECK(has_phrase(
        text, "A field appended at the end is caught by the member count"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "A field appended at the end is NOT caught by the member count"));

    // A structured binding binds one local name per member in declaration
    // order; it does not compare those names with the real member names. The
    // document once claimed a rename was caught by the member count, which is
    // false. Keep the corrected statement and reject the old one. Matching on
    // normalized whitespace means the old claim cannot return by re-wrapping.
    CRPC_CHECK_FALSE(forbidden_phrase(text, "a renamed member is caught"));
    CRPC_CHECK_FALSE(forbidden_phrase(text, "renamed member is caught by the relative"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "member is caught by the relative chains and the member counts"));
    CRPC_CHECK(has_phrase(text, "does **not** validate names"));
    CRPC_CHECK(has_phrase(text, "no relative chain"));

    // The only record with no relative chain is `rpc_error`. The four HTTP/2
    // value types do have CRPC_AFTER chains, so they must not be lumped in with
    // `rpc_error` as "no chain"; that would understate the coverage and a
    // reader would not know their renames are caught.
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "no relative chain (`rpc_error`, `status`, `http2"));
    CRPC_CHECK_FALSE(forbidden_phrase(text, "(`rpc_error`, `status`, `http2::*`)"));
    CRPC_CHECK(has_phrase(
        text, "The only cross-ABI record with no relative chain is `rpc_error`"));
    CRPC_CHECK(has_phrase(text, "**do** have `CRPC_AFTER` chains"));

    // A reorder does not change the member count, so the count cannot catch
    // one. The document must not claim it does, and must say that a reorder in
    // `rpc_error` is not caught.
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "and by the member count on every architecture"));
    CRPC_CHECK(has_phrase(text, "provides no coverage for a reorder at all"));
    CRPC_CHECK(has_phrase(
        text, "a reorder inside `rpc_error` is not caught"));

    // The remaining blind spot (member type changes in rpc_error/status) must
    // stay stated rather than silently dropped.
    CRPC_CHECK(has_phrase(text, "Member **type** changes"));
}

CRPC_TEST(abi_docs_chinese_matches_the_english_guard_boundaries) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.zh-CN.md");
    CRPC_CHECK(!text.empty());

    // Same three boundaries as the English document: member count, the x86
    // tail-padding blind spot, and the rename limitation.
    CRPC_CHECK(has_phrase(text, "成员数量"));
    CRPC_CHECK(has_phrase(text, "尾部"));
    CRPC_CHECK(has_phrase(text, "不校验名字"));
    CRPC_CHECK_FALSE(forbidden_phrase(text, "重命名成员：由相对成员链与成员数量"));

    // The only record with no relative chain is `rpc_error`; the HTTP/2 value
    // types are chained, so the Chinese text must not lump them together.
    CRPC_CHECK_FALSE(forbidden_phrase(text, "（`rpc_error`、`status`、`http2::*`）"));
    CRPC_CHECK(has_phrase(text, "唯一没有相对链的跨 ABI 记录是"));
    CRPC_CHECK(has_phrase(text, "**都有** `CRPC_AFTER` 链"));

    // A reorder does not change the member count; the text must say the count
    // gives no coverage for it and that `rpc_error` reorders are not caught.
    CRPC_CHECK_FALSE(forbidden_phrase(text, "以及所有架构上的成员数量捕获"));
    CRPC_CHECK(has_phrase(text, "对重排恒为盲"));
    CRPC_CHECK(has_phrase(text, "`rpc_error` 内部的重排不会被捕获"));

    // The rpc_error member-type limitation must be stated in both languages.
    CRPC_CHECK(has_phrase(text, "成员**类型**变更"));
}


// --- Cross-ABI record scan --------------------------------------------------
//
// The ABI documents used to carry a hand-written list of records that cross the
// ABI boundary without a compile-time layout guard. Every revision of that list
// missed at least one record, because a hand-written enumeration drifts as soon
// as a new record is added. The list is now derived from the code instead: this
// scanner reproduces the definition of "crosses the boundary" and the set of
// guarded records, and the tests below assert that the document matches the
// scan. Adding a cross-boundary record without documenting it fails the build.
//
// Names are compared fully qualified. Two different records are called
// `frame_header` (`::crpc::frame_header` in frame.hpp and
// `::crpc::http2::frame_header` in http2/frame.hpp); matching on the bare name
// would treat the guarded one as covering the unguarded one.

struct ScannedRecord {
    std::string qual;
    std::string file;
    std::string body;
    bool is_template = false;
};

struct ScannedSig {
    std::string text;
    std::vector<std::string> scope;
    bool is_api = false;
};

bool ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

std::string join_ns(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += "::";
        }
        out += parts[i];
    }
    return out;
}

std::vector<std::string> split_ns(const std::string& qual) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = qual.find("::", start);
        if (pos == std::string::npos) {
            out.push_back(qual.substr(start));
            break;
        }
        out.push_back(qual.substr(start, pos - start));
        start = pos + 2;
    }
    return out;
}

// Drop comments and string/char literals so their contents cannot be mistaken
// for declarations.
std::string strip_cpp(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "//") == 0) {
            const std::size_t end = text.find('\n', i);
            i = (end == std::string::npos) ? text.size() : end;
        } else if (text.compare(i, 2, "/*") == 0) {
            const std::size_t end = text.find("*/", i + 2);
            i = (end == std::string::npos) ? text.size() : end + 2;
        } else if (text[i] == '"' || text[i] == '\'') {
            const char quote = text[i++];
            while (i < text.size() && text[i] != quote) {
                if (text[i] == '\\') {
                    ++i;
                }
                ++i;
            }
            if (i < text.size()) {
                ++i;
            }
        } else {
            out.push_back(text[i++]);
        }
    }
    return out;
}

// Whole-token occurrence of `spelling` in `sig`.
bool has_spelling(const std::string& sig, const std::string& spelling) {
    if (spelling.empty()) {
        return false;
    }
    std::size_t pos = 0;
    while ((pos = sig.find(spelling, pos)) != std::string::npos) {
        const bool left_ok =
            pos == 0 || (!ident_char(sig[pos - 1]) && sig[pos - 1] != ':');
        const std::size_t end = pos + spelling.size();
        const bool right_ok =
            end >= sig.size() || (!ident_char(sig[end]) && sig[end] != ':');
        if (left_ok && right_ok) {
            return true;
        }
        ++pos;
    }
    return false;
}

// Whole-token occurrence that is not passed by reference or pointer and is not
// inside a template argument list.
bool has_spelling_by_value(const std::string& sig, const std::string& spelling) {
    if (spelling.empty()) {
        return false;
    }
    std::size_t pos = 0;
    while ((pos = sig.find(spelling, pos)) != std::string::npos) {
        const bool left_ok =
            pos == 0 || (!ident_char(sig[pos - 1]) && sig[pos - 1] != ':');
        const std::size_t end = pos + spelling.size();
        const bool right_ok =
            end >= sig.size() || (!ident_char(sig[end]) && sig[end] != ':');
        if (left_ok && right_ok) {
            std::size_t k = end;
            while (k < sig.size() && (sig[k] == ' ' || sig[k] == '\t' ||
                                      sig[k] == '\n' || sig[k] == '\r')) {
                ++k;
            }
            const bool direct =
                k >= sig.size() || (sig[k] != '&' && sig[k] != '*');
            bool in_template = false;
            int angle = 0;
            for (std::size_t b = pos; b > 0; --b) {
                const char ch = sig[b - 1];
                if (ch == '>') {
                    ++angle;
                } else if (ch == '<') {
                    if (angle == 0) {
                        in_template = true;
                        break;
                    }
                    --angle;
                }
            }
            if (direct && !in_template) {
                return true;
            }
        }
        ++pos;
    }
    return false;
}

// Track `namespace a::b {` scopes and record every struct/class definition.
void scan_definitions(const std::string& text, const std::string& file,
                      std::vector<ScannedRecord>& out) {
    std::vector<std::pair<int, std::string>> ns;
    int depth = 0;
    bool pending_template = false;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (ident_char(c)) {
            std::size_t j = i;
            while (j < text.size() && ident_char(text[j])) {
                ++j;
            }
            const std::string word = text.substr(i, j - i);

            if (word == "template") {
                std::size_t k = j;
                while (k < text.size() && text[k] != '<') {
                    ++k;
                }
                int angle = 0;
                for (; k < text.size(); ++k) {
                    if (text[k] == '<') {
                        ++angle;
                    } else if (text[k] == '>') {
                        --angle;
                        if (angle == 0) {
                            ++k;
                            break;
                        }
                    }
                }
                pending_template = true;
                i = k;
                continue;
            }

            if (word == "namespace") {
                std::size_t k = j;
                while (k < text.size() && (text[k] == ' ' || text[k] == '\t')) {
                    ++k;
                }
                std::string name;
                while (k < text.size() &&
                       (ident_char(text[k]) || text[k] == ':')) {
                    name.push_back(text[k]);
                    ++k;
                }
                const std::size_t brace = text.find('{', k);
                if (brace != std::string::npos) {
                    ++depth;
                    std::size_t start = 0;
                    for (;;) {
                        const std::size_t sep = name.find("::", start);
                        const std::string part =
                            (sep == std::string::npos)
                                ? name.substr(start)
                                : name.substr(start, sep - start);
                        if (!part.empty()) {
                            ns.emplace_back(depth, part);
                        }
                        if (sep == std::string::npos) {
                            break;
                        }
                        start = sep + 2;
                    }
                    i = brace + 1;
                    continue;
                }
                i = k;
                continue;
            }

            if (word == "struct" || word == "class") {
                std::size_t back = i;
                while (back > 0 && (text[back - 1] == ' ' ||
                                    text[back - 1] == '\t')) {
                    --back;
                }
                if (back >= 4 && text.compare(back - 4, 4, "enum") == 0) {
                    i = j;
                    continue;
                }
                std::size_t k = j;
                auto skip_space = [&] {
                    while (k < text.size() &&
                           (text[k] == ' ' || text[k] == '\t' ||
                            text[k] == '\n' || text[k] == '\r')) {
                        ++k;
                    }
                };
                skip_space();
                if (text.compare(k, 8, "CRPC_API") == 0) {
                    k += 8;
                    skip_space();
                }
                const std::size_t name_start = k;
                while (k < text.size() && ident_char(text[k])) {
                    ++k;
                }
                const std::string name = text.substr(name_start, k - name_start);
                const std::size_t brace = text.find('{', k);
                const std::size_t semi = text.find(';', k);
                if (brace != std::string::npos &&
                    (semi == std::string::npos || brace < semi) &&
                    !name.empty()) {
                    int body_depth = 0;
                    std::size_t end = brace;
                    for (; end < text.size(); ++end) {
                        if (text[end] == '{') {
                            ++body_depth;
                        } else if (text[end] == '}') {
                            --body_depth;
                            if (body_depth == 0) {
                                break;
                            }
                        }
                    }
                    std::vector<std::string> path;
                    for (const auto& entry : ns) {
                        path.push_back(entry.second);
                    }
                    ScannedRecord record;
                    record.qual =
                        path.empty() ? name : join_ns(path) + "::" + name;
                    record.file = file;
                    record.body = text.substr(brace, end - brace + 1);
                    record.is_template = pending_template;
                    out.push_back(std::move(record));
                    pending_template = false;
                    i = end + 1;
                    continue;
                }
                pending_template = false;
                i = k;
                continue;
            }
            i = j;
            continue;
        }
        if (c == '{') {
            ++depth;
            ++i;
            continue;
        }
        if (c == '}') {
            if (!ns.empty() && ns.back().first == depth) {
                ns.pop_back();
            }
            --depth;
            ++i;
            continue;
        }
        ++i;
    }
}

// Record the text of every declaration that mentions CRPC_API or virtual.
void scan_signatures(const std::string& text, std::vector<ScannedSig>& out) {
    std::vector<std::pair<int, std::string>> ns;
    int depth = 0;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (ident_char(c)) {
            std::size_t j = i;
            while (j < text.size() && ident_char(text[j])) {
                ++j;
            }
            const std::string word = text.substr(i, j - i);

            if (word == "namespace") {
                std::size_t k = j;
                while (k < text.size() && (text[k] == ' ' || text[k] == '\t')) {
                    ++k;
                }
                std::string name;
                while (k < text.size() &&
                       (ident_char(text[k]) || text[k] == ':')) {
                    name.push_back(text[k]);
                    ++k;
                }
                const std::size_t brace = text.find('{', k);
                if (brace != std::string::npos) {
                    ++depth;
                    std::size_t start = 0;
                    for (;;) {
                        const std::size_t sep = name.find("::", start);
                        const std::string part =
                            (sep == std::string::npos)
                                ? name.substr(start)
                                : name.substr(start, sep - start);
                        if (!part.empty()) {
                            ns.emplace_back(depth, part);
                        }
                        if (sep == std::string::npos) {
                            break;
                        }
                        start = sep + 2;
                    }
                    i = brace + 1;
                    continue;
                }
                i = k;
                continue;
            }

            if (word == "CRPC_API" || word == "virtual") {
                std::size_t start = i;
                while (start > 0 && text[start - 1] != ';' &&
                       text[start - 1] != '}' && text[start - 1] != '{') {
                    --start;
                }
                std::size_t end = i;
                int paren = 0;
                for (; end < text.size(); ++end) {
                    const char ch = text[end];
                    if (ch == '(' || ch == '[') {
                        ++paren;
                    } else if (ch == ')' || ch == ']') {
                        --paren;
                    } else if (paren <= 0 && (ch == ';' || ch == '{')) {
                        break;
                    }
                }
                ScannedSig sig;
                sig.text = text.substr(start, end - start);
                for (const auto& entry : ns) {
                    sig.scope.push_back(entry.second);
                }
                sig.is_api = (word == "CRPC_API");
                out.push_back(std::move(sig));
            }
            i = j;
            continue;
        }
        if (c == '{') {
            ++depth;
            ++i;
            continue;
        }
        if (c == '}') {
            if (!ns.empty() && ns.back().first == depth) {
                ns.pop_back();
            }
            --depth;
            ++i;
            continue;
        }
        ++i;
    }
}

// A plain aggregate record: no template, no pure virtual, no private or
// protected section, no user-declared constructor or destructor.
bool is_plain_record(const ScannedRecord& record) {
    if (record.is_template) {
        return false;
    }
    const std::string& body = record.body;
    if (body.find("private:") != std::string::npos ||
        body.find("protected:") != std::string::npos) {
        return false;
    }
    for (std::size_t k = 0; k + 3 < body.size(); ++k) {
        if (body.compare(k, 4, "= 0;") != 0) {
            continue;
        }
        std::size_t start = k;
        while (start > 0 && body[start - 1] != ';' && body[start - 1] != '{' &&
               body[start - 1] != '}') {
            --start;
        }
        const std::size_t virt = body.find("virtual", start);
        if (virt != std::string::npos && virt < k) {
            return false;
        }
    }
    for (std::size_t k = 0; k + record.qual.size() < body.size(); ++k) {
        const std::size_t bare_pos = record.qual.rfind("::");
        const std::string bare = (bare_pos == std::string::npos)
                                     ? record.qual
                                     : record.qual.substr(bare_pos + 2);
        if (body.compare(k, bare.size(), bare) != 0) {
            continue;
        }
        if (k > 0 && ident_char(body[k - 1])) {
            continue;
        }
        std::size_t m = k + bare.size();
        while (m < body.size() && (body[m] == ' ' || body[m] == '\t' ||
                                   body[m] == '\n' || body[m] == '\r')) {
            ++m;
        }
        if (m < body.size() && body[m] == '(') {
            return false;
        }
    }
    if (body.find("~" + record.qual.substr(record.qual.rfind("::") ==
                                                   std::string::npos
                                               ? 0
                                               : record.qual.rfind("::") + 2)) !=
        std::string::npos) {
        return false;
    }
    return true;
}

// Qualified names guarded in src/abi_check.cpp.
std::set<std::string> guarded_names(const fs::path& root) {
    const std::string src =
        strip_cpp(read_text(root / "src" / "abi_check.cpp"));
    static const char* const macros[] = {
        "CRPC_MEMBER_COUNT", "CRPC_ABS_SIZE", "CRPC_ABS_OFF", "CRPC_AFTER",
        "CRPC_STEP_SAME",    "CRPC_STEP_MIXED", "CRPC_STEP"};
    std::set<std::string> guarded;
    for (const char* macro : macros) {
        const std::string key = std::string(macro) + "(";
        std::size_t pos = 0;
        while ((pos = src.find(key, pos)) != std::string::npos) {
            std::size_t k = pos + key.size();
            while (k < src.size() && (src[k] == ' ' || src[k] == '\t' ||
                                      src[k] == '\n' || src[k] == '\r')) {
                ++k;
            }
            if (std::string(macro) == "CRPC_MEMBER_COUNT") {
                const std::size_t comma = src.find(',', k);
                k = (comma == std::string::npos) ? k : comma + 1;
                while (k < src.size() && (src[k] == ' ' || src[k] == '\t' ||
                                          src[k] == '\n' || src[k] == '\r')) {
                    ++k;
                }
            }
            std::size_t end = k;
            while (end < src.size() && (ident_char(src[end]) || src[end] == ':')) {
                ++end;
            }
            std::string name = src.substr(k, end - k);
            while (!name.empty() && name[0] == ':') {
                name.erase(name.begin());
            }
            if (!name.empty() && name != "tag" && name != "T") {
                if (name.rfind("crpc::", 0) != 0) {
                    name = "crpc::" + name;
                }
                guarded.insert(name);
            }
            pos = end;
        }
    }
    return guarded;
}

// Records that cross the ABI boundary and have no compile-time layout guard.
std::vector<std::string> unguarded_crossing_records(const fs::path& root,
                                                    std::vector<std::string>* in_scope) {
    std::vector<ScannedRecord> records;
    std::vector<ScannedSig> signatures;
    std::vector<fs::path> headers;
    for (const auto& entry :
         fs::recursive_directory_iterator(root / "include" / "crpc")) {
        if (entry.is_regular_file() && entry.path().extension() == ".hpp") {
            headers.push_back(entry.path());
        }
    }
    std::sort(headers.begin(), headers.end());
    for (const auto& path : headers) {
        const std::string text = strip_cpp(read_text(path));
        const std::string file = fs::relative(path, root).generic_string();
        scan_definitions(text, file, records);
        scan_signatures(text, signatures);
    }

    std::set<std::string> crossing;
    // `has_spelling` accepts a spelling only when the characters on both sides
    // of the match are neither identifier characters nor ':', so a spelling that
    // matches is exactly one maximal run of [ident_char | ':'] in the signature
    // text. Indexing those runs turns the scan below from
    // O(signatures x records) string searches into a handful of hash lookups per
    // record. The cross product was 24 s in a Debug x86 build, which is close
    // enough to the 30 s CTest timeout that a loaded machine failed the gate.
    std::unordered_map<std::string, std::vector<std::size_t>> by_token;
    for (std::size_t i = 0; i < signatures.size(); ++i) {
        const std::string& text = signatures[i].text;
        for (std::size_t p = 0; p < text.size();) {
            if (!ident_char(text[p]) && text[p] != ':') {
                ++p;
                continue;
            }
            std::size_t q = p;
            while (q < text.size() && (ident_char(text[q]) || text[q] == ':')) {
                ++q;
            }
            by_token[text.substr(p, q - p)].push_back(i);
            p = q;
        }
    }

    for (std::size_t ri = 0; ri < records.size(); ++ri) {
        const ScannedRecord& record = records[ri];
        const std::vector<std::string> path = split_ns(record.qual);
        // Every spelling the per-scope walk below can test is the qualified name
        // with some namespace prefix removed, so collect the signatures that
        // could possibly match before doing any exact check.
        std::unordered_set<std::size_t> candidates;
        for (std::size_t k = 0; k < path.size(); ++k) {
            const std::string spelling =
                join_ns(std::vector<std::string>(path.begin() + k, path.end()));
            const auto found = by_token.find(spelling);
            if (found != by_token.end()) {
                candidates.insert(found->second.begin(), found->second.end());
            }
        }
        for (const std::size_t si : candidates) {
            const ScannedSig& sig = signatures[si];
            const std::vector<std::string> record_ns(path.begin(), path.end() - 1);
            bool hit = false;
            bool hit_by_value = false;
            for (std::size_t k = 0; k <= sig.scope.size() && !hit; ++k) {
                if (k > record_ns.size()) {
                    break;
                }
                if (!std::equal(sig.scope.begin(), sig.scope.begin() + k,
                                record_ns.begin())) {
                    continue;
                }
                const std::vector<std::string> rest(path.begin() + k, path.end());
                const std::string spelling = join_ns(rest);
                if (has_spelling(sig.text, spelling)) {
                    hit = true;
                }
                if (has_spelling_by_value(sig.text, spelling)) {
                    hit_by_value = true;
                }
            }
            if (!hit) {
                if (has_spelling(sig.text, record.qual)) {
                    hit = true;
                }
                if (has_spelling_by_value(sig.text, record.qual)) {
                    hit_by_value = true;
                }
            }
            if (hit && (sig.is_api || hit_by_value)) {
                crossing.insert(record.qual);
            }
        }
    }

    const std::set<std::string> guarded = guarded_names(root);
    std::vector<std::string> unguarded;
    for (const auto& qual : crossing) {
        const ScannedRecord* record = nullptr;
        for (const auto& candidate : records) {
            if (candidate.qual == qual) {
                record = &candidate;
                break;
            }
        }
        if (record == nullptr || !is_plain_record(*record)) {
            continue;
        }
        if (in_scope != nullptr) {
            in_scope->push_back(qual);
        }
        if (guarded.count(qual) == 0) {
            unguarded.push_back(qual);
        }
    }
    return unguarded;
}

// Every cross-boundary plain record must be either guarded or listed in the
// document. This is what turns the hand-written list into a checkable one.
CRPC_TEST(abi_docs_lists_every_unguarded_cross_boundary_record) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    std::vector<std::string> in_scope;
    const std::vector<std::string> unguarded =
        unguarded_crossing_records(root, &in_scope);
    CRPC_CHECK(!in_scope.empty());

    const std::string english = read_text(root / "docs" / "abi.md");
    const std::string chinese = read_text(root / "docs" / "abi.zh-CN.md");

    // Match the list-item form, not any occurrence: the surrounding prose also
    // names one of these records when it explains the frame_header collision,
    // and a bare containment test would accept that mention as the entry.
    std::vector<std::string> missing;
    for (const auto& qual : unguarded) {
        const std::string item = "- `::" + qual + "`";
        if (!has_phrase(english, item.c_str()) ||
            !has_phrase(chinese, item.c_str())) {
            missing.push_back(qual);
        }
    }
    for (const auto& qual : missing) {
        std::printf("  missing from docs: %s\n", qual.c_str());
    }
    CRPC_CHECK(missing.empty());
}

// The scanner above checks the *list*. These two cases check the prose around
// it, which no scan can derive: the per-record guard description, the
// standard-layout facts, and the scope counts. They were dropped once while the
// list was rewritten, which left the corrected sentences unguarded; a rewrite
// that flips any of them must fail here.
CRPC_TEST(abi_docs_english_states_the_guard_facts) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.md");

    // Counts and scope wording.
    CRPC_CHECK(has_phrase(text, "15 of the public records"));
    CRPC_CHECK(has_phrase(text, "an exact member count for the 16 records"));
    CRPC_CHECK(has_phrase(text, "have **no compile-time layout guard at all**"));

    // The four standard-library-bearing records are described one by one, and
    // only rpc_error is fully guarded.
    CRPC_CHECK(has_phrase(text, "only the first is fully guarded"));
    CRPC_CHECK(has_phrase(
        text, "have **no layout guard beyond a minimum alignment**"));
    CRPC_CHECK(has_phrase(text, "`rpc_error` is the only one of the four"));
    CRPC_CHECK(has_phrase(text, "not asserted to be standard-layout either"));
    // buffer is the one exception across the audited toolchains.
    CRPC_CHECK(has_phrase(
        text, "`buffer` is on MSVC and GCC but not on Clang"));
    // offsetof on these records is well-formed; the reason for not asserting it
    // is portability, not ill-formedness.
    CRPC_CHECK(has_phrase(text, "it is well-formed and yields the"));

    // The corrected sentences must not regress to the old false claims.
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "an exact member count for every record that crosses"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "so `offsetof` on them would be ill-formed"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "standard-library types are not standard-layout, so neither"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "They are guarded by their member count, their standard-layout"));
    CRPC_CHECK_FALSE(forbidden_phrase(text, "all four are fully guarded"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "MSVC, GCC and Clang alike"));
}

// The two counts in the prose are different numbers and have been confused once:
// `is_standard_layout` is asserted for every record that can carry it, while
// `CRPC_MEMBER_COUNT` also covers `rpc_error`, which deliberately gets no
// standard-layout assertion (src/abi_check.cpp explains why). Deriving both from
// the source and matching them against the document is the only way a rewrite
// cannot silently collapse the two into one number.
CRPC_TEST(abi_docs_counts_match_abi_check_source) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string src =
        strip_cpp(read_text(root / "src" / "abi_check.cpp"));
    CRPC_CHECK(!src.empty());
    const auto count_at_line_start = [&src](const std::string& key) {
        std::size_t total = 0;
        std::size_t pos = 0;
        while ((pos = src.find(key, pos)) != std::string::npos) {
            const bool at_start =
                pos == 0 || src[pos - 1] == '\n';
            if (at_start) {
                ++total;
            }
            pos += key.size();
        }
        return total;
    };
    const std::size_t member_counts = count_at_line_start("CRPC_MEMBER_COUNT(");
    const std::size_t standard_layouts =
        count_at_line_start("static_assert(std::is_standard_layout<");
    // Both guards exist, and rpc_error is the record that carries only one.
    CRPC_CHECK(standard_layouts >= 1);
    CRPC_CHECK(member_counts >= 1);
    CRPC_CHECK(member_counts > standard_layouts);

    const std::string en = read_text(root / "docs" / "abi.md");
    const std::string zh = read_text(root / "docs" / "abi.zh-CN.md");
    const std::string en_phrase =
        std::to_string(standard_layouts) + " of the public records";
    const std::string en_count =
        "an exact member count for the " + std::to_string(member_counts) + " records";
    const std::string zh_phrase =
        std::to_string(standard_layouts) + " 个公开记录";
    const std::string zh_count =
        "**" + std::to_string(member_counts) + " 个**记录的精确成员数量";
    CRPC_CHECK(has_phrase(en, en_phrase.c_str()));
    CRPC_CHECK(has_phrase(en, en_count.c_str()));
    CRPC_CHECK(has_phrase(zh, zh_phrase.c_str()));
    CRPC_CHECK(has_phrase(zh, zh_count.c_str()));

    // The same number appears in more than one sentence, and only one of them was
    // updated the first time round, which left the document contradicting itself.
    // Pin every place that states the member-count total, and forbid the stale
    // standard-layout total from being reused for it.
    const std::string en_member_sentence =
        "among the " + std::to_string(member_counts) + " member-counted records";
    const std::string en_stale_sentence =
        "among the " + std::to_string(standard_layouts) + " member-counted records";
    CRPC_CHECK(has_phrase(en, en_member_sentence.c_str()));
    CRPC_CHECK_FALSE(has_phrase(en, en_stale_sentence.c_str()));

    // The Chinese document carries the same sentence in its own wording, and it
    // was missed when the English one was corrected. Guard it symmetrically: a
    // one-sided guard lets the other language drift back to the stale number.
    const std::string zh_member_sentence =
        "就 " + std::to_string(member_counts) + "\r\n  个有成员数量护栏的记录而言";
    const std::string zh_stale_sentence =
        "就 " + std::to_string(standard_layouts) + "\r\n  个有成员数量护栏的记录而言";
    CRPC_CHECK(has_phrase(zh, zh_member_sentence.c_str()));
    CRPC_CHECK_FALSE(has_phrase(zh, zh_stale_sentence.c_str()));
}

CRPC_TEST(abi_docs_chinese_states_the_guard_facts) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.zh-CN.md");
    CRPC_CHECK(!text.empty());

    // Counts and scope wording, mirroring the English document.
    CRPC_CHECK(has_phrase(text, "15 个公开记录"));
    CRPC_CHECK(has_phrase(text, "**16 个**记录的精确成员数量"));
    CRPC_CHECK(has_phrase(text, "**没有任何编译期布局护栏**"));

    // The four standard-library-bearing records, described one by one.
    CRPC_CHECK(has_phrase(text, "只有第一个是完整的"));
    CRPC_CHECK(has_phrase(text, "**除最小对齐外没有任何布局护栏**"));
    CRPC_CHECK(has_phrase(text, "四者中只有 `rpc_error` 的成员类型"));
    CRPC_CHECK(has_phrase(text, "也没有被断言为标准布局"));
    CRPC_CHECK(has_phrase(text, "`buffer` 在 MSVC 与 GCC 上是、在 Clang 上不是"));
    CRPC_CHECK(has_phrase(text, "它是合法的且能得到"));

    // Same anti-regression checks as the English document.
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "每个跨边界记录的精确成员数量"));
    // The claim was written with two different verbs across revisions (做 and
    // 取), so both spellings must be forbidden; forbidding only the current one
    // lets the older wording be restored silently.
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "因此对其成员做 `offsetof` 会非法"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "因此对其成员取 `offsetof` 会非法"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "标准库类型不是标准布局，因此两者都不是"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "由成员数量、标准布局前置条件以及"));
    CRPC_CHECK_FALSE(forbidden_phrase(text, "四者都受到完整守护"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "MSVC、GCC 与 Clang 上都是"));
}

std::string between_headings(const std::string& text, const char* begin,
                             const char* end) {
    const std::size_t from = text.find(begin);
    if (from == std::string::npos) {
        return {};
    }
    const std::size_t to = text.find(end, from + 1);
    if (to == std::string::npos) {
        return text.substr(from);
    }
    return text.substr(from, to - from);
}

CRPC_TEST(abi_docs_section_2_does_not_call_internal_layout_invisible) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string english =
        between_headings(read_text(root / "docs" / "abi.md"), "## 2.", "## 3.");
    const std::string chinese =
        between_headings(read_text(root / "docs" / "abi.zh-CN.md"), "## 2.", "## 3.");
    CRPC_CHECK(!english.empty());
    CRPC_CHECK(!chinese.empty());

    // Meta-assertion: without this, deleting the whole section would leave every
    // forbidden phrase absent and the test would pass vacuously.
    CRPC_CHECK(has_phrase(english, "implementation-internal"));
    CRPC_CHECK(has_phrase(chinese, "实现内部细节"));

    // EN: every claim measured false in this cycle.
    CRPC_CHECK_FALSE(forbidden_phrase(
        english, "cannot construct, hold or name"));
    CRPC_CHECK_FALSE(forbidden_phrase(english, "not an ABI break"));
    CRPC_CHECK_FALSE(forbidden_phrase(english, "is a MINOR change"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        english, "no consumer may depend on their layout"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        english, "any further field crosses the 488-byte boundary"));
    CRPC_CHECK_FALSE(forbidden_phrase(english, "subject to the same vptr"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        english, "allocates or holds them by value"));
    // Carried over from 6227530: the old text also claimed the library itself
    // instantiates these types (it does for connection_slot/client_liveness, but
    // not for call_state<Resp>, which is the one that matters).
    CRPC_CHECK_FALSE(forbidden_phrase(
        english, "the library itself instantiates"));

    // ZH mirrors.
    CRPC_CHECK_FALSE(forbidden_phrase(chinese, "无法构造、持有或命名"));
    CRPC_CHECK_FALSE(forbidden_phrase(chinese, "不构成 ABI 破坏"));
    CRPC_CHECK_FALSE(forbidden_phrase(chinese, "属于 MINOR 变更"));
    CRPC_CHECK_FALSE(forbidden_phrase(chinese, "再追加任何字段就会跨过 488"));
    CRPC_CHECK_FALSE(forbidden_phrase(chinese, "同受 vptr"));
    CRPC_CHECK_FALSE(forbidden_phrase(chinese, "按值分配或持有它们"));

    // The replacement must name the real mechanism, the load-bearing rule, and
    // the x86 padding budget (not a boundary).
    CRPC_CHECK(has_phrase(english, "inside the consumer's own translation unit"));
    CRPC_CHECK(has_phrase(english, "rebuilt from the same headers"));
    CRPC_CHECK(has_phrase(english, "inner_mutex"));
    CRPC_CHECK(has_phrase(
        english, "the padding is only four bytes and three remain"));
    CRPC_CHECK(has_phrase(english, "wider than the three remaining bytes"));

    // The x86 tail-padding sentence is only true while `retry` ends at 484 and
    // `sizeof(call_core)` is 488; both numbers are stated so a layout change
    // forces this sentence to be revisited.
    CRPC_CHECK(has_phrase(english, "retry` ends at 484"));
    CRPC_CHECK(has_phrase(english, "stays 488"));

    // Who allocates what: the two library-only types must not be described as
    // consumer-allocated, and the library-only fact must be stated.
    CRPC_CHECK(has_phrase(english, "allocated only by the library"));
    // Allocating in the library does NOT make the layout invisible: the member
    // offsets are baked into the consumer translation unit by header-inline
    // code, so a field added to either type still breaks an older consumer.
    CRPC_CHECK_FALSE(forbidden_phrase(
        english, "does not break a consumer built from older headers"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        english, "never allocates them by value"));
    CRPC_CHECK(has_phrase(
        english, "member offsets are still baked into consumer"));
    // Only two of the four are polymorphic; the other two are plain structs.
    CRPC_CHECK(has_phrase(
        english, "Only `call_state_base` and `call_state<Resp>` are"));
    CRPC_CHECK(has_phrase(english, "polymorphic"));
    CRPC_CHECK(has_phrase(english, "plain structs with"));
    CRPC_CHECK(has_phrase(english, "no vptr"));
    CRPC_CHECK(has_phrase(english, "is the polymorphic base of `call_state<Resp>`"));

    // Handle size stability must not be read as heap-object safety.
    CRPC_CHECK(has_phrase(english, "covers the handle, not the heap"));

    CRPC_CHECK(has_phrase(chinese, "在消费方自己的翻译单元内实例化"));
    CRPC_CHECK(has_phrase(chinese, "同一份头文件"));
    CRPC_CHECK(has_phrase(chinese, "padding 只有 4 字节"));
    CRPC_CHECK(has_phrase(chinese, "只由库分配"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        chinese, "不会破坏用旧头文件构建的消费方"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        chinese, "消费方从不会按值分配它们"));
    CRPC_CHECK(has_phrase(chinese, "成员偏移仍被头内代码烘焙进消费方"));
    CRPC_CHECK(has_phrase(chinese, "没有 vptr 的普通结构体"));
    CRPC_CHECK(has_phrase(chinese, "只覆盖句柄，不覆盖它指向的堆对象"));
}

// The flow-control change grows `rpc::client` by one pointer-sized member, which
// is a MAJOR layout change that a header-side `static_assert` cannot pin: the
// class owns standard-library members, so its width follows the standard library
// configuration (MSVC Debug and Release differ). The tripwire is the out-of-line
// size probe plus this consumer-side assertion: if the size this translation unit
// compiled against and the size the library was built with ever disagree, the
// build is mixing headers and a binary.
CRPC_TEST(abi_rpc_client_size_probe_matches_consumer) {
    const std::size_t consumer_size = sizeof(crpc::rpc::client);
    const std::size_t library_size = crpc::rpc::rpc_client_size_probe();
    CRPC_CHECK_EQ(consumer_size, library_size);
    // A flow controller is one pointer: the class must be at least as wide as a
    // pointer more than its own scratch state, which is not asserted here (the
    // members are private); the probe relation above is the contract.
    CRPC_CHECK(library_size > 0);
}

// --- Public API call sites -------------------------------------------------
//
// An exported method that no code ever calls is a promise that was declared and
// never delivered, and nothing in the build notices: it compiles, it links, and
// coverage is not gated. `distributed_lock::start_watchdog()` sat in that state --
// its renewal thread was never started by anything, so a holder that outlived the
// TTL silently lost the lock.
//
// Counting bare `name(` is not enough, twice over. A same-named helper in another
// file inflated the count (tests/test_pipeline_deadlock.cpp defines its own
// `stop_watchdog`), and scoping to files did not help because a same-named helper
// can be added to the very file being watched. What is counted here is therefore
// the call shape, not the identifier: an entry states the exact prefix that a real
// call site must contain (`lock.start_watchdog(`), so a decoy has to be spelled as
// a call on a `lock` object -- at which point it is the real call and there is
// nothing left to bypass.
//
// Definitions are excluded by construction: `void distributed_lock::start_watchdog()`
// has no `lock.` receiver, so it never matches. That also means no entry has to
// budget for "the definition itself counts once", which is the accounting that made
// the previous version pass with every caller deleted.
struct api_call_contract {
    // Exact text a real call site contains, including the receiver.
    const char* prefix;
    const char* const* files;
    std::size_t file_count;
    std::size_t min_calls;
};

const char* const k_redis_lock_test[] = {"tests/test_redis_lock.cpp"};

CRPC_TEST(abi_docs_exported_helpers_have_call_sites) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }

    // Each prefix is a call on a `distributed_lock` object named `lock`, which is
    // how every site in the suite spells it. The two watchdog cases are the only
    // callers: one renews past a TTL, the other proves the lease lapses without it.
    const api_call_contract contracts[] = {
        {"lock.start_watchdog(", k_redis_lock_test, 1, 1},
        {"lock.stop_watchdog(", k_redis_lock_test, 1, 2},
    };

    for (const api_call_contract& contract : contracts) {
        const std::string needle = contract.prefix;
        std::size_t found = 0;
        for (std::size_t i = 0; i < contract.file_count; ++i) {
            const fs::path path = root / contract.files[i];
            CRPC_CHECK(fs::exists(path));
            if (!fs::exists(path)) {
                continue;
            }
            const std::string text = strip_cpp(read_text(path));
            std::size_t pos = 0;
            while ((pos = text.find(needle, pos)) != std::string::npos) {
                ++found;
                pos += needle.size();
            }
        }
        CRPC_CHECK(found >= contract.min_calls);
        if (found < contract.min_calls) {
            std::printf("  %s occurs %zu time(s), needs %zu -- lost a caller?\n",
                        contract.prefix, found, contract.min_calls);
        }
    }
}

CRPC_TEST_MAIN()
