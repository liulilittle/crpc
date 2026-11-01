// CRPC -- client channel implementation.
#include <crpc/rpc/client.hpp>

#include <string>
#include <system_error>
#include <vector>

#include <crpc/endian.hpp>

namespace crpc::rpc {
namespace {

// Advances the 31-bit stream-id counter, skipping zero and the reserved high
// bit. The caller re-checks the in-flight set after each step.
u32 next_candidate(u32& counter) noexcept {
    counter += 1;
    if (counter == 0 || counter > max_stream_id) {
        counter = 1;
    }
    return counter;
}

// The flow controller works in raw nanoseconds from the steady-clock epoch,
// because a probe token travels on the wire and must be a plain integer.
u64 duration_ns_since_epoch(std::chrono::steady_clock::time_point point) noexcept {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(point.time_since_epoch()).count());
}

} // namespace

// Recursive write lock with a deferred failure report. The outermost release
// reports a fatal write error once, with `write_mutex_` no longer held, so the
// report can take `completion_mutex_` without inverting against a completion
// callback that starts a new call.
struct client::write_lock {
    explicit write_lock(client& owner) noexcept : owner_(owner) {
        owner_.write_mutex_.lock();
        ++owner_.write_depth_;
    }
    ~write_lock() {
        // The depth is read while the lock is still held. Reading it after
        // unlock() raced with another thread incrementing it in the
        // constructor (ThreadSanitizer reports exactly that pair), and the
        // report itself must run with the mutex released so it can take
        // completion_mutex_ without inverting lock order.
        const bool outermost = (--owner_.write_depth_ == 0);
        owner_.write_mutex_.unlock();
        if (outermost) {
            owner_.report_write_failure();
        }
    }
    write_lock(const write_lock&) = delete;
    write_lock& operator=(const write_lock&) = delete;
    write_lock(write_lock&&) = delete;
    write_lock& operator=(write_lock&&) = delete;

private:
    client& owner_;
};

