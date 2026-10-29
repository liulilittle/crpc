// CRPC -- MySQL over TLS channel. See tls_channel.hpp for the framing rules.
#include "tls_channel.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <utility>

#if defined(CRPC_HAVE_OPENSSL)
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

namespace crpc {
namespace db {
namespace mysql {

#if !defined(CRPC_HAVE_OPENSSL)

bool tls_backend_available() noexcept { return false; }

result<std::unique_ptr<db::channel>> open_tls_channel(std::unique_ptr<db::channel>& inner,
                                                      const tls_client_options& options) {
    (void)options;
    // The caller keeps the transport; the connection reports the explicit
    // failure and closes the link itself.
    (void)inner;
    return rpc_error{rpc_code::failed_precondition,
                     "mysql TLS needs a build with CRPC_WITH_OPENSSL=ON"};
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

// One SSL object, one reader plus one writer. The MySQL connection itself runs
// lockstep (it never reads and writes concurrently), but the channel contract
// allows a concurrent reader and writer, so every SSL_* call and the BIO
// drain that follows it happen under one mutex: the wire order of outgoing
// records is the SSL_write order.
class tls_channel final : public db::channel {
public:
    tls_channel(std::unique_ptr<db::channel> inner, SSL_CTX* context)
        : inner_(std::move(inner)), context_(context) {}

    ~tls_channel() override { close(); }

    tls_channel(const tls_channel&) = delete;
    tls_channel& operator=(const tls_channel&) = delete;

    [[nodiscard]] status handshake(const tls_client_options& options);
    // True only after the handshake completed; an object that exists always
    // completed it (open_tls_channel returns nullptr on failure).
    [[nodiscard]] bool is_secure() const noexcept override { return true; }
    // Releases the wrapped transport without destroying it. Used on the
    // handshake-failure path so the caller's channel outlives the attempt.
    [[nodiscard]] std::unique_ptr<db::channel> detach_inner() noexcept {
        return std::move(inner_);
    }

    [[nodiscard]] result<std::vector<std::byte>> read_exact(std::size_t count) override;
    [[nodiscard]] result<std::size_t> read_some(span<std::byte> buffer) override;
    [[nodiscard]] status write(span<const std::byte> data) override;
    void close() noexcept override;

private:
    [[nodiscard]] status flush_records();
    [[nodiscard]] status fill_ciphertext();
    [[nodiscard]] result<std::size_t> decrypt(span<std::byte> buffer);
    void release() noexcept;

    std::unique_ptr<db::channel> inner_;
    SSL_CTX* context_ = nullptr;
    SSL* session_ = nullptr;
    BIO* incoming_ = nullptr;
    BIO* outgoing_ = nullptr;
    std::mutex ssl_mutex_;
    std::atomic<bool> closed_{false};
};

status tls_channel::handshake(const tls_client_options& options) {
    session_ = SSL_new(context_);
    incoming_ = BIO_new(BIO_s_mem());
    outgoing_ = BIO_new(BIO_s_mem());
    if (session_ == nullptr || incoming_ == nullptr || outgoing_ == nullptr) {
        return tls_failure("creating the TLS session");
    }
    // "no more input" must be a retry, not a close: the server's next record
    // has simply not been read from the transport yet.
    BIO_set_mem_eof_return(incoming_, -1);
    SSL_set_bio(session_, incoming_, outgoing_);
    if (!options.server_name.empty()) {
        (void)SSL_set_tlsext_host_name(session_, options.server_name.c_str());
        if (options.verify_server) {
            // SSL_set1_host is deprecated in OpenSSL 4; the verify-parameter
            // form is the supported spelling and checks the same names.
            X509_VERIFY_PARAM* param = SSL_get0_param(session_);
            if (param == nullptr ||
                X509_VERIFY_PARAM_set1_host(param, options.server_name.c_str(), 0) != 1) {
                return tls_failure("configuring TLS hostname verification");
            }
        }
    }
    SSL_set_connect_state(session_);

    for (;;) {
        int progressed = 0;
        int reason = 0;
        {
            const std::lock_guard<std::mutex> guard(ssl_mutex_);
            ERR_clear_error();
            progressed = SSL_do_handshake(session_);
            if (progressed != 1) {
                reason = SSL_get_error(session_, progressed);
            }
        }
        const status flushed = flush_records();
        if (!flushed) {
            return flushed;
        }
        if (progressed == 1) {
            return status{};
        }
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
        // A verification failure is its own error: a caller that disabled the
        // check must not be told it failed, and a caller that left it on must
        // be told the chain or the name did not verify -- not "handshake".
        if (reason == SSL_ERROR_SSL) {
            const long verified = SSL_get_verify_result(session_);
            if (verified != X509_V_OK) {
                return rpc_error{
                    rpc_code::failed_precondition,
                    "TLS certificate verification failed: " +
                        std::string(X509_verify_cert_error_string(verified))};
            }
            return tls_failure("the TLS handshake");
        }
        return tls_failure("the TLS handshake");
    }
}

status tls_channel::flush_records() {
    if (!inner_) {
        return status{};
    }
    std::lock_guard<std::mutex> guard(ssl_mutex_);
    char buffer[4096];
    for (;;) {
        const int got = BIO_read(outgoing_, buffer, static_cast<int>(sizeof buffer));
        if (got <= 0) {
            return status{};
        }
        const status sent =
            inner_->write(span<const std::byte>(reinterpret_cast<const std::byte*>(buffer),
                                                static_cast<std::size_t>(got)));
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
        return rpc_error{rpc_code::eof, "the transport closed during the TLS handshake"};
    }
    const std::lock_guard<std::mutex> guard(ssl_mutex_);
    if (BIO_write(incoming_, buffer, static_cast<int>(got.value())) <= 0) {
        return tls_failure("buffering ciphertext");
    }
    return status{};
}

// One decryption attempt: a positive count, or "the caller must feed more
// ciphertext / flush" signalled by returning 0 with a want_* flag.
result<std::size_t> tls_channel::decrypt(span<std::byte> buffer) {
    int got = 0;
    int reason = 0;
    {
        const std::lock_guard<std::mutex> guard(ssl_mutex_);
        ERR_clear_error();
        got = SSL_read(session_, buffer.data(),
                       static_cast<int>(std::min<std::size_t>(buffer.size(),
                                                              max_plaintext_chunk)));
        if (got <= 0) {
            reason = SSL_get_error(session_, got);
        }
    }
    if (got > 0) {
        return static_cast<std::size_t>(got);
    }
    if (reason == SSL_ERROR_WANT_READ) {
        const status filled = fill_ciphertext();
        if (!filled) {
            return filled.error();
        }
        return std::size_t{0};
    }
    if (reason == SSL_ERROR_WANT_WRITE) {
        const status flushed = flush_records();
        if (!flushed) {
            return flushed.error();
        }
        return std::size_t{0};
    }
    if (reason == SSL_ERROR_ZERO_RETURN) {
        return rpc_error{rpc_code::eof, "the peer closed the TLS stream"};
    }
    return tls_failure("TLS read");
}

result<std::vector<std::byte>> tls_channel::read_exact(std::size_t count) {
    if (session_ == nullptr || closed_.load()) {
        return rpc_error{rpc_code::shutting_down, "the TLS channel is closed"};
    }
    std::vector<std::byte> out;
    out.reserve(count);
    while (out.size() < count) {
        std::byte buffer[max_plaintext_chunk];
        const std::size_t want = count - out.size();
        result<std::size_t> step =
            decrypt(span<std::byte>(buffer, std::min(want, sizeof buffer)));
        if (!step) {
            return step.error();
        }
        if (step.value() != 0) {
            out.insert(out.end(), buffer, buffer + step.value());
        }
    }
    return out;
}

result<std::size_t> tls_channel::read_some(span<std::byte> buffer) {
    if (session_ == nullptr || closed_.load()) {
        return rpc_error{rpc_code::shutting_down, "the TLS channel is closed"};
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
    for (;;) {
        result<std::size_t> step = decrypt(buffer);
        if (!step) {
            return step.error();
        }
        if (step.value() != 0) {
            return step;
        }
    }
}

status tls_channel::write(span<const std::byte> data) {
    if (session_ == nullptr || closed_.load()) {
        return status{rpc_code::shutting_down, "the TLS channel is closed"};
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t rest = data.size() - offset;
        const int cap = static_cast<int>(std::min(rest, max_plaintext_chunk));
        int wrote = 0;
        int reason = 0;
        {
            const std::lock_guard<std::mutex> guard(ssl_mutex_);
            ERR_clear_error();
            wrote = SSL_write(session_, data.data() + offset, cap);
            if (wrote <= 0) {
                reason = SSL_get_error(session_, wrote);
            }
        }
        // The record exists in the memory BIO now; push it out in one piece so
        // the wire never interleaves two records.
        const status flushed = flush_records();
        if (!flushed) {
            return flushed;
        }
        if (wrote > 0) {
            offset += static_cast<std::size_t>(wrote);
            continue;
        }
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
        if (reason == SSL_ERROR_ZERO_RETURN) {
            return status{rpc_code::eof, "the peer closed the TLS stream"};
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
    if (closed_.exchange(true)) {
        return;
    }
    if (session_ != nullptr) {
        const std::lock_guard<std::mutex> guard(ssl_mutex_);
        ERR_clear_error();
        (void)SSL_shutdown(session_);
    }
    (void)flush_records();
    if (inner_) {
        inner_->close();
    }
    release();
}

} // namespace

bool tls_backend_available() noexcept { return true; }

result<std::unique_ptr<db::channel>> open_tls_channel(std::unique_ptr<db::channel>& inner,
                                                      const tls_client_options& options) {
    if (!inner) {
        return rpc_error{rpc_code::failed_precondition, "the TLS channel has no transport"};
    }
    SSL_CTX* context = SSL_CTX_new(TLS_client_method());
    if (context == nullptr) {
        return tls_failure("creating a TLS context");
    }
    // MySQL 8 negotiates TLS 1.2 and 1.3; nothing needs to be pinned above the
    // library floor.
    (void)SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
    (void)SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION);
    if (options.verify_server) {
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
        const int loaded =
            !options.ca_bundle.empty()
                ? SSL_CTX_load_verify_locations(context, options.ca_bundle.c_str(), nullptr)
                : SSL_CTX_set_default_verify_paths(context);
        if (loaded != 1) {
            SSL_CTX_free(context);
            return tls_failure("loading the TLS trust anchors");
        }
    } else {
        SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
    }

    auto channel = std::make_unique<tls_channel>(std::move(inner), context);
    const status established = channel->handshake(options);
    if (!established) {
        // Hand the transport back: the caller still owns it (closed by the
        // wrapper's teardown below or by its own failure path) and the session
        // is about to fail, but the channel object outlives the attempt.
        std::unique_ptr<db::channel> returned(channel->detach_inner());
        channel.reset();
        inner = std::move(returned);
        return established.error();
    }
    return std::unique_ptr<db::channel>(std::move(channel));
}

#endif // CRPC_HAVE_OPENSSL

} // namespace mysql
} // namespace db
} // namespace crpc
