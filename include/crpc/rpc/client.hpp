// CRPC -- client channel: one connection, one reader, one writer queue.
//
// A `client` owns a transport `connection` and turns typed calls into REQUEST
// frames and RESPONSE frames back into call results. It is the single writer of
// every stream id and every call slot on that connection.
//
// Thread model
// ------------
// The client does not spawn any thread. It runs on whatever thread drives the
// backend: the in-process backend delivers `source::on_data` synchronously on
// the pushing thread, and a production backend delivers on its shard thread.
// The write loop (`start_request`/`flush_write`) may run on a caller thread
// while a sink completion runs on the backend thread, so the mutating entry
// points are serialized by a recursive lock (A-4). The lock is recursive
// because a synchronous backend completes `push` inside `flush_write` and the
// completion re-enters `flush_write`.
//
// `get()` blocks the calling thread on a condition variable until the terminal
// state. On the single-threaded in-process path the response has already been
// delivered by the time `start_call<Resp>` returns, so `get()` does not block.
// A real deployment drives the reader on a shard thread while a caller coroutine
// parks; the wait primitive is the same.
//
// Allocation
// ----------
// Slots come from a fixed `call_slot_pool`, so a call handle never allocates.
// The client owns two scratch buffers (payload and frame) and one output queue;
// all three grow once and are reused. The only per-call allocation is a slot's
// response buffer on its first non-empty response, which is kept for reuse.
//
// No-stall policy
// ---------------
// The writer queue is bounded by `client_options::max_pending_write_bytes`. When
// a new frame would exceed the bound the call is rejected immediately with
// `rpc_code::resource_exhausted`; the client never blocks waiting for the sink.
#ifndef CRPC_RPC_CLIENT_HPP
#define CRPC_RPC_CLIENT_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/span.hpp>

namespace crpc::rpc {

// Monotonic clock source. Injected so a test can control time; the default
// reads `std::chrono::steady_clock`.
using clock_fn = std::chrono::steady_clock::time_point (*)(void* ctx) noexcept;

[[nodiscard]] CRPC_API std::chrono::steady_clock::time_point system_clock_now(void* ctx) noexcept;

struct client_options {
    // Call slots preallocated for this connection.
    u32 slot_capacity = 65536;
    // Upper bound on bytes staged for the sink but not yet accepted. A frame
    // that would exceed it is rejected with resource_exhausted.
    std::size_t max_pending_write_bytes = 4u * 1024u * 1024u;
    // Monotonic clock; the default is steady_clock. `clock_ctx` is passed to it.
    clock_fn clock = &system_clock_now;
    void* clock_ctx = nullptr;
    // Applied to a call that sets no explicit deadline. Zero means none.
    std::chrono::nanoseconds default_timeout{0};
    // Identity of the process for the idempotency-key triple.
    std::array<std::byte, 16> client_id{};
    u64 client_epoch = 1;
};

// Allocates the next stream id from `counter`, skipping 0 and any id for which
// `in_flight(ctx, id)` is true. Wrap-safe: the 31-bit space is cycled, never
// reusing an id that is still in flight. Callers guarantee the in-flight set is
// smaller than the id space, so the loop terminates.
[[nodiscard]] CRPC_API u32 allocate_stream_id(u32& counter, bool (*in_flight)(void* ctx, u32 id),
                                              void* ctx) noexcept;

class CRPC_API client {
public:
    explicit client(std::unique_ptr<connection> connection, client_options options = {});
    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;

    // The source the backend must feed. Bind it with `connection::bind_source`
    // before traffic; the constructor already does so for the owned connection.
    [[nodiscard]] io::source& source() noexcept { return source_; }

    [[nodiscard]] bool is_open() const noexcept;
    // Fails every in-flight call with shutting_down and closes the transport.
    void close() noexcept;

    // Starts a call. `request_body` and the response are encoded/decoded with
    // `call_codec<Resp>`; the request body is taken already encoded. Returns a
    // call handle or a failure (pool exhausted, expired deadline, write-queue
    // overflow, closed client).
    template <typename Resp>
    [[nodiscard]] rpc_result<call<Resp>> start_call(u64 service_id, u64 method_id,
                                                    span<const std::byte> request_body,
                                                    call_context ctx = {}) {
        // Serializes with a backend-thread write completion (A-4).
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (closed_) {
            return rpc_result<call<Resp>>(rpc_error(rpc_code::shutting_down, "client is closed"));
        }
        rpc_error failure;
        call_slot* slot = begin_call(service_id, method_id, ctx, failure);
        if (slot == nullptr) {
            return rpc_result<call<Resp>>(std::move(failure));
        }
        const status written = start_request(*slot, service_id, method_id, request_body, ctx);
        if (!written.ok()) {
            // The slot was never published to the in-flight map, so it can be
            // recycled directly.
            pool_.release(slot);
            return rpc_result<call<Resp>>(rpc_error(written.code(), written.message()));
        }
        call<Resp> handle;
        handle.slot_ = slot;
        handle.generation_ = slot->generation;
        handle.pool_ = &pool_;
        return rpc_result<call<Resp>>(std::move(handle));
    }

