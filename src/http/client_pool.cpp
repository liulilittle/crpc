// CRPC -- HTTP client connection pool.
//
// The contract mirrors `db::pool` (src/db/pool.cpp) rule for rule, because the
// two pools manage the same shape of resource: a bounded set of durable links
// handed out through RAII leases. The HTTP-specific parts are the health check
// (a channel that observed its peer's FIN or TLS close_notify reports closed,
// which is what `validate_on_borrow` consults) and the lease's single
// transparent retry for idempotent verbs, which covers the race where the peer
// closes after the health check but before the request lands.
//
// Pool contract (each rule has a test in tests/test_http_keepalive.cpp):
//   P1  1:1          every successful `acquire()` corresponds to exactly one
//                    return, and `lease` is the only way to hold a connection.
//   P2  idempotent   releasing twice returns the connection once.
//   P4  close        `close()` rejects further acquires with `shutting_down`
//                    and closes idle connections; a lease released afterwards
//                    is closed, never returned.
//   P5  bounded      at most `max_size` connections exist; a caller beyond
//                    that waits up to `acquire_timeout` and then gets
//                    `resource_exhausted`.
//   P6  single writer
//                    every counter and container is mutated under one mutex;
//                    a lease never mutates pool state except through
//                    `release()`/`discard()`.
//   P7  monotonic    a connection older than `max_lifetime` is closed instead
//                    of reused, measured from when it was opened.
//   P8  max_idle     a release beyond `max_idle` closes the connection.
//   P9  validated    an idle connection whose channel reports closed is
//                    discarded, and the next idle one is tried.
//   P10 lifetime     a lease outliving its pool is safe: it holds the shared
//                    state, and releasing into a destroyed pool closes the
//                    connection.
//   P11 isolation    two pools never share state, even over one factory.
//   P12 cancel       not modelled: an acquire can only be timed out.
//   P13 statistics   idle/in_use/total/opened are readable at any time.
//   P14 poisoned     the caller reports a connection whose protocol is known
//                    broken with `lease::discard()`; a connection the client
//                    itself found unusable (poisoned framing, retired by the
//                    server, failed mid-exchange) is discarded automatically.
//   P15 idempotent   `close()` may be called any number of times.
#include <crpc/http/client.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace crpc {
namespace http {

client_factory::~client_factory() = default;

dial_factory::dial_factory(client_options options, crpc::backend_factory& backend,
                           crpc::endpoint endpoint, crpc::runtime* shard)
    : options_(std::move(options)),
      backend_(&backend),
      endpoint_(std::move(endpoint)),
      shard_(shard) {}

result<std::unique_ptr<client>> dial_factory::open() {
    return make_client(options_, *backend_, endpoint_, shard_);
}

// --- pool state --------------------------------------------------------------

struct client_pool::impl {
    struct entry {
        client* held = nullptr;
        // When the connection became idle, for `idle_timeout`.
        std::chrono::steady_clock::time_point idle_since{};
        // When the connection was opened, for `max_lifetime`.
        std::chrono::steady_clock::time_point created{};
    };

    client_pool_options options;
    client_factory& factory;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<entry> idle;
    std::size_t in_use = 0;
    std::size_t live = 0;
    std::uint64_t opened = 0;
    bool closed = false;

    impl(client_pool_options opts, client_factory& owner)
        : options(opts), factory(owner) {}

    ~impl() {
        for (entry& item : idle) {
            item.held->close();
            delete item.held;
        }
        idle.clear();
    }

    // True when the connection must be closed instead of reused: it sat idle
    // too long, or it is older than the lifetime cap. An idle timeout sheds
    // capacity the callers stopped using; a lifetime cap recycles a connection
    // before either end's state can go stale around it.
    [[nodiscard]] bool expired(const entry& item,
                               std::chrono::steady_clock::time_point now) const {
        if (options.idle_timeout.count() > 0 &&
            now - item.idle_since >= options.idle_timeout) {
            return true;
        }
        if (options.max_lifetime.count() > 0 &&
            now - item.created >= options.max_lifetime) {
            return true;
        }
        return false;
    }

