// CRPC -- TDS over TLS channel. See tls_channel.hpp for the framing rules.
#include "tls_channel.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <crpc/db/tds/protocol.hpp>

#if defined(CRPC_HAVE_OPENSSL)
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace crpc {
namespace db {
namespace tds {

#if !defined(CRPC_HAVE_OPENSSL)

bool tls_backend_available() noexcept { return false; }

result<std::unique_ptr<db::channel>> open_tls_channel(std::unique_ptr<db::channel> inner,
                                                      const tls_client_options& options,
                                                      std::uint8_t& packet_id) {
    (void)inner;
    (void)options;
    (void)packet_id;
    return rpc_error{rpc_code::failed_precondition,
                     "TDS encryption needs a build with CRPC_WITH_OPENSSL=ON"};
}

#else

namespace {

// A TLS record bigger than this is split by OpenSSL itself; the cap only keeps
// one SSL_write from asking for an unbounded amount at once.
constexpr std::size_t max_plaintext_chunk = 16 * 1024;

[[nodiscard]] std::string openssl_reason() {
    const unsigned long code = ERR_get_error();
    if (code == 0) {
        return std::string("no OpenSSL error was queued");
    }
    char text[256];
    ERR_error_string_n(code, text, sizeof text);
    return std::string(text);
}

[[nodiscard]] rpc_error tls_failure(const char* what) {
    return rpc_error{rpc_code::failed_precondition,
                     std::string(what) + " failed: " + openssl_reason()};
}

class tls_channel final : public db::channel {
public:
    tls_channel(std::unique_ptr<db::channel> inner, std::uint16_t packet_size)
        : inner_(std::move(inner)), packet_size_(packet_size) {}

    ~tls_channel() override { close(); }

    tls_channel(const tls_channel&) = delete;
    tls_channel& operator=(const tls_channel&) = delete;

    [[nodiscard]] status handshake(const tls_client_options& options,
                                   std::uint8_t& packet_id);

    [[nodiscard]] result<std::vector<std::byte>> read_exact(std::size_t count) override;
    [[nodiscard]] result<std::size_t> read_some(span<std::byte> buffer) override;
    [[nodiscard]] status write(span<const std::byte> data) override;
    void close() noexcept override;

private:
    [[nodiscard]] status flush_handshake(std::uint8_t& packet_id);
    [[nodiscard]] status send_wrapped(span<const std::byte> data, std::uint8_t& packet_id);
    [[nodiscard]] result<std::vector<std::byte>> read_wrapped();
    [[nodiscard]] status flush_stream();
    [[nodiscard]] status fill_ciphertext();
    void release() noexcept;

    std::unique_ptr<db::channel> inner_;
    std::uint16_t packet_size_;
    SSL_CTX* context_ = nullptr;
    SSL* session_ = nullptr;
    BIO* incoming_ = nullptr;
    BIO* outgoing_ = nullptr;
    bool closed_ = false;
};

status tls_channel::handshake(const tls_client_options& options, std::uint8_t& packet_id) {
    context_ = SSL_CTX_new(TLS_client_method());
    if (context_ == nullptr) {
        return tls_failure("creating a TLS context");
    }
    // SQL Server 2022 on Linux speaks TLS 1.2 for TDS; pinning both ends keeps a
    // 1.3-capable client from negotiating a version the server's TDS layer does
    // not associate with the session. This is also what the reference client
    // (pytds, TLSv1_2_METHOD) does.
    (void)SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION);
    (void)SSL_CTX_set_max_proto_version(context_, TLS1_2_VERSION);
    (void)SSL_CTX_set_options(context_, SSL_OP_NO_COMPRESSION);
    if (options.verify_server) {
        SSL_CTX_set_verify(context_, SSL_VERIFY_PEER, nullptr);
        const int loaded =
            !options.ca_bundle.empty()
                ? SSL_CTX_load_verify_locations(context_, options.ca_bundle.c_str(), nullptr)
                : SSL_CTX_set_default_verify_paths(context_);
        if (loaded != 1) {
            return tls_failure("loading the TLS trust anchors");
        }
    } else {
        SSL_CTX_set_verify(context_, SSL_VERIFY_NONE, nullptr);
    }

    session_ = SSL_new(context_);
    incoming_ = BIO_new(BIO_s_mem());
    outgoing_ = BIO_new(BIO_s_mem());
    if (session_ == nullptr || incoming_ == nullptr || outgoing_ == nullptr) {
        return tls_failure("creating the TLS session");
    }
    // "no more input" must be a retry, not a close: the peer's next record has
    // simply not been read from the transport yet.
    BIO_set_mem_eof_return(incoming_, -1);
    SSL_set_bio(session_, incoming_, outgoing_);
    if (!options.server_name.empty()) {
        (void)SSL_set_tlsext_host_name(session_, options.server_name.c_str());
        if (options.verify_server) {
            // SSL_set1_host is deprecated in OpenSSL 4; the verify-parameter form is
            // the supported spelling and does the same hostname check.
            X509_VERIFY_PARAM* param = SSL_get0_param(session_);
            if (param == nullptr ||
                X509_VERIFY_PARAM_set1_host(param, options.server_name.c_str(), 0) != 1) {
                return tls_failure("configuring TLS hostname verification");
            }
        }
    }
    SSL_set_connect_state(session_);

    for (;;) {
        ERR_clear_error();
        const int started = SSL_do_handshake(session_);
        // Whatever the handshake produced -- ClientHello, ClientKeyExchange,
        // ChangeCipherSpec, Finished -- goes out inside PRELOGIN packets.
        const status sent = flush_handshake(packet_id);
        if (!sent) {
            return sent;
        }
        if (started == 1) {
            return status{};
        }
        const int reason = SSL_get_error(session_, started);
        if (reason == SSL_ERROR_WANT_READ) {
            result<std::vector<std::byte>> body = read_wrapped();
            if (!body) {
                return body.error();
            }
            if (!body.value().empty() &&
                BIO_write(incoming_, body.value().data(),
                          static_cast<int>(body.value().size())) <= 0) {
                return tls_failure("buffering the server's handshake");
            }
            continue;
        }
        if (reason == SSL_ERROR_WANT_WRITE) {
            continue;
        }
        return tls_failure("the TLS handshake");
    }
}

status tls_channel::flush_handshake(std::uint8_t& packet_id) {
    std::vector<std::byte> pending;
    char buffer[4096];
    for (;;) {
        const int got = BIO_read(outgoing_, buffer, static_cast<int>(sizeof buffer));
        if (got <= 0) {
            break;
        }
        const std::byte* first = reinterpret_cast<const std::byte*>(buffer);
        pending.insert(pending.end(), first, first + got);
    }
    if (pending.empty()) {
        return status{};
    }
    return send_wrapped(span<const std::byte>(pending.data(), pending.size()), packet_id);
}

status tls_channel::send_wrapped(span<const std::byte> data, std::uint8_t& packet_id) {
    const std::size_t body_max =
        packet_size_ > header_size ? static_cast<std::size_t>(packet_size_) - header_size : 0;
    if (body_max == 0) {
        return status{rpc_code::failed_precondition,
                      "the negotiated TDS packet size is too small for a TLS handshake"};
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t chunk = std::min(data.size() - offset, body_max);
        packet_header header;
        header.type = packet_type::prelogin;
        header.length = static_cast<std::uint16_t>(header_size + chunk);
        header.packet_id = packet_id;
        header.status = (offset + chunk >= data.size())
                            ? static_cast<std::uint8_t>(packet_status::eom)
                            : static_cast<std::uint8_t>(packet_status::normal);
        std::vector<std::byte> packet(header_size);
        std::size_t written = 0;
        const status encoded =
            encode_header(span<std::byte>(packet.data(), packet.size()), header, written);
        if (!encoded || written != header_size) {
            return status{rpc_code::protocol_error,
                          "a TDS handshake packet header did not encode"};
        }
        packet.insert(packet.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                      data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
        const status sent = inner_->write(span<const std::byte>(packet.data(), packet.size()));
        if (!sent) {
            return sent;
        }
        packet_id = static_cast<std::uint8_t>(packet_id + 1);
        offset += chunk;
    }
    return status{};
}

result<std::vector<std::byte>> tls_channel::read_wrapped() {
    result<std::vector<std::byte>> head = inner_->read_exact(header_size);
    if (!head) {
        return head.error();
    }
    packet_header header;
    const status decoded =
        decode_header(span<const std::byte>(head.value().data(), head.value().size()), header);
    if (!decoded) {
        return decoded.error();
    }
    if (header.length < header_size) {
        return rpc_error{rpc_code::protocol_error,
                         "a TDS handshake packet claims fewer bytes than its header"};
    }
    const std::size_t body = static_cast<std::size_t>(header.length) - header_size;
    if (body == 0) {
        return std::vector<std::byte>{};
    }
    result<std::vector<std::byte>> chunk = inner_->read_exact(body);
    if (!chunk) {
        return chunk.error();
    }
    return std::move(chunk.value());
}

status tls_channel::flush_stream() {
    char buffer[4096];
    for (;;) {
        const int got = BIO_read(outgoing_, buffer, static_cast<int>(sizeof buffer));
        if (got <= 0) {
            return status{};
        }
        // After the handshake the records are the transport payload: no TDS
        // header, exactly as the reference client writes them.
        const std::byte* first = reinterpret_cast<const std::byte*>(buffer);
        const status sent = inner_->write(span<const std::byte>(first, static_cast<std::size_t>(got)));
        if (!sent) {
            return sent;
        }
    }
}

status tls_channel::fill_ciphertext() {
    std::byte buffer[4096];
    result<std::size_t> got = inner_->read_some(span<std::byte>(buffer, sizeof buffer));
    if (!got) {
        return got.error();
    }
    if (got.value() == 0) {
        return rpc_error{rpc_code::eof, "the transport closed during TLS"};
    }
    if (BIO_write(incoming_, buffer, static_cast<int>(got.value())) <= 0) {
        return tls_failure("buffering ciphertext");
    }
    return status{};
}

result<std::vector<std::byte>> tls_channel::read_exact(std::size_t count) {
    if (session_ == nullptr || closed_) {
        return rpc_error{rpc_code::shutting_down, "the TLS channel is closed"};
    }
    std::vector<std::byte> out;
    out.reserve(count);
    while (out.size() < count) {
        char buffer[max_plaintext_chunk];
        const std::size_t want = count - out.size();
        const int cap = static_cast<int>(std::min(want, sizeof buffer));
        ERR_clear_error();
        const int got = SSL_read(session_, buffer, cap);
        if (got > 0) {
            const std::byte* first = reinterpret_cast<const std::byte*>(buffer);
            out.insert(out.end(), first, first + got);
            continue;
        }
        const int reason = SSL_get_error(session_, got);
        if (reason == SSL_ERROR_WANT_READ) {
            const status filled = fill_ciphertext();
            if (!filled) {
                return filled.error();
            }
            continue;
        }
        if (reason == SSL_ERROR_WANT_WRITE) {
            const status flushed = flush_stream();
            if (!flushed) {
                return flushed.error();
            }
            continue;
        }
        if (reason == SSL_ERROR_ZERO_RETURN) {
            return rpc_error{rpc_code::eof, "the peer closed the TLS stream"};
        }
        return tls_failure("TLS read");
    }
    return out;
}

result<std::size_t> tls_channel::read_some(span<std::byte> buffer) {
    if (session_ == nullptr || closed_) {
        return rpc_error{rpc_code::shutting_down, "the TLS channel is closed"};
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
    for (;;) {
        ERR_clear_error();
        const int got = SSL_read(session_, buffer.data(),
                                 static_cast<int>(std::min<std::size_t>(buffer.size(),
                                                                        max_plaintext_chunk)));
        if (got > 0) {
            return static_cast<std::size_t>(got);
        }
        const int reason = SSL_get_error(session_, got);
        if (reason == SSL_ERROR_WANT_READ) {
            const status filled = fill_ciphertext();
            if (!filled) {
                return filled.error();
            }
            continue;
        }
        if (reason == SSL_ERROR_WANT_WRITE) {
            const status flushed = flush_stream();
            if (!flushed) {
                return flushed.error();
            }
            continue;
        }
        if (reason == SSL_ERROR_ZERO_RETURN) {
            return rpc_error{rpc_code::eof, "the peer closed the TLS stream"};
        }
        return tls_failure("TLS read");
    }
}

status tls_channel::write(span<const std::byte> data) {
    if (session_ == nullptr || closed_) {
        return status{rpc_code::shutting_down, "the TLS channel is closed"};
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t rest = data.size() - offset;
        const int cap = static_cast<int>(std::min(rest, max_plaintext_chunk));
        ERR_clear_error();
        const int wrote = SSL_write(session_, data.data() + offset, cap);
        const status flushed = flush_stream();
        if (!flushed) {
            return flushed;
        }
        if (wrote > 0) {
            offset += static_cast<std::size_t>(wrote);
            continue;
        }
        const int reason = SSL_get_error(session_, wrote);
        if (reason == SSL_ERROR_WANT_READ) {
            const status filled = fill_ciphertext();
            if (!filled) {
                return filled;
            }
            continue;
        }
        if (reason == SSL_ERROR_WANT_WRITE) {
            continue;
        }
        return tls_failure("TLS write");
    }
    return status{};
}

void tls_channel::release() noexcept {
    if (session_ != nullptr) {
        // SSL_free also frees the two BIOs handed to SSL_set_bio.
        SSL_free(session_);
        session_ = nullptr;
        incoming_ = nullptr;
        outgoing_ = nullptr;
    }
    if (context_ != nullptr) {
        SSL_CTX_free(context_);
        context_ = nullptr;
    }
}

void tls_channel::close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    if (session_ != nullptr) {
        ERR_clear_error();
        (void)SSL_shutdown(session_);
        (void)flush_stream();
    }
    if (inner_) {
        inner_->close();
    }
    release();
}

} // namespace

bool tls_backend_available() noexcept { return true; }

result<std::unique_ptr<db::channel>> open_tls_channel(std::unique_ptr<db::channel> inner,
                                                      const tls_client_options& options,
                                                      std::uint8_t& packet_id) {
    if (!inner) {
        return rpc_error{rpc_code::failed_precondition, "the TLS channel has no transport"};
    }
    auto channel = std::make_unique<tls_channel>(std::move(inner), options.packet_size);
    const status established = channel->handshake(options, packet_id);
    if (!established) {
        return established.error();
    }
    return std::unique_ptr<db::channel>(std::move(channel));
}

#endif // CRPC_HAVE_OPENSSL

} // namespace tds
} // namespace db
} // namespace crpc