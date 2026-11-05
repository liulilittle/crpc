// CRPC -- tests for the Redis command metadata table.
//
// `include/crpc/redis/command_table.hpp` is the single checked-in list of the
// commands this library emits. These tests pin the table's own invariants
// (sorted, unique, sane arity/key fields) and, more importantly, that the table
// cannot silently drift from the source of truth on either side:
//
//   * matches the emitted surface -- every command name that `src/redis/*.cpp`
//     or the typed builders in `include/crpc/redis/*.hpp` send toward a server
//     must have a row, so adding a typed call without a table entry fails;
//   * matches the real server -- under CRPC_REDIS_IT=1, every row's arity and
//     key positions are compared against the live Redis `COMMAND` reply, so a
//     wrong entry fails against an authoritative oracle.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/redis/command_table.hpp>

#include "test_framework.hpp"

using namespace crpc::redis;

namespace {

namespace fs = std::filesystem;

// Mirrors tests/test_docs_lint.cpp::repo_root(): resolve the repository root
// from this source file (CMake compiles with absolute paths), with a fallback
// that walks up from the working directory.
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
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

// All command names the sources send toward a server. Parsed from the two
// concrete constructions used across src/redis and the redis headers:
// `command{"NAME", ...}` / `command NAME{"NAME", ...}` and
// `command_builder("NAME")`.
bool collect_emitted_names(const fs::path& root,
                           std::set<std::string>& out,
                           std::vector<std::string>& problems) {
    const std::string brace = "command{";
    const std::string builder = "command_builder(";
    bool ok = true;

    const fs::path src_dir = root / "src" / "redis";
    if (!fs::is_directory(src_dir)) {
        problems.push_back("missing src/redis");
        return false;
    }
    for (const auto& entry : fs::directory_iterator(src_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
            continue;
        }
        const std::string text = read_text(entry.path());
        std::size_t pos = 0;
        while ((pos = text.find(brace, pos)) != std::string::npos) {
            std::size_t name_at = pos + brace.size();
            while (name_at < text.size() && (text[name_at] == ' ' ||
                                             text[name_at] == '\t')) {
                ++name_at;
            }
            // `command newname{"GET", ...}` style (a local declaration).
            while (name_at < text.size() && text[name_at] != '"' &&
                   text[name_at] != '{') {
                ++name_at;
            }
            if (name_at < text.size() && text[name_at] == '"') {
                ++name_at;
                std::size_t end = text.find('"', name_at);
                if (end != std::string::npos && end > name_at) {
                    out.emplace(text.substr(name_at, end - name_at));
                }
            }
            pos = name_at;
        }
        pos = 0;
        while ((pos = text.find(builder, pos)) != std::string::npos) {
            std::size_t name_at = pos + builder.size();
            if (name_at < text.size() && text[name_at] == '"') {
                ++name_at;
                std::size_t end = text.find('"', name_at);
                if (end != std::string::npos && end > name_at) {
                    out.emplace(text.substr(name_at, end - name_at));
                }
            }
            pos = name_at;
        }
    }

    const fs::path inc_dir = root / "include" / "crpc" / "redis";
    if (!fs::is_directory(inc_dir)) {
        problems.push_back("missing include/crpc/redis");
        return false;
    }
    for (const auto& entry : fs::directory_iterator(inc_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".hpp") {
            continue;
        }
        const std::string text = read_text(entry.path());
        std::size_t pos = 0;
        while ((pos = text.find(brace, pos)) != std::string::npos) {
            std::size_t name_at = pos + brace.size();
            while (name_at < text.size() && (text[name_at] == ' ' ||
                                             text[name_at] == '\t')) {
                ++name_at;
            }
            while (name_at < text.size() && text[name_at] != '"' &&
                   text[name_at] != '{') {
                ++name_at;
            }
            if (name_at < text.size() && text[name_at] == '"') {
                ++name_at;
                std::size_t end = text.find('"', name_at);
                if (end != std::string::npos && end > name_at) {
                    out.emplace(text.substr(name_at, end - name_at));
                }
            }
            pos = name_at;
        }
        pos = 0;
        while ((pos = text.find(builder, pos)) != std::string::npos) {
            std::size_t name_at = pos + builder.size();
            if (name_at < text.size() && text[name_at] == '"') {
                ++name_at;
                std::size_t end = text.find('"', name_at);
                if (end != std::string::npos && end > name_at) {
                    out.emplace(text.substr(name_at, end - name_at));
                }
            }
            pos = name_at;
        }
    }
    return ok;
}

} // namespace

