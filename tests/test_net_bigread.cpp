// CRPC -- net::connection multi-packet read regression tests.
//
// Regression coverage for the TDS big-read hang: a reply larger than one
// transport packet must be readable, a wait against a silent peer must return
// deadline_exceeded inside operation_timeout, and count > max_buffered_bytes
// must make progress instead of starving the delivery thread.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/net/connection.hpp>

using namespace std::chrono_literals;

namespace {

// A hung read must fail the binary, not stall ctest for its whole timeout:
// a watchdog thread aborts the process when the watched operation does not
// finish; the test framework maps abort() to a message plus exit code 3.
class watchdog {
public:
    watchdog(int timeout_ms, const char* what) : what_(what) {
        thread_ = std::thread([this, timeout_ms] {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (std::chrono::steady_clock::now() < deadline) {
                if (done_.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!done_.load(std::memory_order_acquire)) {
                std::fprintf(stderr,
                             "WATCHDOG: %s still running after %d ms (busy-wait regression)\n",
                             what_, timeout_ms);
                std::fflush(stderr);
                std::abort();
            }
        });
    }

    void dismiss() { done_.store(true, std::memory_order_release); }

    ~watchdog() {
        dismiss();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    watchdog(const watchdog&) = delete;
    watchdog& operator=(const watchdog&) = delete;

private:
    const char* what_;
    std::atomic<bool> done_{false};
    std::thread thread_;
};

class noop_completion final : public crpc::io::completion {
public:
    void complete(crpc::io::io_result) noexcept override {}
};

// Server side of the in-process pair: records what the client sends and can
// push scripted replies. Pushed buffers stay alive in `buffers_` because a
// deferred in-process push borrows the caller's span.
class push_source final : public crpc::io::source {
public:
    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        std::lock_guard<std::mutex> lk(mutex_);
        received_.insert(received_.end(), data.begin(), data.end());
        return data.size();
    }

    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

    void attach(crpc::connection* peer) noexcept { peer_ = peer; }

    void push(const std::vector<std::byte>& bytes) {
        buffers_.push_back(bytes);
        const std::vector<std::byte>& stable = buffers_.back();
        peer_->sink().push(
            crpc::span<const std::byte>(stable.data(), stable.size()), &completion_);
    }

private:
    crpc::connection* peer_ = nullptr;
    noop_completion completion_;
    std::mutex mutex_;
    std::vector<std::byte> received_;
    std::deque<std::vector<std::byte>> buffers_;
};

class accept_recorder final : public crpc::accept_sink {
public:
    void on_accept(std::unique_ptr<crpc::connection> conn) noexcept override {
        source_.attach(conn.get());
        conns_.push_back(std::move(conn));
    }

    push_source& source() { return source_; }

private:
    push_source source_;
    std::vector<std::unique_ptr<crpc::connection>> conns_;
};

struct fixture {
    crpc::inproc_backend backend;
    std::unique_ptr<crpc::listener> listener;
    accept_recorder server;
    std::unique_ptr<crpc::net::connection> client;

