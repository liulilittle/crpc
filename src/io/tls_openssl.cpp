// CRPC -- OpenSSL implementation of the TLS decorator.
//
// Everything that names an OpenSSL type is inside
// `#if defined(CRPC_HAVE_OPENSSL)`.  When the macro is absent this file still
// defines the public entry points (returning tls_errc::ssl_not_available) so
// that an application can include the header and link without OpenSSL; the
// paired test prints a skip line in that configuration.
//
// OpenSSL API surface used (present in the OpenSSL 1.1.1 and 3.x public headers;
// the guarded branch is compiled and run under WSL with OpenSSL 3.0.13 and on
// Windows with OpenSSL 4.0.3, CRPC_WITH_OPENSSL=ON, where crpc_test_tls passes
// 8 cases / 80 checks):
//
//   init/context   OPENSSL_init_ssl, TLS_method, SSL_CTX_new, SSL_CTX_free,
//                  SSL_CTX_set_min_proto_version, SSL_CTX_set_max_proto_version,
//                  SSL_CTX_set_options, SSL_OP_NO_COMPRESSION
//   credentials    SSL_CTX_use_certificate_chain_file,
//                  SSL_CTX_use_PrivateKey_file, SSL_FILETYPE_PEM,
//                  SSL_CTX_check_private_key, SSL_CTX_load_verify_locations,
//                  SSL_CTX_set_default_verify_paths, SSL_CTX_set_verify
//   ALPN           SSL_CTX_set_alpn_protos, SSL_CTX_set_alpn_select_cb,
//                  SSL_select_next_proto, OPENSSL_NPN_NEGOTIATED,
//                  SSL_TLSEXT_ERR_OK/ALERT_FATAL, SSL_get0_alpn_selected
//   sessions       SSL_CTX_set_session_cache_mode, SSL_SESS_CACHE_CLIENT/
//                  SERVER/NO_INTERNAL_STORE, SSL_CTX_sess_set_cache_size,
//                  SSL_CTX_set_session_id_context, SSL_get1_session,
//                  SSL_set_session, SSL_SESSION_free, SSL_session_reused
//   connection     SSL_new, SSL_free, SSL_set_bio, BIO_new, BIO_s_mem,
//                  BIO_write, BIO_read, BIO_ctrl_pending, BIO_set_mem_eof_return,
//                  SSL_set_connect_state, SSL_set_accept_state,
//                  SSL_do_handshake, SSL_read, SSL_write, SSL_get_error,
//                  SSL_shutdown, SSL_get_verify_result, SSL_get0_param,
//                  SSL_set1_host, SSL_set_hostflags, SSL_set_tlsext_host_name,
//                  X509_VERIFY_PARAM_set1_ip_asc,
//                  X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS
//   errors         ERR_clear_error, ERR_peek_error, ERR_get_error, ERR_GET_LIB,
//                  ERR_GET_REASON, ERR_LIB_SSL, SSL_R_CERTIFICATE_VERIFY_FAILED,
//                  SSL_R_NO_PEER_CERTIFICATE, SSL_ERROR_*
//
// Ownership and destructor order
// ------------------------------
// `SSL_new` increments the SSL_CTX reference count and `SSL_set_bio` transfers
// ownership of both BIOs to the SSL.  Therefore:
//
//   * the context holder owns SSL_CTX and calls SSL_CTX_free exactly once;
//   * a stream owns SSL and calls SSL_free exactly once, which releases both
//     BIOs (never call BIO_free on a BIO that SSL_set_bio accepted);
//   * on the failure path before SSL_set_bio the BIOs are freed explicitly;
//   * a transport completion owns its ciphertext block, not the stream, so a
//     late completion after stream destruction is safe.
#include <crpc/io/tls.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(CRPC_HAVE_OPENSSL)

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>

#endif // CRPC_HAVE_OPENSSL

namespace crpc::io {
namespace {

// --- Error category (defined in both configurations) -------------------------

class tls_error_category_impl final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "crpc.tls"; }

    [[nodiscard]] std::string message(int value) const override {
        switch (static_cast<tls_errc>(value)) {
        case tls_errc::ok:
            return "no error";
        case tls_errc::ssl_configuration_error:
            return "TLS configuration error";
        case tls_errc::ssl_handshake_failed:
            return "TLS handshake failed";
        case tls_errc::ssl_verification_failed:
            return "TLS certificate or host name verification failed";
        case tls_errc::ssl_protocol_error:
            return "TLS protocol error";
        case tls_errc::ssl_alpn_mismatch:
            return "TLS ALPN negotiation mismatch";
        case tls_errc::ssl_closed:
            return "TLS connection closed";
        case tls_errc::ssl_not_available:
            return "TLS support is not available in this build";
        }
        return "unknown TLS error";
    }
};

} // namespace

const std::error_category& tls_error_category() noexcept {
    static const tls_error_category_impl category;
    return category;
}

std::error_code make_error_code(tls_errc code) noexcept {
    return std::error_code(static_cast<int>(code), tls_error_category());
}

#if defined(CRPC_HAVE_OPENSSL)
namespace {

// Bounds.  The input bound is applied to the memory BIO before more ciphertext
// is accepted; the plaintext bound applies to bytes retained because the
// consumer paused or no consumer was bound yet.
constexpr std::size_t k_max_ciphertext = 256 * 1024;
constexpr std::size_t k_max_plaintext = 256 * 1024;
constexpr std::size_t k_max_encrypted = 256 * 1024;
constexpr std::size_t k_read_chunk = 16 * 1024;
constexpr std::size_t k_write_chunk = 16 * 1024;

// The immutable per-SSL_CTX state.  The ALPN callback receives a pointer to
// this object.  Every stream keeps the holder alive for as long as its SSL
// object exists, so the callback argument cannot dangle.
struct ssl_ctx_holder {
    SSL_CTX* ctx = nullptr;
    tls_config cfg;
    std::vector<unsigned char> client_alpn;
    std::vector<unsigned char> server_alpn;

    ssl_ctx_holder() = default;
    ~ssl_ctx_holder() {
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
    }