void client::report_write_failure() noexcept {
    if (!write_failed_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const std::error_code ec = write_fail_ec_;
    (void)fail_all(map_error(ec), ec.message());
}

u32 allocate_stream_id(u32& counter, bool (*in_flight)(void* ctx, u32 id), void* ctx) noexcept {
    for (;;) {
        const u32 id = next_candidate(counter);
        if (!in_flight(ctx, id)) {
            return id;
        }
    }
}

std::chrono::steady_clock::time_point system_clock_now(void*) noexcept {
    return std::chrono::steady_clock::now();
}

// --- Construction ------------------------------------------------------------

client::client(std::unique_ptr<connection> connection, client_options options)
    : conn_(std::move(connection)),
      options_(std::move(options)),
      pool_(options_.slot_capacity),
      decoder_(max_frame_default),
      completion_(*this),
      source_(*this) {
    if (conn_) {
        conn_->bind_source(&source_);
    }
}

client::~client() {
    close();
    if (conn_) {
        // `source_` is declared after `conn_`, so reverse destruction would
        // free it before the connection. Unbind explicitly: bind_source waits
        // for any in-flight delivery before returning.
        conn_->bind_source(nullptr);
    }
}

bool client::is_open() const noexcept {
    if (closed_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lk(write_mutex_);
    return conn_ != nullptr && conn_->is_open();
}

void client::close() noexcept {
    if (closed_.load(std::memory_order_acquire)) {
        return;
    }
    // Fails every in-flight call first (idempotent; only the first caller wins
    // the closed transition). This takes no lock the writer path holds, so a
    // concurrent start_call either publishes before the transition (and is
    // failed here) or observes closed_ and rejects.
    (void)fail_all(rpc_code::shutting_down, "client closed");
    // Move the transport out and close it without holding any lock. A
    // synchronous in-process close calls the peer's terminal hook, which may
    // take the server's push lock; holding a client lock there would invert
    // against the server send path (push lock -> this lock), a potential
    // deadlock TSan reports. The local owner closes after the unlock.
    std::unique_ptr<connection> closing;
    {
        std::lock_guard<std::recursive_mutex> lk(write_mutex_);
        closing = std::move(conn_);
    }
    if (closing) {
        closing->close();
    }
}

void client::set_flow_control(const flow_control_options& options) {
    // `charge_table_capacity == 0` means "follow this connection's call-slot
    // capacity", which is exactly what the header promises. The facade resolves
    // it too, but a caller that uses the low-level client directly must get the
    // same behaviour: a hard-coded fallback would silently cap concurrency far
    // below `slot_capacity`.
    flow_control_options resolved = options;
    if (resolved.charge_table_capacity == 0) {
        resolved.charge_table_capacity = options_.slot_capacity == 0 ? 1 : options_.slot_capacity;
    }
    std::unique_ptr<flow_controller> fresh;
    if (resolved.enabled) {
        fresh = std::make_unique<flow_controller>(resolved);
    }
    std::lock_guard<std::mutex> map(map_mutex_);
    // Replacing or removing the controller is a configuration act, so it is
    // safe to do at any time: no call can be mid-charge here, because the
    // charge is decided and recorded under this same lock.
    flow_ = std::move(fresh);
}

bool client::can_admit(u64 lane_id, std::size_t bytes) noexcept {
    std::lock_guard<std::mutex> map(map_mutex_);
    if (flow_ == nullptr) {
        return true;
    }
    const auto now = options_.clock(options_.clock_ctx);
    flow_->observe_now(duration_ns_since_epoch(now));
    return flow_->can_admit(lane_id, bytes);
}

flow_state client::flow_snapshot() const {
    std::lock_guard<std::mutex> map(map_mutex_);
    if (flow_ == nullptr) {
        rpc::flow_state off;
        off.enabled = false;
        return off;
    }
    return flow_->snapshot();
}

u64 client::lane_used(u64 lane_id) const noexcept {
    std::lock_guard<std::mutex> map(map_mutex_);
    if (flow_ == nullptr) {
        return 0;
    }
    return flow_->lane_used(lane_id);
}

std::size_t rpc_client_size_probe() noexcept {
    // Out of line on purpose: the caller compares the size it compiled against
    // with the size this library was built with. An accidental layout change
    // (an added member, a changed member type, a different standard-library
    // debug configuration) then fails the consumer's assertion instead of
    // corrupting memory at run time.
    return sizeof(client);
}

// --- Reader ------------------------------------------------------------------

std::size_t client::on_data(span<const std::byte> data) noexcept {
    // Extract every ready frame under the reader lock, then handle them with the
    // lock released. A frame's payload span points into the decoder's buffer (or
    // the caller's chunk), so it is copied into `pending_frame::payload` here.
    // Handling after the unlock is what keeps `completion_mutex_` out of the
    // reader-lock scope: the synchronous in-process drain re-enters `on_data`
    // from the writer, and a completion taken under the reader lock would invert
    // against the completion/detach path.
    std::vector<pending_frame> frames;
    frame_decoder::result result = frame_decoder::result::need_more;
    {
        std::lock_guard<std::recursive_mutex> lk(read_mutex_);
        if (in_process_) {
            // Re-entered from a synchronous push: the outer loop will drain these
            // bytes after the current frame is handled. The chunk does not outlive
            // this call, so it must go through the copying path (which also drains
            // any external chunk the outer loop still holds).
            decoder_.feed(data);
            return data.size();
        }
        // Parse directly out of the transport's chunk: handle_frame() consumes the
        // payload before next() is called again, so the zero-copy window is this
        // loop. Only a trailing partial frame is copied into the decoder.
        decoder_.feed_external(data);
        in_process_ = true;
        frame value;
        // A dropped frame consumes its bytes and leaves the decoder usable, so it
        // is not a terminal outcome: keep parsing the rest of this chunk, matching
        // server_connection::on_data. Borrowing mode has no payload buffer, so a
        // compressed frame fails hard instead and frame_dropped is unreachable
        // here today; the branch is kept so a future payload buffer cannot
        // silently truncate the chunk.
        for (;;) {
            result = decoder_.next(value);
            if (result == frame_decoder::result::frame_dropped) {
                continue;
            }
            if (result != frame_decoder::result::frame_ready) {
                break;
            }
            pending_frame copy;
            copy.header = value.header;
            copy.ordered = value.ordered;
            if (!copy.payload.append(value.payload)) {
                // Allocation failure while staging the frame: drop it and keep
                // parsing the rest of the chunk rather than losing the stream.
                continue;
            }
            // `on_data` is `noexcept` (the backend calls it through a virtual
            // source), so a growth failure here must not reach
            // `std::terminate`. Dropping the staged frame degrades to "this
            // response is lost"; the caller's deadline reports it.
            try {
                frames.push_back(std::move(copy));
            } catch (...) {
                continue;
            }
        }
        in_process_ = false;
        decoder_.drain_external();
    }
    // Reader lock released. `handle_frame` runs without it, so a completion it
    // takes cannot wait on a thread that holds `read_mutex_`.
    for (pending_frame& pending : frames) {
        frame value;
        value.header = pending.header;
        value.ordered = pending.ordered;
        value.payload = pending.payload.readable();
        handle_frame(value);
    }
    if (result == frame_decoder::result::error) {
        protocol_failure();
    }
    return data.size();
}

void client::on_eof() noexcept {
    std::lock_guard<std::recursive_mutex> lk(read_mutex_);
    if (closed_.load(std::memory_order_acquire)) {
        return;
    }
    (void)fail_all(rpc_code::eof, "peer closed the connection");
}

void client::on_error(std::error_code ec) noexcept {
    std::lock_guard<std::recursive_mutex> lk(read_mutex_);
    if (closed_.load(std::memory_order_acquire)) {
        return;
    }
    (void)fail_all(map_error(ec), ec.message());
}

void client::handle_frame(const frame& value) {
    switch (value.header.type) {
    case frame_type::response:
        handle_response(value);
        break;
    case frame_type::ping:
        handle_ping(value);
        break;
    case frame_type::pong:
        handle_pong(value);
        break;
    case frame_type::goaway:
        handle_goaway(value);
        break;
    case frame_type::push:
        handle_push(value);
        break;
    case frame_type::push_ack:
        handle_push_ack(value);
        break;
    default:
        break;
    }
}

void client::handle_response(const frame& value) {
    rpc_code code = rpc_code::ok;
    rpc_error error;
    span<const std::byte> body;
    const status decoded = decode_response(value.payload, code, error, body);

    // `completion_mutex_` is the outer lock: it keeps the slot from being
    // recycled between the lookup and the terminal claim (CF-1). `map_mutex_`
    // is innermost, taken only for the lookup and the in-flight removal, so
    // the completion callback never runs under `map_mutex_` -- a callback that
    // destroys its handle re-enters `map_mutex_` through
    // `release_slot_under_lock`, and a non-recursive `map_mutex_` would throw.
    completion_guard lk(*this);
    call_slot* slot = nullptr;
    {
        std::lock_guard<std::mutex> map(map_mutex_);
        const auto it = in_flight_.find(value.header.stream_id);
        if (it == in_flight_.end()) {
            // Unknown or late stream: dropped, never delivered to a recycled slot.
            return;
        }
        slot = it->second;
    }
    if (!decoded.ok()) {
        finish(*slot, call_state::failed, rpc_code::decode_error,
               rpc_error(rpc_code::decode_error, "response decode failed"), span<const std::byte>(),
               false);
        return;
    }
    finish(*slot, call_state::completed, code, std::move(error), body, true);
}

void client::handle_push(const frame& value) {
    push::push_envelope envelope;
    if (!push::decode_push_envelope(value.payload, envelope).ok()) {
        protocol_failure();
        return;
    }

    push::push_sink_fn sink = nullptr;
    void* sink_ctx = nullptr;
    bool alloc_failed = false;
    {
        std::lock_guard<std::mutex> map(map_mutex_);
        if (envelope.mode == push::delivery_mode::reliable) {
            const auto known = push_ack_upto_.find(value.header.stream_id);
            if (known != push_ack_upto_.end() && envelope.seq <= known->second) {
                // A replayed or stale reliable message: at-least-once delivery
                // is made at-most-once here by the monotonic per-stream
                // sequence.
                return;
            }
            try {
                push_ack_upto_[value.header.stream_id] = envelope.seq;
            } catch (...) {
                alloc_failed = true;
            }
        }
        sink = push_sink_;
        sink_ctx = push_sink_ctx_;
    }
    if (alloc_failed) {
        protocol_failure();
        return;
    }

    if (sink != nullptr) {
        push::push_message_view message;
        message.stream_id = value.header.stream_id;
        message.topic_id = envelope.topic_id;
        message.mode = envelope.mode;
        message.publisher_id = envelope.publisher_id;
        message.epoch = envelope.epoch;
        message.seq = envelope.seq;
        message.topic_seq = envelope.topic_seq;
        message.body = envelope.body;
        sink(sink_ctx, message);
    }

    if (envelope.mode == push::delivery_mode::reliable) {
        send_push_ack(value.header.stream_id, envelope.seq);
    }
}

void client::handle_push_ack(const frame&) {
    // This client only receives PUSH acknowledgements when it is the publisher;
    // the minimal slice publishes from the server side, so there is nothing to
    // match here. The branch exists so a future bidirectional implementation
    // does not silently fall into `default`.
}

void client::set_push_sink(push::push_sink_fn sink, void* ctx) noexcept {
    std::lock_guard<std::mutex> lk(map_mutex_);
    push_sink_ = sink;
    push_sink_ctx_ = ctx;
}

rpc_result<push::push_subscription> client::subscribe_push(std::string_view topic,
                                                           u64 subscriber_id,
                                                           push::delivery_mode mode,
                                                           bool ordered) {
    push::subscribe_request request;
    request.subscriber_id = subscriber_id;
    request.mode = mode;
    request.ordered = ordered;
    request.topic.assign(topic.data(), topic.size());

    const std::size_t body_size = push::encoded_subscribe_request_size(request);
    std::vector<std::byte> body(body_size);
    std::size_t written = 0;
    const status encoded = push::encode_subscribe_request(
        span<std::byte>(body.data(), body.size()), request, written);
    if (!encoded.ok()) {
        return rpc_result<push::push_subscription>(encoded.error());
    }

    auto started = start_call<std::vector<std::byte>>(
        push::push_control_service_id, push::push_control_method_subscribe,
        span<const std::byte>(body.data(), written));
    if (!started.ok()) {
        return rpc_result<push::push_subscription>(started.error());
    }
    // `start_call` only stages the frame; flush it before waiting for the reply.
    flush_pending();

    rpc_result<std::vector<std::byte>> reply = started.value().get();
    if (!reply.ok()) {
        return rpc_result<push::push_subscription>(reply.error());
    }
    const std::vector<std::byte>& bytes = reply.value();
    push::push_subscription subscription;
    if (!push::decode_push_subscription(
             span<const std::byte>(bytes.data(), bytes.size()), subscription)
             .ok()) {
        return rpc_result<push::push_subscription>(rpc_code::decode_error);
    }
    return rpc_result<push::push_subscription>(std::move(subscription));
}

void client::handle_ping(const frame& value) {
    if (value.payload.size() != 8) {
        return;
    }
    // frame_scratch_ and the write queue are writer state; a PING arrives on
    // the reader, so it takes the write lock for the reply.
    write_lock w(*this);
    if (closed_.load(std::memory_order_acquire) || conn_ == nullptr) {
        return;
    }
    frame pong;
    pong.header.type = frame_type::pong;
    pong.header.stream_id = 0;
    pong.payload = value.payload;
    const std::size_t size = encoded_frame_size(pong);
    frame_scratch_.clear();
    if (!frame_scratch_.reserve(size)) {
        return;
    }
    std::size_t written = 0;
    if (!encode_frame(span<std::byte>(frame_scratch_.data(), size), pong, written).ok()) {
        return;
    }
    // A pong that cannot be queued is dropped: the peer will time the probe out on
    // its own, and there is no frame to send a report in. Not flushing is the point
    // -- it keeps the failure from looking like a successful write.
    if (!enqueue_raw(span<const std::byte>(frame_scratch_.data(), written))) {
        return;
    }
    flush_write();
}

void client::handle_pong(const frame& value) {
    // A probe reply carries the send timestamp back verbatim. Only `map_mutex_`
    // is taken: the flow controller is protected by it, and taking any other
    // lock here would put a new edge into the four-lock order (this path runs
    // from the reader, whose only safe inner lock is the map).
    if (value.payload.size() != 8) {
        return;
    }
    const u64 token = endian::load_le64(value.payload.data());
    std::lock_guard<std::mutex> map(map_mutex_);
    if (flow_ == nullptr) {
        return;
    }
    const auto now = options_.clock(options_.clock_ctx);
    flow_->on_probe_reply(token, duration_ns_since_epoch(now));
}

void client::handle_goaway(const frame&) {
    if (closed_.load(std::memory_order_acquire)) {
        return;
    }
    (void)fail_all(rpc_code::shutting_down, "peer sent GOAWAY");
}

void client::protocol_failure() {
    if (closed_.load(std::memory_order_acquire)) {
        return;
    }
    (void)fail_all(rpc_code::protocol_error, "frame decode error");
}

// --- Writer ------------------------------------------------------------------

void client::on_write_complete(io::io_result result) noexcept {
    // Runs on the backend thread; serialize with start_request/flush_write on
    // the caller thread (A-4). A synchronous implementation re-enters through
    // flush_write, hence the recursive lock.
    write_lock lk(*this);
    write_in_flight_ = false;
    if (!result.ok()) {
        write_error_ = true;
        write_ec_ = result.ec;
        return;
    }
    out_sent_ += result.bytes;
    short_write_ = result.bytes < requested_;
    if (!in_push_) {
        // Asynchronous completion: resume the queue from the backend thread.
        flush_write();
    }
}

void client::flush_write() {
    if (write_in_flight_ || closed_.load(std::memory_order_acquire) || conn_ == nullptr) {
        return;
    }
    for (;;) {
        const std::size_t remaining = out_.size() - out_sent_;
        if (remaining == 0) {
            out_.clear();
            out_sent_ = 0;
            return;
        }
        requested_ = remaining;
        write_error_ = false;
        short_write_ = false;
        write_in_flight_ = true;
        in_push_ = true;
        conn_->sink().push(span<const std::byte>(out_.data() + out_sent_, remaining), &completion_);
        in_push_ = false;
        if (write_error_) {
            const std::error_code ec = write_ec_;
            write_error_ = false;
            closed_.store(true, std::memory_order_release);
            // Record the failure; the outermost write-lock holder reports it
            // after releasing the lock (see write_lock), so no completion lock
            // is ever taken under the write lock.
            write_fail_ec_ = ec;
            write_failed_.store(true, std::memory_order_release);
            return;
        }
        if (write_in_flight_) {
            // Asynchronous sink: the completion resumes the queue.
            return;
        }
        if (!short_write_) {
            out_.clear();
            out_sent_ = 0;
            return;
        }
        // A short write accepted a prefix; loop and re-push the remainder.
    }
}

void client::flush_pending() {
    // The caller has released every other lock. Taking `write_mutex_` here means
    // the synchronous in-process drain triggered by `flush_write` reaches
    // `on_data` (which no longer takes `completion_mutex_` under the reader lock)
    // without any facade or client lock held on this stack.
    write_lock w(*this);
    flush_write();
}

bool client::enqueue_raw(span<const std::byte> bytes) {
    return out_.append(bytes);
}

bool client::stage_probe(u64 token) {
    // Called with the write lock and `map_mutex_` held (from `start_request`).
    // The probe is deliberately not charged to the byte window: it exists to
    // measure the path, and a window that also gated its own probes could never
    // learn that the path recovered. It is still bounded by
    // `max_pending_write_bytes`, because an unbounded probe would be a way to
    // grow the output queue without limit.
    //
    // Returns false when the PING was not staged, so the caller can roll the
    // probe reservation back. Without that rollback a single dropped probe would
    // leave the "one probe outstanding" flag set forever and no probe would ever
    // be sent again.
    if (closed_.load(std::memory_order_acquire) || conn_ == nullptr) {
        return false;
    }
    std::byte payload[8];
    endian::store_le64(payload, token);
    frame ping;
    ping.header.type = frame_type::ping;
    ping.header.stream_id = 0;
    ping.payload = span<const std::byte>(payload, sizeof(payload));
    const std::size_t size = encoded_frame_size(ping);
    if (out_.size() - out_sent_ + size > options_.max_pending_write_bytes) {
        return false;
    }
    frame_scratch_.clear();
    if (!frame_scratch_.reserve(size)) {
        return false;
    }
    std::size_t written = 0;
    if (!encode_frame(span<std::byte>(frame_scratch_.data(), size), ping, written).ok()) {
        return false;
    }
    // The queue refusing the bytes is the other way a PING is not staged, and it
    // has to answer false for the same reason as the branches above: the caller
    // rolls the probe reservation back, and a probe reported as staged but never
    // queued would keep the "one probe outstanding" flag set forever.
    return enqueue_raw(span<const std::byte>(frame_scratch_.data(), written));
}

void client::send_rst(u32 stream_id, rpc_code code) {
    write_lock w(*this);
    if (closed_.load(std::memory_order_acquire) || conn_ == nullptr || !conn_->is_open()) {
        return;
    }
    std::byte payload[4];
    endian::store_le32(payload, static_cast<u32>(code));
    frame rst;
    rst.header.type = frame_type::rst_stream;
    rst.header.stream_id = stream_id;
    rst.payload = span<const std::byte>(payload, sizeof(payload));
    const std::size_t size = encoded_frame_size(rst);
    frame_scratch_.clear();
    if (!frame_scratch_.reserve(size)) {
        return;
    }
    std::size_t written = 0;
    if (!encode_frame(span<std::byte>(frame_scratch_.data(), size), rst, written).ok()) {
        return;
    }
    // A reset that cannot be queued is dropped, and the peer keeps waiting for the
    // stream. That is worse than a slow failure but not worse than a truncated
    // frame, which is what appending to a full buffer would produce.
    if (!enqueue_raw(span<const std::byte>(frame_scratch_.data(), written))) {
        return;
    }
    flush_write();
}

// --- Completion scope and deferred RST ---------------------------------------

client::completion_guard::completion_guard(client& owner) noexcept : owner_(owner) {
    owner_.completion_mutex_.lock();
    ++owner_.completion_depth_;
}

client::completion_guard::~completion_guard() {
    // Decide "outermost" while the lock is still held. Reading the depth after
    // the unlock would race with another thread's `++` under the lock (the
    // counter is only written while the mutex is held).
    const bool outermost = (--owner_.completion_depth_ == 0);
    owner_.completion_mutex_.unlock();
    if (outermost) {
        owner_.flush_pending_rst();
    }
}

void client::queue_rst(u32 stream_id) noexcept {
    // Called with `completion_mutex_` held. The vector may allocate, and every
    // caller is `noexcept`; a failure drops the RST, which is best-effort (the
    // peer also learns about the cancel from the deadline or the close).
    try {
        rst_pending_.push_back(stream_id);
    } catch (...) {
    }
}

void client::flush_pending_rst() noexcept {
    // No completion lock is held here, so taking `write_mutex_` is safe even
    // though the write path may re-enter the reader and take the completion
    // lock. Swap the queue out under the lock, then write outside it.
    std::vector<u32> pending;
    {
        std::lock_guard<std::recursive_mutex> lk(completion_mutex_);
        if (rst_pending_.empty()) {
            return;
        }
        pending.swap(rst_pending_);
    }
    for (const u32 stream_id : pending) {
        send_rst(stream_id, rpc_code::canceled);
    }
}

void client::send_push_ack(u32 stream_id, u64 seq) {
    write_lock w(*this);
    if (closed_.load(std::memory_order_acquire) || conn_ == nullptr || !conn_->is_open()) {
        return;
    }
    std::byte payload[push::push_ack_size];
    endian::store_le64(payload, seq);
    frame ack;
    ack.header.type = frame_type::push_ack;
    ack.header.stream_id = stream_id;
    ack.payload = span<const std::byte>(payload, sizeof(payload));
    const std::size_t size = encoded_frame_size(ack);
    frame_scratch_.clear();
    if (!frame_scratch_.reserve(size)) {
        return;
    }
    std::size_t written = 0;
    if (!encode_frame(span<std::byte>(frame_scratch_.data(), size), ack, written).ok()) {
        return;
    }
    // An acknowledgement that cannot be queued leaves the server thinking the push
    // was not received, so it retransmits. Dropping it is the safe direction: a
    // duplicate delivery is handled by the sequence number, a lost frame is not.
    if (!enqueue_raw(span<const std::byte>(frame_scratch_.data(), written))) {
        return;
    }
    flush_write();
}

// --- Call lifecycle ----------------------------------------------------------

call_id client::next_call_id() {
    call_id id;
    id.client_id = options_.client_id;
    id.client_epoch = options_.client_epoch;
    id.seq = ++seq_;
    return id;
}

call_slot* client::begin_call(u64 service_id, u64 method_id, const call_context& ctx,
                              rpc_error& failure,
                              std::chrono::steady_clock::time_point& now_out) {
    // The slot fields, the stream-id allocation (which probes in_flight_) and
    // the closed check are all map state.
    std::lock_guard<std::mutex> map(map_mutex_);
    if (closed_.load(std::memory_order_acquire)) {
        failure = rpc_error(rpc_code::shutting_down, "client is closed");
        return nullptr;
    }
    call_slot* slot = pool_.try_acquire();
    if (slot == nullptr) {
        failure = rpc_error(rpc_code::resource_exhausted, "call slot pool exhausted");
        return nullptr;
    }

    const auto now = options_.clock(options_.clock_ctx);
    now_out = now;
    if (ctx.has_deadline && ctx.deadline <= now) {
        pool_.release(slot);
        failure = rpc_error(rpc_code::deadline_exceeded, "deadline already passed");
        return nullptr;
    }

    slot->service_id = service_id;
    slot->method_id = method_id;
    slot->idempotent = ctx.idempotent;
    slot->one_way = ctx.mode == delivery_mode::unreliable;
    if (ctx.has_idempotency_key && !ctx.idempotency.empty()) {
        slot->idem = ctx.idempotency;
    } else {
        slot->idem = next_call_id();
    }
    if (ctx.has_deadline) {
        slot->has_deadline = true;
        slot->deadline = ctx.deadline;
    } else if (options_.default_timeout.count() > 0) {
        slot->has_deadline = true;
        slot->deadline = now + options_.default_timeout;
    }
    slot->stream_id = allocate_stream_id(stream_seq_, &client::in_flight_probe, this);
    slot->owner = this;
    slot->owner_cancel = &client::cancel_thunk;
    slot->owner_detach = &client::detach_thunk;
    slot->owner_release = &client::release_thunk;
    set_state(*slot, call_state::queued);
    return slot;
}

namespace {

// Everything that decides which envelope blocks a REQUEST carries. One builder
// serves both the real send path and the size prediction, so the two can never
// disagree about the byte count.
struct envelope_inputs {
    u64 service_id = 0;
    u64 method_id = 0;
    span<const std::byte> body;
    const obs::trace_context* trace = nullptr;
    bool has_deadline = false;
    u64 deadline_ns = 0;
    bool has_idempotency_key = false;
    span<const std::byte> idempotency_key;
};

void build_envelope(const envelope_inputs& in, call_envelope& envelope) noexcept {
    envelope.service_id = in.service_id;
    envelope.method_id = in.method_id;
    envelope.codec = codec_id::crpc_binary;
    envelope.body = in.body;

    trace_extension& ext = envelope.extensions;
    ext.flags = 0;
    if (in.trace != nullptr && in.trace->valid()) {
        ext.flags = static_cast<u8>(ext.flags | trace_extension::has_trace_id |
                                    trace_extension::has_span_id);
        ext.trace_id = in.trace->trace.bytes;
        ext.span_id = in.trace->span.bytes;
        if (in.trace->parent.valid()) {
            ext.flags = static_cast<u8>(ext.flags | trace_extension::has_parent_span_id);
            ext.parent_span_id = in.trace->parent.bytes;
        }
    }
    if (in.has_deadline) {
        ext.flags = static_cast<u8>(ext.flags | trace_extension::has_deadline);
        ext.deadline_ns = in.deadline_ns;
    }
    if (in.has_idempotency_key) {
        ext.flags = static_cast<u8>(ext.flags | trace_extension::has_idempotency_key);
        ext.idempotency_key = in.idempotency_key;
    }
    if (ext.flags != 0) {
        envelope.call_flags = static_cast<u8>(envelope.call_flags | call_flag_has_extensions);
    }
}

} // namespace

std::size_t client::predicted_frame_size(u64 service_id, u64 method_id,
                                         span<const std::byte> body,
                                         const call_context& ctx) noexcept {
    // Uses the same envelope builder and the same `encoded_call_envelope_size`
    // as `start_request`, so this is exact rather than approximate. An upper
    // bound would refuse a call that would have fitted, and would make the
    // pre-check and the real charge disagree about the same lane.
    envelope_inputs in;
    in.service_id = service_id;
    in.method_id = method_id;
    in.body = body;
    in.trace = &ctx.trace;
    in.has_deadline = ctx.has_deadline;
    in.deadline_ns = 0; // the field is 8 bytes whatever the value
    in.has_idempotency_key = ctx.idempotent || ctx.has_idempotency_key;
    // `start_request` always sends the canonical 32-byte call id, so only the
    // presence of the block matters for the size.
    static const std::array<std::byte, 32> key{};
    in.idempotency_key = span<const std::byte>(key.data(), key.size());

    call_envelope envelope;
    build_envelope(in, envelope);

    frame request;
    request.header.type = ctx.mode == delivery_mode::unreliable ? frame_type::one_way
                                                                : frame_type::request;
    request.header.flags = ctx.lane.has_value() ? frame_flag_ordered : 0;
    request.payload = span<const std::byte>(nullptr, encoded_call_envelope_size(envelope));
    return encoded_frame_size(request);
}

status client::start_request(call_slot& slot, u64 service_id, u64 method_id,
                             span<const std::byte> body, const call_context& ctx,
                             const std::chrono::steady_clock::time_point& started_at) {
    // The wire carries the time left, not an absolute instant: steady-clock
    // epochs are per process, so an absolute value means nothing on the peer
    // (P12, design section 8.5). A deadline that has already elapsed is sent as
    // zero so the server rejects without executing.
    u64 deadline_ns = 0;
    if (slot.has_deadline) {
        const auto now = options_.clock(options_.clock_ctx);
        deadline_ns = slot.deadline > now
                          ? static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 slot.deadline - now)
                                                 .count())
                          : 0;
    }
    const std::array<std::byte, 32> key = slot.idem.encode();

    envelope_inputs in;
    in.service_id = service_id;
    in.method_id = method_id;
    in.body = body;
    in.trace = &ctx.trace;
    in.has_deadline = slot.has_deadline;
    in.deadline_ns = deadline_ns;
    in.has_idempotency_key = ctx.idempotent || ctx.has_idempotency_key;
    in.idempotency_key = span<const std::byte>(key.data(), key.size());
    call_envelope envelope;
    build_envelope(in, envelope);

    {
        write_lock w(*this);
        const std::size_t payload_size = encoded_call_envelope_size(envelope);
        payload_scratch_.clear();
        if (!payload_scratch_.reserve(payload_size)) {
            return status{rpc_code::resource_exhausted};
        }
        std::size_t payload_written = 0;
        status encoded = encode_call_envelope(
            span<std::byte>(payload_scratch_.data(), payload_size), envelope, payload_written);
        if (!encoded.ok()) {
            return encoded;
        }

        frame request;
        request.header.type = slot.one_way ? frame_type::one_way : frame_type::request;
        request.header.flags = slot.one_way ? frame_flag_unreliable : 0;
        request.header.stream_id = slot.stream_id;
        request.payload = span<const std::byte>(payload_scratch_.data(), payload_written);
        // Ordered delivery opt-in: a lane token on the context becomes the
        // frame's ORDERED extension. `lane_id == 0` is sent as-is; the server
        // treats it (and a missing token) as "no ordering", which keeps the
        // wire honest about what the caller asked for.
        if (ctx.lane.has_value()) {
            request.header.flags =
                static_cast<u8>(request.header.flags | frame_flag_ordered);
            request.ordered.lane_id = ctx.lane->lane_id;
            request.ordered.lane_seq = ctx.lane->seq;
        }

        const std::size_t frame_size = encoded_frame_size(request);
        if (out_.size() - out_sent_ + frame_size > options_.max_pending_write_bytes) {
            return status{rpc_code::resource_exhausted};
        }
        frame_scratch_.clear();
        if (!frame_scratch_.reserve(frame_size)) {
            return status{rpc_code::resource_exhausted};
        }
        std::size_t frame_written = 0;
        encoded = encode_frame(span<std::byte>(frame_scratch_.data(), frame_size), request,
                               frame_written);
        if (!encoded.ok()) {
            return encoded;
        }

        {
            std::lock_guard<std::mutex> map(map_mutex_);
            // Re-check under the map lock: a close between begin_call and here
            // must reject the call rather than publish a slot nothing will
            // ever complete.
            if (closed_.load(std::memory_order_acquire)) {
                return status{rpc_code::shutting_down};
            }
            // Flow control is charged and released on this exact path, so it is
            // decided here, under the same lock that publishes the slot: a
            // charge that was decided outside the publish could be released
            // before it was ever recorded.
            const u64 lane = ctx.lane.has_value() ? ctx.lane->lane_id : 0;
            if (flow_ != nullptr && !slot.one_way) {
                flow_->observe_now(duration_ns_since_epoch(started_at));
                if (!flow_->can_admit(lane, frame_size)) {
                    // A local admission refusal. The marker is what lets the
                    // facade classify it as `local_gate` (never retried, never
                    // counted against the endpoint) instead of a capacity
                    // failure, and what lets a caller tell "not now" from "no".
                    rpc_error refused(rpc_code::resource_exhausted,
                                      "flow control refused the call");
                    refused.metadata.set("crpc.flow_rejected", "1");
                    return status{std::move(refused)};
                }
            }
            if (flow_ != nullptr && !slot.one_way) {
                // Charge first: if the charge table cannot index this call,
                // nothing has been published or appended yet, so the refusal is
                // simply an early return. An unindexed charge could never be
                // released and would leak window bytes until the connection
                // stopped admitting anything.
                if (!flow_->on_admitted(lane, frame_size, slot.stream_id, slot.generation)) {
                    rpc_error refused(rpc_code::resource_exhausted,
                                      "flow control charge table exhausted");
                    refused.metadata.set("crpc.flow_rejected", "1");
                    return status{std::move(refused)};
                }
            }
            if (!out_.append(span<const std::byte>(frame_scratch_.data(), frame_written))) {
                if (flow_ != nullptr && !slot.one_way) {
                    flow_->release(slot.stream_id, slot.generation, false);
                }
                return status{rpc_code::resource_exhausted};
            }
            slot.written = true;
            in_flight_.emplace(slot.stream_id, &slot);
            if (slot.has_deadline) {
                deadlines_.emplace(slot.deadline, &slot);
            }
            if (flow_ != nullptr && !slot.one_way) {
                // A probe is staged, not flushed: flushing here would drain the
                // peer synchronously and take `completion_mutex_` while the map
                // lock is held, which is the documented ABBA cycle. The caller
                // flushes outside every lock.
                const u64 token = flow_->begin_probe();
                if (token != ~0ull) {
                    // If the PING cannot be staged (write bound, closed
                    // connection) the reservation must be rolled back: leaving
                    // it outstanding would stop every future probe.
                    if (stage_probe(token)) {
                        flow_->confirm_probe_staged();
                    } else {
                        flow_->abort_probe();
                    }
                }
            }
        }
        set_state(slot, call_state::sent);
    }

    // Complete a ONE_WAY outside the write lock: finish takes the completion
    // lock, and no path may hold the write lock while taking it. The staged
    // bytes are flushed by the caller's `flush_pending()` (outside every lock
    // this path can reach `completion_mutex_` under), so a synchronous drain
    // cannot invert against a completion.
    if (slot.one_way) {
        // `finish` takes `completion_mutex_` then `map_mutex_` (the single order
        // used in this file), claims the terminal state, removes the in-flight
        // entry and runs the callback with the map lock released (CF-1).
        finish(slot, call_state::completed, rpc_code::ok, rpc_error{},
               span<const std::byte>(), false);
    }
    return status{};
}