    explicit fixture(const char* name, const crpc::net::connection_config& config)
        : listener(backend.listen(crpc::endpoint::inproc(name), crpc::listen_options{})) {
        CRPC_CHECK(listener != nullptr);
        if (listener != nullptr) {
            listener->start(&server);
        }
        client = std::make_unique<crpc::net::connection>(config, backend, nullptr);
        CRPC_CHECK(client->is_open());
    }
};

[[nodiscard]] crpc::net::connection_config base_config(const char* endpoint_name) {
    crpc::net::connection_config config;
    config.ep = crpc::endpoint::inproc(endpoint_name);
    config.operation_timeout = std::chrono::seconds(5);
    return config;
}

[[nodiscard]] std::vector<std::byte> pattern(std::size_t count, unsigned char seed) {
    std::vector<std::byte> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = std::byte{static_cast<unsigned char>(seed + static_cast<unsigned char>(i * 31u))};
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> to_bytes(const char* text) {
    const std::size_t n = std::strlen(text);
    std::vector<std::byte> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

void expect_bytes(const std::vector<std::byte>& got, const std::vector<std::byte>& want) {
    CRPC_CHECK(got.size() == want.size());
    CRPC_CHECK(got.size() == want.size() &&
               (want.empty() || std::memcmp(got.data(), want.data(), want.size()) == 0));
}

// The observed TDS hang: one 4096-byte packet body (0xff8 = 4088) that arrives
// in pieces. The client must assemble header + body across deliveries.
constexpr std::size_t kHeaderBytes = 8;
constexpr std::size_t kBodyBytes = 4088;
constexpr int kPackets = 3;

std::vector<std::byte> packet_bytes(int packet) {
    const auto seed = static_cast<unsigned char>(packet * 2);
    std::vector<std::byte> out = pattern(kHeaderBytes + kBodyBytes, seed);
    for (std::size_t i = 0; i < kHeaderBytes; ++i) {
        out[i] = std::byte{static_cast<unsigned char>(0xA0 + packet)};
    }
    return out;
}

} // namespace

CRPC_TEST(net_bigread_multi_packet_reply_assembled_from_pieces) {
    fixture fx("crpc-net-bigread-pieces", base_config("crpc-net-bigread-pieces"));

    std::thread sender([&fx] {
        for (int p = 0; p < kPackets; ++p) {
            const std::vector<std::byte> full = packet_bytes(p);
            std::vector<std::byte> first(full.begin(), full.begin() + 1008);
            fx.server.source().push(first);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::vector<std::byte> rest(full.begin() + 1008, full.end());
            fx.server.source().push(rest);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    watchdog guard(8000, "multi-packet read_exact");
    for (int p = 0; p < kPackets; ++p) {
        const std::vector<std::byte> full = packet_bytes(p);
        const auto head = fx.client->read_exact(kHeaderBytes);
        CRPC_CHECK(head.ok());
        if (head.ok()) {
            expect_bytes(head.value(),
                         std::vector<std::byte>(full.begin(), full.begin() + kHeaderBytes));
        }
        const auto body = fx.client->read_exact(kBodyBytes);
        CRPC_CHECK(body.ok());
        if (body.ok()) {
            expect_bytes(body.value(),
                         std::vector<std::byte>(full.begin() + kHeaderBytes, full.end()));
        }
    }
    guard.dismiss();
    sender.join();
}

CRPC_TEST(net_bigread_read_exact_larger_than_max_buffered) {
    crpc::net::connection_config config = base_config("crpc-net-bigread-overmax");
    config.max_buffered_bytes = 512;
    fixture fx("crpc-net-bigread-overmax", config);

    const std::vector<std::byte> payload = pattern(8192, 0x5A);
    std::thread sender([&fx, &payload] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fx.server.source().push(payload);
    });

    watchdog guard(8000, "read_exact(8192) with max_buffered_bytes=512");
    const auto got = fx.client->read_exact(payload.size());
    guard.dismiss();
    CRPC_CHECK(got.ok());
    if (got.ok()) {
        expect_bytes(got.value(), payload);
    }
    sender.join();
}

CRPC_TEST(net_bigread_partial_reply_then_silence_times_out) {
    crpc::net::connection_config config = base_config("crpc-net-bigread-silent");
    config.operation_timeout = std::chrono::seconds(1);
    fixture fx("crpc-net-bigread-silent", config);

    const std::vector<std::byte> prefix = pattern(kHeaderBytes + 1000, 0x11);
    fx.server.source().push(prefix);

    const auto head = fx.client->read_exact(kHeaderBytes);
    CRPC_CHECK(head.ok());
    if (head.ok()) {
        expect_bytes(head.value(), std::vector<std::byte>(prefix.begin(), prefix.begin() + kHeaderBytes));
    }

    watchdog guard(8000, "read_exact(4088) against a silent peer");
    const auto started = std::chrono::steady_clock::now();
    const auto got = fx.client->read_exact(kBodyBytes);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    guard.dismiss();
    CRPC_CHECK_FALSE(got.ok());
    CRPC_CHECK(!got.ok() && got.error().code == crpc::rpc_code::deadline_exceeded);
    CRPC_CHECK(elapsed >= std::chrono::milliseconds(900));
    CRPC_CHECK(elapsed <= std::chrono::seconds(7));
    CRPC_CHECK_EQ(std::size_t{1000}, fx.client->buffered());
}

CRPC_TEST(net_bigread_read_until_waits_for_bytes_beyond_the_scan) {
    fixture fx("crpc-net-bigread-until", base_config("crpc-net-bigread-until"));

    std::thread sender([&fx] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fx.server.source().push(to_bytes("abc"));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        fx.server.source().push(to_bytes("def\ngh\n"));
    });

    watchdog guard(8000, "read_until across a delimiter-free prefix");
    const auto first = fx.client->read_until(std::byte{'\n'});
    CRPC_CHECK(first.ok());
    if (first.ok()) {
        expect_bytes(first.value(), to_bytes("abcdef\n"));
    }
    guard.dismiss();
    sender.join();

    const auto second = fx.client->read_until(std::byte{'\n'});
    CRPC_CHECK(second.ok());
    if (second.ok()) {
        expect_bytes(second.value(), to_bytes("gh\n"));
    }
}

CRPC_TEST_MAIN()
