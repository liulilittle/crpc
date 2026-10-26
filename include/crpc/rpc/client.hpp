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
//
// A single recursive lock used to serialize the whole path, so a reader
// delivering a response and a caller starting a request could never make
// progress at the same time, and a slow completion callback stalled every
// writer for its whole duration. The path is now split across four locks:
//
//   * `read_mutex_` (recursive) -- the inbound drain (`decoder_` and the
//     re-entrancy flag). Held only while frames are extracted; each ready
//     frame's payload is copied out and handled after the lock is released, so
//     no completion lock is ever taken under the reader lock. Recursive because
//     a synchronous backend re-enters `on_data` from inside `flush_write` on the
//     same thread.
//   * `write_mutex_` (recursive) -- the writer state (`out_`, the scratch
//     buffers, `conn_`) and the write loop. Recursive because a synchronous
//     sink completes `push` inside `flush_write`.
//   * `map_mutex_` -- the in-flight/deadline maps, the stream and call
//     counters, the PUSH sink, and the `closed_` transition.
//   * `completion_mutex_` (recursive) -- serializes the terminal completion
//     against a handle's detach/recycle. It is held from before the terminal
//     claim until after the callback returns, so a handle destructor can never
//     free a slot a completion is still touching (CF-1).
//
// Invariants that keep the four from deadlocking:
//   * `completion_mutex_` is the outer lock and `map_mutex_` the inner one:
//     every path that needs both takes them in that order. The reverse order
//     (`map_mutex_` then `completion_mutex_`) was a real ABBA deadlock between
//     `handle_response` and `release_slot_under_lock`; it must not be
//     reintroduced. `finish` claims the terminal state and runs the callback
//     under `completion_mutex_`; the in-flight removal is done by the caller
//     under `map_mutex_` inside that completion scope.
//   * `completion_mutex_` is never held while taking `write_mutex_` (the cancel
//     RST and the transport close run after it is released), so a writer that
//     re-enters the reader under `write_mutex_` cannot invert against a
//     completion.
//   * `read_mutex_` and `write_mutex_` may nest in either order, but only on
//     the synchronous in-process path; the backend serializes source callbacks
//     per connection (I8), so at most one thread is ever inside a drain and
//     the two orders never interleave across threads.
//
// The completion callback runs under `completion_mutex_` with `map_mutex_`
// released, so a slow callback blocks other completions and a handle detach,
// but never a writer or the reader's own frame drain.
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
#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/push/wire.hpp>
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

// --- Allocation-free call maps -----------------------------------------------
//
// The low-level client used to keep its in-flight set in a `std::unordered_map`
// and its deadline set in a `std::multimap`. Both allocate a node (and, for the
// multimap, a header) on every insert, so a call reached the system allocator
// even when its call slot came from a pool. These two containers provide the
// exact subset of the STL API the client uses, over a fixed array allocated once
// at construction, so insert/erase/lookup allocate nothing per call.
//
// `stream_map` is an open-addressing table keyed by the 31-bit stream id. Stream
// id 0 is never allocated (`allocate_stream_id` skips it), so 0 is the empty
// sentinel and no separate occupancy bitmap is needed. Deletion uses
// backward-shift, which keeps the probe chains intact without tombstones.
class stream_map {
public:
    stream_map() { init(16); }
    ~stream_map() = default;
    stream_map(const stream_map&) = delete;
    stream_map& operator=(const stream_map&) = delete;

    // `capacity` is a hint for the initial table size; the table still grows on
    // demand, so a client that never sees more than a handful of in-flight calls
    // never allocates past construction.
    void init(u32 capacity) {
        std::size_t size = 2;
        while (size < static_cast<std::size_t>(capacity) * 2) {
            size <<= 1;
        }
        capacity_ = static_cast<u32>(size);
        slots_.reset(new std::pair<u32, call_slot*>[capacity_]);
        clear();
    }

    class iterator {
    public:
        iterator() = default;
        iterator(stream_map* owner, u32 index) : owner_(owner), index_(index) {}
        std::pair<u32, call_slot*>& operator*() const { return owner_->slots_[index_]; }
        std::pair<u32, call_slot*>* operator->() const { return &owner_->slots_[index_]; }
        iterator& operator++() {
            index_ = owner_->next_used(index_ + 1);
            return *this;
        }
        bool operator==(const iterator& other) const {
            return owner_ == other.owner_ && index_ == other.index_;
        }
        bool operator!=(const iterator& other) const { return !(*this == other); }

    private:
        friend class stream_map;
        stream_map* owner_ = nullptr;
        u32 index_ = 0;
    };

