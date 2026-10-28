// CRPC -- net backoff and eager retry tests.
//
// `net::backoff` is pure delay computation, so the exponential sequence, the
// ceiling and the jitter bounds are asserted against the exact values a caller
// would observe, with no sleeps. The integration half uses `connect_with_retry`
// against a real localhost listener and only sleeps the submillisecond delays
// the backoff itself computed.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/net/address.hpp>
#include <crpc/net/backoff.hpp>
#include <crpc/net/channel.hpp>
#include <crpc/net/connector.hpp>
#include <crpc/net/listener.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::net::address;
using crpc::net::backoff;
using crpc::net::backoff_config;
using crpc::net::connect_with_retry;
using crpc::net::retry_report;

// A port on loopback that is free at the moment of the call: bind an ephemeral
// listener, read its port, let it close.
[[nodiscard]] crpc::u16 free_loopback_port() {
    auto bound = crpc::net::listener::bind(address::tcp("127.0.0.1", 0));
    if (!bound) {
        return 0;
    }
    return bound.value()->local().port();
}

[[nodiscard]] backoff_config no_jitter() {
    backoff_config config;
    config.base = std::chrono::milliseconds(10);
    config.ceiling = std::chrono::milliseconds(240);
    config.multiplier = 2;
    config.jitter = 0.0;
    config.max_attempts = 0;
    return config;
}

} // namespace

CRPC_TEST(net_backoff_expotential_sequence) {
    backoff schedule(no_jitter());
    const std::chrono::nanoseconds expected[] = {
        std::chrono::milliseconds(10),  // base
        std::chrono::milliseconds(20),  // x2
        std::chrono::milliseconds(40),  // x4
        std::chrono::milliseconds(80),  // x8
        std::chrono::milliseconds(160), // x16
        std::chrono::milliseconds(240), // clamped at ceiling
        std::chrono::milliseconds(240), // stays clamped
    };
    for (const std::chrono::nanoseconds want : expected) {
        CRPC_CHECK_EQ(want.count(), schedule.next_attempt_delay().count());
    }
    CRPC_CHECK_EQ(std::size_t{7}, schedule.attempts());
    CRPC_CHECK_FALSE(schedule.exhausted());
    schedule.reset();
    CRPC_CHECK_EQ(std::size_t{0}, schedule.attempts());
    CRPC_CHECK_EQ(std::chrono::nanoseconds(std::chrono::milliseconds(10)).count(),
                  schedule.next_attempt_delay().count());
}

CRPC_TEST(net_backoff_exhaustion) {
    backoff_config config = no_jitter();
    config.max_attempts = 3;
    backoff schedule(config);
    CRPC_CHECK_FALSE(schedule.exhausted());
    (void)schedule.next_attempt_delay();
    CRPC_CHECK_FALSE(schedule.exhausted());
    (void)schedule.next_attempt_delay();
    CRPC_CHECK_FALSE(schedule.exhausted());
    (void)schedule.next_attempt_delay();
    CRPC_CHECK(schedule.exhausted());
}

CRPC_TEST(net_backoff_jitter_stays_in_bounds) {
    backoff_config config = no_jitter();
    config.base = std::chrono::milliseconds(100);
    config.ceiling = std::chrono::seconds(10);
    config.jitter = 0.25;
    backoff schedule(config, 12345u);

    const double lo = 100.0 * 0.75;
    const double hi = 100.0 * 1.25;
    for (int i = 0; i < 500; ++i) {
        schedule.reset();  // keep every sample at the jittered base, not 2^i
        const double delay_ms =
            static_cast<double>(schedule.next_attempt_delay().count()) / 1e6;
        CRPC_CHECK(delay_ms >= lo);
        CRPC_CHECK(delay_ms <= hi);
        if (delay_ms < lo || delay_ms > hi) {
            return;
        }
    }
}

CRPC_TEST(net_backoff_deterministic_for_seed) {
    const backoff_config config = no_jitter();
    backoff first(config, 7u);
    backoff second(config, 7u);
    for (int i = 0; i < 20; ++i) {
        CRPC_CHECK_EQ(first.next_attempt_delay().count(),
                      second.next_attempt_delay().count());
    }
}

