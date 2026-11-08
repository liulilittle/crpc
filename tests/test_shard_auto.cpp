// CRPC -- automatic shard-count policy (G36, opt-in).
//
// The automatic shard count stays opt-in: `server::options::shards` keeps its
// historical default of 1, `builder::shards(0)` still maps to 1, and only an
// explicit `builder::shards_auto()` call selects the physical-core-derived
// policy. The policy itself is a pure, machine-independent function so the
// boundary table can be asserted on any host. `builder::shards_auto(cores)`
// injects the core count, which keeps the builder wiring testable on a 1-core
// CI box (the default `shards_auto()` reads the host and is only checked for
// agreement with the same pure function).
//
// The multi-shard configuration is mutually exclusive with the server PUSH
// path: `server::publish` returns `unsupported_configuration` and push-control
// subscriptions return `unavailable` when `shards != 1` (see
// src/server/server.cpp:1288-1290, :2957-2958). That is why auto sharding is
// opt-in rather than the default.
#include "test_framework.hpp"

#include <cstddef>
#include <limits>
#include <memory>

#include <crpc/server.hpp>

namespace {

constexpr std::size_t kMaxShards = 32;

void check_pure(std::size_t physical_cores, std::size_t expected) {
    CRPC_CHECK_EQ(expected, crpc::rpc::default_shard_count(physical_cores));
}

} // namespace

CRPC_TEST(default_shard_count_boundaries) {
    // The whole boundary set is deterministic and host-independent:
    //   0 -> 1 (guard against size_t 0-1 wrap), then clamp(cores-1, 1, 32).
    check_pure(0, 1);
    check_pure(1, 1);
    check_pure(2, 1);
    check_pure(3, 2);
    check_pure(16, 15);
    check_pure(32, 31);
    check_pure(33, 32);
    check_pure(64, kMaxShards);
    check_pure(std::numeric_limits<std::size_t>::max(), kMaxShards);
}

CRPC_TEST(options_default_shard_count_is_one) {
    // The default is part of the contract: auto sharding is opt-in only. A
    // silent flip of this default would break PUSH and regress single-client
    // throughput, so it is pinned here.
    crpc::server::options opt{};
    CRPC_CHECK_EQ(std::size_t{1}, opt.shards);
}

CRPC_TEST(builder_shards_zero_still_maps_to_one) {
    // `shards(0)` keeps its historical "one shard" meaning; it is not a
    // backdoor for auto sharding.
    crpc::server::builder builder;
    builder.shards(0);
    std::unique_ptr<crpc::server> built = builder.build();
    CRPC_REQUIRE(built != nullptr);
    CRPC_CHECK_EQ(std::size_t{1}, built->shard_count());
}

CRPC_TEST(builder_shards_explicit_is_preserved) {
    crpc::server::builder builder;
    builder.shards(5);
    std::unique_ptr<crpc::server> built = builder.build();
    CRPC_REQUIRE(built != nullptr);
    CRPC_CHECK_EQ(std::size_t{5}, built->shard_count());
}

CRPC_TEST(builder_shards_auto_injected) {
    // Injected core count: 3 physical cores -> clamp(3-1, 1, 32) == 2. This
    // is deterministic on any host, including a 1-core CI runner, so it is
    // what catches "shards_auto() forgot to write the computed value".
    crpc::server::builder builder;
    builder.shards_auto(3);
    std::unique_ptr<crpc::server> built = builder.build();
    CRPC_REQUIRE(built != nullptr);
    CRPC_CHECK_EQ(std::size_t{2}, built->shard_count());
}

CRPC_TEST(builder_shards_auto_matches_pure_policy) {
    // The parameterless fallback reads the host's physical core count and must
    // agree with the pure policy applied to the same reading.
    crpc::server::builder builder;
    builder.shards_auto();
    std::unique_ptr<crpc::server> built = builder.build();
    CRPC_REQUIRE(built != nullptr);
    CRPC_CHECK_EQ(crpc::rpc::default_shard_count(), built->shard_count());
}

CRPC_TEST_MAIN()
