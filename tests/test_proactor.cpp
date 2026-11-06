// CRPC -- proactor (completion-driven I/O) tests.
//
// The proactor is the "completion" half of the concurrency batch: the caller
// submits an operation and gets exactly one completion back, whatever happens
// -- success, failure or cancellation. These tests pin that contract against
// the real in-process backend (`io::inproc_backend`), so the adaptation is
// exercised end to end rather than against a mock:
//
//   1. `open` connects through the backend and installs the caller's
//      `io::source`, so bytes pushed by the peer arrive at that source;
//   2. a submit that fits completes exactly once (on the submitting thread);
//   3. a deferred submit completes exactly once when the connection is
//      canceled, and the completion says `operation_canceled` (the M2 mutation
//      target: cancel that does not produce a completion);
//   4. `pump` counts completions and waits out its timeout without spinning;
//   5. a second submit on a connection with one write in flight is refused
//      (io contract I5) instead of being silently dropped;
//   6. every failure path still completes exactly once, and the destruction of
//      the proactor cancels in-flight work with a completion rather than
//      dropping it.
//
// No `sleep_for` is used as a synchronisation device: waits go through
// `crpc_test::wait_for` (bounded poll) or the proactor's own condition
// variable.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/proactor/proactor.hpp>

namespace {

using namespace std::chrono_literals;

// Collects the server side of the in-process listener.
struct capture_sink final : crpc::accept_sink {
    std::unique_ptr<crpc::connection> peer;

    void on_accept(std::unique_ptr<crpc::connection> c) noexcept override {
        peer = std::move(c);
    }
};

// Records every byte the backend delivers.
struct capture_source final : crpc::io::source {
    std::atomic<std::size_t> received{0};
    std::atomic<int> eof_calls{0};
    std::atomic<int> error_calls{0};

    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        received.fetch_add(data.size(), std::memory_order_release);
        return data.size();
    }

    void on_eof() noexcept override {
        eof_calls.fetch_add(1, std::memory_order_release);
    }

    void on_error(std::error_code) noexcept override {
        error_calls.fetch_add(1, std::memory_order_release);
    }
};

// Counts completions. A second completion for one submit would show up as
// calls == 2, which every exactly-once check below rejects.
struct count_completion final : crpc::io::completion {
    std::atomic<int> calls{0};
    crpc::io::io_result last{};

    void complete(crpc::io::io_result result) noexcept override {
        last = result;
        calls.fetch_add(1, std::memory_order_release);
    }
};

// A queue small enough that an 8-byte write cannot fit, which forces the
// completion to be deferred; that is what makes cancel observable.
[[nodiscard]] crpc::inproc_options tiny_queue() {
    crpc::inproc_options options;
    options.max_queued_bytes = 4;
    return options;
}

[[nodiscard]] crpc::span<const std::byte> bytes8(const std::byte* data) {
    return crpc::span<const std::byte>(data, 8);
}

} // namespace

