// CRPC -- transport backend contract and the in-process backend.
//
// The transport layer is deliberately two interfaces wide (R1):
//
//   * `io::source`  -- backend -> framework: "bytes arrived, consume what you
//                      can".  The return value is a *credit*: the number of
//                      bytes consumed.  0 means "pause" (backpressure).
//   * `io::sink`    -- framework -> backend: "send these bytes".  Exactly one
//                      completion per push.
//
// Everything else -- establishment, close, cancellation, options -- lives on
// `connection` / `backend_factory`, never on the two interfaces.  The core RPC
// layer therefore depends only on `backend_factory&`; there is no
// `if (is_tcp)` anywhere above this header.
//
// Contract invariants (the ones this backend enforces):
//
//   I1  Every `completion::complete` runs exactly once (success, failure or
//       cancellation).
//   I2  A completion may run on any thread; in this backend it runs on the
//       thread that called `push`, `resume_read` or `close` (the current
//       drain owner) or on the pushing thread when the write fits.
//   I3  A completion must not block and must not throw; `complete` is
//       `noexcept`.  It must not synchronously start another push on the same
//       connection (the backend tolerates it, but the recursion guard in the
//       contract stays in force).
//   I4  After `close()`, no *new* source delivery and no *new* sink callback
//       starts.  In-flight completions still run exactly once, canceled.
//   I5  At most one sink `push` is in flight per direction.
//   I6  The backend completes partial writes itself; the framework never
//       re-submits a tail.
//   I7  The source span is valid until `on_data` returns.  The sink span is
//       valid until `complete` returns.
//   I8  One reader and one writer per connection at any instant.
//   I9  `on_data`'s return value never exceeds `data.size()`; the backend
//       clamps a violating return instead of reading out of bounds.
//   I10 After `on_data` returns 0 or a short count, the backend retains the
//       unconsumed tail and delivers it only after `resume_read()`.
//
// Thread-safety of the in-process backend:
//
//   * `push`, `close`, `resume_read` and `is_open` may be called from any
//     thread.  `push` and `close` are safe to call concurrently with each
//     other and with the peer's `push`.
//   * Source callbacks (`on_data`, `on_eof`, `on_error`) are serialized per
//     connection pair: at most one callback runs at a time.  They run on the
//     thread that currently owns the drain loop -- the caller of `push`,
//     `resume_read` or `close`, never a hidden background thread.  A callback
//     may call `push`/`close`; the nested push is queued and the outer drain
//     loop continues it, so the stack depth stays constant.
//   * `on_eof` and `on_error` are terminal: each runs at most once, never both,
//     and no callback follows them.
//   * `bind_source` is not thread-safe with respect to delivery; install the
//     framework source before the first push.
//   * Fault hooks are test-only; configure them before traffic starts.
#ifndef CRPC_IO_HPP
#define CRPC_IO_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace io {

// The result of one sink operation.  `bytes` is the number of input bytes the
// backend accepted (for a canceled or failed write: the number accepted before
// the failure, or 0 when nothing was accepted).  `ec` is empty on success.
struct io_result {
    std::error_code ec;
    std::size_t bytes = 0;

    [[nodiscard]] bool ok() const noexcept { return !ec; }
};

// One outstanding write.  Exactly one `complete` call per `sink::push`.
class CRPC_API completion {
public:
    completion() = default;
    virtual ~completion() = default;
    completion(const completion&) = delete;
    completion& operator=(const completion&) = delete;
    completion(completion&&) = delete;
    completion& operator=(completion&&) = delete;

    virtual void complete(io_result result) noexcept = 0;
};

// Backend -> framework.  The framework implements this interface and binds it
// to a connection with `connection::bind_source`.
class CRPC_API source {
public:
    virtual ~source() = default;

    // Returns the number of bytes consumed.  0 pauses reading; a short count
    // consumes a prefix and pauses.  The framework must copy or parse every
    // consumed byte before returning; the span is only valid for this call.
    virtual std::size_t on_data(span<const std::byte> data) noexcept = 0;

    // Terminal: the peer closed its side.  Runs at most once, after every
    // byte that will ever be delivered.
    virtual void on_eof() noexcept = 0;

    // Terminal: the channel failed.  Runs at most once; no `on_eof` follows.
    virtual void on_error(std::error_code ec) noexcept = 0;
};

// Framework -> backend.  `push` must eventually call `c->complete` exactly
// once.  The span stays valid until `complete` returns.  `complete` must not
// synchronously start another push on the same connection.
class CRPC_API sink {
public:
    virtual ~sink() = default;

