// CRPC -- in-process (loopback) transport backend.
//
// Scheduling contract
// -------------------
// There is no hidden I/O thread.  Every byte moves on the stack of a thread
// that called `push`, `resume_read` or `close`:
//
//   1. `push` copies as much of the span as fits into the peer's bounded
//      queue.  If the whole span fits, the completion runs before `push`
//      returns.  If only a prefix fits, the remainder stays borrowed from the
//      caller (valid until `complete`, per I7) and the completion is deferred.
//   2. `push` then enters the drain loop.  A per-pair `draining` flag makes the
//      loop single-entry: a nested `push` issued from inside `on_data` only
//      enqueues and returns, and the outer loop keeps going.  Stack depth is
//      therefore constant (one drain frame), not a function of traffic.
//   3. The drain loop hands one chunk at a time to the peer's `on_data` with no
//      lock held.  A short or zero return pauses that direction and retains the
//      tail; only `resume_read` clears the pause.
//   4. EOF and error notifications are delivered by the same drain loop, so
//      they are serialized with data delivery and can never nest inside it.
//
// Bytes are always copied into the channel; the span handed to `on_data`
// therefore points at backend-owned storage and stays valid for the duration
// of the call even if the writer closes concurrently.  A chunk that is
// currently being delivered is detached from the queue, which bounds the
// transient overshoot at one chunk.
#include <crpc/io.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

namespace crpc {
namespace {

struct chunk_node {
    chunk_node* next = nullptr;
    std::unique_ptr<std::byte[]> data;
    std::size_t size = 0;
    std::size_t offset = 0;

    [[nodiscard]] std::size_t remaining() const noexcept { return size - offset; }
};

[[nodiscard]] chunk_node* make_chunk(std::size_t size) noexcept {
    chunk_node* node = new (std::nothrow) chunk_node();
    if (node == nullptr) {
        return nullptr;
    }
    node->data.reset(new (std::nothrow) std::byte[size]);
    if (!node->data) {
        delete node;
        return nullptr;
    }
    node->size = size;
    return node;
}

// One direction of flow: the writer at endpoint k sends to the reader at
// endpoint 1-k.
struct channel {
    // `reader` points at the peer connection's built-in adapter, so the peer's
    // destructor must not run while a callback is executing on another thread
    // (that was a real vptr race: ~source vs drain's on_eof). `delivering`
    // counts callbacks currently running into that adapter; the destructor
    // clears `reader` and waits for it to reach zero. Both transitions happen
    // under the state mutex, and no lock is held while the callback runs, so
    // this cannot invert against a mutex the callback itself takes.
    int delivering = 0;
    std::condition_variable delivery_cv;

    chunk_node* head = nullptr;
    chunk_node* tail = nullptr;
    std::size_t queued_bytes = 0;
    std::size_t capacity = 1;

    io::source* reader = nullptr;

    bool reader_paused = false;   // on_data returned short; wait for resume
    // Number of resumptions the writer of this channel must still honour.
    //
    // A `resume_read()` cannot simply clear `reader_paused`: the drain loop
    // calls `on_data` with the state lock released and only records the pause
    // after that call returns, so a resume arriving in that window clears a
    // pause that has not been written yet and the pause the loop goes on to
    // record silently discards the wake. The channel then stays paused with
    // bytes queued and nothing to schedule it again.
    //
    // Every resume therefore increments this counter, and the drain loop, in
    // the same critical section in which it records the pause, consumes one
    // demand and skips that pause instead. The counter is the only place a
    // demand can be remembered, because `reader_paused` alone cannot represent
    // "paused now, but a resume was already asked for".
    std::uint64_t pending_wakes = 0;
    bool reader_closed = false;   // the reading endpoint called close()
    bool dead = false;            // terminal state; no further callbacks
    bool eof_pending = false;     // writer closed; deliver on_eof
    bool eof_delivered = false;
    bool error_pending = false;   // injected read error; deliver on_error
    std::error_code error_code{};

    std::size_t delay_pushes = 0; // while > 0, delivery is deferred

    // At most one deferred push (I5).  The pointer borrows the caller's span
    // until `pending_completion` runs.
    const std::byte* pending_data = nullptr;
    std::size_t pending_remaining = 0;
    std::size_t pending_accepted = 0;
    io::completion* pending_completion = nullptr;

