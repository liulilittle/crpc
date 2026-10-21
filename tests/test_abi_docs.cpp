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

// Prose wraps at 80 columns, so a phrase can be split across lines or have its
// runs of spaces changed by an edit that does not change the claim. Matching on
// a whitespace-normalized copy keeps the checks about the wording and not about
// the line breaks, which also means a forbidden phrase cannot be smuggled back
// in by re-wrapping it.
std::string normalize_space(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (char c : text) {
        const bool is_space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (is_space) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(c);
    }
    return out;
}

bool has_phrase(const std::string& text, const char* needle) {
    return normalize_space(text).find(needle) != std::string::npos;
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
    // The rpc_error member-type limitation must be stated in both languages.
    CRPC_CHECK(has_phrase(text, "成员**类型**变更"));
}

CRPC_TEST_MAIN()