    ssl_ctx_holder(const ssl_ctx_holder&) = delete;
    ssl_ctx_holder& operator=(const ssl_ctx_holder&) = delete;
};

[[nodiscard]] int to_openssl_version(tls_version version) noexcept {
    switch (version) {
    case tls_version::tls_1_2:
        return TLS1_2_VERSION;
    case tls_version::tls_1_3:
        return TLS1_3_VERSION;
    }
    return 0;
}

[[nodiscard]] int to_int_size(std::size_t value) noexcept {
    const std::size_t limit = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    return value > limit ? (std::numeric_limits<int>::max)()
                         : static_cast<int>(value);
}

// BIO_ctrl_pending returns `long` in OpenSSL 1.1.1 and `size_t` in 3.x.
// Normalizing here keeps both configurations warning-free.
[[nodiscard]] std::size_t bio_pending(BIO* bio) noexcept {
    const auto value = BIO_ctrl_pending(bio);
    using value_type = std::remove_cv_t<decltype(value)>;
    if constexpr (std::is_signed<value_type>::value) {
        return value > 0 ? static_cast<std::size_t>(value) : 0;
    } else {
        return static_cast<std::size_t>(value);
    }
}

// Encodes a protocol list in the wire format used by ALPN/NPN: one length byte
// followed by that many bytes per protocol.
[[nodiscard]] bool build_alpn_wire(const std::vector<std::string>& protocols,
                                   std::vector<unsigned char>& out) {
    out.clear();
    for (const std::string& protocol : protocols) {
        if (protocol.empty() || protocol.size() > 255) {
            return false;
        }
        out.push_back(static_cast<unsigned char>(protocol.size()));
        out.insert(out.end(), protocol.begin(), protocol.end());
    }
    return true;
}

// Server-side ALPN selection.  SSL_select_next_proto walks the server list in
// order, so the server preference wins over the client's ordering.
int alpn_select_cb(SSL*, const unsigned char** out, unsigned char* outlen,
                   const unsigned char* in, unsigned int inlen, void* arg) {
    const auto* holder = static_cast<const ssl_ctx_holder*>(arg);
    unsigned char* selected = nullptr;
    unsigned char selected_len = 0;
    const int result = SSL_select_next_proto(
        &selected, &selected_len, holder->server_alpn.data(),
        static_cast<unsigned int>(holder->server_alpn.size()), in, inlen);
    if (result == OPENSSL_NPN_NEGOTIATED) {
        *out = selected;
        *outlen = selected_len;
        return SSL_TLSEXT_ERR_OK;
    }
    // ALPN is mandatory for CRPC TLS; a client that offers no common protocol
    // is rejected rather than silently downgraded (specification section 12.1).
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

[[nodiscard]] bool looks_like_ip(const std::string& host) noexcept {
    if (host.empty()) {
        return false;
    }
    bool has_colon = false;
    for (const char c : host) {
        if (c == ':') {
            has_colon = true;
            continue;
        }
        if (c == '.' || (c >= '0' && c <= '9')) {
            continue;
        }
        if (has_colon &&
            ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '%')) {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] std::error_code map_ssl_error(SSL* ssl, int ssl_error,
                                             bool during_handshake) noexcept {
    switch (ssl_error) {
    case SSL_ERROR_NONE:
        return {};
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
    case SSL_ERROR_WANT_X509_LOOKUP:
    case SSL_ERROR_WANT_ASYNC:
    case SSL_ERROR_WANT_CONNECT:
    case SSL_ERROR_WANT_ACCEPT:
        return {};
    case SSL_ERROR_ZERO_RETURN:
        // A clean close_notify is EOF.
        return make_error_code(tls_errc::ssl_closed);
    case SSL_ERROR_SYSCALL:
        // The task contract: a syscall error with an empty OpenSSL error queue
        // is an orderly EOF (the peer closed without a close_notify).
        if (ERR_peek_error() == 0) {
            return make_error_code(tls_errc::ssl_closed);
        }
        return make_error_code(tls_errc::ssl_protocol_error);
    case SSL_ERROR_SSL:
    default: {
        bool verify_failed = SSL_get_verify_result(ssl) != X509_V_OK;
        unsigned long error = 0;
        while ((error = ERR_get_error()) != 0) {
            if (ERR_GET_LIB(error) == ERR_LIB_SSL) {
                const int reason = ERR_GET_REASON(error);
                if (reason == SSL_R_CERTIFICATE_VERIFY_FAILED) {
                    verify_failed = true;
                }
            }
        }
        if (verify_failed) {
            return make_error_code(tls_errc::ssl_verification_failed);
        }
        return make_error_code(during_handshake ? tls_errc::ssl_handshake_failed
                                                : tls_errc::ssl_protocol_error);
    }
    }
}

[[nodiscard]] std::shared_ptr<ssl_ctx_holder> build_holder(const tls_config& cfg,
                                                            std::error_code& ec) noexcept {
    ec.clear();
    if (static_cast<int>(cfg.min_version) > static_cast<int>(cfg.max_version)) {
        ec = make_error_code(tls_errc::ssl_configuration_error);
        return nullptr;
    }
    if (cfg.role == tls_role::server &&
        (cfg.certificate_chain.empty() || cfg.private_key.empty())) {
        ec = make_error_code(tls_errc::ssl_configuration_error);
        return nullptr;
    }
    if (cfg.require_alpn && cfg.alpn_protocols.empty()) {
        ec = make_error_code(tls_errc::ssl_configuration_error);
        return nullptr;
    }

    try {
        auto holder = std::shared_ptr<ssl_ctx_holder>(
            new (std::nothrow) ssl_ctx_holder());
        if (!holder) {
            ec = std::make_error_code(std::errc::not_enough_memory);
            return nullptr;
        }
        holder->cfg = cfg;

        if (!build_alpn_wire(cfg.alpn_protocols, holder->client_alpn) ||
            !build_alpn_wire(cfg.alpn_protocols, holder->server_alpn)) {
            ec = make_error_code(tls_errc::ssl_configuration_error);
            return nullptr;
        }

        if (OPENSSL_init_ssl(0, nullptr) != 1) {
            ec = make_error_code(tls_errc::ssl_configuration_error);
            return nullptr;
        }

        ERR_clear_error();
        holder->ctx = SSL_CTX_new(TLS_method());
        if (holder->ctx == nullptr) {
            ERR_clear_error();
            ec = make_error_code(tls_errc::ssl_configuration_error);
            return nullptr;
        }

        (void)SSL_CTX_set_options(holder->ctx, SSL_OP_NO_COMPRESSION);

        if (SSL_CTX_set_min_proto_version(holder->ctx,
                                          to_openssl_version(cfg.min_version)) != 1 ||
            SSL_CTX_set_max_proto_version(holder->ctx,
                                          to_openssl_version(cfg.max_version)) != 1) {
            ERR_clear_error();
            ec = make_error_code(tls_errc::ssl_configuration_error);
            return nullptr;
        }

        // Verification.  A server does not request a client certificate in v1.
        if (cfg.role == tls_role::client && cfg.verify == tls_verify_mode::peer) {
            SSL_CTX_set_verify(holder->ctx, SSL_VERIFY_PEER, nullptr);
            if (!cfg.ca_bundle.empty()) {
                if (SSL_CTX_load_verify_locations(holder->ctx, cfg.ca_bundle.c_str(),
                                                  nullptr) != 1) {
                    ERR_clear_error();
                    ec = make_error_code(tls_errc::ssl_configuration_error);
                    return nullptr;
                }
            } else if (SSL_CTX_set_default_verify_paths(holder->ctx) != 1) {
                ERR_clear_error();
                ec = make_error_code(tls_errc::ssl_configuration_error);
                return nullptr;
            }
        } else {
            SSL_CTX_set_verify(holder->ctx, SSL_VERIFY_NONE, nullptr);
        }

        // Server credentials; client credentials are optional and unused in v1
        // but loading them costs nothing and leaves the door open for mTLS.
        if (!cfg.certificate_chain.empty()) {
            if (SSL_CTX_use_certificate_chain_file(holder->ctx,
                                                   cfg.certificate_chain.c_str()) != 1) {
                ERR_clear_error();
                ec = make_error_code(tls_errc::ssl_configuration_error);
                return nullptr;
            }
        }
        if (!cfg.private_key.empty()) {
            if (SSL_CTX_use_PrivateKey_file(holder->ctx, cfg.private_key.c_str(),
                                            SSL_FILETYPE_PEM) != 1) {
                ERR_clear_error();
                ec = make_error_code(tls_errc::ssl_configuration_error);
                return nullptr;
            }
        }
        if (!cfg.certificate_chain.empty() && !cfg.private_key.empty() &&
            SSL_CTX_check_private_key(holder->ctx) != 1) {
            ERR_clear_error();
            ec = make_error_code(tls_errc::ssl_configuration_error);
            return nullptr;
        }

        // ALPN.
        if (!holder->client_alpn.empty()) {
            if (SSL_CTX_set_alpn_protos(
                    holder->ctx, holder->client_alpn.data(),
                    static_cast<unsigned int>(holder->client_alpn.size())) != 0) {
                ERR_clear_error();
                ec = make_error_code(tls_errc::ssl_configuration_error);
                return nullptr;
            }
        }
        if (cfg.role == tls_role::server && !holder->server_alpn.empty()) {
            SSL_CTX_set_alpn_select_cb(holder->ctx, &alpn_select_cb, holder.get());
        }

        // Session cache.  The client keeps its own bounded map; the server uses
        // OpenSSL's internal session-id cache with the same bound.
        if (SSL_CTX_set_session_id_context(
                holder->ctx, reinterpret_cast<const unsigned char*>("crpc-tls"), 8) != 1) {
            ERR_clear_error();
            ec = make_error_code(tls_errc::ssl_configuration_error);
            return nullptr;
        }
        if (cfg.session_reuse) {
            if (cfg.role == tls_role::client) {
                (void)SSL_CTX_set_session_cache_mode(
                    holder->ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
            } else {
                (void)SSL_CTX_set_session_cache_mode(holder->ctx, SSL_SESS_CACHE_SERVER);
                SSL_CTX_sess_set_cache_size(
                    holder->ctx,
                    static_cast<long>(std::min<std::size_t>(
                        cfg.session_cache_entries, 0x7fffffffU)));
            }
        } else {
            (void)SSL_CTX_set_session_cache_mode(holder->ctx, SSL_SESS_CACHE_OFF);
        }

        ec.clear();
        return holder;
    } catch (const std::bad_alloc&) {
        ec = std::make_error_code(std::errc::not_enough_memory);
    } catch (...) {
        ec = make_error_code(tls_errc::ssl_configuration_error);
    }
    return nullptr;
}

} // namespace

// --- tls_context::impl -------------------------------------------------------

struct tls_context::impl {
    using session_key = std::pair<const void*, std::string>;

    mutable std::mutex mutex;
    std::shared_ptr<const ssl_ctx_holder> holder;
    std::map<session_key, std::weak_ptr<tls_stream>> live;

    std::mutex cache_mutex;
    std::map<std::string, SSL_SESSION*> sessions;
    std::deque<std::string> session_order;
    std::size_t session_limit = 1024;

    ~impl() {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (auto& entry : sessions) {
            SSL_SESSION_free(entry.second);
        }
        sessions.clear();
        session_order.clear();
    }

    [[nodiscard]] std::shared_ptr<const ssl_ctx_holder> current() const {
        std::lock_guard<std::mutex> lock(mutex);
        return holder;
    }

    void install(std::shared_ptr<const ssl_ctx_holder> next) {
        std::lock_guard<std::mutex> lock(mutex);
        holder = std::move(next);
    }

    [[nodiscard]] SSL_SESSION* fetch_session(const std::string& host) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        const auto it = sessions.find(host);
        if (it == sessions.end()) {
            return nullptr;
        }
        if (SSL_SESSION_up_ref(it->second) != 1) {
            return nullptr;
        }
        return it->second;
    }

    // Takes ownership of one reference to `session`.
    void store_session(const std::string& host, SSL_SESSION* session) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        const auto it = sessions.find(host);
        if (it != sessions.end()) {
            SSL_SESSION_free(it->second);
            it->second = session;
            return;
        }
        if (sessions.size() >= session_limit) {
            while (!session_order.empty() &&
                   sessions.find(session_order.front()) == sessions.end()) {
                session_order.pop_front();
            }
            if (!session_order.empty()) {
                const std::string victim = session_order.front();
                session_order.pop_front();
                const auto victim_it = sessions.find(victim);
                if (victim_it != sessions.end()) {
                    SSL_SESSION_free(victim_it->second);
                    sessions.erase(victim_it);
                }
            }
        }
        sessions.emplace(host, session);
        session_order.push_back(host);
    }

    void clear_sessions() noexcept {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (auto& entry : sessions) {
            SSL_SESSION_free(entry.second);
        }
        sessions.clear();
        session_order.clear();
    }

    void set_session_limit(std::size_t limit) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        session_limit = limit == 0 ? 1 : limit;
    }