    chunk_node* held = nullptr;   // first half of a reorder pair
    std::size_t reorder_pairs = 0;
};

struct endpoint_slot {
    // `bound` is the user's source (installed with bind_source). The transport
    // calls into it from whichever thread drives the drain, while the owner may
    // destroy it on another thread -- bind_source explicitly does not take
    // ownership. `delivering` counts calls in progress and `delivery_cv`
    // publishes their completion, both under `inproc_state::mutex`;
    // bind_source (including bind_source(nullptr)) waits for the count to drain
    // before it returns. Without this, `~source` raced the virtual call (TSan,
    // both the HTTP session's reader_source and the facade's guarded_source).
    io::source* bound = nullptr;
    int delivering = 0;
    std::condition_variable delivery_cv;
    bool closed = false;
    inproc_faults faults;
};

} // namespace

struct inproc_state {
    mutable std::mutex mutex;
    std::atomic<bool> draining{false};
    channel dir[2];       // dir[k]: endpoint k -> endpoint 1-k
    endpoint_slot ep[2];
    inproc_options opt;
};

namespace {

void append_node(channel& ch, chunk_node* node) noexcept {
    node->next = nullptr;
    if (ch.tail != nullptr) {
        ch.tail->next = node;
    } else {
        ch.head = node;
    }
    ch.tail = node;
}

void drop_all(channel& ch) noexcept {
    chunk_node* node = ch.head;
    while (node != nullptr) {
        chunk_node* next = node->next;
        delete node;
        node = next;
    }
    ch.head = nullptr;
    ch.tail = nullptr;
    delete ch.held;
    ch.held = nullptr;
    ch.queued_bytes = 0;
}

// Moves as much of the deferred push into the queue as fits.  Returns a
// completion that must run outside the lock, or nullptr.
io::completion* promote_locked(channel& ch, io::io_result& out) noexcept {
    if (ch.pending_completion == nullptr) {
        return nullptr;
    }
    const std::size_t space = ch.capacity - ch.queued_bytes;
    if (space == 0) {
        return nullptr;
    }
    const std::size_t count = std::min(space, ch.pending_remaining);
    chunk_node* node = make_chunk(count);
    if (node == nullptr) {
        io::completion* c = ch.pending_completion;
        ch.pending_completion = nullptr;
        ch.pending_data = nullptr;
        ch.pending_remaining = 0;
        out = io::io_result{std::make_error_code(std::errc::not_enough_memory),
                         ch.pending_accepted};
        return c;
    }
    std::memcpy(node->data.get(), ch.pending_data, count);
    append_node(ch, node);
    ch.queued_bytes += count;
    ch.pending_data += count;
    ch.pending_remaining -= count;
    if (ch.pending_remaining == 0) {
        io::completion* c = ch.pending_completion;
        ch.pending_completion = nullptr;
        out = io::io_result{{}, ch.pending_accepted};
        return c;
    }
    return nullptr;
}

void drain(inproc_state& st) noexcept;

void close_endpoint(inproc_state& st, int index) noexcept {
    io::completion* canceled = nullptr;
    io::completion* broken = nullptr;
    {
        std::lock_guard<std::mutex> lk(st.mutex);
        if (st.ep[index].closed) {
            return;
        }
        st.ep[index].closed = true;

        channel& out = st.dir[index];
        channel& in = st.dir[1 - index];

        // Outgoing side: cancel the in-flight write and discard whatever has
        // not been delivered yet, then tell the peer this side is gone.
        canceled = out.pending_completion;
        out.pending_completion = nullptr;
        out.pending_data = nullptr;
        out.pending_remaining = 0;
        out.delay_pushes = 0;
        drop_all(out);
        if (!out.dead && !out.reader_closed) {
            out.eof_pending = true;
        }

        // Incoming side: this endpoint no longer accepts callbacks.  A write
        // the peer still has in flight toward us fails as a broken pipe.
        in.reader_closed = true;
        in.reader_paused = false;
        broken = in.pending_completion;
        in.pending_completion = nullptr;
        in.pending_data = nullptr;
        in.pending_remaining = 0;
        drop_all(in);
        in.dead = true;
    }
    if (canceled != nullptr) {
        canceled->complete(
            io::io_result{std::make_error_code(std::errc::operation_canceled), 0});
    }
    if (broken != nullptr) {
        broken->complete(
            io::io_result{std::make_error_code(std::errc::broken_pipe), 0});
    }
    drain(st);
}

// Called with the state lock held.  Mirrors the decision logic in `drain`;
// it exists to close the check-then-clear window at the end of the loop.
bool has_pending_work_locked(const inproc_state& st) noexcept {
    for (int k = 0; k < 2; ++k) {
        const channel& ch = st.dir[k];
        if (ch.dead) {
            continue;
        }
        if (ch.reader_closed || ch.error_pending || ch.eof_pending) {
            return true;
        }
        if (!ch.reader_paused && ch.delay_pushes == 0 && ch.head != nullptr) {
            return true;
        }
        if (ch.pending_completion != nullptr && ch.queued_bytes < ch.capacity) {
            return true;
        }
    }
    return false;
}

void drain(inproc_state& st) noexcept {
    if (st.draining.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    for (;;) {
        bool work = false;
        for (int k = 0; k < 2; ++k) {
            channel& ch = st.dir[k];

            enum class action { none, data, eof, error };
            action act = action::none;
            io::source* src = nullptr;
            std::error_code ec{};
            chunk_node* node = nullptr;
            std::size_t offered = 0;
            io::completion* ready = nullptr;
            io::io_result ready_result{};

            {
                std::lock_guard<std::mutex> lk(st.mutex);
                if (ch.dead) {
                    // Terminal state reached; nothing may be delivered.
                } else if (ch.reader_closed) {
                    ch.dead = true;
                    ready = ch.pending_completion;
                    if (ready != nullptr) {
                        ready_result =
                            io::io_result{std::make_error_code(std::errc::broken_pipe), 0};
                        ch.pending_completion = nullptr;
                        ch.pending_data = nullptr;
                        ch.pending_remaining = 0;
                    }
                    drop_all(ch);
                    work = true;
                } else if (ch.error_pending) {
                    act = action::error;
                    ec = ch.error_code;
                    ch.error_pending = false;
                    ch.dead = true;
                    ready = ch.pending_completion;
                    if (ready != nullptr) {
                        ready_result = io::io_result{ec, ch.pending_accepted};
                        ch.pending_completion = nullptr;
                        ch.pending_data = nullptr;
                        ch.pending_remaining = 0;
                    }
                    drop_all(ch);
                    src = ch.reader;
                    work = true;
                } else if (ch.eof_pending) {
                    act = action::eof;
                    ch.eof_pending = false;
                    ch.eof_delivered = true;
                    ch.dead = true;
                    drop_all(ch);
                    src = ch.reader;
                    work = true;
                } else if (!ch.reader_paused && ch.delay_pushes == 0 &&
                           ch.head != nullptr) {
                    node = ch.head;
                    ch.head = node->next;
                    if (ch.head == nullptr) {
                        ch.tail = nullptr;
                    }
                    node->next = nullptr;
                    offered = node->remaining();
                    ch.queued_bytes -= offered;
                    src = ch.reader;
                    act = action::data;
                    work = true;
                } else {
                    ready = promote_locked(ch, ready_result);
                    if (ready != nullptr) {
                        work = true;
                    }
                }
                // Announce the delivery while still under the state mutex: the
                // peer's destructor clears `reader` under this same lock, so
                // either it sees this count and waits, or the callback below
                // sees a null reader and is skipped. No lock is held while the
                // callback runs, so it cannot invert against a mutex the
                // callback takes (the previous design did exactly that).
                if (src != nullptr) {
                    ++ch.delivering;
                }
            }

            std::size_t consumed = 0;
            if (src != nullptr) {
                if (act == action::error) {
                    src->on_error(ec);
                } else if (act == action::eof) {
                    src->on_eof();
                } else if (act == action::data) {
                    consumed = src->on_data(span<const std::byte>(
                        node->data.get() + node->offset, offered));
                    if (consumed > offered) {
                        consumed = offered; // I9: clamp, never read out of bounds
                    }
                }
            }
            if (act != action::data) {
                if (src != nullptr) {
                    std::lock_guard<std::mutex> lk(st.mutex);
                    if (--ch.delivering == 0) {
                        ch.delivery_cv.notify_all();
                    }
                }
            } else {

                io::completion* done = nullptr;
                io::io_result done_result{};
                {
                    std::lock_guard<std::mutex> lk(st.mutex);
                    if (src != nullptr && --ch.delivering == 0) {
                        ch.delivery_cv.notify_all();
                    }
                    node->offset += consumed;
                    const bool terminal = ch.dead || ch.reader_closed ||
                                          ch.eof_pending || ch.error_pending;
                    if (terminal) {
                        delete node;
                    } else {
                        const std::size_t left = node->remaining();
                        ch.queued_bytes += left;
                        if (left == 0) {
                            delete node;
                        } else {
                            node->next = ch.head;
                            ch.head = node;
                            if (ch.tail == nullptr) {
                                ch.tail = node;
                            }
                        }
                        // A short consumption normally pauses the channel, but a
                        // resume_read() issued while on_data was running already
                        // asked for the next slice: consume that demand here, in
                        // the same critical section that would record the pause,
                        // so the wake cannot be swallowed either way.
                        if (consumed < offered) {
                            if (ch.pending_wakes != 0) {
                                --ch.pending_wakes;
                            } else {
                                ch.reader_paused = true;
                            }
                        } else if (ch.pending_wakes != 0) {
                            // A full consumption leaves the channel runnable, so a
                            // demand queued against the delivery attempt it just
                            // finished is spent by staying runnable.
                            --ch.pending_wakes;
                        }
                        done = promote_locked(ch, done_result);
                    }
                }
                if (done != nullptr) {
                    done->complete(done_result);
                }
            }
            if (ready != nullptr) {
                ready->complete(ready_result);
            }
        }
        if (work) {
            continue;
        }

        // Publish "not draining", then re-check under the lock.  A push that
        // enqueued between the last iteration and this store either observed
        // `draining == true` (and left its data for us) or will observe `false`
        // and take over itself.  Re-acquiring here closes that window.
        st.draining.store(false, std::memory_order_release);
        bool more = false;
        {
            std::lock_guard<std::mutex> lk(st.mutex);
            more = has_pending_work_locked(st);
        }
        if (!more) {
            break;
        }
        if (st.draining.exchange(true, std::memory_order_acq_rel)) {
            break;
        }
    }
}

void push_data(inproc_state& st, int index, span<const std::byte> data,
               io::completion* c) noexcept {
    if (c == nullptr) {
        return;
    }

    io::completion* immediate = nullptr;
    io::io_result immediate_result{};
    bool close_after = false;

    {
        std::lock_guard<std::mutex> lk(st.mutex);
        channel& ch = st.dir[index];
        endpoint_slot& ep = st.ep[index];

        if (ep.closed || ch.reader_closed || ch.dead) {
            immediate = c;
            immediate_result =
                io::io_result{std::make_error_code(std::errc::broken_pipe), 0};
        } else if (ch.pending_completion != nullptr) {
            // I5 violation: a second push while one is still in flight.
            immediate = c;
            immediate_result =
                io::io_result{std::make_error_code(std::errc::operation_not_supported), 0};
        } else {
            inproc_faults& faults = ep.faults;

            if (faults.inject_write_error) {
                const std::error_code ec = faults.inject_write_error;
                faults.inject_write_error = {};
                immediate = c;
                immediate_result = io::io_result{ec, 0};
                close_after = true;
            } else {
                if (faults.inject_read_error) {
                    ch.error_pending = true;
                    ch.error_code = faults.inject_read_error;
                    faults.inject_read_error = {};
                }
                if (faults.delay_chunks > 0) {
                    faults.delay_chunks = 0;
                    ++ch.delay_pushes;
                }
                if (faults.reorder_pairs > 0) {
                    ch.reorder_pairs = faults.reorder_pairs;
                    faults.reorder_pairs = 0;
                }

                std::size_t skip = 0;
                if (faults.drop_bytes > 0) {
                    skip = std::min(faults.drop_bytes, data.size());
                    faults.drop_bytes -= skip;
                }
                const span<const std::byte> payload = data.subspan(skip);

                if (payload.empty()) {
                    immediate = c;
                    immediate_result = io::io_result{{}, data.size()};
                } else {
                    const std::size_t space = ch.capacity - ch.queued_bytes;

                    if (ch.held == nullptr && ch.reorder_pairs > 0 &&
                        space >= payload.size()) {
                        chunk_node* node = make_chunk(payload.size());
                        if (node == nullptr) {
                            immediate = c;
                            immediate_result = io::io_result{
                                std::make_error_code(std::errc::not_enough_memory), 0};
                        } else {
                            std::memcpy(node->data.get(), payload.data(),
                                        payload.size());
                            ch.held = node;
                            ch.queued_bytes += payload.size();
                            immediate = c;
                            immediate_result = io::io_result{{}, data.size()};
                        }
                    } else {
                        const std::size_t copy_count =
                            std::min(payload.size(), space);
                        bool enqueued = true;
                        if (copy_count != 0) {
                            chunk_node* node = make_chunk(copy_count);
                            if (node == nullptr) {
                                enqueued = false;
                            } else {
                                std::memcpy(node->data.get(), payload.data(),
                                            copy_count);
                                append_node(ch, node);
                                ch.queued_bytes += copy_count;
                            }
                        }

                        if (!enqueued) {
                            immediate = c;
                            immediate_result = io::io_result{
                                std::make_error_code(std::errc::not_enough_memory), 0};
                        } else if (copy_count == payload.size()) {
                            if (ch.held != nullptr) {
                                append_node(ch, ch.held);
                                ch.held = nullptr;
                                if (ch.reorder_pairs > 0) {
                                    --ch.reorder_pairs;
                                }
                            }
                            immediate = c;
                            immediate_result = io::io_result{{}, data.size()};
                        } else {
                            if (ch.held != nullptr) {
                                // Not enough room to honour the reorder;
                                // release the held chunk in arrival order.
                                append_node(ch, ch.held);
                                ch.held = nullptr;
                                if (ch.reorder_pairs > 0) {
                                    --ch.reorder_pairs;
                                }
                            }
                            ch.pending_data = payload.data() + copy_count;
                            ch.pending_remaining = payload.size() - copy_count;
                            ch.pending_accepted = data.size();
                            ch.pending_completion = c;
                        }
                    }
                }
            }
        }
    }

    if (immediate != nullptr) {
        immediate->complete(immediate_result);
    }
    if (close_after) {
        close_endpoint(st, index);
    } else {
        drain(st);
    }
}

void resume_read_impl(inproc_state& st, int index) noexcept {
    {
        std::lock_guard<std::mutex> lk(st.mutex);
        channel& in = st.dir[1 - index];
        if (in.dead || in.reader_closed) {
            return;
        }
        // Queue the demand rather than clearing the pause outright. The drain
        // loop may be inside this channel's on_data right now, holding no lock,
        // and about to record a pause from the short consumption it has already
        // made; a snapshot taken before that point would be overwritten and the
        // wake lost. With a demand queued, the loop consumes it under this same
        // mutex when it would otherwise pause (see `drain`).
        //
        // Queued only when it can be fulfilled -- a pause to clear, bytes still
        // retained, or a delivery attempt in flight (`delivering != 0`), which is
        // the case neither of the other two can see. Incrementing with none of
        // them present would leave a demand that never gets consumed and would
        // later cancel an unrelated, legitimate pause.
        if (in.reader_paused || in.head != nullptr || in.held != nullptr ||
            in.delivering != 0) {
            ++in.pending_wakes;
        }
        in.reader_paused = false;
    }
    drain(st);
}

} // namespace


namespace {

// The endpoints this thread is currently delivering into, innermost last.
// bind_source must not wait for its own delivery: a callback that unbinds
// would otherwise wait for itself forever. A stack (rather than a single slot)
// preserves the outer marker when delivery nests across endpoints.
constexpr std::size_t kMaxDeliveryDepth = 16;
thread_local const endpoint_slot* delivery_stack[kMaxDeliveryDepth] = {};
thread_local std::size_t delivery_depth = 0;
thread_local std::size_t delivery_overflow = 0;

void push_delivery(const endpoint_slot* ep) noexcept {
    if (delivery_depth < kMaxDeliveryDepth) {
        delivery_stack[delivery_depth++] = ep;
    } else {
        ++delivery_overflow;
    }
}

void pop_delivery() noexcept {
    if (delivery_overflow > 0) {
        --delivery_overflow;
    } else if (delivery_depth > 0) {
        --delivery_depth;
    }
}

bool is_delivering_on(const endpoint_slot* ep) noexcept {
    if (delivery_overflow > 0) {
        // Pathological nesting beyond the fixed stack: conservatively treat
        // this thread as delivering so a self-unbind cannot deadlock.
        return true;
    }
    for (std::size_t i = 0; i < delivery_depth; ++i) {
        if (delivery_stack[i] == ep) {
            return true;
        }
    }
    return false;
}

// Announces one call into a bound source. Returns null when no source is bound
// (or the endpoint is closing); the caller must call leave_bound() with the
// same endpoint after the call. Both run under the state mutex, and no lock is
// held while the user callback executes.
io::source* enter_bound(inproc_state& st, int index) noexcept {
    std::lock_guard<std::mutex> lk(st.mutex);
    endpoint_slot& ep = st.ep[index];
    if (ep.bound == nullptr || ep.closed) {
        return nullptr;
    }
    ++ep.delivering;
    push_delivery(&ep);
    return ep.bound;
}

void leave_bound(inproc_state& st, int index) noexcept {
    std::lock_guard<std::mutex> lk(st.mutex);
    endpoint_slot& ep = st.ep[index];
    pop_delivery();
    if (--ep.delivering == 0) {
        ep.delivery_cv.notify_all();
    }
}

} // namespace

// --- inproc_connection -------------------------------------------------------

inproc_connection::inproc_connection(std::shared_ptr<inproc_state> state,
                                     int index) noexcept
    : state_(std::move(state)),
      index_(index),
      source_adapter_(*this),
      sink_adapter_(*this) {
    state_->dir[1 - index_].reader = &source_adapter_;
}

inproc_connection::~inproc_connection() {
    // The peer's drain loop calls into `source_adapter_` through a raw pointer
    // in the channel. Retire it under the state mutex (so no new delivery can
    // pick it up) and then wait for the deliveries already announced to
    // finish; otherwise the object could be destroyed mid-callback.
    close();
    std::unique_lock<std::mutex> lk(state_->mutex);
    channel& in = state_->dir[1 - index_];
    in.reader = nullptr;
    in.delivery_cv.wait(lk, [&in] { return in.delivering == 0; });
}

void inproc_connection::bind_source(io::source* source) noexcept {
    {
        std::unique_lock<std::mutex> lk(state_->mutex);
        endpoint_slot& ep = state_->ep[index_];
        if (ep.bound == source) {
            return;
        }
        // Unbinding (or replacing) must wait for calls already running into the
        // old source: the caller may free it as soon as this returns. The wait
        // releases the mutex, so an in-flight callback can finish its own
        // leave_bound. A callback that unbinds its own endpoint is exempt (it
        // would otherwise wait for itself).
        ep.bound = source;
        if (!is_delivering_on(&ep)) {
            ep.delivery_cv.wait(lk, [&ep] { return ep.delivering == 0; });
        }
        if (source == nullptr) {
            return;
        }
        channel& in = state_->dir[1 - index_];
        if (!in.dead && !in.reader_closed) {
            // Same reasoning as resume_read_impl: a binding installed while the
            // drain loop is inside a delivery attempt must leave a demand, not
            // just clear a flag that the loop is about to set again.
            if (in.reader_paused || in.head != nullptr || in.held != nullptr ||
                in.delivering != 0) {
                ++in.pending_wakes;
            }
            in.reader_paused = false;
        }
    }
    drain(*state_);
}

void inproc_connection::resume_read() noexcept {
    resume_read_impl(*state_, index_);
}

void inproc_connection::close() noexcept {
    close_endpoint(*state_, index_);
}

bool inproc_connection::is_open() const noexcept {
    std::lock_guard<std::mutex> lk(state_->mutex);
    return !state_->ep[index_].closed;
}

endpoint inproc_connection::remote() const {
    return endpoint::inproc(state_->opt.name);
}

inproc_faults& inproc_connection::faults() noexcept {
    return state_->ep[index_].faults;
}

void inproc_connection::flush_delayed() noexcept {
    {
        std::lock_guard<std::mutex> lk(state_->mutex);
        state_->dir[index_].delay_pushes = 0;
    }
    drain(*state_);
}

std::size_t inproc_connection::forward_data(span<const std::byte> data) noexcept {
    io::source* s = enter_bound(*state_, index_);
    if (s == nullptr) {
        return 0;
    }
    const std::size_t used = s->on_data(data);
    leave_bound(*state_, index_);
    return used;
}

void inproc_connection::forward_eof() noexcept {
    io::source* s = enter_bound(*state_, index_);
    if (s == nullptr) {
        return;
    }
    s->on_eof();
    leave_bound(*state_, index_);
}

void inproc_connection::forward_error(std::error_code ec) noexcept {
    io::source* s = enter_bound(*state_, index_);
    if (s == nullptr) {
        return;
    }
    s->on_error(ec);
    leave_bound(*state_, index_);
}

std::size_t inproc_connection::source_adapter::on_data(
    span<const std::byte> data) noexcept {
    return owner_.forward_data(data);
}

void inproc_connection::source_adapter::on_eof() noexcept {
    owner_.forward_eof();
}

void inproc_connection::source_adapter::on_error(std::error_code ec) noexcept {
    owner_.forward_error(ec);
}

void inproc_connection::sink_adapter::push(span<const std::byte> data,
                                           io::completion* c) noexcept {
    push_data(*owner_.state_, owner_.index_, data, c);
}

// --- inproc_pair -------------------------------------------------------------

std::pair<std::unique_ptr<inproc_connection>, std::unique_ptr<inproc_connection>>
inproc_pair::make_typed(const inproc_options& opt) {
    auto state = std::make_shared<inproc_state>();
    state->opt = opt;
    const std::size_t capacity = opt.max_queued_bytes == 0 ? 1 : opt.max_queued_bytes;
    for (int k = 0; k < 2; ++k) {
        state->dir[k].capacity = capacity;
        state->ep[k].faults = opt.faults;
    }
    std::unique_ptr<inproc_connection> first(new inproc_connection(state, 0));
    std::unique_ptr<inproc_connection> second(new inproc_connection(state, 1));
    return {std::move(first), std::move(second)};
}

std::pair<std::unique_ptr<connection>, std::unique_ptr<connection>>
inproc_pair::make(const inproc_options& opt) {
    auto typed = make_typed(opt);
    return {std::unique_ptr<connection>(std::move(typed.first)),
            std::unique_ptr<connection>(std::move(typed.second))};
}

// --- inproc_backend ----------------------------------------------------------

struct inproc_backend::impl {
    struct listener_impl final : listener {
        std::weak_ptr<impl> owner;
        std::string name;
        accept_sink* sink = nullptr;
        bool started = false;
        inproc_options options;

