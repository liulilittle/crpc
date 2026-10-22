// CRPC -- machine checks for the `crpc-degradable` documentation claims.
//
// The server now writes a reserved response meta entry on four admission
// refusals. Prose is the only place a reader learns which four, so the prose can
// drift back to "the admission stages set neither" without any behavioural test
// noticing. This file turns the claims into substring assertions, following the
// pattern established by tests/test_abi_docs.cpp:
//
//   * a required phrase is matched with all whitespace removed, because the
//     documents wrap at 80 columns and a break can fall inside a word;
//   * a forbidden phrase uses the same normalization, so the old claim cannot
//     return by re-wrapping.
//
// It also guards the single-source requirement for the 1 ms deadline floor: the
// literal may appear only in resilience/admission.hpp, never again in the two
// readers.
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
// absolute paths, so `__FILE__` already points at <root>/tests/... in every
// supported generator; the walk-up fallback keeps a manual compile working.
fs::path repo_root() {
    fs::path from_source = fs::path(__FILE__).parent_path().parent_path();
    if (fs::exists(from_source / "docs" / "protocol.md")) {
        return from_source;
    }
    fs::path probe = fs::current_path();
    for (int depth = 0; depth < 8 && !probe.empty(); ++depth) {
        if (fs::exists(probe / "docs" / "protocol.md")) {
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

std::string strip_space(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        // ASCII-only lowercasing. A locale-sensitive std::tolower would be
        // wrong here twice over: it needs an unsigned-char cast to avoid UB on
        // bytes >= 0x80, and it would fold UTF-8 continuation bytes in the
        // Chinese documents. Only 'A'..'Z' can turn a retired English claim
        // back into something that slips past a lowercase needle, so only those
        // are folded.
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool has_phrase(const std::string& text, const char* needle) {
    return strip_space(text).find(strip_space(needle)) != std::string::npos;
}

bool forbidden_phrase(const std::string& text, const char* needle) {
    return has_phrase(text, needle);
}

// Counts non-overlapping occurrences of `needle` after whitespace stripping.
// Used to pin "exactly N rows/points" claims, which a mere name check cannot.
std::size_t count_phrase(const std::string& text, const char* needle) {
    const std::string haystack = strip_space(text);
    const std::string pin = strip_space(needle);
    if (pin.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t at = haystack.find(pin);
    while (at != std::string::npos) {
        ++count;
        at = haystack.find(pin, at + pin.size());
    }
    return count;
}

// Counts the data rows of the Markdown table whose header row contains
// `header`. The header row itself and the `|---|` separator are not data rows.
// This is what catches an extra refusal row, which no name-based assertion can
// see.
std::size_t count_table_rows(const std::string& text, const char* header) {
    const std::size_t header_at = text.find(header);
    if (header_at == std::string::npos) {
        return 0;
    }
    // Start at the line after the header row.
    std::size_t at = text.find('\n', header_at);
    std::size_t rows = 0;
    while (at != std::string::npos) {
        ++at; // first character of the next line
        if (at >= text.size() || text[at] != '|') {
            break;
        }
        const std::size_t end = text.find('\n', at);
        const std::string line =
            text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (line.find("---") == std::string::npos) {
            ++rows;
        }
        if (end == std::string::npos) {
            break;
        }
        at = end;
    }
    return rows;
}

// The documents are UTF-8. U+FFFD means a file was decoded with the wrong
// encoding and re-encoded, which silently destroys the Chinese text.
bool has_replacement_character(const std::string& text) {
    return text.find("\xEF\xBF\xBD") != std::string::npos;
}

} // namespace

CRPC_TEST(degradable_protocol_english_names_the_four_marked_refusals) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "protocol.md");
    CRPC_CHECK(!text.empty());

    // The marker, its exact value and the four marked refusals.
    CRPC_CHECK(has_phrase(text, "crpc-degradable: true"));
    CRPC_CHECK(has_phrase(text, "key length 15, value length 4"));

    // The count itself, not just the names: "four" becoming "five" must fail.
    CRPC_CHECK(has_phrase(text, "exactly four pre-execution refusal points"));

    // Each refusal row, including the rpc_code it answers with. Checking the
    // whole row is what catches a row whose code is silently changed (for
    // example the bulkhead row's (8) becoming (13)).
    CRPC_CHECK(has_phrase(text, "| per-service circuit breaker open | `unavailable` (3) |"));
    CRPC_CHECK(has_phrase(
        text, "| per-service rate bucket empty, deadline still reachable | "
              "`resource_exhausted` (13) |"));
    CRPC_CHECK(has_phrase(text, "| per-service bulkhead full | `overload` (8) |"));
    CRPC_CHECK(has_phrase(text, "| per-service in-flight cap reached | `overload` (8) |"));

    // The table has exactly four data rows: a fifth row must fail, whatever its
    // stage name is (so counting "| per-service " alone would be too weak).
    CRPC_CHECK_EQ(static_cast<std::size_t>(4),
                  count_table_rows(text, "| Refusal | `rpc_code` |"));
    // ... and exactly two `overload` (8) rows, so a code swap cannot hide.
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), count_phrase(text, "`overload` (8) |"));

    // The five paths that never carry it, and the one-way rule. The count is
    // asserted so "Five" becoming "Four" fails.
    CRPC_CHECK(has_phrase(text, "Five other error paths never carry it"));
    CRPC_CHECK(has_phrase(text, "per-connection flood bucket"));
    CRPC_CHECK(has_phrase(text, "per-connection in-flight cap"));
    CRPC_CHECK(has_phrase(text, "cannot meet the deadline"));
    CRPC_CHECK(has_phrase(text, "dispatcher's own pre-execution deadline refusal"));
    CRPC_CHECK(has_phrase(text, "every handler-produced error"));
    CRPC_CHECK(has_phrase(text, "A one-way request never carries it either"));

    // The property that makes the marker necessary, and the client's use of it.
    CRPC_CHECK(has_phrase(text, "not derivable from the code"));
    CRPC_CHECK(has_phrase(text, "precondition for its fallback gates"));

    // The retired claim must not come back in any wrapping or casing.
    // `strip_space` lowercases, so a capitalized revival is caught too.
    CRPC_CHECK_FALSE(
        forbidden_phrase(text, "the admission stages set neither"));
    CRPC_CHECK_FALSE(
        forbidden_phrase(text, "The admission stages set neither"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "reserved for fallback (the primitives are implemented, not "
              "wired)"));
}