    [[nodiscard]] std::shared_ptr<tls_stream> find(const session_key& key) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = live.find(key);
        if (it == live.end()) {
            return nullptr;
        }
        return it->second.lock();
    }

    [[nodiscard]] std::shared_ptr<tls_stream> install_if_absent(
        const session_key& key, const std::shared_ptr<tls_stream>& candidate) {
        std::lock_guard<std::mutex> lock(mutex);
        auto& slot = live[key];
        if (auto existing = slot.lock()) {
            return existing;
        }
        slot = candidate;
        return candidate;
    }

    void erase_if_expired(const session_key& key) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = live.find(key);
        if (it != live.end() && it->second.expired()) {
            live.erase(it);
        }
    }
};

// --- tls_stream::impl --------------------------------------------------------

class tls_stream::impl : public std::enable_shared_from_this<tls_stream::impl> {
public:
    impl(io::sink& transport, std::shared_ptr<tls_context::impl> context,
         std::shared_ptr<const ssl_ctx_holder> holder, std::string hostname)
        : transport_(transport),
          context_(std::move(context)),
          holder_(std::move(holder)),
          hostname_(std::move(hostname)) {}

    ~impl() {
        close();
        if (ssl_ != nullptr) {
            SSL_free(ssl_);
            ssl_ = nullptr;
            rbio_ = nullptr;
            wbio_ = nullptr;
        }
    }

    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    // The completion handed to the transport.  It owns its bytes, so the
    // transport's span stays valid even if the stream is destroyed first.
    struct tx_block final : completion {
        std::weak_ptr<impl> owner;
        std::vector<std::byte> bytes;

        void complete(io_result result) noexcept override {
            if (auto locked = owner.lock()) {
                locked->on_transport_complete(result);
            }
            delete this;
        }
    };

