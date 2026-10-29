// CRPC -- net listener edge cases on real localhost sockets.
//
// This file holds the platform-behaviour proofs that the listener layer is
// built on: the second-bind-must-fail rule (the SO_REUSEADDR trap), IPv6 `::1`
// and IPv4-over-IPv6 dual stack, the AF_UNIX loop, and cancellation of a
// blocked accept. Server threads only record outcome strings; the main thread
// asserts them after `join` so the framework counters are never raced.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/net/address.hpp>
#include <crpc/net/channel.hpp>
#include <crpc/net/connector.hpp>
#include <crpc/net/listener.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::net::address;
using crpc::net::listener;
using crpc::net::tcp_stream;

[[nodiscard]] std::string temp_unix_path(const char* tag) {
    static unsigned long sequence = 0;
    ++sequence;
    return (std::filesystem::temp_directory_path() /
            ("crpc-net-" + std::string(tag) + "-" +
             std::to_string(sequence) + "-" +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count()) +
             ".sock"))
        .string();
}

struct outcome {
    std::mutex mutex;
    bool ok = true;
    std::string error;

    void fail(const std::string& what) {
        std::lock_guard<std::mutex> lock(mutex);
        ok = false;
        error = what;
    }
    [[nodiscard]] bool good() const { return ok; }
    [[nodiscard]] const std::string& message() const { return error; }
};

} // namespace

CRPC_TEST(net_second_listener_on_same_port_must_fail) {
    auto first = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }
    const crpc::u16 port = first.value()->local().port();
    const address bound_addr = address::tcp("127.0.0.1", port);

    // Default options (reuse_address on for restarts). On Windows the safe
    // SO_EXCLUSIVEADDRUSE must make a second bind fail; on POSIX SO_REUSEADDR
    // only forgives TIME_WAIT, so a live second bind fails there too.
    auto second = listener::bind(bound_addr);
    CRPC_CHECK_FALSE(second.ok());
    if (!second.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(second.code()));
    }

    // Explicitly opting out of reuse must fail the same way on every platform.
    crpc::net::listen_options strict;
    strict.reuse_address = false;
    auto third = listener::bind(bound_addr, strict);
    CRPC_CHECK_FALSE(third.ok());
    if (!third.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(third.code()));
    }
}

CRPC_TEST(net_accept_deadline) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    auto accepted = bound.value()->accept(std::chrono::milliseconds(120));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_FALSE(accepted.ok());
    if (!accepted.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                      static_cast<int>(accepted.code()));
    }
    CRPC_CHECK(elapsed >= std::chrono::milliseconds(60));
}

CRPC_TEST(net_accept_cancelled_by_close) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    std::atomic<bool> returned{false};
    std::atomic<int> code{static_cast<int>(crpc::rpc_code::ok)};
    std::thread waiter([&] {
        auto result = bound.value()->accept(std::chrono::nanoseconds::zero());
        code.store(static_cast<int>(result.code()), std::memory_order_release);
        returned.store(true, std::memory_order_release);
    });

    // Let the waiter block in select, then cancel from this thread.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto started = std::chrono::steady_clock::now();
    bound.value()->close();
    CRPC_CHECK(crpc_test::wait_for([&] { return returned.load(); }, 4000));
    waiter.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down),
                  code.load());
    CRPC_CHECK(elapsed < std::chrono::seconds(3));
    CRPC_CHECK_FALSE(bound.value()->is_open());
    // Idempotent.
    bound.value()->close();
}

CRPC_TEST(net_listener_run_accept_loop) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    auto& server = bound.value();
    const address target = address::tcp(server->local().host(), server->local().port());

    outcome server_outcome;
    std::thread service([&] {
        const crpc::status loop = server->run([&](std::unique_ptr<tcp_stream> stream) {
            auto request = stream->read_exact(1);
            if (!request) {
                server_outcome.fail("run loop stream read failed");
            } else {
                (void)stream->write(
                    crpc::span<const std::byte>(request.value().data(),
                                                request.value().size()));
            }
            return false;  // one connection, then stop
        });
        if (!loop.ok()) {
            server_outcome.fail("accept loop returned an error");
        }
    });

    auto client = crpc::net::connect(target);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        server->close();
        service.join();
        return;
    }
    const std::byte byte = std::byte{'z'};
    CRPC_CHECK(client.value()->write(crpc::span<const std::byte>(&byte, 1)).ok());
    auto back = client.value()->read_exact(1);
    CRPC_CHECK(back.ok());
    if (back.ok()) {
        CRPC_CHECK(back.value()[0] == std::byte{'z'});
    }
    service.join();
    CRPC_CHECK(server_outcome.good());
}

CRPC_TEST(net_peer_and_local_addresses_reported) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    auto& server = bound.value();
    const address target = address::tcp(server->local().host(), server->local().port());

    outcome server_outcome;
    std::thread service([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (!accepted) {
            server_outcome.fail("accept failed");
            return;
        }
        if (accepted.value()->peer().host() != "127.0.0.1") {
            server_outcome.fail("peer address is not loopback: " +
                                accepted.value()->peer().to_string());
        }
    });

    auto client = crpc::net::connect(target);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        server->close();
        service.join();
        return;
    }
    // The client's local address is whatever ephemeral interface was picked.
    CRPC_CHECK(client.value()->local().valid());
    CRPC_CHECK_EQ(std::string("127.0.0.1"), client.value()->peer().host());
    service.join();
    CRPC_CHECK(server_outcome.good());
    if (!server_outcome.good()) {
        CRPC_CHECK(server_outcome.message().empty());
    }
}

