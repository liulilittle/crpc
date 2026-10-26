// CRPC -- regression test for the `stream_entry` name collision.
//
// `types.hpp` used to declare `crpc::redis::stream_entry` (the reply
// projection of `decode_stream_entries`) while `stream.hpp` declared a
// different `crpc::redis::stream_entry` (an entry carrying a parsed ID). Two
// definitions of one name in one namespace meant a single translation unit
// could not include both headers. The `types.hpp` record is now
// `redis_stream_entry`, so this TU -- which includes `types.hpp`, `stream.hpp`
// and `rtti.hpp` together -- must compile, and the two spellings must stay
// distinct types.
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <crpc/redis/rtti.hpp>
#include <crpc/redis/stream.hpp>
#include <crpc/redis/types.hpp>

#include "test_framework.hpp"

// Both records exist and are different types. Before the rename the two
// headers could not even be included together, so reaching this point is the
// coexistence proof; the assertions pin the separation so the collision cannot
// come back.
static_assert(!std::is_same<crpc::redis::stream_entry,
                           crpc::redis::redis_stream_entry>::value,
              "types.hpp and stream.hpp must not declare the same stream_entry");
static_assert(!std::is_same<crpc::redis::stream_id,
                           crpc::redis::redis_stream_entry>::value,
              "stream_id must remain distinct from the decoded entry");
static_assert(!std::is_same<crpc::redis::stream_id,
                           crpc::redis::stream_entry>::value,
              "stream_id must remain distinct from the stream.hpp entry");

// The two records are both usable in one TU: one is the raw projection, the
// other is the stream.hpp entry with a parsed ID.
CRPC_TEST(redis_header_conflict_both_records_usable) {
    crpc::redis::redis_stream_entry decoded;
    decoded.id = "1-1";
    decoded.fields = {{"f", "v"}};
    CRPC_CHECK_EQ(decoded.id, std::string("1-1"));
    CRPC_CHECK_EQ(decoded.fields.size(), std::size_t{1});
    CRPC_CHECK_EQ(decoded.fields[0].first, std::string("f"));

    crpc::redis::stream_entry parsed;
    parsed.id = "1-1";
    parsed.parsed_id = crpc::redis::stream_id(1, 1);
    parsed.fields = {{"f", "v"}};
    CRPC_CHECK_EQ(parsed.id, std::string("1-1"));
    CRPC_CHECK_EQ(parsed.parsed_id.ms, std::uint64_t{1});
    CRPC_CHECK_EQ(parsed.parsed_id.seq, std::uint64_t{1});
}

CRPC_TEST_MAIN()