void client::finish(call_slot& slot, call_state terminal, rpc_code code, rpc_error error,
                    span<const std::byte> body, bool acknowledged) {
    // Hold the completion lock from before the claim until after the callback
    // returns, so a handle destructor cannot detach and recycle the slot while
    // this path still touches it (CF-1). `completion_mutex_` is the outer lock
    // and `map_mutex_` the inner one; the in-flight removal happens between the
    // claim and the callback, so a callback that destroys its handle finds the
    // entry already gone and cannot hand the slot back to the pool while the
    // removal below still reads it. The callback itself runs with the map lock
    // released, so it may re-enter the map.
    completion_guard lk(*this);
    if (!finish_slot(slot, terminal, code, std::move(error), body)) {
        return;
    }
    {
        std::lock_guard<std::mutex> map(map_mutex_);
        remove_in_flight(slot, acknowledged);
    }
    // The atomic claim inside invoke_callback decides whether `then` or this
    // completion path runs the callback exactly once.
    slot.invoke_callback();
}

void client::cancel_slot(call_slot& slot) noexcept {
    bool won = false;
    bool written = false;
    {
        // `completion_mutex_` is the outer lock and `map_mutex_` the inner one.
        // Holding both across the claim and the in-flight removal keeps the slot
        // from being recycled in between (CF-1).
        completion_guard lk(*this);
        std::lock_guard<std::mutex> map(map_mutex_);
        won = finish_slot(slot, call_state::canceled, rpc_code::canceled,
                          rpc_error(rpc_code::canceled, "call canceled"),
                          span<const std::byte>());
        if (won) {
            written = slot.written;
            // A cancellation never received a response, so it returns the
            // window bytes without growing the window.
            remove_in_flight(slot, false);
        }
    }
    if (!won) {
        return;
    }
    // The RST is a write, and writing takes `write_mutex_` whose path re-enters
    // the reader and therefore `completion_mutex_`. A cancel that runs inside a
    // `then` callback (a callback destroying a written sibling) is nested inside
    // an outer completion scope, so writing here would invert the two locks.
    // Queue it instead: the outermost completion scope writes it after the lock
    // is released.
    if (written) {
        completion_guard lk(*this);
        queue_rst(slot.stream_id);
    }
    completion_guard lk(*this);
    slot.invoke_callback();
}

