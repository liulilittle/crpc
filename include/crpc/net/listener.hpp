// CRPC -- the unified inbound listener.
//
// `net::listener` is the server-side half of the socket framework: one bind,
// then a cancelable accept loop that hands `net::tcp_stream`s to a handler.
// Every accepted stream implements the `net::channel` contract, so protocol
// layers read and write it exactly like a connector's stream.
//
// The accept loop
// ---------------
// `accept(timeout)` blocks until a connection is ready (or `deadline_exceeded`
// when the wait outlives `timeout`; a timeout of 0 means "wait forever").
// `run(handler)` is the loop form: it calls `accept` forever and forwards each
// stream to the handler until `close()` cancels it. Both are cancelable from
// any thread by closing the listener; a blocked `accept` wakes promptly instead
// of sleeping out its wait.
//
// SO_REUSEADDR semantics (documented, not silently relied on)
// -----------------------------------------------------------
// The flag means something different on the two platforms:
//
//   * POSIX: it allows binding while a previous listener is still in TIME_WAIT,
//     so a restarted server does not wait for idle connections to age out. It
//     does NOT permit two live listeners on one port.
//   * Windows: SO_REUSEADDR actively permits a SECOND bind to an in-use port,
//     and the second listener then silently never accepts a connection -- the
//     first listener keeps them all. That silent failure is worse than a bind
//     error, so this layer does not set SO_REUSEADDR on Windows at all; it sets
//     SO_EXCLUSIVEADDRUSE instead, and a conflicting bind then fails clearly
//     with WSAEADDRINUSE/WSAEACCES before a connection can ever go astray.
//
// On both platforms the outcome a caller can rely on is the same: a port is
// bound by exactly one live listener, and a second bind *fails* instead of
// silently shadowing the first. `reuse_address = false` gives a plain bind.
//
// IPv4/IPv6 dual stack
// --------------------
// Binding an IPv6 wildcard (e.g. "::") with `ipv6_only = false` accepts IPv4
// connections through the mapped form (dual stack). Windows ships with
// `IPV6_V6ONLY` defaulting to 1, so the option must be cleared explicitly;
// `ipv6_only = true` makes the socket IPv6-only on every platform. A listener
// bound to a concrete address (a literal or a name) is single-stack regardless.
#ifndef CRPC_NET_LISTENER_HPP
#define CRPC_NET_LISTENER_HPP

#include <chrono>
#include <cstddef>
#include <memory>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/net/address.hpp>
#include <crpc/net/channel.hpp>

namespace crpc {
namespace net {

// Tunables for one listening socket.
struct listen_options {
    // Kernel accept-queue depth, the `listen(2)` backlog. 0 selects 128.
    std::size_t backlog = 128;

    // See the SO_REUSEADDR discussion in the file comment.
    bool reuse_address = true;

    // Only meaningful when binding an IPv6 address: `false` enables dual stack
    // (IPv4 mapped), `true` keeps the socket IPv6-only.
    bool ipv6_only = false;

    // Applied to every accepted stream.
    bool tcp_no_delay = true;

    stream_options stream{};
};

// The bound, listening socket the service loop hands streams out from.
class listener;

namespace detail {
// Internal bridge; see the channel header for the rationale.
struct listener_factory {
    [[nodiscard]] static std::unique_ptr<listener> adopt(std::uintptr_t native,
                                                         address local,
                                                         listen_options options);
};
} // namespace detail

// One bound, listening socket.
class CRPC_API listener {
public:
    // Binds and listens on `bind_address` (TCP host/port or a Unix-domain
    // path). The reported `local()` is the real bound address, so an ephemeral
    // port (0) can be learned.
    [[nodiscard]] static result<std::unique_ptr<listener>> bind(
        const address& bind_address, const listen_options& options = listen_options{});

    ~listener();

    listener(const listener&) = delete;
    listener& operator=(const listener&) = delete;
    listener(listener&&) = delete;
    listener& operator=(listener&&) = delete;

    // Blocks up to `timeout` (0 = forever) for an accepted connection and
    // returns its stream. After `close()` every call fails with
    // `shutting_down`; a wait that outlives `timeout` fails with
    // `deadline_exceeded`.
    [[nodiscard]] result<std::unique_ptr<tcp_stream>> accept(
        std::chrono::nanoseconds timeout);

    // Runs the accept loop on the calling thread: accepts and hands each stream
    // to `handler` until the listener is closed. `handler` returning false also
    // stops the loop with `ok`. A stream that could not be accepted (rare
    // resource failure) is dropped after reporting.
    template <class Fn>
    status run(Fn&& handler) {
        for (;;) {
            result<std::unique_ptr<tcp_stream>> accepted = accept(std::chrono::nanoseconds::zero());
            if (!accepted) {
                if (accepted.code() == rpc_code::shutting_down) {
                    return status{};
                }
                return status{accepted.error()};
            }
            const bool continue_loop = handler(std::move(accepted.value()));
            if (!continue_loop) {
                return status{};
            }
        }
    }

    // Cancels a blocked `accept`/`run` and releases the listening socket.
    // Idempotent and safe from any thread.
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] address local() const;
    [[nodiscard]] const listen_options& options() const noexcept;

private:
    explicit listener(std::uintptr_t native, address local, listen_options options);
    friend struct detail::listener_factory;

    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace net
} // namespace crpc

#endif // CRPC_NET_LISTENER_HPP