    [[nodiscard]] std::error_code initialize() {
        ERR_clear_error();
        ssl_ = SSL_new(holder_->ctx);
        if (ssl_ == nullptr) {
            ERR_clear_error();
            return make_error_code(tls_errc::ssl_configuration_error);
        }
        rbio_ = BIO_new(BIO_s_mem());
        wbio_ = BIO_new(BIO_s_mem());
        if (rbio_ == nullptr || wbio_ == nullptr) {
            if (rbio_ != nullptr) {
                BIO_free(rbio_);
            }
            if (wbio_ != nullptr) {
                BIO_free(wbio_);
            }
            rbio_ = nullptr;
            wbio_ = nullptr;
            SSL_free(ssl_);
            ssl_ = nullptr;
            return std::make_error_code(std::errc::not_enough_memory);
        }
        // SSL owns both BIOs from here on; they are released by SSL_free.
        SSL_set_bio(ssl_, rbio_, wbio_);

        const tls_config& cfg = holder_->cfg;
        if (cfg.role == tls_role::client) {
            SSL_set_connect_state(ssl_);
            if (hostname_.empty()) {
                if (cfg.verify == tls_verify_mode::peer) {
                    return make_error_code(tls_errc::ssl_configuration_error);
                }
            } else if (looks_like_ip(hostname_)) {
                if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl_),
                                                  hostname_.c_str()) != 1) {
                    ERR_clear_error();
                    return make_error_code(tls_errc::ssl_configuration_error);
                }
            } else {
                if (SSL_set_tlsext_host_name(ssl_, hostname_.c_str()) != 1) {
                    ERR_clear_error();
                    return make_error_code(tls_errc::ssl_configuration_error);
                }
                if (cfg.verify == tls_verify_mode::peer) {
                    X509_VERIFY_PARAM* param = SSL_get0_param(ssl_);
                    if (X509_VERIFY_PARAM_set1_host(param, hostname_.c_str(), 0) != 1) {
                        ERR_clear_error();
                        return make_error_code(tls_errc::ssl_configuration_error);
                    }
                    X509_VERIFY_PARAM_set_hostflags(
                        param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
                }
            }

            if (cfg.session_reuse) {
                SSL_SESSION* session = context_->fetch_session(hostname_);
                if (session != nullptr) {
                    if (SSL_set_session(ssl_, session) != 1) {
                        ERR_clear_error();
                    }
                    SSL_SESSION_free(session);
                }
            }
        } else {
            SSL_set_accept_state(ssl_);
        }
        return {};
    }

    void start() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (started_) {
                return;
            }
            started_ = true;
        }
        drive();
    }

    std::size_t on_data(span<const std::byte> data) noexcept {
        if (data.empty()) {
            drive();
            return 0;
        }
        std::size_t accepted = 0;
        std::error_code failure{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == state::failed || state_ == state::closed) {
                return 0;
            }
            if (ssl_ == nullptr || rbio_ == nullptr) {
                return 0;
            }
            const std::size_t pending = bio_pending(rbio_);
            const std::size_t room =
                pending < k_max_ciphertext ? k_max_ciphertext - pending : 0;
            const std::size_t count = std::min(data.size(), room);
            if (count != 0) {
                const int written =
                    BIO_write(rbio_, data.data(), to_int_size(count));
                if (written <= 0) {
                    // A memory BIO only fails when it cannot grow.
                    failure = std::make_error_code(std::errc::not_enough_memory);
                    fail_locked(failure);
                } else {
                    accepted = static_cast<std::size_t>(written);
                }
            }
        }
        drive();
        return accepted;
    }

    void on_eof() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == state::failed || state_ == state::closed) {
                return;
            }
            eof_signaled_ = true;
            if (rbio_ != nullptr) {
                // Make an empty memory BIO report EOF (0) instead of "retry".
                // A truncated record still fails inside SSL_read, which is the
                // difference between a clean EOF and a protocol error.
                (void)BIO_set_mem_eof_return(rbio_, 0);
            }
        }
        drive();
    }

    void on_error(std::error_code ec) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == state::failed || state_ == state::closed) {
                return;
            }
            fail_locked(ec);
        }
        drive();
    }

    void push(span<const std::byte> data, completion* c) noexcept {
        if (c == nullptr) {
            return;
        }
        bool immediate = false;
        io_result immediate_result{};
        completion* canceled_write = nullptr;
        io_result canceled_result{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == state::failed) {
                immediate = true;
                immediate_result = io_result{error_, 0};
            } else if (state_ == state::closed) {
                immediate = true;
                immediate_result =
                    io_result{std::make_error_code(std::errc::operation_canceled), 0};
            } else if (write_completion_ != nullptr) {
                // I5: one sink push in flight.
                immediate = true;
                immediate_result =
                    io_result{std::make_error_code(std::errc::operation_not_supported), 0};
            } else if (data.empty()) {
                immediate = true;
                immediate_result = io_result{{}, 0};
            } else if (data.size() >
                       static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
                immediate = true;
                immediate_result =
                    io_result{std::make_error_code(std::errc::value_too_large), 0};
            } else {
                // Borrow the caller's span. The common case completes the
                // write synchronously inside drive() below, so no copy of the
                // plaintext is needed. If the write parks (transport backlog,
                // or SSL_write wants read/write), materialize_borrowed_locked()
                // copies the unwritten remainder into write_buffer_ before
                // push() returns, which is what preserves the "the caller may
                // reuse the span afterwards" contract.
                borrowed_plaintext_ = data;
                write_buffer_.clear();
                write_buffer_base_ = 0;
                write_length_ = data.size();
                write_offset_ = 0;
                write_completion_ = c;
            }
        }
        if (immediate) {
            c->complete(immediate_result);
            return;
        }
        drive();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            materialize_borrowed_locked(canceled_write, canceled_result);
        }
        // The completion runs outside the lock: it is user code and must not be
        // invoked while mutex_ is held (it may re-enter the stream).
        if (canceled_write != nullptr) {
            canceled_write->complete(canceled_result);
        }
    }

    void bind_source(source* plaintext) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            plaintext_ = plaintext;
        }
        drive();
    }

    void resume_read() noexcept {
        drive();
    }

    void close() noexcept {
        completion* canceled = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == state::closed) {
                return;
            }
            if (state_ == state::failed) {
                state_ = state::closed;
                canceled = write_completion_;
                write_completion_ = nullptr;
                write_buffer_.clear();
                write_buffer_base_ = 0;
                borrowed_plaintext_ = span<const std::byte>();
                write_length_ = 0;
                write_offset_ = 0;
                source_terminal_ = true;
            } else {
                cache_session_locked();
                state_ = state::closed;
                source_terminal_ = true;
                canceled = write_completion_;
                write_completion_ = nullptr;
                write_buffer_.clear();
                write_buffer_base_ = 0;
                borrowed_plaintext_ = span<const std::byte>();
                write_length_ = 0;
                write_offset_ = 0;
                if (ssl_ != nullptr && established_ever_) {
                    ERR_clear_error();
                    (void)SSL_shutdown(ssl_);
                    ERR_clear_error();
                    drain_wbio_locked();
                }
            }
        }
        if (canceled != nullptr) {
            canceled->complete(
                io_result{std::make_error_code(std::errc::operation_canceled), 0});
        }
        pump_tx();
    }

    [[nodiscard]] bool handshake_complete() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return established_ever_;
    }

    [[nodiscard]] bool handshake_failed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_ == state::failed && !established_ever_;
    }

    [[nodiscard]] bool session_reused() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_reused_;
    }

    [[nodiscard]] std::string alpn_protocol() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (alpn_pointer_ == nullptr || alpn_length_ == 0) {
            return std::string();
        }
        return std::string(reinterpret_cast<const char*>(alpn_pointer_),
                           alpn_length_);
    }

    [[nodiscard]] std::string server_name() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ssl_ == nullptr) {
            return std::string();
        }
        const char* name = SSL_get_servername(ssl_, TLSEXT_NAMETYPE_host_name);
        return name != nullptr ? std::string(name) : std::string();
    }

    [[nodiscard]] std::error_code error() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