bool client::fail_all(rpc_code code, std::string_view message) {
    // `completion_mutex_` is the outer lock and is taken BEFORE the map, and it
    // is held across the whole drain. The map is only held to pick the next
    // victim, never across `finish`.
    //
    // Same hazard as `poll`: a `then` callback runs under the completion lock
    // and may legally destroy a *sibling* handle, which detaches/cancels that
    // sibling and returns its slot to the pool. A snapshot of raw `call_slot*`
    // taken before the loop can therefore point at an already-recycled slot.
    // Re-reading `in_flight_` each round only ever hands `finish` a slot that
    // was observed live under the lock, and holding the completion lock keeps
    // it alive until its claim is done.
    completion_guard lk(*this);
    {
        std::lock_guard<std::mutex> map(map_mutex_);
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
    }
    for (;;) {
        call_slot* victim = nullptr;
        // The release key is captured here, under the map lock, together with
        // the victim. It is never re-read from the slot later: `finish` runs the
        // user callback, which may recycle the slot (a sibling destroy), and a
        // recycled slot's generation belongs to a different call.
        u32 victim_stream_id = 0;
        u32 victim_generation = 0;
        {
            std::lock_guard<std::mutex> map(map_mutex_);
            if (in_flight_.empty()) {
                deadlines_.clear();
                break;
            }
            victim_stream_id = in_flight_.begin()->first;
            victim = in_flight_.begin()->second;
            victim_generation = victim != nullptr ? victim->generation : 0;
        }
        if (victim != nullptr) {
            try {
                finish(*victim, call_state::failed, code,
                       rpc_error(code, std::string(message)), span<const std::byte>(), false);
            } catch (...) {
                // Allocation failure while building the failure error. The
                // client is closed; drop this slot's completion.
            }
        }
        // Guarantee progress even when `finish` lost the claim or threw: drop
        // the entry that was picked if it is still the head of the map. The
        // flow charge is released here too, because this path skips
        // `remove_in_flight`; the `(stream_id, generation)` key makes the
        // release idempotent when `finish` already ran it.
        std::lock_guard<std::mutex> map(map_mutex_);
        if (!in_flight_.empty() && in_flight_.begin()->second == victim) {
            in_flight_.erase(in_flight_.begin());
            if (flow_ != nullptr && victim != nullptr) {
                flow_->release(victim_stream_id, victim_generation, false);
            }
        }
    }
    if (flow_ != nullptr) {
        // A reconnect starts from a clean estimate: the path may be entirely
        // different, and carrying the old baseline over would begin the new
        // connection with the wrong window. `reset` also restores slow start,
        // which is the correct state for a path nobody has measured yet.
        std::lock_guard<std::mutex> map(map_mutex_);
        flow_->reset();
    }
    return true;
}

