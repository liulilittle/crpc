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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

// Prose wraps at 80 columns, so a phrase can be split across lines, and the
// split can fall inside a word. Runs of spaces can also change without the
// claim changing. Both the document and the needle are reduced to their
// non-whitespace characters before comparing, so the checks are about the
// words and not about the line breaks.
//
// Collapsing whitespace to a single space on both sides is not enough: it still
// rejects a phrase that the document wraps inside a word while the needle does
// not wrap there, and it accepts nothing that stripping does not. Stripping
// compares the words only, which is the property the checks need. It also means
// a forbidden phrase cannot be smuggled back in by re-wrapping it.
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

bool has_phrase(const std::string& text, const char* needle) {
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

CRPC_TEST(abi_docs_english_says_no_abi_checker_runs) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.md");

    CRPC_CHECK(has(text, "No ABI checker is implemented"));
    CRPC_CHECK_FALSE(has(text, "CI runs the ABI check"));
    CRPC_CHECK_FALSE(has(text, "CI compares exported symbols"));
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

CRPC_TEST(abi_docs_chinese_does_not_claim_ci_abi_check) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.zh-CN.md");

    CRPC_CHECK(has(text, "未实现 ABI 检查器"));
    CRPC_CHECK_FALSE(has(text, "CI 运行 ABI 检查任务"));
    // Migration guides are absent; the text must say so rather than point at one.
    CRPC_CHECK(has(text, "均不存在"));
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

    // A structured binding binds one local name per member in declaration
    // order; it does not compare those names with the real member names. The
    // document once claimed a rename was caught by the member count, which is
    // false. Keep the corrected statement and reject the old one. Matching on
    // normalized whitespace means the old claim cannot return by re-wrapping.
    CRPC_CHECK_FALSE(has_phrase(text, "a renamed member is caught"));
    CRPC_CHECK_FALSE(has_phrase(text, "renamed member is caught by the relative"));
    CRPC_CHECK_FALSE(has_phrase(
        text, "member is caught by the relative chains and the member counts"));
    CRPC_CHECK(has_phrase(text, "does **not** validate names"));
    CRPC_CHECK(has_phrase(text, "no relative chain"));

    // The only record with no relative chain is `rpc_error`. The four HTTP/2
    // value types do have CRPC_AFTER chains, so they must not be lumped in with
    // `rpc_error` as "no chain"; that would understate the coverage and a
    // reader would not know their renames are caught.
    CRPC_CHECK_FALSE(has_phrase(
        text, "no relative chain (`rpc_error`, `status`, `http2"));
    CRPC_CHECK_FALSE(has_phrase(text, "(`rpc_error`, `status`, `http2::*`)"));
    CRPC_CHECK(has_phrase(
        text, "The only cross-ABI record with no relative chain is `rpc_error`"));
    CRPC_CHECK(has_phrase(text, "**do** have `CRPC_AFTER` chains"));

    // A reorder does not change the member count, so the count cannot catch
    // one. The document must not claim it does, and must say that a reorder in
    // `rpc_error` is not caught.
    CRPC_CHECK_FALSE(has_phrase(
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
    CRPC_CHECK_FALSE(has_phrase(text, "重命名成员：由相对成员链与成员数量"));

    // The only record with no relative chain is `rpc_error`; the HTTP/2 value
    // types are chained, so the Chinese text must not lump them together.
    CRPC_CHECK_FALSE(has_phrase(text, "（`rpc_error`、`status`、`http2::*`）"));
    CRPC_CHECK(has_phrase(text, "唯一没有相对链的跨 ABI 记录是"));
    CRPC_CHECK(has_phrase(text, "**都有** `CRPC_AFTER` 链"));

    // A reorder does not change the member count; the text must say the count
    // gives no coverage for it and that `rpc_error` reorders are not caught.
    CRPC_CHECK_FALSE(has_phrase(text, "以及所有架构上的成员数量捕获"));
    CRPC_CHECK(has_phrase(text, "对重排恒为盲"));
    CRPC_CHECK(has_phrase(text, "`rpc_error` 内部的重排不会被捕获"));

    // The rpc_error member-type limitation must be stated in both languages.
    CRPC_CHECK(has_phrase(text, "成员**类型**变更"));
}

CRPC_TEST(abi_docs_english_scopes_the_guards_to_the_listed_records) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.md");

    // The member count covers the 14 records the unit lists, not "every record
    // that crosses the boundary". The old overstatement must not return.
    CRPC_CHECK_FALSE(
        has_phrase(text, "an exact member count for every record that crosses"));
    CRPC_CHECK(has_phrase(text, "an exact member count for the 14 records"));
    CRPC_CHECK(has_phrase(text, "13 of the public records"));

    // The records that cross the boundary without any layout guard must be
    // named, so a reader does not assume they are covered.
    CRPC_CHECK(has_phrase(
        text, "have **no compile-time layout guard at all**"));
    for (const char* name : {"`frame`", "`call_envelope`", "`trace_extension`",
                             "`ordered_extension`", "`io::io_result`",
                             "`rpc::call_id`", "`endpoint`"}) {
        CRPC_CHECK(has_phrase(text, name));
    }

    // The four standard-library-bearing records are not all guarded the same
    // way. The old text claimed all four had a member count and a
    // standard-layout precondition, which was false.
    CRPC_CHECK_FALSE(has_phrase(
        text, "They are guarded by their member count, their standard-layout"));
    CRPC_CHECK(has_phrase(text, "only the first is fully guarded"));
    CRPC_CHECK(has_phrase(
        text, "have **no layout guard beyond a minimum alignment**"));
    CRPC_CHECK(has_phrase(text, "`rpc_error` is the only one of the four"));

    // The old text claimed these records are not standard-layout, so that
    // `offsetof` would be ill-formed. That is false and must not return. The
    // corrected text may mention the claim only to refute it, so the negative
    // needles must be the assertion forms, not the bare phrase.
    CRPC_CHECK_FALSE(has_phrase(
        text, "standard-library types are not standard-layout, so neither"));
    CRPC_CHECK_FALSE(has_phrase(text, "so `offsetof` on them would be ill-formed"));
    CRPC_CHECK(has_phrase(text, "not asserted to be standard-layout either"));
    // `buffer` is the one exception, so the text must not claim all four.
    CRPC_CHECK(has_phrase(
        text, "`buffer` is on MSVC and GCC but not on Clang"));
}

CRPC_TEST(abi_docs_chinese_scopes_the_guards_to_the_listed_records) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "abi.zh-CN.md");
    CRPC_CHECK(!text.empty());

    // Same scope correction as the English document.
    CRPC_CHECK_FALSE(
        has_phrase(text, "每个跨边界记录的精确成员数量"));
    CRPC_CHECK(has_phrase(text, "**14 个**记录的精确成员数量"));
    CRPC_CHECK(has_phrase(text, "13 个公开记录"));

    CRPC_CHECK(has_phrase(text, "**没有任何编译期布局护栏**"));
    for (const char* name : {"`frame`", "`call_envelope`", "`trace_extension`",
                             "`ordered_extension`", "`io::io_result`",
                             "`rpc::call_id`", "`endpoint`"}) {
        CRPC_CHECK(has_phrase(text, name));
    }

    CRPC_CHECK(has_phrase(text, "只有第一个是完整的"));
    CRPC_CHECK(has_phrase(text, "**除最小对齐外没有任何布局护栏**"));
    CRPC_CHECK(has_phrase(text, "四者中只有 `rpc_error` 的成员类型"));

    // Same standard-layout correction as the English document.
    CRPC_CHECK_FALSE(has_phrase(text, "标准库类型不是标准布局，因此两者都不是"));
    CRPC_CHECK_FALSE(has_phrase(text, "因此对其成员取 `offsetof` 会非法"));
    CRPC_CHECK(has_phrase(text, "也没有被断言为标准布局"));
    CRPC_CHECK(has_phrase(text, "`buffer` 在\nMSVC 与 GCC 上是、在 Clang 上不是"));
}

CRPC_TEST_MAIN()