private:
    enum class state : std::uint8_t { idle, handshaking, established, closed, failed };

    enum class consume : std::uint8_t { progress, paused, failed };

    void drive() noexcept {
        // One driver at a time.  A re-entrant call (for example a source that
        // pushes an echo from inside on_data) only sets drive_again_ and lets
        // the outer loop run another pass.
        for (;;) {
            bool expected = false;
            if (!driving_.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
                drive_again_.store(true, std::memory_order_release);
                return;
            }
            for (;;) {
                drive_once();
                if (!drive_again_.exchange(false, std::memory_order_acq_rel)) {
                    break;
                }
            }
            driving_.store(false, std::memory_order_release);
            if (!drive_again_.exchange(false, std::memory_order_acq_rel)) {
                return;
            }
        }
    }

    void drive_once() noexcept {
        for (;;) {
            enum class action : std::uint8_t { none, plaintext, eof, fail };
            action act = action::none;
            std::size_t plaintext_length = 0;
            std::error_code failure{};
            completion* completed_write = nullptr;
            io_result completed_result{};

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (state_ == state::failed) {
                    act = action::fail;
                    failure = error_;
                } else if (state_ == state::closed) {
                    act = action::eof;
                } else {
                    if (state_ == state::idle) {
                        state_ = state::handshaking;
                    }

                    if (state_ == state::handshaking) {
                        ERR_clear_error();
                        const int result = SSL_do_handshake(ssl_);
                        if (result == 1) {
                            state_ = state::established;
                            established_ever_ = true;
                            post_handshake_locked(failure);
                            if (failure) {
                                fail_locked(failure);
                            }
                        } else {
                            const int ssl_error = SSL_get_error(ssl_, result);
                            if (ssl_error != SSL_ERROR_WANT_READ &&
                                ssl_error != SSL_ERROR_WANT_WRITE) {
                                const std::error_code ec =
                                    map_ssl_error(ssl_, ssl_error, true);
                                if (ec == make_error_code(tls_errc::ssl_closed)) {
                                    state_ = state::closed;
                                } else {
                                    fail_locked(ec);
                                }
                            }
                        }
                        drain_wbio_locked();
                    }

                    if (state_ == state::established) {
                        if (plaintext_offset_ < plaintext_pending_.size()) {
                            const std::size_t count = std::min(
                                k_read_chunk,
                                plaintext_pending_.size() - plaintext_offset_);
                            std::memcpy(scratch_.data(),
                                        plaintext_pending_.data() + plaintext_offset_,
                                        count);
                            plaintext_length = count;
                            act = action::plaintext;
                        } else {
                            plaintext_pending_.clear();
                            plaintext_offset_ = 0;
                            if (eof_signaled_ && bio_pending(rbio_) == 0) {
                                // The transport reported EOF and the read BIO is
                                // empty: a clean EOF. OpenSSL 3.x/4.x reports an
                                // empty-BIO read as an unexpected EOF, so the
                                // signal is classified here rather than from
                                // SSL_get_error(); a record truncated mid-flight
                                // still fails inside SSL_read below.
                                state_ = state::closed;
                            } else if (!app_paused_) {
                                ERR_clear_error();
                                const int result =
                                    SSL_read(ssl_, scratch_.data(),
                                             to_int_size(scratch_.size()));
                                if (result > 0) {
                                    plaintext_length =
                                        static_cast<std::size_t>(result);
                                    act = action::plaintext;
                                } else {
                                    const int ssl_error = SSL_get_error(ssl_, result);
                                    if (ssl_error != SSL_ERROR_WANT_READ &&
                                        ssl_error != SSL_ERROR_WANT_WRITE) {
                                        const std::error_code ec =
                                            map_ssl_error(ssl_, ssl_error, false);
                                        if (ec ==
                                            make_error_code(tls_errc::ssl_closed)) {
                                            state_ = state::closed;
                                        } else {
                                            fail_locked(ec);
                                        }
                                    }
                                }
                                drain_wbio_locked();
                            }
                        }

                        if (state_ == state::established &&
                            write_completion_ != nullptr) {
                            try_write_locked(completed_write, completed_result);
                        }
                    }

                    if (state_ == state::failed) {
                        act = action::fail;
                        failure = error_;
                    } else if (state_ == state::closed) {
                        act = action::eof;
                    }
                }
            }

            if (completed_write != nullptr) {
                completed_write->complete(completed_result);
            }

            if (act == action::plaintext) {
                const consume outcome = deliver_plaintext(plaintext_length);
                if (outcome == consume::progress) {
                    continue;
                }
                if (outcome == consume::failed) {
                    continue;
                }
                break;
            }
            if (act == action::eof) {
                deliver_eof();
                break;
            }
            if (act == action::fail) {
                deliver_error(failure);
                break;
            }
            break;
        }
        pump_tx();
    }

    [[nodiscard]] consume deliver_plaintext(std::size_t length) noexcept {
        source* consumer = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            consumer = plaintext_;
        }

        std::size_t consumed = 0;
        if (consumer != nullptr) {
            consumed = consumer->on_data(
                span<const std::byte>(scratch_.data(), length));
            if (consumed > length) {
                consumed = length; // I9: never trust an over-claim
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (plaintext_offset_ < plaintext_pending_.size()) {
            plaintext_offset_ += consumed;
            if (plaintext_offset_ >= plaintext_pending_.size()) {
                plaintext_pending_.clear();
                plaintext_offset_ = 0;
                app_paused_ = false;
                return consume::progress;
            }
            // A full slice was consumed: keep draining in this drive pass.
            // Only a genuinely short/zero credit pauses the pending buffer.
            if (consumed < length) {
                app_paused_ = true;
                return consume::paused;
            }
            app_paused_ = false;
            return consume::progress;
        }

        if (consumed < length) {
            const std::size_t tail = length - consumed;
            if (plaintext_pending_.size() + tail > k_max_plaintext) {
                fail_locked(std::make_error_code(std::errc::no_buffer_space));
                return consume::failed;
            }
            try {
                plaintext_pending_.assign(scratch_.begin() + consumed,
                                          scratch_.begin() + length);
            } catch (const std::bad_alloc&) {
                fail_locked(std::make_error_code(std::errc::not_enough_memory));
                return consume::failed;
            }
            plaintext_offset_ = 0;
            app_paused_ = true;
            return consume::paused;
        }

        app_paused_ = false;
        return consume::progress;
    }

    void try_write_locked(completion*& completed, io_result& result) noexcept {
        if (write_completion_ == nullptr || state_ != state::established ||
            ssl_ == nullptr) {
            return;
        }
        if (write_length_ == 0) {
            completed = write_completion_;
            write_completion_ = nullptr;
            borrowed_plaintext_ = span<const std::byte>();
            write_buffer_base_ = 0;
            result = io_result{{}, 0};
            return;
        }

        if (encrypted_buffer_.size() >= k_max_encrypted) {
            // The transport is behind.  Leave the plaintext parked; the write
            // completion runs when the encrypted backlog drains.
            return;
        }
        const std::size_t pending = bio_pending(wbio_);
        if (pending != 0 && encrypted_buffer_.size() + pending >= k_max_encrypted) {
            return;
        }

        const std::size_t remaining = write_length_ - write_offset_;
        const std::size_t count = std::min(remaining, k_write_chunk);
        // The plaintext is either the caller's borrowed span (the common
        // synchronous case) or write_buffer_ once a parked write was
        // materialised. In the materialised case write_buffer_ holds the bytes
        // from write_buffer_base_ on, so the index into it is offset by the
        // base; write_offset_/write_length_ keep describing the full accepted
        // range so the completion still reports the original byte count.
        const bool borrowed = !borrowed_plaintext_.empty();
        const std::byte* const plaintext =
            borrowed ? borrowed_plaintext_.data() : write_buffer_.data();
        const std::size_t index =
            borrowed ? write_offset_ : write_offset_ - write_buffer_base_;
        ERR_clear_error();
        const int written = SSL_write(ssl_, plaintext + index,
                                      to_int_size(count));
        if (written == to_int_size(count)) {
            write_offset_ += count;
            if (write_offset_ == write_length_) {
                const std::size_t total = write_length_;
                completed = write_completion_;
                write_completion_ = nullptr;
                write_buffer_.clear();
                write_buffer_base_ = 0;
                borrowed_plaintext_ = span<const std::byte>();
                write_length_ = 0;
                write_offset_ = 0;
                result = io_result{{}, total};
            }
        } else if (written > 0) {
            // Partial-write mode is deliberately not enabled, so a positive
            // short count is a contract violation in OpenSSL.
            fail_locked(make_error_code(tls_errc::ssl_protocol_error));
        } else {
            const int ssl_error = SSL_get_error(ssl_, written);
            if (ssl_error != SSL_ERROR_WANT_READ &&
                ssl_error != SSL_ERROR_WANT_WRITE) {
                const std::error_code ec = map_ssl_error(ssl_, ssl_error, false);
                if (ec == make_error_code(tls_errc::ssl_closed)) {
                    state_ = state::closed;
                } else if (ec) {
                    fail_locked(ec);
                }
            }
        }
        drain_wbio_locked();
    }

    // Called at the end of push(), after drive(), while mutex_ is held. If the
    // write parked with the caller's borrowed plaintext still active, copy the
    // unwritten remainder into write_buffer_ so the borrowed span can be
    // released when push() returns. A fully written or failed write has already
    // cleared borrowed_plaintext_ in try_write_locked()/deliver_*().
    //
    // If the copy itself throws bad_alloc the write can no longer be completed
    // later, so `canceled`/`result` receive the parked completion and its
    // not_enough_memory result. The caller completes it after the lock is
    // released, preserving the "push completes exactly once" contract.
    void materialize_borrowed_locked(completion*& canceled,
                                     io_result& result) noexcept {
        if (borrowed_plaintext_.empty()) {
            return;
        }
        if (write_completion_ != nullptr) {
            try {
                write_buffer_.assign(borrowed_plaintext_.data() + write_offset_,
                                     borrowed_plaintext_.data() + write_length_);
                write_buffer_base_ = write_offset_;
            } catch (const std::bad_alloc&) {
                canceled = write_completion_;
                write_completion_ = nullptr;
                result =
                    io_result{std::make_error_code(std::errc::not_enough_memory),
                              0};
                fail_locked(std::make_error_code(std::errc::not_enough_memory));
            }
        }
        borrowed_plaintext_ = span<const std::byte>();
    }

    void post_handshake_locked(std::error_code& ec) noexcept {
        const tls_config& cfg = holder_->cfg;

        const unsigned char* selected = nullptr;
        unsigned int selected_length = 0;
        SSL_get0_alpn_selected(ssl_, &selected, &selected_length);
        if (selected != nullptr && selected_length != 0) {
            alpn_pointer_ = selected;
            alpn_length_ = selected_length;
        }

        if (cfg.require_alpn && !cfg.alpn_protocols.empty()) {
            if (alpn_pointer_ == nullptr || alpn_length_ == 0) {
                ec = make_error_code(tls_errc::ssl_alpn_mismatch);
                return;
            }
            bool offered = false;
            for (const std::string& protocol : cfg.alpn_protocols) {
                if (protocol.size() == alpn_length_ &&
                    std::memcmp(protocol.data(), alpn_pointer_, alpn_length_) == 0) {
                    offered = true;
                    break;
                }
            }
            if (!offered) {
                ec = make_error_code(tls_errc::ssl_alpn_mismatch);
                return;
            }
        }

        session_reused_ = SSL_session_reused(ssl_) != 0;
        if (cfg.role == tls_role::client && cfg.session_reuse) {
            cache_session_locked();
        }
    }

    void cache_session_locked() noexcept {
        if (ssl_ == nullptr || holder_->cfg.role != tls_role::client ||
            !holder_->cfg.session_reuse) {
            return;
        }
        SSL_SESSION* session = SSL_get1_session(ssl_);
        if (session == nullptr) {
            return;
        }
        try {
            context_->store_session(hostname_, session);
        } catch (...) {
            SSL_SESSION_free(session);
        }
    }

    void fail_locked(std::error_code ec) noexcept {
        if (state_ == state::failed || state_ == state::closed) {
            return;
        }
        state_ = state::failed;
        error_ = ec;
    }

    void deliver_eof() noexcept {
        source* consumer = nullptr;
        completion* canceled = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (source_terminal_) {
                return;
            }
            consumer = plaintext_;
            // With no consumer the terminal notification is retained so that a
            // source bound later still observes it.  Once a consumer exists it
            // runs at most once.
            if (consumer != nullptr) {
                source_terminal_ = true;
            }
            canceled = write_completion_;
            write_completion_ = nullptr;
            write_buffer_.clear();
            write_buffer_base_ = 0;
            borrowed_plaintext_ = span<const std::byte>();
            write_length_ = 0;
            write_offset_ = 0;
        }
        if (canceled != nullptr) {
            canceled->complete(
                io_result{std::make_error_code(std::errc::operation_canceled), 0});
        }
        if (consumer != nullptr) {
            consumer->on_eof();
        }
    }

    void deliver_error(std::error_code ec) noexcept {
        source* consumer = nullptr;
        completion* canceled = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (source_terminal_) {
                return;
            }
            consumer = plaintext_;
            if (consumer != nullptr) {
                source_terminal_ = true;
            }
            canceled = write_completion_;
            write_completion_ = nullptr;
            write_buffer_.clear();
            write_buffer_base_ = 0;
            borrowed_plaintext_ = span<const std::byte>();
            write_length_ = 0;
            write_offset_ = 0;
        }
        if (canceled != nullptr) {
            canceled->complete(io_result{ec, 0});
        }
        if (consumer != nullptr) {
            consumer->on_error(ec);
        }
    }

    void drain_wbio_locked() noexcept {
        if (ssl_ == nullptr || wbio_ == nullptr) {
            return;
        }
        try {
            for (;;) {
                const std::size_t pending = bio_pending(wbio_);
                if (pending == 0) {
                    break;
                }
                if (encrypted_buffer_.size() >= k_max_encrypted) {
                    break;
                }
                const std::size_t room = k_max_encrypted - encrypted_buffer_.size();
                const std::size_t count = std::min(
                    pending, std::min(room, static_cast<std::size_t>(
                                                (std::numeric_limits<int>::max)())));
                const std::size_t old_size = encrypted_buffer_.size();
                encrypted_buffer_.resize(old_size + count);
                const int read = BIO_read(wbio_,
                                          encrypted_buffer_.data() + old_size,
                                          to_int_size(count));
                if (read <= 0) {
                    encrypted_buffer_.resize(old_size);
                    break;
                }
                encrypted_buffer_.resize(old_size +
                                         static_cast<std::size_t>(read));
            }
        } catch (const std::bad_alloc&) {
            fail_locked(std::make_error_code(std::errc::not_enough_memory));
        }
    }

    void pump_tx() noexcept {
        bool expected = false;
        if (!tx_pumping_.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel)) {
            tx_again_.store(true, std::memory_order_release);
            return;
        }
        for (;;) {
            tx_block* block = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!tx_in_flight_) {
                    if (encrypted_buffer_.empty()) {
                        drain_wbio_locked();
                    }
                    if (!encrypted_buffer_.empty()) {
                        block = new (std::nothrow) tx_block();
                        if (block == nullptr) {
                            fail_locked(std::make_error_code(
                                std::errc::not_enough_memory));
                        } else {
                            block->owner = weak_from_this();
                            try {
                                block->bytes = std::move(encrypted_buffer_);
                                encrypted_buffer_.clear();
                                tx_in_flight_ = true;
                                tx_chunk_ = block->bytes.size();
                            } catch (...) {
                                delete block;
                                block = nullptr;
                                fail_locked(std::make_error_code(
                                    std::errc::not_enough_memory));
                            }
                        }
                    }
                }
            }

            if (block == nullptr) {
                break;
            }
            transport_.push(
                span<const std::byte>(block->bytes.data(), block->bytes.size()),
                block);
        }

        tx_pumping_.store(false, std::memory_order_release);
        if (tx_again_.exchange(false, std::memory_order_acq_rel)) {
            pump_tx();
            return;
        }
        // A failure detected by a synchronous transport completion must be
        // delivered after the transport's push stack has unwound.
        if (drive_after_pump_.exchange(false, std::memory_order_acq_rel)) {
            drive();
        }
    }

    void on_transport_complete(io_result result) noexcept {
        bool need_drive = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!tx_in_flight_) {
                return;
            }
            tx_in_flight_ = false;
            if (result.ec) {
                fail_locked(result.ec);
                need_drive = true;
            } else if (result.bytes != 0 && result.bytes != tx_chunk_) {
                // A successful sink completion must account for every byte.
                fail_locked(make_error_code(tls_errc::ssl_protocol_error));
                need_drive = true;
            }
        }

        if (need_drive) {
            if (tx_pumping_.load(std::memory_order_acquire)) {
                // Do not run user callbacks inside the transport's push stack;
                // pump_tx delivers after its loop has unwound.
                drive_after_pump_.store(true, std::memory_order_release);
            } else {
                drive();
            }
        }
        pump_tx();
    }

    io::sink& transport_;
    std::shared_ptr<tls_context::impl> context_;
    std::shared_ptr<const ssl_ctx_holder> holder_;
    std::string hostname_;

    mutable std::mutex mutex_;
    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr; // non-owning after SSL_set_bio
    BIO* wbio_ = nullptr; // non-owning after SSL_set_bio

    state state_ = state::idle;
    bool started_ = false;
    bool established_ever_ = false;
    bool session_reused_ = false;
    bool source_terminal_ = false;
    // Set by on_eof(): the transport reported that the peer closed its side.
    // A read that then finds the read BIO empty is a clean EOF, not a protocol
    // error (OpenSSL 3.x/4.x reports an empty-BIO read as an unexpected EOF).
    bool eof_signaled_ = false;
    std::error_code error_{};

    source* plaintext_ = nullptr;

    std::vector<std::byte> write_buffer_;
    // Non-empty only while a push() is borrowing the caller's span and has not
    // yet materialised it into write_buffer_; see push()/materialize_borrowed_locked().
    span<const std::byte> borrowed_plaintext_;
    // Offset of write_buffer_[0] within the accepted plaintext range once a
    // parked borrowed write has been materialised; 0 otherwise.
    std::size_t write_buffer_base_ = 0;
    std::size_t write_length_ = 0;
    std::size_t write_offset_ = 0;
    completion* write_completion_ = nullptr;

    std::array<std::byte, k_read_chunk> scratch_{};
    std::vector<std::byte> plaintext_pending_;
    std::size_t plaintext_offset_ = 0;
    bool app_paused_ = false;

    std::vector<std::byte> encrypted_buffer_;
    bool tx_in_flight_ = false;
    std::size_t tx_chunk_ = 0;

    std::atomic<bool> driving_{false};
    std::atomic<bool> drive_again_{false};
    std::atomic<bool> tx_pumping_{false};
    std::atomic<bool> tx_again_{false};
    std::atomic<bool> drive_after_pump_{false};

    const unsigned char* alpn_pointer_ = nullptr; // owned by SSL
    unsigned int alpn_length_ = 0;
};