CRPC_TEST(degradable_protocol_chinese_matches_the_english_claims) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string text = read_text(root / "docs" / "protocol.zh-CN.md");
    CRPC_CHECK(!text.empty());
    CRPC_CHECK_FALSE(has_replacement_character(text));

    CRPC_CHECK(has_phrase(text, "crpc-degradable: true"));
    CRPC_CHECK(has_phrase(text, "key 长 15，value 长 4"));

    // The count itself, and each row with its code.
    CRPC_CHECK(has_phrase(text, "恰好四个执行前拒绝点"));
    CRPC_CHECK(has_phrase(text, "| 服务级熔断器打开 | `unavailable`（3） |"));
    CRPC_CHECK(has_phrase(
        text, "| 服务级令牌桶为空、deadline 仍可达 | `resource_exhausted`（13） |"));
    CRPC_CHECK(has_phrase(text, "| 服务级 bulkhead 满载 | `overload`（8） |"));
    CRPC_CHECK(has_phrase(text, "| 服务级在途上限已满 | `overload`（8） |"));
    CRPC_CHECK_EQ(static_cast<std::size_t>(4),
                  count_table_rows(text, "| 拒绝点 | `rpc_code` |"));
    CRPC_CHECK_EQ(static_cast<std::size_t>(2),
                  count_phrase(text, "`overload`（8） |"));

    // Five unmarked paths, one-way, and the non-derivability property.
    CRPC_CHECK(has_phrase(text, "另外五条错误路径永不携带它"));
    CRPC_CHECK(has_phrase(text, "连接级泛洪令牌桶"));
    CRPC_CHECK(has_phrase(text, "连接级在途上限"));
    CRPC_CHECK(has_phrase(text, "无法满足 deadline"));
    CRPC_CHECK(has_phrase(text, "dispatcher 自身的执行前 deadline 拒绝"));
    CRPC_CHECK(has_phrase(text, "所有 handler 产生的错误"));
    CRPC_CHECK(has_phrase(text, "one-way 请求也永不携带"));
    CRPC_CHECK(has_phrase(text, "不可由 code 推导"));

    CRPC_CHECK_FALSE(forbidden_phrase(text, "准入阶段不会设置它们"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        text, "留给 fallback（原语已实现、未接线）"));
}

