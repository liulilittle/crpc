// CRPC -- shared connection-pool contract tests.
//
// `crpc::net::connection_pool` is used by the Redis client, the memcached client
// and the database adapters, so these rules are tested once here rather than three
// times downstream: idle/lifetime bounds, a double release, close semantics and the
// open counter. The backend is in-process, so no server is needed.
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/net/connection.hpp>
#include <crpc/runtime.hpp>

namespace {

using crpc::net::connection_pool;
using crpc::net::pool_config;

// Each connect gets its own live in-process pair. The server side must be kept
// alive: dropping it delivers EOF to the client link, and the pool would then
// correctly discard every connection it just opened.
class pair_backend final : public crpc::backend_factory {
public:
    std::unique_ptr<crpc::connection> connect(const crpc::endpoint&,
                                              const crpc::connect_options&) override {
        auto pair = crpc::inproc_pair::make_typed();
        peers_.push_back(std::move(pair.second));
        return std::move(pair.first);
    }

    std::unique_ptr<crpc::listener> listen(const crpc::endpoint&,
                                          const crpc::listen_options&) override {
        return nullptr;
    }

    [[nodiscard]] std::size_t connects() const noexcept { return peers_.size(); }

private:
    std::vector<std::unique_ptr<crpc::connection>> peers_;
};

[[nodiscard]] pool_config base_config(const char* endpoint_name) {
    pool_config config;
    config.connection.ep = crpc::endpoint::inproc(endpoint_name);
    config.min_size = 0;
    config.max_size = 2;
    config.max_idle = 2;
    config.acquire_timeout = std::chrono::milliseconds(120);
    config.idle_timeout = std::chrono::seconds(60);
    config.max_lifetime = std::chrono::minutes(30);
    return config;
}

} // namespace

CRPC_TEST(net_pool_opens_and_reuses) {
    pair_backend backend;
    connection_pool pool(base_config("crpc-net-pool-basic"), backend);
    CRPC_CHECK_EQ(std::size_t{0}, pool.total());

    {
        auto acquired = pool.acquire();
        CRPC_CHECK(acquired.ok());
        if (!acquired.ok()) {
            return;
        }
        CRPC_CHECK(acquired.value()->is_open());
        CRPC_CHECK_EQ(std::size_t{1}, pool.in_use());
        CRPC_CHECK_EQ(std::uint64_t{1}, pool.opened());
        // The raw acquire/release API requires an explicit release; there is no
        // scope-based lease here (db::pool provides one).
        pool.release(acquired.value());
    }
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());
    CRPC_CHECK_EQ(std::size_t{0}, pool.in_use());

    // The next acquire reuses the idle link instead of opening another.
    auto second = pool.acquire();
    CRPC_CHECK(second.ok());
    CRPC_CHECK_EQ(std::uint64_t{1}, pool.opened());
    CRPC_CHECK_EQ(std::size_t{1}, pool.total());
}

CRPC_TEST(net_pool_double_release_does_not_duplicate) {
    pair_backend backend;
    connection_pool pool(base_config("crpc-net-pool-double"), backend);
    auto acquired = pool.acquire();
    CRPC_CHECK(acquired.ok());
    if (!acquired.ok()) {
        return;
    }
    std::shared_ptr<crpc::net::connection> link = acquired.value();
    pool.release(link);
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());

    // Releasing the same handle again must not insert it twice: two borrowers would
    // then share one connection, and their replies would interleave.
    pool.release(link);
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());
    CRPC_CHECK_EQ(std::size_t{1}, pool.total());
}

CRPC_TEST(net_pool_max_idle_closes_the_excess) {
    pair_backend backend;
    pool_config config = base_config("crpc-net-pool-maxidle");
    config.max_size = 3;
    config.max_idle = 1;
    connection_pool pool(config, backend);

    auto first = pool.acquire();
    auto second = pool.acquire();
    CRPC_CHECK(first.ok());
    CRPC_CHECK(second.ok());
    if (!first.ok() || !second.ok()) {
        return;
    }
    pool.release(first.value());
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());
    // The second release exceeds max_idle, so that link is closed instead of kept.
    pool.release(second.value());
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());
    CRPC_CHECK_EQ(std::size_t{1}, pool.total());
    // Both were opened; one was dropped, so the counter proves the difference
    // between "opened" and "live".
    CRPC_CHECK_EQ(std::uint64_t{2}, pool.opened());
}