    // Advances the injected clock and expires overdue calls. Idempotent; a
    // client with no deadlines does nothing.
    void poll() noexcept;

    [[nodiscard]] u32 in_flight() const noexcept {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        return static_cast<u32>(in_flight_.size());
    }
    [[nodiscard]] u32 slots_in_use() const noexcept { return pool_.in_use(); }
    [[nodiscard]] u32 slot_capacity() const noexcept { return pool_.capacity(); }

private:
    class reader_source final : public io::source {
    public:
        explicit reader_source(client& owner) noexcept : owner_(owner) {}
        std::size_t on_data(span<const std::byte> data) noexcept override {
            return owner_.on_data(data);
        }
        void on_eof() noexcept override { owner_.on_eof(); }
        void on_error(std::error_code ec) noexcept override { owner_.on_error(ec); }

    private:
        client& owner_;
    };

    class write_completion final : public io::completion {
    public:
        explicit write_completion(client& owner) noexcept : owner_(owner) {}
        void complete(io::io_result result) noexcept override { owner_.on_write_complete(result); }

    private:
        client& owner_;
    };

    // --- reader entry points (called through reader_source) ---
    std::size_t on_data(span<const std::byte> data) noexcept;
    void on_eof() noexcept;
    void on_error(std::error_code ec) noexcept;
    void handle_frame(const frame& value);
    void handle_response(const frame& value);
    void handle_ping(const frame& value);
    void handle_goaway(const frame& value);

    // --- writer ---
    void on_write_complete(io::io_result result) noexcept;
    void flush_write();
    void enqueue_raw(span<const std::byte> bytes);
    void send_rst(u32 stream_id, rpc_code code);

    // --- call lifecycle ---
    call_slot* begin_call(u64 service_id, u64 method_id, const call_context& ctx,
                          rpc_error& failure);
    status start_request(call_slot& slot, u64 service_id, u64 method_id,
                         span<const std::byte> body, const call_context& ctx);
    void finish(call_slot& slot, call_state terminal, rpc_code code, rpc_error error,
                span<const std::byte> body);
    void cancel_slot(call_slot& slot) noexcept;
    // Disarms a slot's completion callback under `mutex_` and synchronizes with
    // any callback already running, so the caller may then recycle the slot
    // safely (CF-1).
    void detach_slot(call_slot& slot) noexcept;
    void fail_all(rpc_code code, std::string message);
    void protocol_failure();
    void remove_in_flight(call_slot& slot);
    void remove_deadline(call_slot& slot);
    call_id next_call_id();

    static bool in_flight_probe(void* ctx, u32 id) noexcept;
    static void cancel_thunk(void* ctx, call_slot* slot) noexcept;
    static void detach_thunk(void* ctx, call_slot* slot) noexcept;
    static rpc_code map_error(std::error_code ec) noexcept;

    std::unique_ptr<connection> conn_;
    client_options options_;
    call_slot_pool pool_;
    frame_decoder decoder_;
    buffer payload_scratch_;
    buffer frame_scratch_;
    buffer out_;
    std::size_t out_sent_ = 0;
    bool write_in_flight_ = false;
    bool short_write_ = false;
    bool write_error_ = false;
    bool in_push_ = false;
    std::error_code write_ec_{};
    std::size_t requested_ = 0;
    bool closed_ = false;
    bool in_process_ = false;
    u32 stream_seq_ = 0;
    u64 seq_ = 0;
    std::unordered_map<u32, call_slot*> in_flight_;
    std::multimap<std::chrono::steady_clock::time_point, call_slot*> deadlines_;
    write_completion completion_;
    reader_source source_;
    // Guards the mutating entry points against a backend-thread completion.
    // Recursive because a synchronous sink completion re-enters flush_write.
    mutable std::recursive_mutex mutex_;
};

} // namespace crpc::rpc

#endif // CRPC_RPC_CLIENT_HPP
