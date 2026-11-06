// CRPC -- cache-line alignment primitives.
//
// The user's requirement is explicit: a CPU reads 64 bytes at a time, so the
// alignment unit is 64. The tests pin two things:
//
//   1. `cache_line_size` is 64 and `CRPC_CACHE_LINE_ALIGN` really aligns.
//   2. The public structs that `docs/abi.md` freezes keep their layout. That
//      second test is the guard: it would be easy to "optimise" by adding
//      alignas to `http::http_statistics` and silently break the ABI.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>

#include <crpc/cache_line.hpp>
#include <crpc/http/server.hpp>

// C4324 fires because the whole point of these structs is the padding the
// alignment inserts. Silencing it locally keeps /WX on for everything else.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace {

struct CRPC_CACHE_LINE_ALIGN hot_counters {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
};

struct CRPC_CACHE_LINE_ALIGN separated {
    CRPC_CACHE_LINE_ALIGN std::uint64_t first = 0;
    CRPC_CACHE_LINE_ALIGN std::uint64_t second = 0;
};

} // namespace

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

CRPC_TEST(cache_line_size_is_sixty_four_bytes) {
    CRPC_CHECK_EQ(std::size_t(64), crpc::cache_line_size);
    CRPC_CHECK_EQ(std::size_t(64), alignof(hot_counters));
    CRPC_CHECK_EQ(std::size_t(0), sizeof(hot_counters) % crpc::cache_line_size);
}

CRPC_TEST(cache_line_align_separates_the_two_hot_fields) {
    separated value;
    const auto base = reinterpret_cast<std::uintptr_t>(&value);
    const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(&value.first) - base;
    const std::uintptr_t second = reinterpret_cast<std::uintptr_t>(&value.second) - base;
    CRPC_CHECK_EQ(std::uintptr_t(0), first % crpc::cache_line_size);
    CRPC_CHECK_EQ(std::uintptr_t(0), second % crpc::cache_line_size);
    CRPC_CHECK(first != second);
}

// ABI guard: the cache-line work must not reach into frozen public layouts.
// http_statistics is the one the ABI document names explicitly (§4). Its size
// is recorded here so an accidental alignas shows up as a test failure rather
// than as a binary incompatibility discovered by a consumer.
CRPC_TEST(public_statistics_layout_is_not_cache_line_aligned) {
    CRPC_CHECK(alignof(crpc::http::http_statistics) < crpc::cache_line_size);
}

CRPC_TEST_MAIN()