CRPC_TEST(proactor_open_connects_and_binds_the_source) {
    crpc::inproc_backend backend;
    auto listener = backend.listen(crpc::endpoint::inproc("proactor-open"), crpc::listen_options{});
    CRPC_REQUIRE(listener != nullptr);
    capture_sink accepted;
    listener->start(&accepted);

    capture_source source;
    crpc::proactor::proactor<crpc::inproc_backend> p(backend);

    auto opened = p.open(crpc::endpoint::inproc("proactor-open"), source);
    CRPC_REQUIRE(opened.ok());
    crpc::connection* client = opened.value();
    CRPC_REQUIRE(client != nullptr);
    CRPC_CHECK(client->is_open());
    CRPC_REQUIRE(accepted.peer != nullptr);
    CRPC_CHECK_EQ(std::size_t(1), p.connections());

    // Bytes from the peer land in the source installed by open().
    const std::byte payload[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
    count_completion done;
    accepted.peer->sink().push(crpc::span<const std::byte>(payload, 3), &done);
    CRPC_CHECK_EQ(1, done.calls.load());
    CRPC_CHECK(done.last.ok());
    CRPC_CHECK(crpc_test::wait_for([&] {
        return source.received.load(std::memory_order_acquire) == 3;
    }, 2000));
    CRPC_CHECK_EQ(std::size_t(3), source.received.load());
}

CRPC_TEST(proactor_submit_completes_exactly_once) {
    crpc::inproc_backend backend;
    auto listener = backend.listen(crpc::endpoint::inproc("proactor-submit"), crpc::listen_options{});
    CRPC_REQUIRE(listener != nullptr);
    capture_sink accepted;
    listener->start(&accepted);

    capture_source source;
    crpc::proactor::proactor<crpc::inproc_backend> p(backend);
    auto opened = p.open(crpc::endpoint::inproc("proactor-submit"), source);
    CRPC_REQUIRE(opened.ok());
    crpc::connection& client = *opened.value();

    const std::byte payload[8] = {};
    count_completion done;
    const crpc::status submitted = p.submit(client, bytes8(payload), done);
    CRPC_CHECK(submitted.ok());
    CRPC_CHECK_EQ(1, done.calls.load());
    CRPC_CHECK(done.last.ok());
    CRPC_CHECK_EQ(std::size_t(8), done.last.bytes);
    CRPC_CHECK_EQ(std::size_t(0), p.outstanding());
    CRPC_CHECK_EQ(std::uint64_t(0), p.double_completions());

    // The completion happened; pump reports it exactly once.
    CRPC_CHECK_EQ(std::size_t(1), p.pump(0ms));
    CRPC_CHECK_EQ(std::size_t(0), p.pump(0ms));
    CRPC_CHECK_EQ(std::size_t(1), p.completed());
}

// M2 target: cancel must produce a completion. The queue is too small for the
// 8-byte write, so the completion is deferred and only the cancel can finish
// it.
CRPC_TEST(proactor_cancel_produces_exactly_one_completion) {
    crpc::inproc_backend backend(tiny_queue());
    auto listener = backend.listen(crpc::endpoint::inproc("proactor-cancel"), crpc::listen_options{});
    CRPC_REQUIRE(listener != nullptr);
    capture_sink accepted;
    listener->start(&accepted);

    capture_source source;
    crpc::proactor::proactor<crpc::inproc_backend> p(backend);
    auto opened = p.open(crpc::endpoint::inproc("proactor-cancel"), source);
    CRPC_REQUIRE(opened.ok());
    crpc::connection& client = *opened.value();

    const std::byte payload[8] = {};
    count_completion done;
    CRPC_REQUIRE(p.submit(client, bytes8(payload), done).ok());
    CRPC_CHECK_EQ(0, done.calls.load());
    CRPC_CHECK_EQ(std::size_t(1), p.outstanding());

    CRPC_CHECK(p.cancel(client).ok());
    CRPC_CHECK_EQ(1, done.calls.load());
    CRPC_CHECK_EQ(std::size_t(0), p.outstanding());
    CRPC_CHECK_EQ(static_cast<int>(std::errc::operation_canceled),
                  done.last.ec.value());
    CRPC_CHECK_EQ(std::uint64_t(0), p.double_completions());

    // Cancelling again finds nothing in flight; it must not fabricate a
    // second completion.
    const crpc::status again = p.cancel(client);
    CRPC_CHECK_FALSE(again.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(again.code()));
    CRPC_CHECK_EQ(1, done.calls.load());
}

CRPC_TEST(proactor_cancel_without_in_flight_is_not_found) {
    crpc::inproc_backend backend;
    auto listener = backend.listen(crpc::endpoint::inproc("proactor-idle"), crpc::listen_options{});
    CRPC_REQUIRE(listener != nullptr);
    capture_sink accepted;
    listener->start(&accepted);

    capture_source source;
    crpc::proactor::proactor<crpc::inproc_backend> p(backend);
    auto opened = p.open(crpc::endpoint::inproc("proactor-idle"), source);
    CRPC_REQUIRE(opened.ok());

    const crpc::status canceled = p.cancel(*opened.value());
    CRPC_CHECK_FALSE(canceled.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(canceled.code()));
    CRPC_CHECK_EQ(std::size_t(0), p.completed());
}

CRPC_TEST(proactor_second_submit_is_refused_while_one_is_in_flight) {
    crpc::inproc_backend backend(tiny_queue());
    auto listener = backend.listen(crpc::endpoint::inproc("proactor-i5"), crpc::listen_options{});
    CRPC_REQUIRE(listener != nullptr);
    capture_sink accepted;
    listener->start(&accepted);

    capture_source source;
    crpc::proactor::proactor<crpc::inproc_backend> p(backend);
    auto opened = p.open(crpc::endpoint::inproc("proactor-i5"), source);
    CRPC_REQUIRE(opened.ok());
    crpc::connection& client = *opened.value();

    const std::byte payload[8] = {};
    count_completion first;
    count_completion second;
    CRPC_REQUIRE(p.submit(client, bytes8(payload), first).ok());
    CRPC_CHECK_EQ(std::size_t(1), p.outstanding());

    const crpc::status refused = p.submit(client, bytes8(payload), second);
    CRPC_CHECK_FALSE(refused.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(refused.code()));
    CRPC_CHECK_EQ(0, second.calls.load());

    // The refused submit never reaches the backend, so only one completion is
    // ever possible and cancel finishes it.
    CRPC_CHECK(p.cancel(client).ok());
    CRPC_CHECK_EQ(1, first.calls.load());
    CRPC_CHECK_EQ(0, second.calls.load());
    CRPC_CHECK_EQ(std::size_t(1), p.completed());
}

CRPC_TEST(proactor_pump_waits_out_the_timeout_and_counts_completions) {
    crpc::inproc_backend backend;
    auto listener = backend.listen(crpc::endpoint::inproc("proactor-pump"), crpc::listen_options{});
    CRPC_REQUIRE(listener != nullptr);
    capture_sink accepted;
    listener->start(&accepted);

    capture_source source;
    crpc::proactor::proactor<crpc::inproc_backend> p(backend);
    auto opened = p.open(crpc::endpoint::inproc("proactor-pump"), source);
    CRPC_REQUIRE(opened.ok());
    crpc::connection& client = *opened.value();

    const std::byte payload[8] = {};
    count_completion done;
    CRPC_REQUIRE(p.submit(client, bytes8(payload), done).ok());
    CRPC_CHECK_EQ(std::size_t(1), p.pump(0ms));
    CRPC_CHECK_EQ(std::size_t(0), p.pump(0ms));

    const auto start = std::chrono::steady_clock::now();
    const std::size_t empty = p.pump(40ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CRPC_CHECK_EQ(std::size_t(0), empty);
    CRPC_CHECK(elapsed >= 30ms);
}

// The destructor is a cancel path too: whatever is still in flight must
// complete exactly once as canceled, not vanish.
CRPC_TEST(proactor_destructor_cancels_in_flight_and_completes_once) {
    crpc::inproc_backend backend(tiny_queue());
    auto listener = backend.listen(crpc::endpoint::inproc("proactor-dtor"), crpc::listen_options{});
    CRPC_REQUIRE(listener != nullptr);
    capture_sink accepted;
    listener->start(&accepted);

    capture_source source;
    count_completion done;
    {
        crpc::proactor::proactor<crpc::inproc_backend> p(backend);
        auto opened = p.open(crpc::endpoint::inproc("proactor-dtor"), source);
        CRPC_REQUIRE(opened.ok());
        const std::byte payload[8] = {};
        CRPC_REQUIRE(p.submit(*opened.value(), bytes8(payload), done).ok());
        CRPC_CHECK_EQ(0, done.calls.load());
        CRPC_CHECK_EQ(std::size_t(1), p.outstanding());
    }
    CRPC_CHECK_EQ(1, done.calls.load());
    CRPC_CHECK_EQ(static_cast<int>(std::errc::operation_canceled),
                  done.last.ec.value());
}

CRPC_TEST(proactor_rejects_unmanaged_connection) {
    crpc::inproc_backend backend;
    crpc::proactor::proactor<crpc::inproc_backend> p(backend);
    auto pair = crpc::inproc_pair::make();
    CRPC_REQUIRE(pair.first != nullptr);

    const std::byte payload[4] = {};
    count_completion done;
    const crpc::status refused = p.submit(
        *pair.first, crpc::span<const std::byte>(payload, 4), done);
    CRPC_CHECK_FALSE(refused.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(refused.code()));
    CRPC_CHECK_EQ(0, done.calls.load());

    const crpc::status canceled = p.cancel(*pair.first);
    CRPC_CHECK_FALSE(canceled.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(canceled.code()));
}

CRPC_TEST(proactor_open_reports_invalid_and_refused_endpoints) {
    crpc::inproc_backend backend;
    crpc::proactor::proactor<crpc::inproc_backend> p(backend);
    capture_source source;

    const auto invalid = p.open(crpc::endpoint{}, source);
    CRPC_CHECK_FALSE(invalid.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(invalid.code()));

    const auto refused =
        p.open(crpc::endpoint::inproc("proactor-nobody-listens"), source);
    CRPC_CHECK_FALSE(refused.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_refused),
                  static_cast<int>(refused.code()));
    CRPC_CHECK_EQ(std::size_t(0), p.connections());
}

// A submit on a closed connection is still a submitted operation: the backend
// reports the failure through the completion, exactly once.
CRPC_TEST(proactor_submit_after_close_completes_once_with_error) {
    crpc::inproc_backend backend(tiny_queue());
    auto listener = backend.listen(crpc::endpoint::inproc("proactor-closed"), crpc::listen_options{});
    CRPC_REQUIRE(listener != nullptr);
    capture_sink accepted;
    listener->start(&accepted);

    capture_source source;
    crpc::proactor::proactor<crpc::inproc_backend> p(backend);
    auto opened = p.open(crpc::endpoint::inproc("proactor-closed"), source);
    CRPC_REQUIRE(opened.ok());
    crpc::connection& client = *opened.value();

    const std::byte payload[8] = {};
    count_completion first;
    CRPC_REQUIRE(p.submit(client, bytes8(payload), first).ok());
    CRPC_REQUIRE(p.cancel(client).ok());
    CRPC_CHECK_EQ(1, first.calls.load());

    count_completion after;
    CRPC_CHECK(p.submit(client, bytes8(payload), after).ok());
    CRPC_CHECK_EQ(1, after.calls.load());
    CRPC_CHECK_FALSE(after.last.ok());
    CRPC_CHECK_EQ(static_cast<int>(std::errc::broken_pipe), after.last.ec.value());
    CRPC_CHECK_EQ(std::uint64_t(0), p.double_completions());
}

CRPC_TEST_MAIN()