void client::remove_in_flight(call_slot& slot, bool acknowledged) {
    const auto it = in_flight_.find(slot.stream_id);
    if (it != in_flight_.end() && it->second == &slot) {
        in_flight_.erase(it);
    }
    if (flow_ != nullptr) {
        // Give the window bytes back. The key is `(stream_id, generation)`, so
        // this is idempotent against the `fail_all` fallback and a recycled
        // stream id (a new generation) is never released by a stale call.
        flow_->release(slot.stream_id, slot.generation, acknowledged);
    }
    remove_deadline(slot);
}

void client::remove_deadline(call_slot& slot) {
    if (!slot.has_deadline) {
        return;
    }
    const auto range = deadlines_.equal_range(slot.deadline);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == &slot) {
            deadlines_.erase(it);
            return;
        }
    }
}

void client::poll() noexcept {
    // Collecting expired slots and building each `rpc_error` allocates, and this
    // function is `noexcept` (the facade's timer thread calls it). An allocation
    // failure must not terminate; every call whose deadline entry was not
    // consumed stays armed, so a failed poll is retried on the next tick.
    //
    // Shape: take `completion_mutex_`, then repeatedly (a) pick ONE expired,
    // still-live slot under `map_mutex_`, (b) drop the map lock, (c) finish that
    // slot, and (d) loop. A snapshot of raw `call_slot*` collected up front is
    // NOT safe here: `finish` runs the user `then` callback under the
    // (recursive) completion lock, and that callback may legally destroy a
    // *sibling* handle. Destroying a sibling detaches it, cancels it and
    // returns its slot to the pool, so a pre-collected list can hold a pointer
    // to a slot that is already recycled and possibly re-published. Re-reading
    // the map each round means a slot is only ever touched after it was observed
    // live under the lock, and `completion_mutex_` (held throughout) prevents
    // any concurrent detach/release in between.
    try {
        completion_guard lk(*this);
        for (;;) {
            call_slot* victim = nullptr;
            {
                std::lock_guard<std::mutex> map(map_mutex_);
                if (closed_.load(std::memory_order_acquire)) {
                    return;
                }
                if (deadlines_.empty()) {
                    return;
                }
                const auto now = options_.clock(options_.clock_ctx);
                auto it = deadlines_.begin();
                while (it != deadlines_.end() && it->first <= now) {
                    call_slot* slot = it->second;
                    if (slot == nullptr || result_ready(*slot)) {
                        // Stale duplicate or already finished: consume it so the
                        // loop makes progress.
                        it = deadlines_.erase(it);
                        continue;
                    }
                    victim = slot;
                    // The deadline entry is NOT erased here. `finish` removes it
                    // through `remove_in_flight`, and building the error below
                    // allocates: if that throws, the entry must still be armed so
                    // the next poll retries the call instead of leaving it in
                    // flight with no deadline at all.
                    break;
                }
                if (victim == nullptr) {
                    return;
                }
            }
            rpc_error error(rpc_code::deadline_exceeded, "deadline exceeded");
            // Local-only marker: the facade distinguishes this from a
            // server-produced deadline (which must not open the breaker).
            error.metadata.set("crpc.local_timeout", "1");
            if (victim->written && !victim->idempotent) {
                error.metadata.set("unknown_side_effect", "1");
            }
            finish(*victim, call_state::timed_out, rpc_code::deadline_exceeded,
                   std::move(error), span<const std::byte>(), false);
        }
    } catch (...) {
        // Allocation failure while building a deadline error. The client stays
        // usable and the call's deadline entry is still armed, so the next poll
        // retries it. The catch body performs no allocation.
    }
}