    virtual void push(span<const std::byte> data, completion* c) noexcept = 0;
};

} // namespace io

// --- Endpoints ---------------------------------------------------------------

// A strong endpoint type: either a TCP host/port pair or an in-process name.
// There is no implicit conversion from a string, so a port cannot be confused
// with a host and an inproc name cannot be passed to a TCP backend.
class endpoint {
public:
    enum class kind : std::uint8_t { none, tcp, inproc };

    // Not constexpr: std::string is not a literal type, and GCC rejects a
    // constexpr default constructor for a class with a non-literal member.
    endpoint() noexcept = default;

    [[nodiscard]] static endpoint tcp(std::string host, u16 port) {
        endpoint ep;
        ep.kind_ = kind::tcp;
        ep.host_ = std::move(host);
        ep.port_ = port;
        return ep;
    }

    [[nodiscard]] static endpoint inproc(std::string name) {
        endpoint ep;
        ep.kind_ = kind::inproc;
        ep.host_ = std::move(name);
        return ep;
    }

    [[nodiscard]] kind type() const noexcept { return kind_; }
    [[nodiscard]] bool valid() const noexcept { return kind_ != kind::none; }
    [[nodiscard]] const std::string& host() const noexcept { return host_; }
    [[nodiscard]] const std::string& name() const noexcept { return host_; }
    [[nodiscard]] u16 port() const noexcept { return port_; }

    [[nodiscard]] std::string to_string() const {
        switch (kind_) {
        case kind::tcp:
            return "tcp:" + host_ + ":" + std::to_string(port_);
        case kind::inproc:
            return "inproc:" + host_;
        case kind::none:
            break;
        }
        return "none";
    }

    friend bool operator==(const endpoint& a, const endpoint& b) noexcept {
        return a.kind_ == b.kind_ && a.host_ == b.host_ && a.port_ == b.port_;
    }
    friend bool operator!=(const endpoint& a, const endpoint& b) noexcept {
        return !(a == b);
    }

private:
    kind kind_ = kind::none;
    std::string host_;
    u16 port_ = 0;
};

struct connect_options {
    std::chrono::nanoseconds connect_timeout{std::chrono::seconds(3)};
    std::size_t recv_buffer_bytes = 64 * 1024;
    std::size_t send_buffer_bytes = 64 * 1024;
    bool tcp_no_delay = true;
};

struct listen_options {
    std::size_t backlog = 1024;
    bool reuse_port = true;
    std::size_t recv_buffer_bytes = 64 * 1024;
    std::size_t send_buffer_bytes = 64 * 1024;
};

// --- Connection / listener / factory ----------------------------------------

class connection;

class CRPC_API accept_sink {
public:
    virtual ~accept_sink() = default;
    virtual void on_accept(std::unique_ptr<connection> c) noexcept = 0;
};

class CRPC_API connection {
public:
    virtual ~connection() = default;

    // Backend view of the receive side.  The framework normally does not call
    // these directly; it installs its own `io::source` with `bind_source`.
    virtual io::source& source() noexcept = 0;

    // The backend's write side.  The framework pushes frames here.
    virtual io::sink& sink() noexcept = 0;

    // Installs the framework's receive callbacks.  The spec's connection shape
    // omitted this binding point; it is required because `source` is
    // implemented by the framework, not by the backend.  Must be called before
    // traffic starts.  Passing nullptr unbinds (delivery pauses).
    virtual void bind_source(io::source* source) noexcept = 0;

    // Resumes a paused read.  Idempotent; may be called from any thread.
    virtual void resume_read() noexcept = 0;

    // Abortive close: idempotent; cancels the in-flight write, discards
    // undelivered queued bytes, and makes the peer observe EOF.
    virtual void close() noexcept = 0;

    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    [[nodiscard]] virtual endpoint remote() const = 0;
};

class CRPC_API listener {
public:
    virtual ~listener() = default;
    virtual void start(accept_sink* sink) = 0;
    virtual void stop() noexcept = 0;
};

class CRPC_API backend_factory {
public:
    virtual ~backend_factory() = default;
    virtual std::unique_ptr<connection> connect(const endpoint& ep,
                                                const connect_options& opt) = 0;
    virtual std::unique_ptr<listener> listen(const endpoint& ep,
                                             const listen_options& opt) = 0;
};

// --- In-process backend ------------------------------------------------------

// Deterministic fault hooks.  They are off when every field is zero/empty and
// cost only a few predictable branches on the push path.
struct inproc_faults {
    // Drop this many leading outgoing payload bytes (across pushes).  The peer
    // observes a gap; `io_result::bytes` still counts the accepted input.
    std::size_t drop_bytes = 0;