    [[nodiscard]] iterator find(u32 key) const {
        if (capacity_ == 0) {
            return end();
        }
        u32 index = hash(key) & (capacity_ - 1);
        for (u32 probe = 0; probe < capacity_; ++probe) {
            const u32 slot_key = slots_[index].first;
            if (slot_key == 0) {
                return end();
            }
            if (slot_key == key) {
                return iterator(const_cast<stream_map*>(this), index);
            }
            index = (index + 1) & (capacity_ - 1);
        }
        return end();
    }

    void emplace(u32 key, call_slot* value) {
        if (size_ + 1 > capacity_ * 3 / 4) {
            grow();
        }
        u32 index = hash(key) & (capacity_ - 1);
        for (;;) {
            const u32 slot_key = slots_[index].first;
            if (slot_key == 0) {
                ++size_;
            }
            if (slot_key == 0 || slot_key == key) {
                slots_[index].first = key;
                slots_[index].second = value;
                return;
            }
            index = (index + 1) & (capacity_ - 1);
        }
    }

    void erase(iterator it) {
        if (it.owner_ != this) {
            return;
        }
        const u32 index = it.index_;
        slots_[index].first = 0;
        slots_[index].second = nullptr;
        --size_;
        // Backward-shift deletion: scan forward from the hole and pull forward
        // every entry whose ideal slot is at or before the hole, so no lookup
        // that used to reach it is broken. The scan stops at the first empty
        // slot, which must exist because the table is never full.
        u32 i = index;
        u32 j = index;
        for (;;) {
            j = (j + 1) & (capacity_ - 1);
            const u32 next_key = slots_[j].first;
            if (next_key == 0) {
                break;
            }
            const u32 ideal = hash(next_key) & (capacity_ - 1);
            if (cyclically_between(ideal, i, j)) {
                slots_[i] = slots_[j];
                slots_[j].first = 0;
                slots_[j].second = nullptr;
                i = j;
            }
        }
        slots_[i].first = 0;
        slots_[i].second = nullptr;
    }

    [[nodiscard]] u32 size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    void clear() noexcept {
        if (capacity_ == 0) {
            return;
        }
        for (u32 i = 0; i < capacity_; ++i) {
            slots_[i].first = 0;
            slots_[i].second = nullptr;
        }
        size_ = 0;
    }

    [[nodiscard]] iterator begin() const {
        return iterator(const_cast<stream_map*>(this), next_used(0));
    }
    [[nodiscard]] iterator end() const {
        return iterator(const_cast<stream_map*>(this), capacity_);
    }

private:
    static u32 hash(u32 key) noexcept { return key * 2654435761u; }

    // True when the hole `i` lies in the entry's probe chain, i.e. in the
    // cyclic half-open range `[ideal, j)` for the entry at `j`. Only then may
    // that entry be pulled back into the hole without breaking its lookup.
    static bool cyclically_between(u32 ideal, u32 i, u32 j) noexcept {
        if (ideal <= j) {
            return ideal <= i && i < j;
        }
        return i >= ideal || i < j;
    }

    void grow() {
        const u32 new_capacity = capacity_ == 0 ? 16 : capacity_ * 2;
        std::unique_ptr<std::pair<u32, call_slot*>[]> fresh(
            new std::pair<u32, call_slot*>[new_capacity]());
        for (u32 i = 0; i < capacity_; ++i) {
            const u32 key = slots_[i].first;
            if (key == 0) {
                continue;
            }
            u32 index = hash(key) & (new_capacity - 1);
            while (fresh[index].first != 0) {
                index = (index + 1) & (new_capacity - 1);
            }
            fresh[index] = slots_[i];
        }
        slots_ = std::move(fresh);
        capacity_ = new_capacity;
    }

    [[nodiscard]] u32 next_used(u32 from) const noexcept {
        for (u32 i = from; i < capacity_; ++i) {
            if (slots_[i].first != 0) {
                return i;
            }
        }
        return capacity_;
    }

    std::unique_ptr<std::pair<u32, call_slot*>[]> slots_;
    u32 capacity_ = 0;
    u32 size_ = 0;
};

// Intrusive singly-linked list of `(deadline, call_slot*)` sorted by deadline,
// with a free list over a fixed array. `poll()` walks it from the earliest
// deadline; `equal_range` finds the exact-key run that `remove_deadline()`
// scans. Insert and erase are linear but allocate nothing, and erasing a node
// only unlinks it, so every other iterator stays valid (the property the
// `std::multimap` it replaces gave callers).
class deadline_map {
private:
    static constexpr u32 kNone = 0xFFFFFFFFu;