CRPC_TEST(net_retry_refused_reports_sequence) {
    const crpc::u16 port = free_loopback_port();
    CRPC_CHECK(port != 0);
    if (port == 0) {
        return;
    }
    crpc::net::connect_options options;
    options.timeout = std::chrono::milliseconds(400);

    backoff_config config;
    config.base = std::chrono::milliseconds(5);
    config.ceiling = std::chrono::milliseconds(40);
    config.multiplier = 2;
    config.jitter = 0.0;
    config.max_attempts = 3;

    retry_report report;
    auto result = connect_with_retry(address::tcp("127.0.0.1", port), options,
                                     config, &report);
    CRPC_CHECK_FALSE(result.ok());
    if (result.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{3}, report.attempts);
    CRPC_CHECK_EQ(std::size_t{2}, report.delays.size());
    if (report.delays.size() == 2) {
        CRPC_CHECK_EQ(std::chrono::nanoseconds(std::chrono::milliseconds(5)).count(),
                      report.delays[0].count());
        CRPC_CHECK_EQ(std::chrono::nanoseconds(std::chrono::milliseconds(10)).count(),
                      report.delays[1].count());
    }
    // A closed endpoint refuses on hosts that RST closed ports (connection_refused)
    // and silently drops the SYN on hosts that do not (deadline_exceeded). Both are
    // legitimate Windows connect outcomes; neither is a "success".
    const bool refused = report.last_error == crpc::rpc_code::connection_refused ||
                         report.last_error == crpc::rpc_code::deadline_exceeded;
    CRPC_CHECK(refused);
}

CRPC_TEST(net_retry_recovers_once_listener_starts) {
    const crpc::u16 port = free_loopback_port();
    CRPC_CHECK(port != 0);
    if (port == 0) {
        return;
    }

    // A listener appears a little later; the first attempts must fail and a
    // later one must win.
    std::atomic<bool> up{false};
    std::thread server([port, &up] {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        auto bound =
            crpc::net::listener::bind(address::tcp("127.0.0.1", port));
        if (!bound) {
            up.store(true, std::memory_order_release);
            return;
        }
        up.store(true, std::memory_order_release);
        auto accepted = bound.value()->accept(std::chrono::seconds(5));
        if (accepted) {
            auto echoed = accepted.value()->read_exact(3);
            if (echoed) {
                (void)accepted.value()->write(
                    crpc::span<const std::byte>(echoed.value().data(),
                                                echoed.value().size()));
            }
        }
    });

    crpc::net::connect_options options;
    options.timeout = std::chrono::milliseconds(300);

    backoff_config config;
    config.base = std::chrono::milliseconds(5);
    config.ceiling = std::chrono::milliseconds(40);
    config.multiplier = 2;
    config.jitter = 0.0;
    config.max_attempts = 8;

    retry_report report;
    auto result = connect_with_retry(address::tcp("127.0.0.1", port), options,
                                     config, &report);
    CRPC_CHECK(result.ok());
    if (result.ok()) {
        CRPC_CHECK(result.value()->is_open());
        const std::byte payload[] = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, };
        CRPC_CHECK(result.value()->write(crpc::span<const std::byte>(payload)).ok());
        auto back = result.value()->read_exact(3);
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK_EQ(std::size_t{3}, back.value().size());
            CRPC_CHECK(back.value()[0] == std::byte{'a'});
            CRPC_CHECK(back.value()[1] == std::byte{'b'});
            CRPC_CHECK(back.value()[2] == std::byte{'c'});
        }
        // The first attempt was refused; the report must show a retried connect
        // and the recorded delays before the successful one.
        CRPC_CHECK(report.attempts >= std::size_t{2});
        CRPC_CHECK(report.delays.size() == report.attempts - 1);
        if (!report.delays.empty()) {
            CRPC_CHECK_EQ(std::chrono::nanoseconds(std::chrono::milliseconds(5)).count(),
                          report.delays[0].count());
        }
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                      static_cast<int>(report.last_error));
    }
    server.join();
}

CRPC_TEST_MAIN()