bool client::in_flight_probe(void* ctx, u32 id) noexcept {
    const auto* self = static_cast<const client*>(ctx);
    return self->in_flight_.find(id) != self->in_flight_.end();
}

void client::cancel_thunk(void* ctx, call_slot* slot) noexcept {
    static_cast<client*>(ctx)->cancel_slot(*slot);
}

void client::detach_slot(call_slot& slot) noexcept {
    // Every completion callback invocation runs under `completion_mutex_`, so
    // holding it here guarantees that once this returns no callback is running
    // and none can start for this slot.
    completion_guard lk(*this);
    slot.callback_armed.store(false, std::memory_order_release);
    slot.callback_invoke = nullptr;
    slot.callback_owner = nullptr;
}

void client::detach_thunk(void* ctx, call_slot* slot) noexcept {
    static_cast<client*>(ctx)->detach_slot(*slot);
}

void client::release_thunk(void* ctx, call_slot* slot) noexcept {
    static_cast<client*>(ctx)->release_slot_under_lock(slot);
}

void client::release_slot_under_lock(call_slot* slot) noexcept {
    if (slot == nullptr) {
        return;
    }
    // Lock order: `completion_mutex_` outer, `map_mutex_` inner -- the single
    // order used by every other path in this file (`handle_response`,
    // `cancel_slot`, `start_request`'s one-way completion, `fail_all`, `poll`).
    // Taking the map lock first here was the second half of an ABBA cycle with
    // `handle_response`: a response thread held `completion_mutex_` and wanted
    // `map_mutex_`, while a handle destructor held `map_mutex_` and wanted
    // `completion_mutex_` (live hang, `.opencode/tmp/pd-live-stack.txt`).
    // Taking `completion_mutex_` here also synchronizes with any callback still
    // running for this slot (CF-1), and the in-flight removal happens under the
    // map lock so a response thread's claim cannot race the reset in
    // `pool_.release`.
    completion_guard lk(*this);
    std::lock_guard<std::mutex> map(map_mutex_);
    // A handle destroyed without a terminal claim never received a response.
    remove_in_flight(*slot, false);
    pool_.release(slot);
}

rpc_code client::map_error(std::error_code ec) noexcept {
    if (ec == std::errc::connection_refused) {
        return rpc_code::connection_refused;
    }
    if (ec == std::errc::broken_pipe) {
        return rpc_code::broken_pipe;
    }
    if (ec == std::errc::operation_canceled) {
        return rpc_code::canceled;
    }
    if (ec == std::errc::connection_reset) {
        return rpc_code::connection_reset;
    }
    return rpc_code::connection_reset;
}

} // namespace crpc::rpc