    struct entry {
        std::chrono::steady_clock::time_point first{};
        call_slot* second = nullptr;
        u32 next = kNone;
    };

public:
    deadline_map() { init(16); }
    ~deadline_map() = default;
    deadline_map(const deadline_map&) = delete;
    deadline_map& operator=(const deadline_map&) = delete;

    void init(u32 capacity) {
        capacity_ = capacity == 0 ? 1 : capacity;
        entries_.reset(new entry[capacity_]);
        head_ = kNone;
        free_head_ = 0;
        size_ = 0;
        for (u32 i = 0; i < capacity_; ++i) {
            entries_[i].next = (i + 1 < capacity_) ? (i + 1) : kNone;
        }
    }

    class iterator {
    public:
        iterator() = default;
        iterator(deadline_map* owner, u32 index) : owner_(owner), index_(index) {}
        entry& operator*() const { return owner_->entries_[index_]; }
        entry* operator->() const { return &owner_->entries_[index_]; }
        iterator& operator++() {
            index_ = owner_->entries_[index_].next;
            return *this;
        }
        bool operator==(const iterator& other) const {
            return owner_ == other.owner_ && index_ == other.index_;
        }
        bool operator!=(const iterator& other) const { return !(*this == other); }

    private:
        friend class deadline_map;
        deadline_map* owner_ = nullptr;
        u32 index_ = 0;
    };

    void emplace(std::chrono::steady_clock::time_point deadline, call_slot* slot) {
        if (free_head_ == kNone) {
            grow();
        }
        u32 prev = kNone;
        u32 cur = head_;
        while (cur != kNone && entries_[cur].first < deadline) {
            prev = cur;
            cur = entries_[cur].next;
        }
        const u32 node = free_head_;
        free_head_ = entries_[node].next;
        entries_[node].first = deadline;
        entries_[node].second = slot;
        entries_[node].next = cur;
        if (prev == kNone) {
            head_ = node;
        } else {
            entries_[prev].next = node;
        }
        ++size_;
    }

    [[nodiscard]] std::pair<iterator, iterator> equal_range(
        std::chrono::steady_clock::time_point deadline) {
        u32 first = head_;
        while (first != kNone && entries_[first].first < deadline) {
            first = entries_[first].next;
        }
        u32 last = first;
        while (last != kNone && entries_[last].first == deadline) {
            last = entries_[last].next;
        }
        return {iterator(this, first), iterator(this, last)};
    }

    iterator erase(iterator it) {
        if (it.owner_ != this || it.index_ == kNone) {
            return end();
        }
        const u32 index = it.index_;
        const u32 next = entries_[index].next;
        u32 prev = kNone;
        u32 cur = head_;
        while (cur != kNone && cur != index) {
            prev = cur;
            cur = entries_[cur].next;
        }
        if (cur == kNone) {
            return end();
        }
        if (prev == kNone) {
            head_ = next;
        } else {
            entries_[prev].next = next;
        }
        entries_[index].next = free_head_;
        free_head_ = index;
        --size_;
        return iterator(this, next);
    }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] u32 size() const noexcept { return size_; }

    void clear() noexcept {
        head_ = kNone;
        free_head_ = 0;
        size_ = 0;
        for (u32 i = 0; i < capacity_; ++i) {
            entries_[i].next = (i + 1 < capacity_) ? (i + 1) : kNone;
        }
    }

    [[nodiscard]] iterator begin() { return iterator(this, head_); }
    [[nodiscard]] iterator end() { return iterator(this, kNone); }

