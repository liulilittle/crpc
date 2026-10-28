// CRPC -- byte channel contract and the TCP stream implementation.
//
// The stream keeps the read side and the write side in separate states so that
// half-close is representable: a peer FIN (EOF) makes every further *read*
// fail with `eof`, but the *write* side stays usable until the caller closes --
// that is exactly what lets a server reply after the client has finished its
// request and `shutdown_send()` its half. A write-side failure (reset, broken
// pipe, or a timed-out write that may have pushed a partial frame) is fatal to
// the whole stream.
#include <crpc/net/channel.hpp>

#include <cstdint>
#include <cstring>
#include <utility>

#include "platform.hpp"

namespace crpc {
namespace net {

channel::~channel() = default;

// --- tcp_stream ---------------------------------------------------------------

struct tcp_stream::impl {
    detail::native_socket sock = detail::invalid_socket;
    address local;
    address peer;
    stream_options options;

    // The socket has been closed locally (`close`/`abort`/destructor).
    bool closed = false;
    // The peer sent FIN; reads fail with `eof`, writes stay allowed.
    bool read_eof = false;
    // First failure on each direction, kept so a later call reports the same
    // terminal outcome instead of a fresh one.
    rpc_error read_terminal;
    rpc_error write_terminal;

    impl(detail::native_socket s, address local_addr, address peer_addr,
         stream_options opts)
        : sock(s), local(std::move(local_addr)), peer(std::move(peer_addr)),
          options(std::move(opts)) {}

    [[nodiscard]] rpc_error read_fail() const noexcept {
        if (read_terminal.code != rpc_code::ok) {
            return read_terminal;
        }
        if (closed || sock == detail::invalid_socket) {
            return rpc_error{rpc_code::shutting_down, "stream is closed"};
        }
        return rpc_error{rpc_code::internal, "stream is not ready"};
    }