CRPC_TEST(net_ipv6_loopback) {
    auto bound = listener::bind(address::tcp("::1", 0));
    if (!bound) {
        crpc_test::skip("IPv6 loopback ::1 is not available on this host");
        return;
    }
    auto& server = bound.value();
    const address target = address::tcp("::1", server->local().port());

    outcome server_outcome;
    std::thread service([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (!accepted) {
            server_outcome.fail("ipv6 accept failed");
            return;
        }
        auto request = accepted.value()->read_exact(2);
        if (!request) {
            server_outcome.fail("ipv6 read failed");
        } else {
            (void)accepted.value()->write(
                crpc::span<const std::byte>(request.value().data(),
                                            request.value().size()));
        }
    });

    crpc::net::connect_options options;
    options.timeout = std::chrono::seconds(2);
    auto client = crpc::net::connect(target, options);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        server->close();
        service.join();
        return;
    }
    const std::byte payload[] = {std::byte{'v'}, std::byte{'6'}};
    CRPC_CHECK(client.value()->write(crpc::span<const std::byte>(payload)).ok());
    auto back = client.value()->read_exact(2);
    CRPC_CHECK(back.ok());
    if (back.ok()) {
        CRPC_CHECK(back.value()[0] == std::byte{'v'});
        CRPC_CHECK(back.value()[1] == std::byte{'6'});
    }
    service.join();
    CRPC_CHECK(server_outcome.good());
}

CRPC_TEST(net_dual_stack_listener_accepts_v4) {
    crpc::net::listen_options options;
    options.ipv6_only = false;  // dual stack: v4-mapped connect must land here
    auto bound = listener::bind(address::tcp("::", 0), options);
    if (!bound) {
        crpc_test::skip("IPv6 wildcard bind is not available on this host");
        return;
    }
    auto& server = bound.value();
    const crpc::u16 port = server->local().port();

    outcome server_outcome;
    std::thread service([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (!accepted) {
            server_outcome.fail("dual-stack accept failed");
            return;
        }
        auto request = accepted.value()->read_exact(3);
        if (!request) {
            server_outcome.fail("dual-stack read failed");
        } else {
            (void)accepted.value()->write(
                crpc::span<const std::byte>(request.value().data(),
                                            request.value().size()));
        }
    });

    crpc::net::connect_options co;
    co.timeout = std::chrono::seconds(2);
    auto client = crpc::net::connect(address::tcp("127.0.0.1", port), co);
    if (!client) {
        server->close();
        service.join();
        crpc_test::skip("this host does not map v4 onto the v6 wildcard listener");
        return;
    }
    const std::byte payload[] = {std::byte{'d'}, std::byte{'s'}, std::byte{'t'}};
    CRPC_CHECK(client.value()->write(crpc::span<const std::byte>(payload)).ok());
    auto back = client.value()->read_exact(3);
    CRPC_CHECK(back.ok());
    if (back.ok()) {
        for (std::size_t i = 0; i < 3; ++i) {
            CRPC_CHECK(back.value()[i] == payload[i]);
        }
    }
    service.join();
    CRPC_CHECK(server_outcome.good());
}

CRPC_TEST(net_unix_domain_socket_loop) {
    const bool supported = crpc::net::unix_socket_supported();
    if (!supported) {
        crpc_test::skip("AF_UNIX is not supported on this host");
        return;
    }
    const std::string path = temp_unix_path("echo");
    auto bound = listener::bind(address::unix_path(path));
    if (!bound) {
        std::error_code remove_error;
        std::filesystem::remove(path, remove_error);
        crpc_test::skip("AF_UNIX bind failed on this host");
        return;
    }
    auto& server = bound.value();

    outcome server_outcome;
    std::thread service([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (!accepted) {
            server_outcome.fail("unix accept failed");
            return;
        }
        auto request = accepted.value()->read_exact(5);
        if (!request) {
            server_outcome.fail("unix read failed");
        } else {
            (void)accepted.value()->write(
                crpc::span<const std::byte>(request.value().data(),
                                            request.value().size()));
        }
    });

    crpc::net::connect_options co;
    co.timeout = std::chrono::seconds(2);
    auto client = crpc::net::connect(address::unix_path(path), co);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        server->close();
        service.join();
        std::error_code remove_error;
        std::filesystem::remove(path, remove_error);
        return;
    }
    const std::byte payload[] = {std::byte{'u'}, std::byte{'n'}, std::byte{'i'},
                                 std::byte{'x'}, std::byte{'!'}};
    CRPC_CHECK(client.value()->write(crpc::span<const std::byte>(payload)).ok());
    auto back = client.value()->read_exact(5);
    CRPC_CHECK(back.ok());
    if (back.ok()) {
        for (std::size_t i = 0; i < 5; ++i) {
            CRPC_CHECK(back.value()[i] == payload[i]);
        }
    }
    service.join();
    CRPC_CHECK(server_outcome.good());

    server->close();
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
}

CRPC_TEST_MAIN()