private:
    void grow() {
        const u32 new_capacity = capacity_ == 0 ? 16 : capacity_ * 2;
        std::unique_ptr<entry[]> fresh(new entry[new_capacity]);
        u32 live = 0;
        for (u32 cur = head_; cur != kNone; cur = entries_[cur].next) {
            fresh[live].first = entries_[cur].first;
            fresh[live].second = entries_[cur].second;
            fresh[live].next = (live + 1 < new_capacity) ? (live + 1) : kNone;
            ++live;
        }
        // The last live node must terminate the live list. Linking it to `live`
        // (the first free node) would absorb the whole free list into the live
        // list, so a later insert would find `free_head_` pointing at a live
        // node and create a cycle.
        if (live > 0) {
            fresh[live - 1].next = kNone;
        }
        for (u32 i = live; i < new_capacity; ++i) {
            fresh[i].next = (i + 1 < new_capacity) ? (i + 1) : kNone;
        }
        entries_ = std::move(fresh);
        capacity_ = new_capacity;
        head_ = live == 0 ? kNone : 0;
        free_head_ = live < new_capacity ? live : kNone;
    }

    std::unique_ptr<entry[]> entries_;
    u32 capacity_ = 0;
    u32 head_ = kNone;
    u32 free_head_ = kNone;
    u32 size_ = 0;
};

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

    // Flushes any bytes staged for the sink but not yet handed to it. `start_call`
    // only stages the request; the caller (or a synchronous in-process consumer)
    // flushes here. This takes `write_mutex_` itself and is safe to call when no
    // other lock is held: the synchronous in-process drain it triggers re-enters
    // `on_data`, whose completion path takes `completion_mutex_`, so a caller that
    // holds a facade connection-slot lock must flush outside that lock.
    void flush_pending();

    // Starts a call. `request_body` and the response are encoded/decoded with
    // `call_codec<Resp>`; the request body is taken already encoded. Returns a
    // call handle or a failure (pool exhausted, expired deadline, write-queue
    // overflow, closed client).
    //
    // `flush = false` stages the frame without handing it to the transport. The
    // caller must then call `flush_pending()` after releasing every other lock it
    // holds: on the in-process path the flush drains the peer synchronously, and
    // that drain re-enters `on_data`, whose completion path takes
    // `completion_mutex_`. The facade uses this form because it calls `start_call`
    // under its connection-slot lock. The default flushes here, which is outside
    // every lock this function takes, so a direct low-level caller keeps the
    // documented "start_call sends" behaviour.
    template <typename Resp>
    [[nodiscard]] rpc_result<call<Resp>> start_call(u64 service_id, u64 method_id,
                                                    span<const std::byte> request_body,
                                                    call_context ctx = {},
                                                    bool flush = true) {
        // `begin_call` takes the map lock and rejects a closed client; the
        // encode/publish path takes the write lock. No lock is held across the
        // two, so a close between them is caught by the publish-time re-check.
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
        if (flush) {
            flush_pending();
        }
        return rpc_result<call<Resp>>(std::move(handle));
    }

    // Strongly typed overload. The `service_id` / `method_id` newtypes make a
    // swapped argument list a compile error, which the `u64` overload above
    // cannot catch.
    template <typename Resp>
    [[nodiscard]] rpc_result<call<Resp>> start_call(service_id service, method_id method,
                                                    span<const std::byte> request_body,
                                                    call_context ctx = {},
                                                    bool flush = true) {
        return start_call<Resp>(service.value, method.value, request_body,
                                std::move(ctx), flush);
    }

    // Installs the PUSH sink. The sink runs on the reader thread (the thread
    // that delivered the frame), with `body` valid only for the call; it must
    // not block and must not destroy this client. Passing nullptr removes it.
    // A reliable PUSH is acknowledged after the sink returns; a duplicate or
    // stale sequence (seq <= the last acknowledged seq on that stream) is
    // dropped without invoking the sink.
    void set_push_sink(push::push_sink_fn sink, void* ctx) noexcept;

    // Subscribes to `topic` over the ordinary REQUEST path. Returns the
    // stream_id/topic_id mapping carried by the subscription reply.
    //
    // This is the in-process/synchronous helper: it waits for the reply on the
    // calling thread, which is what the single-threaded in-process backend
    // guarantees. An asynchronous deployment uses the control service directly
    // instead of blocking a reader thread.
    [[nodiscard]] rpc_result<push::push_subscription> subscribe_push(
        std::string_view topic, u64 subscriber_id,
        push::delivery_mode mode = push::delivery_mode::reliable,
        bool ordered = false);

    // Advances the injected clock and expires overdue calls. Idempotent; a
    // client with no deadlines does nothing.
    void poll() noexcept;

    [[nodiscard]] u32 in_flight() const noexcept {
        std::lock_guard<std::mutex> lk(map_mutex_);
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
    // A decoded frame whose payload has been copied out of the decoder. The
    // decoder's payload span is only valid until the next `next()`/`feed()`, so
    // `on_data` copies each ready frame here and calls `handle_frame` after the
    // read/decoder lock is released. This keeps `completion_mutex_` out of the
    // reader lock scope (see the thread-model note above).
    struct pending_frame {
        frame_header header{};
        ordered_extension ordered{};
        buffer payload;
    };

    std::size_t on_data(span<const std::byte> data) noexcept;
    void on_eof() noexcept;
    void on_error(std::error_code ec) noexcept;
    void handle_frame(const frame& value);
    void handle_response(const frame& value);
    void handle_ping(const frame& value);
    void handle_goaway(const frame& value);
    void handle_push(const frame& value);
    void handle_push_ack(const frame& value);

    // --- writer ---
    // RAII holder of `write_mutex_`. Tracks the recursion depth so that a fatal
    // write error recorded deep in a nested `flush_write` is reported once, by
    // the outermost holder, after the write lock is released. Reporting it while
    // the write lock were held would take `completion_mutex_` under
    // `write_mutex_`, inverting against a completion callback that starts a new
    // call. Defined in client.cpp.
    struct write_lock;
    void report_write_failure() noexcept;

    void on_write_complete(io::io_result result) noexcept;
    void flush_write();
    void enqueue_raw(span<const std::byte> bytes);
    void send_rst(u32 stream_id, rpc_code code);
    void send_push_ack(u32 stream_id, u64 seq);

    // --- call lifecycle ---
    call_slot* begin_call(u64 service_id, u64 method_id, const call_context& ctx,
                          rpc_error& failure);
    status start_request(call_slot& slot, u64 service_id, u64 method_id,
                         span<const std::byte> body, const call_context& ctx);
    void finish(call_slot& slot, call_state terminal, rpc_code code, rpc_error error,
                span<const std::byte> body);
    void cancel_slot(call_slot& slot) noexcept;
    // Disarms a slot's completion callback under `completion_mutex_` and
    // synchronizes with any callback already running, so the caller may then
    // recycle the slot safely (CF-1).
    void detach_slot(call_slot& slot) noexcept;
    // Returns a finished slot to the pool under `completion_mutex_` (see
    // owner_release).
    void release_slot_under_lock(call_slot* slot) noexcept;
    // Marks the client closed (once) and fails every in-flight call. Returns
    // true when this call performed the transition. The caller must hold no
    // lock a completion callback could need. `message` is a view: the callers
    // (`close`, `on_eof`, `on_error`) are `noexcept`, so constructing a
    // `std::string` parameter at the call site would allocate there.
    bool fail_all(rpc_code code, std::string_view message);
    void protocol_failure();
    void remove_in_flight(call_slot& slot);
    void remove_deadline(call_slot& slot);
    call_id next_call_id();

    static bool in_flight_probe(void* ctx, u32 id) noexcept;
    static void cancel_thunk(void* ctx, call_slot* slot) noexcept;
    static void detach_thunk(void* ctx, call_slot* slot) noexcept;
    static void release_thunk(void* ctx, call_slot* slot) noexcept;
    static rpc_code map_error(std::error_code ec) noexcept;

    std::unique_ptr<connection> conn_;
    client_options options_;
    call_slot_pool pool_;
    frame_decoder decoder_;
    buffer payload_scratch_;
    buffer frame_scratch_;
    buffer out_;
    std::size_t out_sent_ = 0;
    int write_depth_ = 0;
    std::error_code write_fail_ec_{};
    std::atomic<bool> write_failed_{false};
    bool write_in_flight_ = false;
    bool short_write_ = false;
    bool write_error_ = false;
    bool in_push_ = false;
    std::error_code write_ec_{};
    std::size_t requested_ = 0;
    bool in_process_ = false;
    u32 stream_seq_ = 0;
    u64 seq_ = 0;
    stream_map in_flight_;
    deadline_map deadlines_;
    push::push_sink_fn push_sink_ = nullptr;
    void* push_sink_ctx_ = nullptr;
    // Highest reliable push sequence accepted per push stream. A PUSH at or
    // below it is a replay duplicate and is not delivered again.
    std::unordered_map<u32, u64> push_ack_upto_;
    write_completion completion_;
    reader_source source_;
    // --- Locks (see the thread-model note above) ---
    // Inbound drain: decoder_ and the re-entrancy flag.
    mutable std::recursive_mutex read_mutex_;
    // Writer state: out_/scratch buffers/conn_ and the write loop.
    mutable std::recursive_mutex write_mutex_;
    // In-flight/deadline maps, stream/call counters, PUSH sink and the
    // closed_ transition. Innermost lock.
    mutable std::mutex map_mutex_;
    // Terminal completion vs. handle detach/recycle. Held across the callback.
    mutable std::recursive_mutex completion_mutex_;
    // Set once on the first terminal event. Read under map_mutex_ for the
    // closed-vs-publish protocol and atomically on the hot writer path.
    std::atomic<bool> closed_{false};
};

} // namespace crpc::rpc

#endif // CRPC_RPC_CLIENT_HPP