    [[nodiscard]] rpc_error write_fail() const noexcept {
        if (write_terminal.code != rpc_code::ok) {
            return write_terminal;
        }
        if (closed || sock == detail::invalid_socket) {
            return rpc_error{rpc_code::shutting_down, "stream is closed"};
        }
        return rpc_error{rpc_code::internal, "stream is not ready"};
    }
};

std::unique_ptr<tcp_stream> detail::stream_factory::adopt(std::uintptr_t native,
                                                          address local, address peer,
                                                          stream_options options) {
    return std::unique_ptr<tcp_stream>(new tcp_stream(
        native, std::move(local), std::move(peer), std::move(options)));
}

tcp_stream::tcp_stream(std::uintptr_t native, address local, address peer,
                       stream_options options)
    : impl_(new impl(static_cast<detail::native_socket>(native), std::move(local),
                     std::move(peer), std::move(options))) {}

tcp_stream::~tcp_stream() { close(); }

result<std::size_t> tcp_stream::read_some(span<std::byte> buffer) {
    auto& self = *impl_;
    if (self.closed || self.sock == detail::invalid_socket) {
        return self.read_fail();
    }
    if (self.read_eof || self.read_terminal.code != rpc_code::ok) {
        return self.read_fail();
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
    for (;;) {
        const int ready = detail::wait_ready(self.sock, detail::wait_kind::read,
                                             self.options.rw_timeout);
        if (ready == 0) {
            // A read deadline is per operation, not terminal; a later read may
            // still succeed.
            return rpc_error{rpc_code::deadline_exceeded, "read timed out"};
        }
        if (ready < 0) {
            const int error = detail::last_error();
            self.read_terminal =
                rpc_error{rpc_code::internal,
                          detail::error_text("waiting for data failed", error)};
            return self.read_fail();
        }
        const std::size_t requested = buffer.size();
#if defined(CRPC_OS_WINDOWS)
        const int count = requested > static_cast<std::size_t>(INT_MAX)
                              ? INT_MAX
                              : static_cast<int>(requested);
        const int got = ::recv(self.sock,
                               reinterpret_cast<char*>(buffer.data()), count, 0);
#else
        const ssize_t got = ::recv(self.sock, buffer.data(), requested, 0);
#endif
        if (got > 0) {
            return static_cast<std::size_t>(got);
        }
        if (got == 0) {
            self.read_eof = true;
            self.read_terminal = rpc_error{rpc_code::eof, "peer closed cleanly"};
            return self.read_fail();
        }
        const int error = detail::last_error();
        if (detail::would_block(error)) {
            continue;
        }
        const rpc_code code = classify_stream_error(error);
        self.read_terminal = rpc_error{code, detail::error_text("recv failed", error)};
        return self.read_fail();
    }
}

result<std::vector<std::byte>> tcp_stream::read_exact(std::size_t count) {
    if (count == 0) {
        return std::vector<std::byte>{};
    }
    std::vector<std::byte> out;
    try {
        out.resize(count);
    } catch (...) {
        return rpc_error{rpc_code::resource_exhausted, "read buffer allocation failed"};
    }
    std::size_t got = 0;
    while (got < count) {
        const std::size_t want = count - got;
        result<std::size_t> part =
            read_some(span<std::byte>(out.data() + got, want));
        if (!part) {
            return part.error();
        }
        if (part.value() == 0) {
            continue;
        }
        got += part.value();
    }
    return out;
}

result<std::size_t> tcp_stream::read_available(span<std::byte> buffer) {
    auto& self = *impl_;
    if (self.closed || self.sock == detail::invalid_socket) {
        return self.read_fail();
    }
    if (self.read_eof || self.read_terminal.code != rpc_code::ok) {
        return self.read_fail();
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
#if defined(CRPC_OS_WINDOWS)
    const int count = buffer.size() > static_cast<std::size_t>(INT_MAX)
                          ? INT_MAX
                          : static_cast<int>(buffer.size());
    const int got = ::recv(self.sock, reinterpret_cast<char*>(buffer.data()), count, 0);
#else
    const ssize_t got = ::recv(self.sock, buffer.data(), buffer.size(), 0);
#endif
    if (got > 0) {
        return static_cast<std::size_t>(got);
    }
    if (got == 0) {
        self.read_eof = true;
        self.read_terminal = rpc_error{rpc_code::eof, "peer closed cleanly"};
        return self.read_fail();
    }
    const int error = detail::last_error();
    if (detail::would_block(error)) {
        return std::size_t{0};
    }
    const rpc_code code = classify_stream_error(error);
    self.read_terminal = rpc_error{code, detail::error_text("read failed", error)};
    return self.read_fail();
}

status tcp_stream::write(span<const std::byte> data) {
    auto& self = *impl_;
    if (self.closed || self.sock == detail::invalid_socket) {
        return self.write_fail();
    }
    if (self.write_terminal.code != rpc_code::ok) {
        return self.write_fail();
    }
    if (data.empty()) {
        return status{};
    }
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (self.closed || self.sock == detail::invalid_socket) {
            return self.write_fail();
        }
        const int ready = detail::wait_ready(self.sock, detail::wait_kind::write,
                                             self.options.rw_timeout);
        if (ready == 0) {
            // A timed-out write may have pushed a partial frame; the stream
            // must not resynchronise on a torn body, so it is closed.
            self.closed = true;
            self.write_terminal =
                rpc_error{rpc_code::deadline_exceeded, "write timed out"};
            return self.write_fail();
        }
        if (ready < 0) {
            const int error = detail::last_error();
            self.closed = true;
            self.write_terminal = rpc_error{
                rpc_code::internal, detail::error_text("waiting to write failed", error)};
            return self.write_fail();
        }
        const std::size_t remaining = data.size() - sent;
#if defined(CRPC_OS_WINDOWS)
        const int count = remaining > static_cast<std::size_t>(INT_MAX)
                              ? INT_MAX
                              : static_cast<int>(remaining);
        const int pushed = ::send(
            self.sock, reinterpret_cast<const char*>(data.data() + sent), count, 0);
#else
        const ssize_t pushed = ::send(self.sock, data.data() + sent, remaining, 0);
#endif
        if (pushed > 0) {
            sent += static_cast<std::size_t>(pushed);
            continue;
        }
        if (pushed == 0) {
            self.closed = true;
            self.write_terminal = rpc_error{rpc_code::broken_pipe, "send returned zero"};
            return self.write_fail();
        }
        const int error = detail::last_error();
        if (detail::would_block(error)) {
            continue;
        }
        const rpc_code code = classify_stream_error(error);
        self.closed = true;
        self.write_terminal = rpc_error{code, detail::error_text("send failed", error)};
        return self.write_fail();
    }
    return status{};
}

status tcp_stream::shutdown_send() {
    auto& self = *impl_;
    if (self.closed || self.sock == detail::invalid_socket) {
        return self.write_fail();
    }
#if defined(CRPC_OS_WINDOWS)
    if (::shutdown(self.sock, SD_SEND) == 0) {
        return status{};
    }
#else
    if (::shutdown(self.sock, SHUT_WR) == 0) {
        return status{};
    }
#endif
    const int error = detail::last_error();
    const rpc_code code = classify_stream_error(error);
    return status{code, detail::error_text("shutdown failed", error)};
}

void tcp_stream::close() noexcept {
    auto& self = *impl_;
    if (self.sock != detail::invalid_socket) {
        detail::close_socket(self.sock);
        self.sock = detail::invalid_socket;
    }
    self.closed = true;
    if (self.read_terminal.code == rpc_code::ok) {
        self.read_terminal = rpc_error{rpc_code::shutting_down, "stream is closed"};
    }
    if (self.write_terminal.code == rpc_code::ok) {
        self.write_terminal = rpc_error{rpc_code::shutting_down, "stream is closed"};
    }
}

void tcp_stream::abort() noexcept {
    auto& self = *impl_;
    if (self.sock != detail::invalid_socket) {
        struct linger setting {};
        setting.l_onoff = 1;   // linger enabled
        setting.l_linger = 0;  // ... but abort immediately: force RST
        (void)::setsockopt(self.sock, SOL_SOCKET, SO_LINGER,
                           reinterpret_cast<const char*>(&setting),
                           static_cast<int>(sizeof(setting)));
    }
    close();
}

bool tcp_stream::is_open() const noexcept {
    const auto& self = *impl_;
    return self.sock != detail::invalid_socket && !self.closed;
}

address tcp_stream::local() const { return impl_->local; }
address tcp_stream::peer() const { return impl_->peer; }
const stream_options& tcp_stream::options() const noexcept { return impl_->options; }

} // namespace net
} // namespace crpc