        ~listener_impl() override {
            if (auto o = owner.lock()) {
                std::lock_guard<std::mutex> lk(o->mutex);
                auto it = o->listeners.find(name);
                if (it != o->listeners.end() && it->second == this) {
                    o->listeners.erase(it);
                }
            }
        }

        void start(accept_sink* s) override {
            sink = s;
            started = true;
        }

        void stop() noexcept override { started = false; }
    };

    mutable std::mutex mutex;
    std::map<std::string, listener_impl*> listeners;
    inproc_options options;
};

inproc_backend::inproc_backend(inproc_options opt)
    : impl_(std::make_shared<impl>()) {
    impl_->options = std::move(opt);
}

inproc_backend::~inproc_backend() = default;

std::unique_ptr<connection> inproc_backend::connect(const endpoint& ep,
                                                    const connect_options&) {
    if (ep.type() != endpoint::kind::inproc) {
        return nullptr;
    }
    impl::listener_impl* target = nullptr;
    inproc_options options;
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        auto it = impl_->listeners.find(ep.name());
        if (it == impl_->listeners.end()) {
            return nullptr;
        }
        target = it->second;
        if (!target->started || target->sink == nullptr) {
            return nullptr;
        }
        options = target->options;
    }
    auto pair = inproc_pair::make_typed(options);
    target->sink->on_accept(
        std::unique_ptr<connection>(std::move(pair.second)));
    return std::unique_ptr<connection>(std::move(pair.first));
}

std::unique_ptr<listener> inproc_backend::listen(const endpoint& ep,
                                                 const listen_options&) {
    if (ep.type() != endpoint::kind::inproc) {
        return nullptr;
    }
    auto l = std::make_unique<impl::listener_impl>();
    l->owner = impl_;
    l->name = ep.name();
    l->options = impl_->options;
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        if (impl_->listeners.find(l->name) != impl_->listeners.end()) {
            return nullptr;
        }
        impl_->listeners.emplace(l->name, l.get());
    }
    return l;
}

} // namespace crpc