    // Defer delivery of the next N pushes until `flush_delayed()` is called.
    std::size_t delay_chunks = 0;

    // Swap the next N pairs of outgoing chunks (first chunk held until the
    // second arrives).  Requires room for both chunks; otherwise order
    // degrades to arrival order.
    std::size_t reorder_pairs = 0;

    // The next push completes with this error and the connection closes.
    std::error_code inject_write_error{};

    // The next delivery raises this error on the peer's source (instead of
    // delivering data) and terminates the channel.
    std::error_code inject_read_error{};

    // Reserved for randomized fault schedules; unused in v1.
    std::uint32_t seed = 0;
};

struct inproc_options {
    // Bound on queued bytes per direction, excluding one detached chunk that
    // is currently being handed to `on_data` (bounded overshoot of one chunk).
    std::size_t max_queued_bytes = 64 * 1024;

    // Delivery is always serialized; kept for API parity with the spec.
    bool serialize = true;

    // Initial faults for both endpoints.
    inproc_faults faults{};

    // Name reported by `remote()`.
    std::string name = "inproc";
};

// Completion timing: in-process `push` completes when the bytes have been
// accepted into the peer's bounded queue (copied out of the caller's span), not
// when the peer's `on_data` consumes them.  This is the in-process analogue of
// a successful kernel write.  A push whose tail does not fit is deferred until
// the drain loop frees room; it is never failed or dropped for backpressure.
//
// Shared state; defined in src/io/inproc.cpp.
struct inproc_state;

class inproc_pair;

class CRPC_API inproc_connection final : public connection {
public:
    ~inproc_connection() override;

    io::source& source() noexcept override { return source_adapter_; }
    io::sink& sink() noexcept override { return sink_adapter_; }

    void bind_source(io::source* source) noexcept override;
    void resume_read() noexcept override;
    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept override;
    [[nodiscard]] endpoint remote() const override;

    // Outgoing-stream fault hooks (test only; configure before traffic).
    [[nodiscard]] inproc_faults& faults() noexcept;

    // Releases chunks held by `delay_chunks`.
    void flush_delayed() noexcept;

private:
    class source_adapter final : public io::source {
    public:
        explicit source_adapter(inproc_connection& owner) noexcept : owner_(owner) {}
        std::size_t on_data(span<const std::byte> data) noexcept override;
        void on_eof() noexcept override;
        void on_error(std::error_code ec) noexcept override;

    private:
        inproc_connection& owner_;
    };

    class sink_adapter final : public io::sink {
    public:
        explicit sink_adapter(inproc_connection& owner) noexcept : owner_(owner) {}
        void push(span<const std::byte> data, io::completion* c) noexcept override;

    private:
        inproc_connection& owner_;
    };

    friend class inproc_pair;

    inproc_connection(std::shared_ptr<inproc_state> state, int index) noexcept;

    std::size_t forward_data(span<const std::byte> data) noexcept;
    void forward_eof() noexcept;
    void forward_error(std::error_code ec) noexcept;

    std::shared_ptr<inproc_state> state_;
    int index_ = 0;
    source_adapter source_adapter_;
    sink_adapter sink_adapter_;
};

class CRPC_API inproc_pair {
public:
    [[nodiscard]] static std::pair<std::unique_ptr<connection>,
                                   std::unique_ptr<connection>>
    make(const inproc_options& opt = inproc_options{});

    [[nodiscard]] static std::pair<std::unique_ptr<inproc_connection>,
                                   std::unique_ptr<inproc_connection>>
    make_typed(const inproc_options& opt = inproc_options{});
};

// A named in-process backend implementing `backend_factory`.  `listen` binds a
// name; `connect` to that name creates a pair and hands the server side to the
// listener's accept sink synchronously on the connecting thread.  There is no
// global registry: endpoints only find each other through this object.
class CRPC_API inproc_backend final : public backend_factory {
public:
    explicit inproc_backend(inproc_options opt = inproc_options{});
    ~inproc_backend() override;

    inproc_backend(const inproc_backend&) = delete;
    inproc_backend& operator=(const inproc_backend&) = delete;
    inproc_backend(inproc_backend&&) = delete;
    inproc_backend& operator=(inproc_backend&&) = delete;

    std::unique_ptr<connection> connect(const endpoint& ep,
                                        const connect_options& opt) override;
    std::unique_ptr<listener> listen(const endpoint& ep,
                                     const listen_options& opt) override;

private:
    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace crpc

#endif // CRPC_IO_HPP


