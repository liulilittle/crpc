// CRPC -- a blocking `db::channel` over one transport, optionally behind TLS.
//
// `socket_channel` is the plain database channel: it exposes the three
// operations a protocol layer needs. A TLS connection cannot use it directly
// because the TLS record layer must be fed by the transport's source callbacks
// and must push ciphertext through the transport's sink. This channel owns that
// wiring: backend deliveries are appended to a bounded buffer and wake the
// reader through a condition variable, while writes go through the TLS stream
// (or straight to the transport sink when no TLS context is supplied).
//
// Both the HTTPS client and the WebSocket transport (wss://) use this class, so
// there is one blocking TLS byte pump in the library rather than one per
// protocol.
//
// Threading: one reader (read_some/read_exact) and one writer (write) may run
// concurrently. Backend callbacks and TLS callbacks arrive on the backend's
// thread; the reader waits on a condition variable with the channel's timeout.
#ifndef CRPC_DB_STREAM_CHANNEL_HPP
#define CRPC_DB_STREAM_CHANNEL_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/channel.hpp>
#include <crpc/io/tls.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {

class CRPC_API stream_channel final : public channel {
public:
    // Cleartext transport. The connection must already be open.
    stream_channel(std::unique_ptr<crpc::connection> link,
                   std::chrono::milliseconds timeout);

    // TLS transport. `context` only needs to live until construction finishes;
    // the TLS stream retains the shared context state. A server passes an empty
    // host name (a server does not send SNI); a client passes the name that
    // becomes its SNI and certificate host-name check target.
    //
    // A zero timeout waits indefinitely; otherwise every read and write is
    // bounded by it.
    stream_channel(std::unique_ptr<crpc::connection> link,
                   io::tls_context& context, std::string hostname,
                   std::chrono::milliseconds timeout);

    ~stream_channel() override;

    stream_channel(const stream_channel&) = delete;
    stream_channel& operator=(const stream_channel&) = delete;

    [[nodiscard]] result<std::vector<std::byte>> read_exact(
        std::size_t count) override;
    [[nodiscard]] result<std::size_t> read_some(span<std::byte> buffer) override;
    [[nodiscard]] status write(span<const std::byte> data) override;
    void close() noexcept override;

    // True only when a TLS context exists and its handshake completed. A
    // cleartext transport reports false even on loopback, matching the
    // caching_sha2 rule that only an end-to-end protected link may carry the
    // plaintext password.
    [[nodiscard]] bool is_secure() const noexcept override;

    [[nodiscard]] bool is_open() const noexcept;

    // TLS observations. They are empty/false on a cleartext channel and before
    // the corresponding handshake information exists.
    [[nodiscard]] std::string tls_server_name() const;
    [[nodiscard]] std::string tls_alpn_protocol() const;
    [[nodiscard]] bool tls_handshake_complete() const noexcept;
    [[nodiscard]] std::error_code tls_error() const noexcept;

private:
    // Backend/TLS deliveries land here.
    [[nodiscard]] std::size_t on_data(span<const std::byte> data) noexcept;
    void on_eof() noexcept;
    void on_error(std::error_code ec) noexcept;

    [[nodiscard]] bool wait_for_bytes(std::unique_lock<std::mutex>& lk,
                                      std::size_t& available);
    void resume_reading() noexcept;
    [[nodiscard]] rpc_error failure(std::error_code ec) const;

    class plain_source final : public io::source {
    public:
        explicit plain_source(stream_channel& owner) noexcept : owner_(owner) {}
        std::size_t on_data(span<const std::byte> data) noexcept override {
            return owner_.on_data(data);
        }
        void on_eof() noexcept override { owner_.on_eof(); }
        void on_error(std::error_code ec) noexcept override {
            owner_.on_error(ec);
        }

    private:
        stream_channel& owner_;
    };

    std::unique_ptr<crpc::connection> link_;
    std::unique_ptr<io::tls_stream> tls_;
    plain_source source_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::mutex write_mu_;
    std::string buf_;
    std::size_t pos_ = 0;
    bool eof_ = false;
    bool closed_ = false;
    std::error_code err_;
    std::chrono::milliseconds timeout_;
    std::size_t high_water_ = 1024u * 1024u;
};

} // namespace db
} // namespace crpc

#endif // CRPC_DB_STREAM_CHANNEL_HPP