    // Closes and forgets every idle connection. Called with the mutex held.
    void close_idle_locked() {
        for (entry& item : idle) {
            item.held->close();
            delete item.held;
            if (live > 0) {
                --live;
            }
        }
        idle.clear();
    }
};

// --- lease -------------------------------------------------------------------

client_pool::lease::lease(std::shared_ptr<void> state, client* held,
                          std::chrono::steady_clock::time_point created) noexcept
    : state_(std::move(state)), held_(held), created_(created) {}

client_pool::lease::~lease() { (void)release(); }

client_pool::lease::lease(lease&& other) noexcept
    : state_(std::move(other.state_)), held_(other.held_), created_(other.created_) {
    other.held_ = nullptr;
}

client_pool::lease& client_pool::lease::operator=(lease&& other) noexcept {
    if (this != &other) {
        (void)release();
        state_ = std::move(other.state_);
        held_ = other.held_;
        created_ = other.created_;
        other.held_ = nullptr;
    }
    return *this;
}

status client_pool::lease::release() noexcept {
    if (held_ == nullptr) {
        // P2: a second release is a no-op, not a second insert.
        return status{};
    }
    client* held = held_;
    held_ = nullptr;

    auto* state = static_cast<impl*>(state_.get());
    if (state == nullptr) {
        // P10: the pool is gone; there is nothing to return to, so the
        // connection is closed rather than leaked.
        held->close();
        delete held;
        state_.reset();
        return status{};
    }

    // A connection the client itself found unusable -- poisoned framing,
    // retired by the server, failed mid-exchange -- is never pooled (P14).
    const bool reusable = held->is_open();
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->in_use > 0) {
            --state->in_use;
        }
        const bool too_old = state->options.max_lifetime.count() > 0 &&
                             now - created_ >= state->options.max_lifetime;
        if (!reusable || too_old || state->closed ||
            state->idle.size() >= state->options.max_idle) {
            held->close();
            delete held;
            if (state->live > 0) {
                --state->live;
            }
        } else {
            impl::entry item;
            item.held = held;
            item.idle_since = now;
            // The creation stamp travels with the lease across borrow cycles,
            // so `max_lifetime` is measured from open, not from the last
            // release.
            item.created = created_;
            state->idle.push_back(item);
        }
        state->cv.notify_one();
    }
    // The lease may hold the last reference to the state (P10); releasing it
    // must happen after the lock guard is gone.
    state_.reset();
    return status{};
}

void client_pool::lease::discard() noexcept {
    if (held_ == nullptr) {
        return;
    }
    client* held = held_;
    held_ = nullptr;

    auto* state = static_cast<impl*>(state_.get());
    if (state != nullptr) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->in_use > 0) {
            --state->in_use;
        }
        if (state->live > 0) {
            --state->live;
        }
        if (!state->closed) {
            state->cv.notify_one();
        }
    }
    held->close();
    delete held;
    state_.reset();
}

// The verbs whose repetition cannot change what the server stores. Anything
// else -- POST/PUT/DELETE/PATCH and every extension method -- is replayed
// never: a request that may have been executed must fail loudly instead.
[[nodiscard]] bool is_idempotent_verb(method verb) noexcept {
    return verb == method::get || verb == method::head || verb == method::options;
}

// The failures a replacement can plausibly fix: the connection turned out to
// be unusable before a complete response arrived. Deterministic failures (a
// body over the limit), protocol garbage and local lifecycle errors are
// returned as they are.
[[nodiscard]] bool is_connection_failure(rpc_code code) noexcept {
    switch (code) {
    case rpc_code::unknown_side_effect:
    case rpc_code::eof:
    case rpc_code::connection_reset:
    case rpc_code::broken_pipe:
    case rpc_code::unavailable:
    case rpc_code::protocol_error:
        return true;
    default:
        return false;
    }
}

result<response> client_pool::lease::send(method verb, std::string_view target,
                                          span<const std::byte> body,
                                          span<const header> extra_headers) {
    if (held_ == nullptr) {
        return rpc_error{rpc_code::failed_precondition,
                         "this lease holds no HTTP connection"};
    }
    result<response> reply = held_->send(verb, target, body, extra_headers);
    if (reply || !is_idempotent_verb(verb) || !is_connection_failure(reply.error().code)) {
        return reply;
    }

    // The pooled connection died between the borrow and the response -- the
    // one death the borrow-time health check cannot see. Idempotent verbs get
    // exactly one replacement: the dead connection is retired, a new one is
    // dialed, and the same request runs again on it.
    auto* state = static_cast<impl*>(state_.get());
    if (state == nullptr) {
        // P10: the pool is gone; report the failure rather than inventing a
        // connection.
        return reply;
    }

    client* dead = held_;
    held_ = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        dead->close();
        delete dead;
        if (state->live > 0) {
            --state->live;
        }
        if (state->closed) {
            return rpc_error{rpc_code::shutting_down, "the HTTP client pool is closed"};
        }
        if (state->live >= state->options.max_size) {
            return rpc_error{rpc_code::resource_exhausted,
                             "the HTTP client pool has no room for a replacement "
                             "connection"};
        }
        // Reserve the replacement's slot before dialing, so a concurrent
        // acquire cannot overshoot `max_size`.
        ++state->live;
        ++state->opened;
    }
    result<std::unique_ptr<client>> fresh = state->factory.open();
    if (!fresh || fresh.value() == nullptr) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->live > 0) {
            --state->live;
        }
        state->cv.notify_one();
        if (!fresh) {
            return fresh.error();
        }
        return rpc_error{rpc_code::unavailable,
                         "the HTTP client pool factory produced no connection"};
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) {
            fresh.value()->close();
            delete fresh.value().release();
            if (state->live > 0) {
                --state->live;
            }
            return rpc_error{rpc_code::shutting_down, "the HTTP client pool is closed"};
        }
        held_ = fresh.value().release();
    }
    return held_->send(verb, target, body, extra_headers);
}