CRPC_TEST(net_pool_idle_timeout_discards_on_acquire) {
    pair_backend backend;
    pool_config config = base_config("crpc-net-pool-idletimeout");
    config.idle_timeout = std::chrono::milliseconds(20);
    connection_pool pool(config, backend);

    {
        auto acquired = pool.acquire();
        CRPC_CHECK(acquired.ok());
        if (!acquired.ok()) {
            return;
        }
        pool.release(acquired.value());
    }
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    auto second = pool.acquire();
    CRPC_CHECK(second.ok());
    // The expired link was dropped and a fresh one opened, not handed out stale.
    CRPC_CHECK_EQ(std::uint64_t{2}, pool.opened());
    CRPC_CHECK_EQ(std::size_t{1}, pool.total());
}

CRPC_TEST(net_pool_lifetime_discards_on_release) {
    pair_backend backend;
    pool_config config = base_config("crpc-net-pool-lifetime");
    config.max_lifetime = std::chrono::milliseconds(20);
    connection_pool pool(config, backend);

    auto acquired = pool.acquire();
    CRPC_CHECK(acquired.ok());
    if (!acquired.ok()) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    pool.release(acquired.value());
    // The link is older than the cap, so it is closed rather than returned: this is
    // what recycles server-side state a long-lived connection accumulates.
    CRPC_CHECK_EQ(std::size_t{0}, pool.idle());
    CRPC_CHECK_EQ(std::size_t{0}, pool.total());
    CRPC_CHECK_EQ(std::uint64_t{1}, pool.opened());
}

CRPC_TEST(net_pool_close_rejects_acquire_and_closes_idle) {
    pair_backend backend;
    connection_pool pool(base_config("crpc-net-pool-close"), backend);
    {
        auto acquired = pool.acquire();
        CRPC_CHECK(acquired.ok());
        if (!acquired.ok()) {
            return;
        }
        pool.release(acquired.value());
    }
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());

    pool.close();
    CRPC_CHECK_EQ(std::size_t{0}, pool.idle());
    CRPC_CHECK_EQ(std::size_t{0}, pool.total());
    // Idempotent.
    pool.close();

    auto rejected = pool.acquire();
    CRPC_CHECK_FALSE(rejected.ok());
    if (!rejected.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down),
                      static_cast<int>(rejected.error().code));
    }
}

CRPC_TEST(net_pool_is_bounded_and_reports_exhaustion) {
    pair_backend backend;
    pool_config config = base_config("crpc-net-pool-bounded");
    config.max_size = 1;
    config.max_idle = 1;
    config.acquire_timeout = std::chrono::milliseconds(60);
    connection_pool pool(config, backend);

    auto first = pool.acquire();
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    auto second = pool.acquire();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_FALSE(second.ok());
    if (!second.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                      static_cast<int>(second.error().code));
        CRPC_CHECK(crpc::is_retryable(second.error().code));
    }
    CRPC_CHECK(elapsed >= std::chrono::milliseconds(40));
    CRPC_CHECK_EQ(std::uint64_t{1}, pool.opened());
}

CRPC_TEST(net_pool_waiter_is_woken_by_release) {
    pair_backend backend;
    pool_config config = base_config("crpc-net-pool-wake");
    config.max_size = 1;
    config.max_idle = 1;
    config.acquire_timeout = std::chrono::seconds(5);
    connection_pool pool(config, backend);

    auto first = pool.acquire();
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }

    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        auto link = pool.acquire();
        acquired.store(link.ok(), std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    pool.release(first.value());
    waiter.join();
    CRPC_CHECK(acquired.load(std::memory_order_acquire));
    // The waiter reused the released link; no second connection was opened.
    CRPC_CHECK_EQ(std::uint64_t{1}, pool.opened());
}

CRPC_TEST_MAIN()