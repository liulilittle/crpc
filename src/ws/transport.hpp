// CRPC -- the byte transport under a WebSocket session.
//
// A blocking `db::channel` over one `crpc::connection`, optionally routed through
// a TLS stream (crpc/io/tls.hpp). Backend deliveries arrive on whatever thread
// the transport uses; they are appended into a bounded buffer and wake the
// reader through a condition variable, so a session can be driven from a plain
// thread exactly like crpc::db::socket_channel but with the TLS decoration
// applied between the wire and the application (wss://).
#ifndef CRPC_WS_TRANSPORT_HPP
#define CRPC_WS_TRANSPORT_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <crpc/db/channel.hpp>
#include <crpc/io.hpp>
#include <crpc/io/tls.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace ws {
namespace detail {

class transport_channel final : public db::channel {
public:
    // Cleartext transport.
    transport_channel(std::unique_ptr<crpc::connection> link,
                      std::chrono::milliseconds timeout);

    // TLS transport. `context` only needs to live until `transport_channel`
    // finishes construction; the TLS stream retains the shared context state.
    transport_channel(std::unique_ptr<crpc::connection> link,
                      io::tls_context& context, std::string hostname,
                      std::chrono::milliseconds timeout);

    ~transport_channel() override;

    transport_channel(const transport_channel&) = delete;
    transport_channel& operator=(const transport_channel&) = delete;

    [[nodiscard]] result<std::vector<std::byte>> read_exact(
        std::size_t count) override;
    [[nodiscard]] result<std::size_t> read_some(span<std::byte> buffer) override;
    [[nodiscard]] status write(span<const std::byte> data) override;
    void close() noexcept override;

    [[nodiscard]] bool is_open() const noexcept;

private:
    // Backend/TLS deliveries land here.
    [[nodiscard]] std::size_t on_data(span<const std::byte> data) noexcept;
    void on_eof() noexcept;
    void on_error(std::error_code ec) noexcept;

    [[nodiscard]] bool wait_for_bytes(std::unique_lock<std::mutex>& lk,
                                      std::size_t& available);
    void resume_reading() noexcept;
    [[nodiscard]] rpc_code map_error(std::error_code ec) const noexcept;
    [[nodiscard]] rpc_error fail_read() const;

    class plain_source final : public io::source {
    public:
        explicit plain_source(transport_channel& owner) noexcept : owner_(owner) {}
        std::size_t on_data(span<const std::byte> data) noexcept override {
            return owner_.on_data(data);
        }
        void on_eof() noexcept override { owner_.on_eof(); }
        void on_error(std::error_code ec) noexcept override {
            owner_.on_error(ec);
        }

    private:
        transport_channel& owner_;
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

} // namespace detail
} // namespace ws
} // namespace crpc

#endif // CRPC_WS_TRANSPORT_HPP