// --- tls_context -------------------------------------------------------------

tls_context::tls_context(std::shared_ptr<impl> impl) noexcept
    : impl_(std::move(impl)) {}

tls_context::~tls_context() = default;

std::unique_ptr<tls_context> tls_context::create(const tls_config& config,
                                                 std::error_code& ec) noexcept {
    try {
        auto state = std::shared_ptr<impl>(new (std::nothrow) impl());
        if (!state) {
            ec = std::make_error_code(std::errc::not_enough_memory);
            return nullptr;
        }
        std::shared_ptr<ssl_ctx_holder> holder = build_holder(config, ec);
        if (!holder) {
            return nullptr;
        }
        state->set_session_limit(
            config.session_reuse ? config.session_cache_entries : 1);
        state->install(std::move(holder));
        auto context =
            std::unique_ptr<tls_context>(new (std::nothrow) tls_context(state));
        if (!context) {
            ec = std::make_error_code(std::errc::not_enough_memory);
            return nullptr;
        }
        ec.clear();
        return context;
    } catch (const std::bad_alloc&) {
        ec = std::make_error_code(std::errc::not_enough_memory);
    } catch (...) {
        ec = make_error_code(tls_errc::ssl_configuration_error);
    }
    return nullptr;
}

bool tls_context::reload(const tls_config& config, std::error_code& ec) noexcept {
    std::shared_ptr<ssl_ctx_holder> holder = build_holder(config, ec);
    if (!holder) {
        return false;
    }
    if (!config.session_reuse) {
        impl_->clear_sessions();
    }
    impl_->set_session_limit(
        config.session_reuse ? config.session_cache_entries : 1);
    impl_->install(std::move(holder));
    ec.clear();
    return true;
}

