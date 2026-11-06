// CRPC -- cache-line alignment.
//
// A CPU reads memory in 64-byte cache lines. Two counters that share a line and
// are written by different threads make every write bounce the line between
// cores (false sharing), which can cost more than the work the counter does.
//
// The size is fixed at 64 rather than taken from
// `std::hardware_destructive_interference_size`: that constant is an optional
// vendor hint, is not available before C++17's library support everywhere this
// library builds, and differs per target. A protocol/runtime library must be
// deterministic across the platforms it claims to support, so 64 is used
// everywhere and pinned by a test.
//
// Scope: this is for *internal* hot state and for new code. It is deliberately
// NOT applied to public structs such as `http::http_statistics` or
// `server::options`: `docs/abi.md` §1 freezes those layouts, and adding an
// alignment changes both `sizeof` and `alignof`, which is a MAJOR break.
#ifndef CRPC_CACHE_LINE_HPP
#define CRPC_CACHE_LINE_HPP

#include <cstddef>

#include <crpc/config.hpp>

namespace crpc {

// The alignment unit: one cache line.
inline constexpr std::size_t cache_line_size = 64;

} // namespace crpc

// Applies cache-line alignment to a whole type or to a single member.
// MSVC needs the declspec form before the declarator; GCC and clang want
// alignas. Both spellings produce the same layout.
#if defined(_MSC_VER)
#define CRPC_CACHE_LINE_ALIGN __declspec(align(64))
#else
#define CRPC_CACHE_LINE_ALIGN alignas(64)
#endif

#endif // CRPC_CACHE_LINE_HPP