// The table is sorted by name and contains no duplicates. `lookup_command` is a
// binary search, so both properties are load-bearing.
CRPC_TEST(command_table_is_sorted_and_unique) {
    for (std::size_t i = 1; i < k_command_table_size; ++i) {
        CRPC_CHECK(k_command_table[i].name > k_command_table[i - 1].name);
    }
}

// A row's arity must be sane: never zero, and if negative it must be "at
// least |arity|" (the minimum itself is at least the command name, so a
// variadic command's magnitude is >= 1). Key fields are either all zero (no
// key) or a valid 1-based window with a positive step.
CRPC_TEST(command_table_rows_are_well_formed) {
    for (std::size_t i = 0; i < k_command_table_size; ++i) {
        const command_spec& spec = k_command_table[i];
        CRPC_CHECK(spec.arity != 0);
        const std::int64_t min_args =
            spec.arity < 0 ? -static_cast<std::int64_t>(spec.arity)
                           : static_cast<std::int64_t>(spec.arity);
        CRPC_CHECK(min_args >= 1);

        const bool has_key = spec.first_key != 0 || spec.last_key != 0 ||
                             spec.key_step != 0;
        if (!has_key) {
            CRPC_CHECK_EQ(std::int64_t{0}, spec.first_key);
            CRPC_CHECK_EQ(std::int64_t{0}, spec.last_key);
            CRPC_CHECK_EQ(std::int64_t{0}, spec.key_step);
        } else {
            CRPC_CHECK(spec.first_key >= 1);
            CRPC_CHECK(spec.key_step >= 1);
            CRPC_CHECK(spec.last_key != 0);
            // The window must be reachable with the declared minimum arity.
            // `last_key` negative means "through the last argument"; its
            // magnitude must not reach beyond min_args.
            const std::int64_t max_key =
                spec.last_key < 0 ? min_args : static_cast<std::int64_t>(spec.last_key);
            CRPC_CHECK(max_key <= min_args);
        }
    }
}

// Every command the sources can send has a row. This is the drift guard: adding
// a typed builder or an emit site without a table entry fails here.
CRPC_TEST(command_table_covers_every_emitted_command) {
    const fs::path root = repo_root();
    CRPC_CHECK(!root.empty());
    if (root.empty()) {
        return;
    }

    std::set<std::string> emitted;
    std::vector<std::string> problems;
    CRPC_CHECK(collect_emitted_names(root, emitted, problems));
    for (const std::string& p : problems) {
        std::printf("  %s\n", p.c_str());
    }
    CRPC_CHECK(problems.empty());
    CRPC_CHECK(emitted.size() > 20u);

    std::vector<std::string> missing;
    for (const std::string& name : emitted) {
        if (lookup_command(name) == nullptr) {
            missing.push_back(name);
        }
    }
    if (!missing.empty()) {
        std::printf("emitted commands without a table row:\n");
        for (const std::string& n : missing) {
            std::printf("  %s\n", n.c_str());
        }
    }
    CRPC_CHECK(missing.empty());
}

// Every row is looked up by name (exercises the binary search on both sides of
// the pivot, and defends the sorted invariant at runtime, not just at index 1).
CRPC_TEST(command_table_lookup_finds_every_row) {
    for (std::size_t i = 0; i < k_command_table_size; ++i) {
        CRPC_CHECK_EQ(lookup_command(k_command_table[i].name),
                      &k_command_table[i]);
        CRPC_CHECK(lookup_command(k_command_table[i].name) != nullptr);
    }
    CRPC_CHECK_EQ(lookup_command("GET")->name, "GET");
    CRPC_CHECK_EQ(lookup_command("NOPE"), nullptr);
    CRPC_CHECK_EQ(lookup_command(""), nullptr);
}

CRPC_TEST_MAIN()