tls_config tls_context::config() const {
    const std::shared_ptr<const ssl_ctx_holder> holder = impl_->current();
    return holder != nullptr ? holder->cfg : tls_config{};
}

bool tls_context::valid() const noexcept {
    const std::shared_ptr<const ssl_ctx_holder> holder = impl_->current();
    return holder != nullptr && holder->ctx != nullptr;
}

bool tls_context::server_side() const noexcept {
    const std::shared_ptr<const ssl_ctx_holder> holder = impl_->current();
    return holder != nullptr && holder->cfg.role == tls_role::server;
}

// --- tls_stream --------------------------------------------------------------

tls_stream::tls_stream(std::shared_ptr<impl> impl) noexcept
    : impl_(std::move(impl)) {}

tls_stream::~tls_stream() = default;

std::unique_ptr<tls_stream> tls_stream::create(io::sink& transport,
                                               tls_context& context,
                                               std::string hostname,
                                               std::error_code& ec) noexcept {
    try {
        if (!context.impl_) {
            ec = make_error_code(tls_errc::ssl_configuration_error);
            return nullptr;
        }
        const std::shared_ptr<const ssl_ctx_holder> holder =
            context.impl_->current();
        if (!holder || holder->ctx == nullptr) {
            ec = make_error_code(tls_errc::ssl_configuration_error);
            return nullptr;
        }
        auto state = std::shared_ptr<impl>(
            new (std::nothrow) impl(transport, context.impl_, holder,
                                    std::move(hostname)));
        if (!state) {
            ec = std::make_error_code(std::errc::not_enough_memory);
            return nullptr;
        }
        ec = state->initialize();
        if (ec) {
            return nullptr;
        }
        auto stream =
            std::unique_ptr<tls_stream>(new (std::nothrow) tls_stream(state));
        if (!stream) {
            ec = std::make_error_code(std::errc::not_enough_memory);
            return nullptr;
        }
        state->start();
        if (state->handshake_failed()) {
            ec = state->error();
            return nullptr;
        }
        ec.clear();
        return stream;
    } catch (const std::bad_alloc&) {
        ec = std::make_error_code(std::errc::not_enough_memory);
    } catch (...) {
        ec = make_error_code(tls_errc::ssl_configuration_error);
    }
    return nullptr;
}

std::size_t tls_stream::on_data(span<const std::byte> data) noexcept {
    return impl_ != nullptr ? impl_->on_data(data) : 0;
}

void tls_stream::on_eof() noexcept {
    if (impl_ != nullptr) {
        impl_->on_eof();
    }
}

void tls_stream::on_error(std::error_code ec) noexcept {
    if (impl_ != nullptr) {
        impl_->on_error(ec);
    }
}

void tls_stream::push(span<const std::byte> data, completion* c) noexcept {
    if (impl_ != nullptr) {
        impl_->push(data, c);
    } else if (c != nullptr) {
        c->complete(io_result{std::make_error_code(std::errc::not_connected), 0});
    }
}