result<response> client_pool::lease::send_text(method verb, std::string_view target,
                                               std::string_view body,
                                               std::string_view content_type) {
    header fields[1];
    if (!content_type.empty()) {
        fields[0].name = "Content-Type";
        fields[0].value = std::string(content_type);
    }
    return send(verb, target,
                span<const std::byte>(
                    reinterpret_cast<const std::byte*>(body.data()), body.size()),
                span<const header>(fields, content_type.empty() ? 0 : 1));
}

// --- pool --------------------------------------------------------------------

client_pool::client_pool(client_pool_options options, client_factory& factory)
    : impl_(std::make_shared<impl>(std::move(options), factory)) {}

client_pool::~client_pool() { close(); }

result<client_pool::lease> client_pool::acquire() {
    std::unique_lock<std::mutex> lock(impl_->mutex);

    if (impl_->closed) {
        return rpc_error{rpc_code::shutting_down, "the HTTP client pool is closed"};
    }

    const auto deadline = std::chrono::steady_clock::now() + impl_->options.acquire_timeout;

    for (;;) {
        if (impl_->closed) {
            return rpc_error{rpc_code::shutting_down, "the HTTP client pool is closed"};
        }

        // Take the newest idle connection that survives the health check. The
        // check is a local state read (`client::is_open` consults the
        // channel's liveness), so it happens under the lock.
        if (!impl_->idle.empty()) {
            impl::entry item = impl_->idle.back();
            impl_->idle.pop_back();

            // P7/P8: an expired connection is closed, never handed out.
            if (impl_->expired(item, std::chrono::steady_clock::now())) {
                item.held->close();
                delete item.held;
                if (impl_->live > 0) {
                    --impl_->live;
                }
                continue;
            }
            // P9: a connection the peer (or a failure) already closed is
            // discarded, and the next idle one is tried.
            if (impl_->options.validate_on_borrow && !item.held->is_open()) {
                item.held->close();
                delete item.held;
                if (impl_->live > 0) {
                    --impl_->live;
                }
                continue;
            }
            ++impl_->in_use;
            return lease(impl_, item.held, item.created);
        }

        // No idle connection: open a new one while below the ceiling. The
        // dial is a blocking round trip, so it happens without the lock held.
        if (impl_->live < impl_->options.max_size) {
            // Reserve the slot so a concurrent acquire cannot overshoot
            // max_size.
            ++impl_->live;
            ++impl_->opened;
            lock.unlock();
            result<std::unique_ptr<client>> opened = impl_->factory.open();
            lock.lock();

            if (!opened || opened.value() == nullptr) {
                if (impl_->live > 0) {
                    --impl_->live;
                }
                if (impl_->closed) {
                    return rpc_error{rpc_code::shutting_down,
                                     "the HTTP client pool is closed"};
                }
                if (!opened) {
                    return opened.error();
                }
                return rpc_error{rpc_code::unavailable,
                                 "the HTTP client pool factory produced no connection"};
            }
            if (impl_->closed) {
                // The pool closed while the connection was being dialed.
                opened.value()->close();
                delete opened.value().release();
                if (impl_->live > 0) {
                    --impl_->live;
                }
                return rpc_error{rpc_code::shutting_down,
                                 "the HTTP client pool is closed"};
            }
            ++impl_->in_use;
            return lease(impl_, opened.value().release(), std::chrono::steady_clock::now());
        }

        // At the ceiling: wait for a release.
        if (std::chrono::steady_clock::now() >= deadline) {
            return rpc_error{rpc_code::resource_exhausted,
                             "the HTTP client pool is exhausted and the acquire "
                             "timed out"};
        }
        if (impl_->cv.wait_until(lock, deadline) == std::cv_status::timeout) {
            return rpc_error{rpc_code::resource_exhausted,
                             "the HTTP client pool is exhausted and the acquire "
                             "timed out"};
        }
    }
}

void client_pool::close() noexcept {
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->closed) {
        return;
    }
    impl_->closed = true;
    impl_->close_idle_locked();
    // Waiters observe `closed` and fail with `shutting_down`; telling one is
    // enough because each failed waiter notifies the next.
    impl_->cv.notify_all();
}

std::size_t client_pool::idle() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->idle.size();
}

std::size_t client_pool::in_use() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->in_use;
}

std::size_t client_pool::total() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->live;
}

std::uint64_t client_pool::opened() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->opened;
}

bool client_pool::is_closed() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->closed;
}

} // namespace http
} // namespace crpc
