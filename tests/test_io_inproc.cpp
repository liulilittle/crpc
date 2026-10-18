// The shared harness reads environment variables with std::getenv; MSVC's
// secure-CRT deprecation warning is noise for a test binary.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

// CRPC -- in-process transport backend tests.
//
// The tests exercise the contract, not the implementation: credit accounting,
// bounded queues, exactly-once completion, terminal notifications, fault hooks
// and the lifetime of the pushed span.
#include <crpc/io.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_framework.hpp"

namespace {

using namespace crpc;

span<const std::byte> bytes_of(const std::string& s) {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()),
                                 s.size());
}

span<const std::byte> bytes_of(const std::vector<std::byte>& v) {
    return span<const std::byte>(v.data(), v.size());
}

std::string text_of(const std::vector<std::byte>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

struct recording_source final : io::source {
    std::vector<std::byte> received;
    std::size_t credit = static_cast<std::size_t>(-1);

    std::atomic<int> data_calls{0};
    std::atomic<int> eof_calls{0};
    std::atomic<int> error_calls{0};
    std::atomic<int> last_error{0};

    std::size_t on_data(span<const std::byte> data) noexcept override {
        data_calls.fetch_add(1);
        const std::size_t n =
            credit < data.size() ? credit : data.size();
        received.insert(received.end(), data.begin(),
                        data.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

    void on_eof() noexcept override { eof_calls.fetch_add(1); }

    void on_error(std::error_code ec) noexcept override {
        last_error.store(ec.value());
        error_calls.fetch_add(1);
    }
};

struct recording_completion final : io::completion {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> bytes{0};
    std::atomic<bool> ok{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> broken{false};

    void complete(io::io_result result) noexcept override {
        bytes.store(result.bytes);
        ok.store(!result.ec);
        canceled.store(result.ec == std::errc::operation_canceled);
        broken.store(result.ec == std::errc::broken_pipe);
        calls.fetch_add(1);
    }
};

struct lifetime_completion final : io::completion {
    const std::vector<std::byte>* buffer = nullptr;
    int calls = 0;
    bool intact = false;

    void complete(io::io_result) noexcept override {
        ++calls;
        intact = buffer != nullptr && buffer->size() == 8;
        if (intact) {
            for (std::size_t i = 0; i < buffer->size(); ++i) {
                if ((*buffer)[i] != static_cast<std::byte>('a' + i)) {
                    intact = false;
                }
            }
        }
    }
};

CRPC_TEST(inproc_round_trip_both_directions) {
    // Sources are declared before the connection pair on purpose: destruction
    // runs in reverse order, so the pair (and its close(), which delivers a
    // final on_eof to the peer's bound source) is torn down while both sources
    // are still alive. The backend stores a raw pointer to the bound source;
    // destroying the source first leaves that pointer dangling and close()
    // calls on_eof through it (ASan: pure virtual call / use-after-free).
    recording_source left;
    recording_source right;
    auto pair = inproc_pair::make_typed();
    pair.first->bind_source(&left);
    pair.second->bind_source(&right);

    const std::string to_right = "hello";
    const std::string to_left = "world!";
    recording_completion first;
    recording_completion second;

    pair.first->sink().push(bytes_of(to_right), &first);
    pair.second->sink().push(bytes_of(to_left), &second);

    CRPC_CHECK_EQ(1, first.calls.load());
    CRPC_CHECK_EQ(1, second.calls.load());
    CRPC_CHECK(first.ok.load());
    CRPC_CHECK(second.ok.load());
    CRPC_CHECK_EQ(to_right.size(), first.bytes.load());
    CRPC_CHECK_EQ(to_left.size(), second.bytes.load());
    CRPC_CHECK_EQ(to_left, text_of(left.received));
    CRPC_CHECK_EQ(to_right, text_of(right.received));
}

CRPC_TEST(inproc_partial_consumption_pauses_until_resume) {
    // See inproc_round_trip_both_directions: the sources must outlive the pair.
    recording_source left;
    recording_source right;
    auto pair = inproc_pair::make_typed();
    pair.first->bind_source(&left);
    pair.second->bind_source(&right);
    right.credit = 1;

    recording_completion done;
    const std::string payload = "abcde";
    pair.first->sink().push(bytes_of(payload), &done);

    CRPC_CHECK_EQ(1, done.calls.load());
    CRPC_CHECK_EQ(1, right.data_calls.load());
    CRPC_CHECK_EQ(std::string("a"), text_of(right.received));

    for (int i = 0; i < 4; ++i) {
        pair.second->resume_read();
    }
    CRPC_CHECK_EQ(5, right.data_calls.load());
    CRPC_CHECK_EQ(payload, text_of(right.received));

    pair.second->resume_read();
    CRPC_CHECK_EQ(5, right.data_calls.load());
}

CRPC_TEST(inproc_bounded_queue_defers_completion) {
    inproc_options opt;
    opt.max_queued_bytes = 8;
    auto pair = inproc_pair::make_typed(opt);
    recording_source right;
    pair.second->bind_source(&right);
    right.credit = 0;

    recording_completion first;
    recording_completion second;
    const std::string eight = "12345678";
    const std::string four = "ABCD";

    pair.first->sink().push(bytes_of(eight), &first);
    CRPC_CHECK_EQ(1, first.calls.load());
    CRPC_CHECK(first.ok.load());
    CRPC_CHECK_EQ(1, right.data_calls.load());
    CRPC_CHECK_EQ(0u, right.received.size());

    pair.first->sink().push(bytes_of(four), &second);
    CRPC_CHECK_EQ(0, second.calls.load());

    right.credit = 100;
    pair.second->resume_read();

    CRPC_CHECK_EQ(1, second.calls.load());
    CRPC_CHECK(second.ok.load());
    CRPC_CHECK_EQ(four.size(), second.bytes.load());
    CRPC_CHECK_EQ(std::string("12345678ABCD"), text_of(right.received));
}

CRPC_TEST(inproc_large_push_drains_incrementally) {
    inproc_options opt;
    opt.max_queued_bytes = 8;
    auto pair = inproc_pair::make_typed(opt);
    recording_source right;
    pair.second->bind_source(&right);
    right.credit = 3;

    const std::string payload = "0123456789ABCDEFGHIJ";
    recording_completion done;
    pair.first->sink().push(bytes_of(payload), &done);
    CRPC_CHECK_EQ(0, done.calls.load());

    for (int i = 0; i < 64 && done.calls.load() == 0; ++i) {
        pair.second->resume_read();
    }

    CRPC_CHECK_EQ(1, done.calls.load());
    CRPC_CHECK(done.ok.load());
    CRPC_CHECK_EQ(payload.size(), done.bytes.load());
    CRPC_CHECK_EQ(payload, text_of(right.received));
}

CRPC_TEST(inproc_close_cancels_inflight_push_once) {
    inproc_options opt;
    opt.max_queued_bytes = 4;
    auto pair = inproc_pair::make_typed(opt);
    recording_source right;
    pair.second->bind_source(&right);
    right.credit = 0;

    recording_completion first;
    recording_completion second;
    const std::string four = "AAAA";
    pair.first->sink().push(bytes_of(four), &first);
    CRPC_CHECK_EQ(1, first.calls.load());
    pair.first->sink().push(bytes_of(four), &second);
    CRPC_CHECK_EQ(0, second.calls.load());

    pair.first->close();

    CRPC_CHECK_EQ(1, second.calls.load());
    CRPC_CHECK(second.canceled.load());
    CRPC_CHECK_EQ(0u, second.bytes.load());
    CRPC_CHECK_EQ(1, right.eof_calls.load());
    CRPC_CHECK_EQ(0u, right.received.size());
    CRPC_CHECK_FALSE(pair.first->is_open());

    pair.second->resume_read();
    CRPC_CHECK_EQ(0u, right.received.size());

    recording_completion third;
    pair.second->sink().push(bytes_of(std::string("x")), &third);
    CRPC_CHECK_EQ(1, third.calls.load());
    CRPC_CHECK(third.broken.load());

    pair.first->close();
    CRPC_CHECK_EQ(1, right.eof_calls.load());
    CRPC_CHECK_EQ(1, second.calls.load());
}

CRPC_TEST(inproc_concurrent_push_and_close) {
    inproc_options opt;
    opt.max_queued_bytes = 1024;
    auto pair = inproc_pair::make_typed(opt);
    recording_source right;
    pair.second->bind_source(&right);

    constexpr int kPushes = 20000;
    std::vector<std::unique_ptr<recording_completion>> completions;
    completions.reserve(static_cast<std::size_t>(kPushes));
    for (int i = 0; i < kPushes; ++i) {
        completions.emplace_back(new recording_completion());
    }

    std::atomic<bool> started{false};
    std::thread pusher([&] {
        started.store(true, std::memory_order_release);
        for (int i = 0; i < kPushes; ++i) {
            const std::string chunk(16, static_cast<char>('a' + (i % 26)));
            pair.first->sink().push(
                bytes_of(chunk),
                completions[static_cast<std::size_t>(i)].get());
        }
    });
    std::thread closer([&] {
        while (!started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        pair.first->close();
    });

    pusher.join();
    closer.join();

    int total = 0;
    for (const auto& c : completions) {
        CRPC_CHECK_EQ(1, c->calls.load());
        CRPC_CHECK(c->ok.load() || c->canceled.load() || c->broken.load());
        total += c->calls.load();
    }
    CRPC_CHECK_EQ(kPushes, total);

    const int deliveries = right.data_calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CRPC_CHECK_EQ(deliveries, right.data_calls.load());
}

CRPC_TEST(inproc_fault_drop_bytes_creates_observable_gap) {
    auto pair = inproc_pair::make_typed();
    recording_source right;
    pair.second->bind_source(&right);
    pair.first->faults().drop_bytes = 2;

    recording_completion done;
    pair.first->sink().push(bytes_of(std::string("abcdef")), &done);

    CRPC_CHECK_EQ(1, done.calls.load());
    CRPC_CHECK(done.ok.load());
    CRPC_CHECK_EQ(std::string("cdef"), text_of(right.received));
}

CRPC_TEST(inproc_fault_reorder_two_chunks) {
    auto pair = inproc_pair::make_typed();
    recording_source right;
    pair.second->bind_source(&right);
    pair.first->faults().reorder_pairs = 1;

    recording_completion first;
    recording_completion second;
    pair.first->sink().push(bytes_of(std::string("AAAA")), &first);
    CRPC_CHECK_EQ(1, first.calls.load());
    CRPC_CHECK_EQ(0u, right.received.size());

    pair.first->sink().push(bytes_of(std::string("BBBB")), &second);
    CRPC_CHECK_EQ(1, second.calls.load());
    CRPC_CHECK_EQ(std::string("BBBBAAAA"), text_of(right.received));
}

CRPC_TEST(inproc_fault_delay_delivery) {
    auto pair = inproc_pair::make_typed();
    recording_source right;
    pair.second->bind_source(&right);
    pair.first->faults().delay_chunks = 1;

    recording_completion done;
    pair.first->sink().push(bytes_of(std::string("AAAA")), &done);
    CRPC_CHECK_EQ(1, done.calls.load());
    CRPC_CHECK_EQ(0u, right.received.size());

    pair.first->flush_delayed();
    CRPC_CHECK_EQ(std::string("AAAA"), text_of(right.received));
}

CRPC_TEST(inproc_fault_read_error_reaches_on_error_once) {
    auto pair = inproc_pair::make_typed();
    recording_source right;
    pair.second->bind_source(&right);
    pair.first->faults().inject_read_error =
        std::make_error_code(std::errc::connection_reset);

    recording_completion done;
    pair.first->sink().push(bytes_of(std::string("x")), &done);

    CRPC_CHECK_EQ(1, done.calls.load());
    CRPC_CHECK_EQ(1, right.error_calls.load());
    CRPC_CHECK_EQ(0, right.data_calls.load());
    CRPC_CHECK_EQ(0, right.eof_calls.load());
    CRPC_CHECK_EQ(static_cast<int>(std::errc::connection_reset),
                  right.last_error.load());

    recording_completion after;
    pair.first->sink().push(bytes_of(std::string("y")), &after);
    CRPC_CHECK_EQ(1, after.calls.load());
    CRPC_CHECK(after.broken.load());
    CRPC_CHECK_EQ(1, right.error_calls.load());
}

CRPC_TEST(inproc_fault_write_error_completes_with_broken_pipe) {
    auto pair = inproc_pair::make_typed();
    recording_source right;
    pair.second->bind_source(&right);
    pair.first->faults().inject_write_error =
        std::make_error_code(std::errc::broken_pipe);

    recording_completion done;
    pair.first->sink().push(bytes_of(std::string("x")), &done);

    CRPC_CHECK_EQ(1, done.calls.load());
    CRPC_CHECK(done.broken.load());
    CRPC_CHECK_FALSE(pair.first->is_open());
    CRPC_CHECK_EQ(1, right.eof_calls.load());
}

CRPC_TEST(inproc_span_valid_at_completion) {
    inproc_options opt;
    opt.max_queued_bytes = 4;
    auto pair = inproc_pair::make_typed(opt);
    recording_source right;
    pair.second->bind_source(&right);
    right.credit = 0;

    recording_completion filler;
    pair.first->sink().push(bytes_of(std::string("1234")), &filler);
    CRPC_CHECK_EQ(1, filler.calls.load());

    std::vector<std::byte> payload(8);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>('a' + i);
    }
    lifetime_completion deferred;
    deferred.buffer = &payload;
    pair.first->sink().push(bytes_of(payload), &deferred);
    CRPC_CHECK_EQ(0, deferred.calls);

    right.credit = 100;
    pair.second->resume_read();

    CRPC_CHECK_EQ(1, deferred.calls);
    CRPC_CHECK(deferred.intact);
    CRPC_CHECK_EQ(std::string("1234abcdefgh"), text_of(right.received));
}

CRPC_TEST(inproc_reentrant_push_is_iterative) {
    auto pair = inproc_pair::make_typed();
    std::atomic<int> budget{50000};
    std::atomic<int> left_calls{0};
    std::atomic<int> right_calls{0};
    int completions = 0;

    struct counter final : io::completion {
        int* value = nullptr;
        void complete(io::io_result) noexcept override { ++*value; }
    } echo_done;
    echo_done.value = &completions;

    struct echo_source final : io::source {
        inproc_connection* conn = nullptr;
        std::atomic<int>* budget = nullptr;
        std::atomic<int>* calls = nullptr;
        io::completion* done = nullptr;

        std::size_t on_data(span<const std::byte> data) noexcept override {
            calls->fetch_add(1);
            if (budget->fetch_sub(1) > 0) {
                conn->sink().push(data, done);
            }
            return data.size();
        }
        void on_eof() noexcept override {}
        void on_error(std::error_code) noexcept override {}
    };

    echo_source left;
    echo_source right;
    left.conn = pair.first.get();
    left.budget = &budget;
    left.calls = &left_calls;
    left.done = &echo_done;
    right.conn = pair.second.get();
    right.budget = &budget;
    right.calls = &right_calls;
    right.done = &echo_done;
    pair.first->bind_source(&left);
    pair.second->bind_source(&right);

    recording_completion first;
    pair.first->sink().push(bytes_of(std::string("ping")), &first);

    CRPC_CHECK_EQ(1, first.calls.load());
    CRPC_CHECK_EQ(50001, left_calls.load() + right_calls.load());
    CRPC_CHECK_EQ(50000, completions);

    // Unbind while the sources are still alive: the pair is destroyed at scope
    // exit and its close() would otherwise deliver on_eof into `left`/`right`
    // after those source objects are gone (see inproc_round_trip_both_directions).
    pair.first->bind_source(nullptr);
    pair.second->bind_source(nullptr);
}

CRPC_TEST(inproc_backend_listen_connect) {
    inproc_backend backend;

    struct acceptor final : accept_sink {
        std::unique_ptr<connection> conn;
        std::atomic<int> calls{0};

        void on_accept(std::unique_ptr<connection> c) noexcept override {
            conn = std::move(c);
            calls.fetch_add(1);
        }
    } accepted;

    auto l = backend.listen(endpoint::inproc("svc"), listen_options{});
    CRPC_CHECK(l != nullptr);
    l->start(&accepted);

    auto client = backend.connect(endpoint::inproc("svc"), connect_options{});
    CRPC_CHECK(client != nullptr);
    CRPC_CHECK_EQ(1, accepted.calls.load());
    CRPC_CHECK(accepted.conn != nullptr);

    recording_source server_source;
    recording_source client_source;
    accepted.conn->bind_source(&server_source);
    client->bind_source(&client_source);

    recording_completion done;
    client->sink().push(bytes_of(std::string("ping")), &done);
    CRPC_CHECK_EQ(1, done.calls.load());
    CRPC_CHECK_EQ(std::string("ping"), text_of(server_source.received));

    // Unbind before the connections are destroyed at scope exit; otherwise
    // close() delivers on_eof into the already-destroyed recording sources
    // (see inproc_round_trip_both_directions).
    accepted.conn->bind_source(nullptr);
    client->bind_source(nullptr);

    l->stop();
    CRPC_CHECK(backend.connect(endpoint::inproc("svc"), connect_options{}) ==
               nullptr);
}

} // namespace

CRPC_TEST_MAIN()