void tls_stream::bind_source(source* plaintext) noexcept {
    if (impl_ != nullptr) {
        impl_->bind_source(plaintext);
    }
}

void tls_stream::resume_read() noexcept {
    if (impl_ != nullptr) {
        impl_->resume_read();
    }
}

void tls_stream::close() noexcept {
    if (impl_ != nullptr) {
        impl_->close();
    }
}

bool tls_stream::handshake_complete() const noexcept {
    return impl_ != nullptr && impl_->handshake_complete();
}

bool tls_stream::handshake_failed() const noexcept {
    return impl_ != nullptr && impl_->handshake_failed();
}

bool tls_stream::session_reused() const noexcept {
    return impl_ != nullptr && impl_->session_reused();
}

std::string tls_stream::alpn_protocol() const {
    return impl_ != nullptr ? impl_->alpn_protocol() : std::string();
}

std::string tls_stream::server_name() const {
    return impl_ != nullptr ? impl_->server_name() : std::string();
}

std::error_code tls_stream::error() const noexcept {
    return impl_ != nullptr ? impl_->error() : make_error_code(tls_errc::ssl_configuration_error);
}

// --- Paired decorators -------------------------------------------------------

namespace {

using session_key = std::pair<const void*, std::string>;

class tls_source_adapter final : public source {
public:
    tls_source_adapter(std::shared_ptr<tls_stream> stream,
                       std::shared_ptr<tls_context::impl> owner,
                       session_key key)
        : stream_(std::move(stream)), owner_(std::move(owner)),
          key_(std::move(key)) {}

    ~tls_source_adapter() override {
        stream_.reset();
        owner_->erase_if_expired(key_);
    }

    std::size_t on_data(span<const std::byte> data) noexcept override {
        return stream_->on_data(data);
    }

    void on_eof() noexcept override { stream_->on_eof(); }
    void on_error(std::error_code ec) noexcept override { stream_->on_error(ec); }

    void bind_plaintext(source* plaintext) noexcept {
        stream_->bind_source(plaintext);
    }

    void resume() noexcept { stream_->resume_read(); }

private:
    std::shared_ptr<tls_stream> stream_;
    std::shared_ptr<tls_context::impl> owner_;
    session_key key_;
};

class tls_sink_adapter final : public sink {
public:
    tls_sink_adapter(std::shared_ptr<tls_stream> stream,
                     std::shared_ptr<tls_context::impl> owner,
                     session_key key)
        : stream_(std::move(stream)), owner_(std::move(owner)),
          key_(std::move(key)) {}

    ~tls_sink_adapter() override {
        stream_.reset();
        owner_->erase_if_expired(key_);
    }

    void push(span<const std::byte> data, completion* c) noexcept override {
        stream_->push(data, c);
    }

private:
    std::shared_ptr<tls_stream> stream_;
    std::shared_ptr<tls_context::impl> owner_;
    session_key key_;
};

} // namespace

struct tls_context_access {
    [[nodiscard]] static std::shared_ptr<tls_stream> acquire(
        io::sink& transport, tls_context& context, const std::string& hostname) {
        if (!context.impl_) {
            return nullptr;
        }
        using key_type = tls_context::impl::session_key;
        const key_type key(&transport, hostname);
        if (std::shared_ptr<tls_stream> existing = context.impl_->find(key)) {
            return existing;
        }
        std::error_code ec;
        std::unique_ptr<tls_stream> created =
            tls_stream::create(transport, context, hostname, ec);
        if (!created) {
            return nullptr;
        }
        std::shared_ptr<tls_stream> shared(std::move(created));
        return context.impl_->install_if_absent(key, shared);
    }
};

std::unique_ptr<source> make_tls_source(io::sink& transport,
                                        tls_context& context,
                                        std::string hostname) {
    std::shared_ptr<tls_stream> stream =
        tls_context_access::acquire(transport, context, hostname);
    if (!stream) {
        return nullptr;
    }
    const session_key key(&transport, hostname);
    auto* adapter = new (std::nothrow)
        tls_source_adapter(std::move(stream), context.impl_, key);
    return std::unique_ptr<source>(adapter);
}

std::unique_ptr<sink> make_tls_sink(io::sink& transport, tls_context& context,
                                    std::string hostname) {
    std::shared_ptr<tls_stream> stream =
        tls_context_access::acquire(transport, context, hostname);
    if (!stream) {
        return nullptr;
    }
    const session_key key(&transport, hostname);
    auto* adapter = new (std::nothrow)
        tls_sink_adapter(std::move(stream), context.impl_, key);
    return std::unique_ptr<sink>(adapter);
}

void bind_tls_source(source& tls_source, source* plaintext) noexcept {
    if (auto* adapter = dynamic_cast<tls_source_adapter*>(&tls_source)) {
        adapter->bind_plaintext(plaintext);
    }
}

void tls_resume_read(source& tls_source) noexcept {
    if (auto* adapter = dynamic_cast<tls_source_adapter*>(&tls_source)) {
        adapter->resume();
    }
}

#else // !CRPC_HAVE_OPENSSL

// --- No-OpenSSL stubs --------------------------------------------------------
//
// The public API is still linkable so that callers do not need their own
// #ifdef.  Every creator fails with ssl_not_available, which is exactly what a
// program should report if it asks for TLS in a build that did not enable the
// CMake option.

tls_context::tls_context(std::shared_ptr<impl>) noexcept {}
tls_context::~tls_context() = default;

std::unique_ptr<tls_context> tls_context::create(const tls_config&,
                                                 std::error_code& ec) noexcept {
    ec = make_error_code(tls_errc::ssl_not_available);
    return nullptr;
}

bool tls_context::reload(const tls_config&, std::error_code& ec) noexcept {
    ec = make_error_code(tls_errc::ssl_not_available);
    return false;
}

tls_config tls_context::config() const { return tls_config{}; }

bool tls_context::valid() const noexcept { return false; }

bool tls_context::server_side() const noexcept { return false; }

tls_stream::tls_stream(std::shared_ptr<impl>) noexcept {}
tls_stream::~tls_stream() = default;

std::unique_ptr<tls_stream> tls_stream::create(io::sink&, tls_context&,
                                               std::string,
                                               std::error_code& ec) noexcept {
    ec = make_error_code(tls_errc::ssl_not_available);
    return nullptr;
}

std::size_t tls_stream::on_data(span<const std::byte>) noexcept { return 0; }
void tls_stream::on_eof() noexcept {}
void tls_stream::on_error(std::error_code) noexcept {}

void tls_stream::push(span<const std::byte>, completion* c) noexcept {
    if (c != nullptr) {
        c->complete(io_result{make_error_code(tls_errc::ssl_not_available), 0});
    }
}

void tls_stream::bind_source(source*) noexcept {}
void tls_stream::resume_read() noexcept {}
void tls_stream::close() noexcept {}

bool tls_stream::handshake_complete() const noexcept { return false; }
bool tls_stream::handshake_failed() const noexcept { return false; }
bool tls_stream::session_reused() const noexcept { return false; }
std::string tls_stream::alpn_protocol() const { return std::string(); }
std::string tls_stream::server_name() const { return std::string(); }
std::error_code tls_stream::error() const noexcept {
    return make_error_code(tls_errc::ssl_not_available);
}

std::unique_ptr<source> make_tls_source(io::sink&, tls_context&, std::string) {
    return nullptr;
}

std::unique_ptr<sink> make_tls_sink(io::sink&, tls_context&, std::string) {
    return nullptr;
}

void bind_tls_source(source&, source*) noexcept {}
void tls_resume_read(source&) noexcept {}

#endif // CRPC_HAVE_OPENSSL

} // namespace crpc::io