CRPC_TEST(degradable_architecture_says_wired_but_no_fallback_execute) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string english = read_text(root / "docs" / "architecture.md");
    const std::string chinese = read_text(root / "docs" / "architecture.zh-CN.md");
    CRPC_CHECK(!english.empty());
    CRPC_CHECK(!chinese.empty());
    CRPC_CHECK_FALSE(has_replacement_character(chinese));

    // The positive claim: four advertisement points, and `fallback_execute`
    // still not called by the server.
    CRPC_CHECK(has_phrase(
        english, "server advertises `crpc-degradable` at four pre-execution "
                 "rejection points"));
    CRPC_CHECK(has_phrase(
        english, "`fallback_execute` is still never called from the server"));
    CRPC_CHECK(has_phrase(chinese, "服务端在四个执行前拒绝点声明 `crpc-degradable`"));
    CRPC_CHECK(has_phrase(chinese, "服务端仍从不调用 `fallback_execute`"));

    // The retired "not wired" sentence must not return.
    CRPC_CHECK_FALSE(forbidden_phrase(
        english, "fallback primitives are implemented but not wired into the "
                 "call path"));
    CRPC_CHECK_FALSE(forbidden_phrase(
        english, "fallback primitives implemented, not wired into the call "
                 "path"));
    CRPC_CHECK_FALSE(forbidden_phrase(chinese, "fallback 原语已实现但尚未接入调用路径"));
    CRPC_CHECK_FALSE(forbidden_phrase(chinese, "fallback 原语已实现，未接入调用路径"));

    // Other subsystems' "not wired" statements are true and must survive: the
    // edit must not have been a blanket replacement.
    CRPC_CHECK(has_phrase(english, "observability interceptor chain is not wired"));
    CRPC_CHECK(has_phrase(english, "AOP not wired into the call path"));
    CRPC_CHECK(has_phrase(english, "Flow control"));
}

CRPC_TEST(degradable_changelog_records_a_behaviour_change_not_an_abi_change) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::string english = read_text(root / "docs" / "CHANGELOG.md");
    const std::string chinese = read_text(root / "docs" / "CHANGELOG.zh-CN.md");
    CRPC_CHECK(!english.empty());
    CRPC_CHECK(!chinese.empty());
    CRPC_CHECK_FALSE(has_replacement_character(chinese));

    CRPC_CHECK(has_phrase(english, "crpc-degradable"));
    CRPC_CHECK(has_phrase(english, "behaviour change, not an ABI change"));
    CRPC_CHECK(has_phrase(english, "No exported symbol is added"));
    CRPC_CHECK(has_phrase(english, "no `server::options` field is added"));

    CRPC_CHECK(has_phrase(chinese, "crpc-degradable"));
    CRPC_CHECK(has_phrase(chinese, "行为变更，不是 ABI 变更"));
    CRPC_CHECK(has_phrase(chinese, "不新增、不移除、不改签名任何导出符号"));
}

CRPC_TEST(degradable_deadline_floor_has_a_single_source) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }

    // The constant lives in the admission header (header-only, no symbol).
    const std::string header =
        read_text(root / "include" / "crpc" / "resilience" / "admission.hpp");
    CRPC_CHECK(has_phrase(header, "inline constexpr u64 min_remaining_ns"));
    CRPC_CHECK(has_phrase(header, "min_remaining_ns = 1'000'000"));

    // Both readers use it, and neither keeps its own copy of the literal.
    const std::string dispatcher =
        read_text(root / "src" / "rpc" / "dispatcher.cpp");
    CRPC_CHECK(has_phrase(dispatcher, "resilience::min_remaining_ns"));
    CRPC_CHECK_FALSE(forbidden_phrase(dispatcher, "1'000'000"));

    const std::string server = read_text(root / "src" / "server" / "server.cpp");
    CRPC_CHECK(has_phrase(server, "resilience::min_remaining_ns"));
    CRPC_CHECK_FALSE(forbidden_phrase(server, "1'000'000"));

    // The four marked branches each name their stage, and the marker is written
    // in exactly one place (send_error's body). The stage label is not
    // observable at run time, so this is the only guard against a wrong stage.
    CRPC_CHECK(has_phrase(server, "stage_kind::breaker"));
    CRPC_CHECK(has_phrase(server, "stage_kind::service_rate"));
    CRPC_CHECK(has_phrase(server, "stage_kind::service_bulkhead"));
    CRPC_CHECK(has_phrase(server, "stage_kind::service_capacity"));
    CRPC_CHECK(has_phrase(server, "degradable_metadata_key"));
}

CRPC_TEST_MAIN()
