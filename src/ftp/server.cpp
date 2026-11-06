// CRPC -- FTP/FTPS server.  See include/crpc/ftp/server.hpp for the contract.
//
// Concurrency shape
// -----------------
// One accept thread owns the listener.  Every accepted control connection is
// served by its own session thread; `active_sessions()` counts those threads.
// A session is strictly sequential: read one line, answer it, repeat.  That is
// the protocol's own model and it is what makes the data-connection pairing
// simple -- there is exactly one armed data endpoint at a time and no command
// can race another on the same session.
//
// Path confinement
// ----------------
// `resolve_path` is the only place a request path becomes a filesystem path.
// It rejects, in order: an empty request, a NUL or control byte, an absolute
// form (`/x`, `\x`, `C:x`, `\\host\share`), any `..` component, a component with
// a colon (drive or NTFS alternate data stream), a component whose trailing
// space or dot makes it a Windows alias, and then resolves the accepted relative
// path against the root with `weakly_canonical` and verifies the result is still
// inside the canonical root.  That last step is what catches a symlink or
// junction that leaves the tree without a single `..` in the request.
#include <crpc/ftp/server.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/ftp/command.hpp>
#include <crpc/ftp/reply.hpp>
#include <crpc/io/tls.hpp>
#include <crpc/net/channel.hpp>
#include <crpc/net/connector.hpp>
#include <crpc/net/listener.hpp>

namespace crpc {
namespace ftp {
namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

// The socket as an `io::sink` for the TLS stream: one synchronous write.
class socket_sink final : public io::sink {
public:
    explicit socket_sink(net::tcp_stream& stream) noexcept : stream_(stream) {}

    void push(span<const std::byte> data, io::completion* c) noexcept override {
        const status written = stream_.write(data);
        if (c != nullptr) {
            if (written.ok()) {
                c->complete(io::io_result{{}, data.size()});
            } else {
                c->complete(io::io_result{
                    std::error_code(static_cast<int>(written.code()),
                                    rpc_error_category()),
                    0});
            }
        }
    }

private:
    net::tcp_stream& stream_;
};

// A blocking byte pipe over one TCP stream, optionally wrapped in TLS.  The
// server-side twin of `detail::transport` in client.cpp: same TLS driving
// model, different role.
class session_io {
public:
    explicit session_io(std::unique_ptr<net::tcp_stream> stream)
        : stream_(std::move(stream)) {}

    ~session_io() { close(); }

    session_io(const session_io&) = delete;
    session_io& operator=(const session_io&) = delete;

    [[nodiscard]] net::tcp_stream& stream() const noexcept { return *stream_; }

    [[nodiscard]] bool is_secure() const noexcept { return tls_ != nullptr; }

    // Runs the server-side TLS handshake over the already-read first bytes.
    // `prefix` holds bytes already consumed from the socket (the ClientHello can
    // legitimately have been read while sniffing for an implicit-TLS session).
    [[nodiscard]] status start_tls(io::tls_context& context,
                                   std::string_view prefix) {
        if (tls_ != nullptr) {
            return status{rpc_code::failed_precondition, "session is already secure"};
        }
        sink_.reset(new socket_sink(*stream_));
        plain_.reset(new plaintext_sink(*this));
        std::error_code ec;
        tls_ = io::tls_stream::create(*sink_, context, std::string(), ec);
        if (!tls_) {
            return status{rpc_code::failed_precondition,
                          "TLS setup failed: " + ec.message()};
        }
        tls_->bind_source(plain_.get());
        if (!prefix.empty()) {
            (void)tls_->on_data(as_bytes(prefix));
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::vector<std::byte> chunk(16 * 1024);
        while (!tls_->handshake_complete()) {
            if (tls_->handshake_failed()) {
                return status{rpc_code::failed_precondition,
                              "TLS handshake failed: " + tls_->error().message()};
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return status{rpc_code::deadline_exceeded,
                              "TLS handshake did not complete in time"};
            }
            auto got = stream_->read_some(span<std::byte>(chunk.data(), chunk.size()));
            if (!got) {
                if (got.code() == rpc_code::deadline_exceeded) {
                    continue; // an idle handshake peer; the outer deadline wins
                }
                tls_->on_eof();
                return status{rpc_code::eof,
                              "connection closed during the TLS handshake"};
            }
            (void)tls_->on_data(span<const std::byte>(chunk.data(), got.value()));
        }
        return status{};
    }

    [[nodiscard]] result<std::size_t> read_some(span<std::byte> buffer) {
        if (tls_ == nullptr) {
            return stream_->read_some(buffer);
        }
        if (buffer.empty()) {
            return std::size_t{0};
        }
        for (;;) {
            if (plaintext_offset_ < plaintext_.size()) {
                const std::size_t available = plaintext_.size() - plaintext_offset_;
                const std::size_t take = std::min(available, buffer.size());
                std::memcpy(buffer.data(), plaintext_.data() + plaintext_offset_, take);
                plaintext_offset_ += take;
                if (plaintext_offset_ == plaintext_.size()) {
                    plaintext_.clear();
                    plaintext_offset_ = 0;
                }
                return take;
            }
            if (error_) {
                return rpc_error{rpc_code::protocol_error,
                                 "TLS transport error: " + error_.message()};
            }
            if (eof_) {
                return rpc_error{rpc_code::eof, "peer closed the TLS session"};
            }
            std::vector<std::byte> chunk(16 * 1024);
            auto got = stream_->read_some(span<std::byte>(chunk.data(), chunk.size()));
            if (!got) {
                if (got.code() == rpc_code::eof) {
                    tls_->on_eof();
                    continue; // drain any decrypted bytes, then report eof
                }
                return got.error(); // deadline or reset: leave TLS state intact
            }
            (void)tls_->on_data(span<const std::byte>(chunk.data(), got.value()));
        }
    }

    [[nodiscard]] status write(span<const std::byte> data) {
        if (tls_ == nullptr) {
            return stream_->write(data);
        }
        if (data.empty()) {
            return status{};
        }
        struct waiter final : io::completion {
            bool done = false;
            io::io_result result{};
            void complete(io::io_result value) noexcept override {
                result = value;
                done = true;
            }
        } completion;
        tls_->push(data, &completion);
        if (!completion.done) {
            tls_->resume_read();
        }
        if (!completion.done) {
            return status{rpc_code::internal, "TLS write did not complete"};
        }
        if (completion.result.ec) {
            return status{rpc_code::broken_pipe,
                          "TLS write failed: " + completion.result.ec.message()};
        }
        if (completion.result.bytes != data.size()) {
            return status{rpc_code::broken_pipe, "TLS write was truncated"};
        }
        return status{};
    }

    void close() noexcept {
        if (tls_ != nullptr) {
            tls_->close();
            tls_.reset();
        }
        if (stream_ != nullptr) {
            stream_->close();
        }
    }

private:
    struct plaintext_sink final : io::source {
        explicit plaintext_sink(session_io& owner) noexcept : owner(owner) {}
        std::size_t on_data(span<const std::byte> data) noexcept override {
            owner.plaintext_.append(reinterpret_cast<const char*>(data.data()),
                                    data.size());
            return data.size();
        }
        void on_eof() noexcept override { owner.eof_ = true; }
        void on_error(std::error_code ec) noexcept override { owner.error_ = ec; }
        session_io& owner;
    };

    std::unique_ptr<net::tcp_stream> stream_;
    std::unique_ptr<socket_sink> sink_;
    std::unique_ptr<io::tls_stream> tls_;
    std::unique_ptr<plaintext_sink> plain_;
    std::string plaintext_;
    std::size_t plaintext_offset_ = 0;
    bool eof_ = false;
    std::error_code error_;
};

// --- temporary PEM files (server credentials) --------------------------------

class pem_file {
public:
    pem_file() = default;
    ~pem_file() { remove(); }
    pem_file(const pem_file&) = delete;
    pem_file& operator=(const pem_file&) = delete;
    pem_file(pem_file&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }
    pem_file& operator=(pem_file&& other) noexcept {
        if (this != &other) {
            remove();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    [[nodiscard]] static result<pem_file> ensure(std::string_view material,
                                                 std::string_view tag) {
        pem_file holder;
        if (material.empty()) {
            return holder;
        }
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(material), ec)) {
            holder.path_.assign(material);
            return holder;
        }
        if (material.find("-----BEGIN") == std::string_view::npos) {
            holder.path_.assign(material);
            return holder;
        }
        static std::atomic<unsigned long long> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string name = "crpc-ftp-srv-" + std::string(tag) + "-" +
                                 std::to_string(stamp) + "-" +
                                 std::to_string(sequence.fetch_add(1)) + ".pem";
        const std::filesystem::path target = std::filesystem::current_path() / name;
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            return rpc_error{rpc_code::failed_precondition,
                             "could not materialise the server PEM"};
        }
        out.write(material.data(), static_cast<std::streamsize>(material.size()));
        out.close();
        holder.path_ = target.string();
        return holder;
    }

private:
    void remove() noexcept {
        if (path_.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(path_), ec);
        path_.clear();
    }

    std::string path_;
};

// --- path confinement --------------------------------------------------------

// The result of resolving one request path into the served tree.
struct resolved_path {
    // Canonical filesystem path inside the root.
    std::filesystem::path absolute;
    // The virtual path the client sees ("/a/b").
    std::string virtual_path;
    // True when the client asked for a directory (`CWD`, `LIST dir`, `NLST`).
    bool is_directory = false;
};

[[nodiscard]] bool has_control(std::string_view text) noexcept {
    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f) {
            return true;
        }
    }
    return false;
}

// Splits on '/' (and '\\', which a Windows client may send), dropping empty
// components so `a//b/` and `a/b` are the same request.
[[nodiscard]] std::vector<std::string> split_components(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = start;
        while (end < path.size() && path[end] != '/' && path[end] != '\\') {
            ++end;
        }
        if (end > start) {
            parts.emplace_back(path.substr(start, end - start));
        }
        if (end >= path.size()) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

// True when the request is syntactically an absolute path on any platform this
// server runs on.
[[nodiscard]] bool looks_absolute(std::string_view path) noexcept {
    if (path.empty()) {
        return false;
    }
    if (path.front() == '/' || path.front() == '\\') {
        return true;
    }
    // A Windows drive-qualified path: `C:`, `C:/`, `C:\`.
    if (path.size() >= 2 &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':') {
        return true;
    }
    return false;
}

// A component the Windows filesystem would alias to something else: a trailing
// dot or space is dropped by Win32, and CON/PRN/... name devices.  Refusing is
// simpler and stricter than normalising.
[[nodiscard]] bool is_alias_component(const std::string& part) {
    if (part.empty()) {
        return true;
    }
    if (part.back() == '.' || part.back() == ' ') {
        return true;
    }
    if (part.find(':') != std::string::npos) {
        return true; // drive letter or alternate data stream
    }
    static const char* const k_devices[] = {"CON", "PRN", "AUX", "NUL",
                                            "COM1", "COM2", "COM3", "COM4",
                                            "COM5", "COM6", "COM7", "COM8",
                                            "COM9", "LPT1", "LPT2", "LPT3",
                                            "LPT4", "LPT5", "LPT6", "LPT7",
                                            "LPT8", "LPT9"};
    std::string stem = part;
    const std::size_t dot = stem.find('.');
    if (dot != std::string::npos) {
        stem.erase(dot);
    }
    for (const char* device : k_devices) {
        if (stem.size() != std::strlen(device)) {
            continue;
        }
        bool match = true;
        for (std::size_t i = 0; i < stem.size(); ++i) {
            const char a = stem[i] >= 'a' && stem[i] <= 'z'
                               ? static_cast<char>(stem[i] - 'a' + 'A')
                               : stem[i];
            if (a != device[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

// Returns the canonical root or an error when it is not a directory.
[[nodiscard]] result<std::filesystem::path> canonical_root(const std::string& root) {
    std::error_code ec;
    const std::filesystem::path candidate(root);
    if (!std::filesystem::exists(candidate, ec) ||
        !std::filesystem::is_directory(candidate, ec)) {
        return rpc_error{rpc_code::invalid_argument,
                         "root_directory is not an existing directory: " + root};
    }
    std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
        return rpc_error{rpc_code::invalid_argument,
                         "root_directory cannot be canonicalised: " + root};
    }
    return canonical;
}

// True when `candidate` is `root` or lives below it.  Both are canonical, so a
// simple component-wise comparison is correct and case is folded on Windows by
// comparing the native strings' canonical spelling.
[[nodiscard]] bool is_inside(const std::filesystem::path& root,
                             const std::filesystem::path& candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end()) {
            return false;
        }
#if defined(CRPC_OS_WINDOWS)
        std::wstring a = root_it->wstring();
        std::wstring b = candidate_it->wstring();
        for (wchar_t& c : a) {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        for (wchar_t& c : b) {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        if (a != b) {
            return false;
        }
#else
        if (*root_it != *candidate_it) {
            return false;
        }
#endif
    }
    return true;
}

// Resolves `request` (the argument of one command) against the session's
// current virtual directory.  `cwd` is a virtual path such as "/a/b".
[[nodiscard]] result<resolved_path> resolve_path(const std::filesystem::path& root,
                                                 const std::string& cwd,
                                                 std::string_view request) {
    std::string text(request);
    if (text.empty()) {
        return rpc_error{rpc_code::invalid_argument, "empty path argument"};
    }
    if (has_control(text)) {
        return rpc_error{rpc_code::invalid_argument,
                         "path contains a control byte"};
    }
    if (looks_absolute(text)) {
        return rpc_error{rpc_code::permission_denied,
                         "absolute paths are not served"};
    }

    std::vector<std::string> parts = split_components(text);
    for (const std::string& part : parts) {
        if (part == ".") {
            return rpc_error{rpc_code::permission_denied,
                             "relative dot components are not served"};
        }
        if (part == "..") {
            return rpc_error{rpc_code::permission_denied,
                             "parent traversal is not served"};
        }
        if (is_alias_component(part)) {
            return rpc_error{rpc_code::permission_denied,
                             "path component is not a plain name"};
        }
    }

    // Walk the virtual path so a `CWD` between commands is honoured.  The cwd is
    // built from previously validated components, so it needs no re-check.
    std::vector<std::string> combined = split_components(cwd);
    for (const std::string& part : parts) {
        combined.push_back(part);
    }

    std::filesystem::path target = root;
    for (const std::string& part : combined) {
        target /= std::filesystem::path(part);
    }

    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(target, ec);
    if (ec) {
        return rpc_error{rpc_code::invalid_argument,
                         "path cannot be canonicalised: " + ec.message()};
    }
    if (!is_inside(root, canonical)) {
        return rpc_error{rpc_code::permission_denied,
                         "path resolves outside the served root"};
    }

    resolved_path resolved;
    resolved.absolute = std::move(canonical);
    resolved.virtual_path = "/";
    for (std::size_t i = 0; i < combined.size(); ++i) {
        if (i != 0) {
            resolved.virtual_path.push_back('/');
        }
        resolved.virtual_path += combined[i];
    }
    return resolved;
}

// --- reply helpers -----------------------------------------------------------

[[nodiscard]] status send_reply(session_io& io, int code, std::string text) {
    ftp_reply reply;
    reply.code = code;
    reply.text = std::move(text);
    auto encoded = encode_reply(reply);
    if (!encoded) {
        return status{encoded.error()};
    }
    return io.write(as_bytes(encoded.value()));
}

[[nodiscard]] status send_multiline(session_io& io, int code,
                                    std::vector<std::string> lines) {
    ftp_reply reply;
    reply.code = code;
    reply.is_multiline = true;
    reply.lines = std::move(lines);
    auto encoded = encode_reply(reply);
    if (!encoded) {
        return status{encoded.error()};
    }
    return io.write(as_bytes(encoded.value()));
}

// `YYYYMMDDHHMMSS` (UTC) for MDTM.
[[nodiscard]] std::string format_mdtm(std::filesystem::file_time_type time) {
    // file_time_type -> system_clock is the same clock on every supported
    // platform here; convert explicitly through a duration so a different
    // epoch would still produce a sane value rather than garbage.
    const auto system_now = std::chrono::system_clock::now();
    const auto file_now = std::filesystem::file_time_type::clock::now();
    const auto system_time =
        system_now + (time - file_now);
    const std::time_t seconds =
        std::chrono::system_clock::to_time_t(system_time);
    std::tm tm{};
#if defined(CRPC_OS_WINDOWS)
    (void)gmtime_s(&tm, &seconds);
#else
    (void)gmtime_r(&seconds, &tm);
#endif
    char text[32];
    std::snprintf(text, sizeof(text), "%04d%02d%02d%02d%02d%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(text);
}

// A `ls -l` line: enough for the client to tell a directory from a file and to
// see the size and name.  The format is the common Unix one (weeks/months are
// not localised because clients parse this).
[[nodiscard]] std::string format_list_line(const std::filesystem::path& path,
                                           const std::filesystem::directory_entry& entry,
                                           std::error_code& ec) {
    const bool directory = entry.is_directory(ec);
    std::uintmax_t size = 0;
    if (!directory) {
        size = entry.file_size(ec);
    }
    const auto time = entry.last_write_time(ec);
    std::string stamp = "Jan 01 00:00";
    if (!ec) {
        const auto system_now = std::chrono::system_clock::now();
        const auto file_now = std::filesystem::file_time_type::clock::now();
        const auto system_time = system_now + (time - file_now);
        const std::time_t seconds = std::chrono::system_clock::to_time_t(system_time);
        std::tm tm{};
#if defined(CRPC_OS_WINDOWS)
        (void)gmtime_s(&tm, &seconds);
#else
        (void)gmtime_r(&seconds, &tm);
#endif
        static const char* const k_months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%s %02d %02d:%02d",
                      k_months[tm.tm_mon % 12], tm.tm_mday, tm.tm_hour, tm.tm_min);
        stamp = buffer;
    }
    const std::string name = path.filename().string();
    char line[512];
    std::snprintf(line, sizeof(line), "%crw-r--r--    1 owner    group %12llu %s %s",
                  directory ? 'd' : '-',
                  static_cast<unsigned long long>(size), stamp.c_str(), name.c_str());
    return std::string(line);
}

} // namespace

// --- server::impl ------------------------------------------------------------

struct server::impl : std::enable_shared_from_this<server::impl> {
    server_options options;
    std::filesystem::path root;
    std::unique_ptr<net::listener> listener;
    std::unique_ptr<io::tls_context> tls;
    pem_file cert_file;
    pem_file key_file;

    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> sessions{0};
    std::thread accept_thread;
    mutable std::mutex mutex;
    std::condition_variable idle;
    std::vector<std::thread> workers;

    impl(const server_options& value, std::filesystem::path canonical)
        : options(value), root(std::move(canonical)) {}

    // ---------------------------------------------------------------- session

    struct session_state {
        std::string cwd = "/";
        bool authenticated = false;
        std::string pending_user;
        bool user_seen = false;
        bool secure = false;
        bool data_secure = false;
        bool pbsz_seen = false;
        bool rename_pending = false;
        std::string rename_from;
        std::uint64_t restart_offset = 0;
        bool restart_pending = false;
        std::string type = "A";
    };

    [[nodiscard]] bool needs_auth(const session_state& state) const noexcept {
        return options.require_auth && !state.authenticated;
    }

    void run_session(std::unique_ptr<session_io> io) {
        const std::shared_ptr<impl> self = shared_from_this();
        struct guard {
            impl& owner;
            ~guard() { owner.session_finished(); }
        } guard{*this};

        session_state state;
        state.secure = io->is_secure();

        // Implicit TLS: when the operator enabled it, a TLS record (0x16 0x03)
        // as the first byte selects a protected session without a greeting.
        // When the probe turns out to be cleartext, those bytes are the start of
        // the first command, so they are kept and prepended below.
        std::string pending_prefix;
        if (!state.secure && options.allow_implicit_tls && tls != nullptr) {
            std::byte probe[3];
            std::size_t got = 0;
            while (got < sizeof(probe)) {
                auto part = io->read_some(span<std::byte>(probe + got,
                                                          sizeof(probe) - got));
                if (!part) {
                    return;
                }
                got += part.value();
            }
            const bool tls_hello = probe[0] == std::byte{0x16} &&
                                   probe[1] == std::byte{0x03} &&
                                   (probe[2] == std::byte{0x01} ||
                                    probe[2] == std::byte{0x02} ||
                                    probe[2] == std::byte{0x03});
            if (tls_hello) {
                const status upgraded = io->start_tls(
                    *tls, std::string_view(reinterpret_cast<const char*>(probe),
                                           sizeof(probe)));
                if (!upgraded) {
                    return;
                }
                state.secure = true;
            } else {
                // The probe bytes are the start of the first cleartext command.
                pending_prefix.assign(reinterpret_cast<const char*>(probe),
                                      sizeof(probe));
            }
        }

        if (!send_reply(*io, 220, "CRPC FTP service ready")) {
            return;
        }

        // The armed passive listener and the per-session line buffer.  The
        // buffer survives a read deadline so an idle connection can be polled
        // for `stopping` without losing a partly received command.
        std::unique_ptr<net::listener> passive;
        u16 passive_port = 0;
        std::string carry = pending_prefix;

        for (;;) {
            if (stopping.load()) {
                (void)send_reply(*io, 421, "Service shutting down");
                return;
            }
            // Extract one complete line from `carry`, reading only when it does
            // not already hold one.
            std::string line;
            for (;;) {
                const std::size_t newline = carry.find('\n');
                if (newline != std::string::npos) {
                    line = carry.substr(0, newline + 1);
                    carry.erase(0, newline + 1);
                    break;
                }
                if (carry.size() > options.max_line_bytes) {
                    (void)send_reply(*io, 500, "Command line too long");
                    return;
                }
                std::vector<std::byte> chunk(4 * 1024);
                auto got = io->read_some(
                    span<std::byte>(chunk.data(), chunk.size()));
                if (!got) {
                    if (got.code() == rpc_code::deadline_exceeded) {
                        if (stopping.load()) {
                            (void)send_reply(*io, 421, "Service shutting down");
                            return;
                        }
                        continue;
                    }
                    return;
                }
                carry.append(reinterpret_cast<const char*>(chunk.data()),
                             got.value());
            }
            if (line.size() > options.max_line_bytes) {
                // A whole line can arrive in one chunk, so the length limit has
                // to be enforced on the extracted line as well.
                (void)send_reply(*io, 500, "Command line too long");
                return;
            }

            if (line.find('\0') != std::string::npos) {
                (void)send_reply(*io, 500, "Command contains a control byte");
                continue;
            }

            auto parsed = parse_command(line);
            if (!parsed) {
                (void)send_reply(*io, 500, "Syntax error, command unrecognized");
                continue;
            }
            ftp_command& command = parsed.value();
            const bool was_secure = state.secure;

            const status answered = dispatch(*io, state, passive, passive_port,
                                             command.verb, command.argument);
            if (!answered) {
                return;
            }
            if (state.secure && !was_secure) {
                // AUTH TLS just completed.  Any command the client pipelined
                // before the handshake is cleartext and must not be executed.
                if (!carry.empty()) {
                    (void)send_reply(*io, 500,
                                     "pipelined command before TLS handshake");
                    return;
                }
            }
            if (!io->stream().is_open()) {
                return;
            }
        }
    }

    // Dispatches one command.  Returns a failed status when the session must
    // end (write failure; QUIT closes the stream and the caller notices through
    // is_open()).
    [[nodiscard]] status dispatch(session_io& io, session_state& state,
                                  std::unique_ptr<net::listener>& passive,
                                  u16& passive_port, const std::string& verb,
                                  const std::string& argument) {

        // Commands allowed before authentication (RFC 959 + RFC 4217 + RFC 2389).
        if (verb == "USER") {
            if (argument.empty()) {
                return send_reply(io, 501, "USER needs a user name");
            }
            state.user_seen = true;
            state.pending_user = argument;
            if (!options.require_auth) {
                state.authenticated = true;
                return send_reply(io, 230, "Logged in");
            }
            return send_reply(io, 331, "Password required");
        }
        if (verb == "PASS") {
            if (!state.user_seen) {
                return send_reply(io, 503, "Log in with USER first");
            }
            if (state.authenticated) {
                return send_reply(io, 503, "Already logged in");
            }
            if (state.pending_user != options.user) {
                state.user_seen = false;
                return send_reply(io, 530, "Login incorrect");
            }
            if (!options.password.empty() && argument != options.password) {
                state.user_seen = false;
                return send_reply(io, 530, "Login incorrect");
            }
            state.authenticated = true;
            return send_reply(io, 230, "Logged in");
        }
        if (verb == "AUTH") {
            if (!options.enable_tls) {
                return send_reply(io, 534, "TLS is not available");
            }
            if (tls == nullptr) {
                return send_reply(io, 534, "TLS is not available");
            }
            std::string mechanism = argument;
            for (char& c : mechanism) {
                c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
            }
            if (mechanism != "TLS" && mechanism != "TLS-C") {
                return send_reply(io, 504, "Unsupported AUTH mechanism");
            }
            if (state.secure) {
                return send_reply(io, 503, "Already using TLS");
            }
            const status sent = send_reply(io, 234, "AUTH TLS OK");
            if (!sent) {
                return sent;
            }
            const status upgraded = io.start_tls(*tls, std::string_view());
            if (!upgraded) {
                return upgraded;
            }
            state.secure = true;
            return status{};
        }
        if (verb == "FEAT") {
            return send_multiline(io, 211,
                                  {"Features:", " UTF8", " MDTM", " SIZE",
                                   " REST STREAM", " EPSV", " PASV", " EPRT",
                                   " AUTH TLS", " PBSZ", " PROT", "End"});
        }
        if (verb == "NOOP") {
            return send_reply(io, 200, "NOOP ok");
        }
        if (verb == "QUIT") {
            (void)send_reply(io, 221, "Goodbye");
            io.close();
            return status{};
        }
        if (verb == "SYST") {
            return send_reply(io, 215, "UNIX Type: L8");
        }
        if (verb == "OPTS") {
            // `OPTS UTF8 ON` is the only option this server implements.
            std::string upper = argument;
            for (char& c : upper) {
                c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
            }
            if (upper == "UTF8 ON" || upper == "UTF8") {
                return send_reply(io, 200, "UTF8 enabled");
            }
            return send_reply(io, 501, "Unknown OPTS");
        }

        // RFC 4217: PBSZ/PROT belong to the transport protection handshake that
        // follows AUTH TLS, not to the user session, so they are allowed before
        // login.  They still require that AUTH TLS already succeeded.
        if (verb == "PBSZ") {
            if (!state.secure) {
                return send_reply(io, 503, "PBSZ needs AUTH TLS first");
            }
            state.pbsz_seen = true;
            return send_reply(io, 200, "PBSZ=0");
        }
        if (verb == "PROT") {
            if (!state.secure) {
                return send_reply(io, 503, "PROT needs AUTH TLS first");
            }
            if (!state.pbsz_seen) {
                return send_reply(io, 503, "PBSZ first");
            }
            std::string level = argument;
            for (char& c : level) {
                c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
            }
            if (level == "P") {
                state.data_secure = true;
                return send_reply(io, 200, "PROT P");
            }
            if (level == "C") {
                state.data_secure = false;
                return send_reply(io, 200, "PROT C");
            }
            return send_reply(io, 504, "Unsupported PROT level");
        }

        if (needs_auth(state)) {
            return send_reply(io, 530, "Please log in first");
        }

        if (verb == "PWD" || verb == "XPWD") {
            std::string quoted = "\"" + state.cwd + "\"";
            // A quote inside the name is doubled (RFC 959 section 6.3).
            return send_reply(io, 257, quoted + " is the current directory");
        }
        if (verb == "CWD" || verb == "XCWD") {
            if (argument.empty()) {
                return send_reply(io, 501, "CWD needs a path");
            }
            auto resolved = resolve_path(root, state.cwd, argument);
            if (!resolved) {
                return send_reply(io, 550, "Directory unavailable");
            }
            std::error_code ec;
            if (!std::filesystem::is_directory(resolved.value().absolute, ec)) {
                return send_reply(io, 550, "Directory unavailable");
            }
            state.cwd = resolved.value().virtual_path;
            return send_reply(io, 250, "Directory changed to " + state.cwd);
        }
        if (verb == "CDUP" || verb == "XCUP") {
            auto resolved = resolve_path(root, state.cwd, ".");
            (void)resolved;
            if (state.cwd == "/" || state.cwd.empty()) {
                return send_reply(io, 250, "Already at the root");
            }
            const std::size_t slash = state.cwd.find_last_of('/');
            state.cwd = slash == std::string::npos || slash == 0
                            ? std::string("/")
                            : state.cwd.substr(0, slash);
            return send_reply(io, 250, "Directory changed to " + state.cwd);
        }
        if (verb == "MKD" || verb == "XMKD") {
            if (options.read_only) {
                return send_reply(io, 550, "Server is read-only");
            }
            if (argument.empty()) {
                return send_reply(io, 501, "MKD needs a path");
            }
            auto resolved = resolve_path(root, state.cwd, argument);
            if (!resolved) {
                return send_reply(io, 550, "Directory cannot be created");
            }
            std::error_code ec;
            if (!std::filesystem::create_directory(resolved.value().absolute, ec) || ec) {
                return send_reply(io, 550, "Directory cannot be created");
            }
            return send_reply(io, 257, "\"" + resolved.value().virtual_path +
                                           "\" created");
        }
        if (verb == "RMD" || verb == "XRMD") {
            if (options.read_only) {
                return send_reply(io, 550, "Server is read-only");
            }
            if (argument.empty()) {
                return send_reply(io, 501, "RMD needs a path");
            }
            auto resolved = resolve_path(root, state.cwd, argument);
            if (!resolved) {
                return send_reply(io, 550, "Directory cannot be removed");
            }
            if (resolved.value().absolute == root) {
                return send_reply(io, 550, "The root cannot be removed");
            }
            std::error_code ec;
            if (!std::filesystem::remove(resolved.value().absolute, ec) || ec) {
                return send_reply(io, 550, "Directory cannot be removed");
            }
            return send_reply(io, 250, "Directory removed");
        }
        if (verb == "DELE") {
            if (options.read_only) {
                return send_reply(io, 550, "Server is read-only");
            }
            if (argument.empty()) {
                return send_reply(io, 501, "DELE needs a path");
            }
            auto resolved = resolve_path(root, state.cwd, argument);
            if (!resolved || resolved.value().absolute == root) {
                return send_reply(io, 550, "File cannot be removed");
            }
            std::error_code ec;
            const bool removed = std::filesystem::remove(resolved.value().absolute, ec);
            if (!removed || ec) {
                return send_reply(io, 550, "File cannot be removed");
            }
            return send_reply(io, 250, "File removed");
        }
        if (verb == "RNFR") {
            if (options.read_only) {
                return send_reply(io, 550, "Server is read-only");
            }
            if (argument.empty()) {
                return send_reply(io, 501, "RNFR needs a path");
            }
            auto resolved = resolve_path(root, state.cwd, argument);
            if (!resolved) {
                return send_reply(io, 550, "File unavailable");
            }
            std::error_code ec;
            if (!std::filesystem::exists(resolved.value().absolute, ec)) {
                return send_reply(io, 550, "File unavailable");
            }
            state.rename_pending = true;
            state.rename_from = resolved.value().absolute.string();
            return send_reply(io, 350, "Ready for RNTO");
        }
        if (verb == "RNTO") {
            if (options.read_only) {
                return send_reply(io, 550, "Server is read-only");
            }
            if (!state.rename_pending) {
                return send_reply(io, 503, "RNFR first");
            }
            state.rename_pending = false;
            if (argument.empty()) {
                return send_reply(io, 501, "RNTO needs a path");
            }
            auto resolved = resolve_path(root, state.cwd, argument);
            if (!resolved) {
                return send_reply(io, 550, "Rename target refused");
            }
            std::error_code ec;
            std::filesystem::rename(state.rename_from, resolved.value().absolute, ec);
            if (ec) {
                return send_reply(io, 550, "Rename failed");
            }
            return send_reply(io, 250, "Rename ok");
        }
        if (verb == "SIZE") {
            if (argument.empty()) {
                return send_reply(io, 501, "SIZE needs a path");
            }
            auto resolved = resolve_path(root, state.cwd, argument);
            if (!resolved) {
                return send_reply(io, 550, "File unavailable");
            }
            std::error_code ec;
            if (!std::filesystem::is_regular_file(resolved.value().absolute, ec)) {
                return send_reply(io, 550, "File unavailable");
            }
            const std::uintmax_t size =
                std::filesystem::file_size(resolved.value().absolute, ec);
            if (ec) {
                return send_reply(io, 550, "File unavailable");
            }
            return send_reply(io, 213, std::to_string(size));
        }
        if (verb == "MDTM") {
            if (argument.empty()) {
                return send_reply(io, 501, "MDTM needs a path");
            }
            auto resolved = resolve_path(root, state.cwd, argument);
            if (!resolved) {
                return send_reply(io, 550, "File unavailable");
            }
            std::error_code ec;
            if (!std::filesystem::exists(resolved.value().absolute, ec)) {
                return send_reply(io, 550, "File unavailable");
            }
            const auto time =
                std::filesystem::last_write_time(resolved.value().absolute, ec);
            if (ec) {
                return send_reply(io, 550, "File unavailable");
            }
            return send_reply(io, 213, format_mdtm(time));
        }
        if (verb == "TYPE") {
            std::string mode = argument;
            for (char& c : mode) {
                c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
            }
            if (mode.empty()) {
                return send_reply(io, 501, "TYPE needs a parameter");
            }
            if (mode != "I" && mode != "A" && mode != "L 8" && mode != "L8") {
                return send_reply(io, 504, "Unsupported TYPE");
            }
            state.type = mode;
            return send_reply(io, 200, "Type set to " + mode);
        }
        if (verb == "MODE") {
            if (argument == "S" || argument == "s") {
                return send_reply(io, 200, "Mode S");
            }
            return send_reply(io, 504, "Unsupported MODE");
        }
        if (verb == "STRU") {
            if (argument == "F" || argument == "f") {
                return send_reply(io, 200, "Structure F");
            }
            return send_reply(io, 504, "Unsupported STRU");
        }
        if (verb == "REST") {
            if (argument.empty()) {
                return send_reply(io, 501, "REST needs an offset");
            }
            std::uint64_t value = 0;
            for (const char c : argument) {
                if (c < '0' || c > '9') {
                    return send_reply(io, 501, "REST needs a decimal offset");
                }
                value = value * 10 + static_cast<std::uint64_t>(c - '0');
            }
            state.restart_pending = true;
            state.restart_offset = value;
            return send_reply(io, 350, "Restarting at " + std::to_string(value));
        }
        if (verb == "EPSV") {
            if (!argument.empty()) {
                return send_reply(io, 501, "EPSV protocol selection is unsupported");
            }
            auto bound = net::listener::bind(net::address::tcp(bind_host(io), 0));
            if (!bound) {
                return send_reply(io, 425, "Cannot open a passive port");
            }
            passive = std::move(bound).value();
            passive_port = passive->local().port();
            return send_reply(io, 229, "Entering Extended Passive Mode (|||" +
                                           std::to_string(passive_port) + "|)");
        }
        if (verb == "PASV") {
            auto bound = net::listener::bind(net::address::tcp(bind_host(io), 0));
            if (!bound) {
                return send_reply(io, 425, "Cannot open a passive port");
            }
            passive = std::move(bound).value();
            passive_port = passive->local().port();
            auto advertised = pasv_address(io);
            if (!advertised) {
                passive.reset();
                passive_port = 0;
                return send_reply(io, 425, "Cannot determine the passive address");
            }
            // PASV carries a dotted quad and a 16-bit port split in two
            // bytes.  A host that is not an IPv4 literal cannot be expressed
            // here at all; such a session must use EPSV.
            std::string tuple;
            int dots = 0;
            for (const char c : advertised.value()) {
                if (c == '.') {
                    ++dots;
                    tuple.push_back(',');
                } else if (c >= '0' && c <= '9') {
                    tuple.push_back(c);
                } else {
                    passive.reset();
                    passive_port = 0;
                    return send_reply(io, 522,
                                      "Use EPSV: the local address is not IPv4");
                }
            }
            if (dots != 3) {
                passive.reset();
                passive_port = 0;
                return send_reply(io, 522,
                                  "Use EPSV: the local address is not IPv4");
            }
            tuple += "," + std::to_string(passive_port / 256) + "," +
                     std::to_string(passive_port % 256);
            return send_reply(io, 227, "Entering Passive Mode (" + tuple + ")");
        }
        if (verb == "EPRT") {
            return send_reply(io, 501, "Active mode is not supported");
        }
        if (verb == "PORT") {
            return send_reply(io, 501, "Active mode is not supported");
        }
        if (verb == "LIST" || verb == "NLST") {
            return serve_listing(io, state, passive, passive_port, verb, argument);
        }
        if (verb == "RETR") {
            return serve_retr(io, state, passive, passive_port, argument);
        }
        if (verb == "STOR" || verb == "APPE") {
            return serve_stor(io, state, passive, passive_port, verb, argument);
        }

        return send_reply(io, 502, "Command not implemented");
    }

    [[nodiscard]] std::string bind_host(session_io& io) const {
        // Share the control connection's family: a client that connected over
        // an interface must be able to reach the passive port on it.  The
        // configured address is the fallback when the accepted stream has no
        // concrete local host.
        const std::string local = io.stream().local().host();
        if (!local.empty()) {
            return local;
        }
        return options.address.empty() ? std::string("127.0.0.1") : options.address;
    }

    // The IPv4 literal (or resolvable name) to advertise in PASV.
    [[nodiscard]] result<std::string> pasv_address(session_io& io) {
        if (!options.passive_address.empty()) {
            const std::string configured = options.passive_address.string();
            // Accept a bare host, `host:port` or a full `tcp://host:port` form;
            // only the host part is advertised.
            std::string host = configured;
            const std::size_t scheme = host.find("://");
            if (scheme != std::string::npos) {
                host.erase(0, scheme + 3);
            }
            const std::size_t colon = host.rfind(':');
            if (colon != std::string::npos && host.find(']') == std::string::npos) {
                host.erase(colon);
            }
            if (host.empty()) {
                return rpc_error{rpc_code::invalid_argument,
                                 "passive_address has no host"};
            }
            return host;
        }
        const std::string peer_host = io.stream().local().host();
        return peer_host.empty() ? std::string("127.0.0.1") : peer_host;
    }

    // Accepts the one data connection the transfer commands consume.  Active
    // mode is refused at the command layer, so this is always a passive accept.
    [[nodiscard]] result<std::unique_ptr<session_io>> accept_data(
        std::unique_ptr<net::listener>& passive, u16& passive_port,
        session_state& state) {
        if (passive == nullptr) {
            return rpc_error{rpc_code::failed_precondition,
                             "no passive listener is armed"};
        }
        auto accepted = passive->accept(
            std::chrono::duration_cast<std::chrono::nanoseconds>(options.io_timeout));
        passive.reset();
        passive_port = 0;
        if (!accepted) {
            return accepted.error();
        }
        std::unique_ptr<session_io> data(
            new session_io(std::move(accepted).value()));
        if (state.data_secure) {
            if (tls == nullptr) {
                return rpc_error{rpc_code::failed_precondition,
                                 "PROT P without a TLS context"};
            }
            const status upgraded = data->start_tls(*tls, std::string_view());
            if (!upgraded) {
                return upgraded.error();
            }
        }
        return data;
    }

    // Closing a data connection has no recoverable failure the caller can act
    // on, so this returns void deliberately.
    void flush_data(std::unique_ptr<session_io>& data) {
        if (data != nullptr) {
            data->close();
            data.reset();
        }
    }

    [[nodiscard]] status serve_listing(session_io& io, session_state& state,
                                       std::unique_ptr<net::listener>& passive,
                                       u16& passive_port, const std::string& verb,
                                       const std::string& argument) {
        std::filesystem::path target = root;
        if (!argument.empty()) {
            auto resolved = resolve_path(root, state.cwd, argument);
            if (!resolved) {
                passive.reset();
                passive_port = 0;
                return send_reply(io, 550, "Path unavailable");
            }
            target = resolved.value().absolute;
        } else {
            auto base = resolve_path(root, state.cwd, ".");
            if (!base) {
                // The cwd cannot be re-resolved (should be impossible): fall
                // back to the root.
                target = root;
            } else {
                target = base.value().absolute;
            }
        }

        std::error_code ec;
        std::vector<std::string> lines;
        if (std::filesystem::is_directory(target, ec)) {
            std::filesystem::directory_iterator iterator(target, ec);
            if (ec) {
                passive.reset();
                passive_port = 0;
                return send_reply(io, 550, "Path unavailable");
            }
            for (const std::filesystem::directory_entry& entry : iterator) {
                std::error_code line_ec;
                const std::string name = entry.path().filename().string();
                if (verb == "NLST") {
                    lines.push_back(name);
                } else {
                    lines.push_back(format_list_line(entry.path(), entry, line_ec));
                }
            }
            std::sort(lines.begin(), lines.end());
        } else if (std::filesystem::exists(target, ec)) {
            std::error_code line_ec;
            std::filesystem::directory_entry entry(target, line_ec);
            if (verb == "NLST") {
                lines.push_back(target.filename().string());
            } else {
                lines.push_back(format_list_line(target, entry, line_ec));
            }
        } else {
            passive.reset();
            passive_port = 0;
            return send_reply(io, 550, "Path unavailable");
        }

        if (passive == nullptr) {
            return send_reply(io, 425, "Use PASV or EPSV first");
        }
        const status preliminary = send_reply(io, 150, "Opening data connection");
        if (!preliminary) {
            return preliminary;
        }
        auto data = accept_data(passive, passive_port, state);
        if (!data) {
            return send_reply(io, 425, "Cannot open the data connection");
        }
        std::unique_ptr<session_io> connection = std::move(data).value();
        for (const std::string& line : lines) {
            const std::string wire = line + "\r\n";
            const status written = connection->write(as_bytes(wire));
            if (!written) {
                flush_data(connection);
                return send_reply(io, 426, "Transfer aborted");
            }
        }
        flush_data(connection);
        return send_reply(io, 226, "Transfer complete");
    }

    [[nodiscard]] status serve_retr(session_io& io, session_state& state,
                                    std::unique_ptr<net::listener>& passive,
                                    u16& passive_port, const std::string& argument) {
        if (argument.empty()) {
            return send_reply(io, 501, "RETR needs a path");
        }
        auto resolved = resolve_path(root, state.cwd, argument);
        if (!resolved) {
            passive.reset();
            passive_port = 0;
            return send_reply(io, 550, "File unavailable");
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(resolved.value().absolute, ec)) {
            passive.reset();
            passive_port = 0;
            return send_reply(io, 550, "File unavailable");
        }
        std::ifstream input(resolved.value().absolute, std::ios::binary);
        if (!input) {
            passive.reset();
            passive_port = 0;
            return send_reply(io, 550, "File unavailable");
        }

        // REST applies to this RETR only; it is consumed here whatever the
        // outcome, so a failed transfer can never offset the next one.
        const std::uint64_t offset = state.restart_pending ? state.restart_offset : 0;
        state.restart_pending = false;
        state.restart_offset = 0;
        if (offset != 0) {
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            if (!input) {
                passive.reset();
                passive_port = 0;
                return send_reply(io, 550, "Restart offset is beyond the file");
            }
        }

        if (passive == nullptr) {
            return send_reply(io, 425, "Use PASV or EPSV first");
        }
        const status preliminary =
            send_reply(io, 150, "Opening data connection for " + argument);
        if (!preliminary) {
            return preliminary;
        }
        auto data = accept_data(passive, passive_port, state);
        if (!data) {
            return send_reply(io, 425, "Cannot open the data connection");
        }
        std::unique_ptr<session_io> connection = std::move(data).value();

        std::vector<char> chunk(64 * 1024);
        for (;;) {
            input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const std::streamsize got = input.gcount();
            if (got <= 0) {
                break;
            }
            const status written = connection->write(
                span<const std::byte>(reinterpret_cast<const std::byte*>(chunk.data()),
                                      static_cast<std::size_t>(got)));
            if (!written) {
                flush_data(connection);
                return send_reply(io, 426, "Transfer aborted");
            }
        }
        if (input.bad()) {
            flush_data(connection);
            return send_reply(io, 451, "Local read error");
        }
        flush_data(connection);
        return send_reply(io, 226, "Transfer complete");
    }

    [[nodiscard]] status serve_stor(session_io& io, session_state& state,
                                    std::unique_ptr<net::listener>& passive,
                                    u16& passive_port, const std::string& verb,
                                    const std::string& argument) {
        if (options.read_only) {
            return send_reply(io, 550, "Server is read-only");
        }
        if (argument.empty()) {
            return send_reply(io, 501, verb + " needs a path");
        }
        auto resolved = resolve_path(root, state.cwd, argument);
        if (!resolved) {
            passive.reset();
            passive_port = 0;
            return send_reply(io, 550, "File unavailable");
        }
        // The parent must already exist: an FTP upload does not create
        // directories, and silently doing so would let a client build arbitrary
        // trees.
        std::error_code ec;
        if (!std::filesystem::is_directory(resolved.value().absolute.parent_path(), ec)) {
            passive.reset();
            passive_port = 0;
            return send_reply(io, 550, "Parent directory unavailable");
        }
        if (std::filesystem::is_directory(resolved.value().absolute, ec)) {
            passive.reset();
            passive_port = 0;
            return send_reply(io, 550, "Target is a directory");
        }

        // REST applies to STOR as an offset, exactly as to RETR.
        const std::uint64_t offset = state.restart_pending ? state.restart_offset : 0;
        state.restart_pending = false;
        state.restart_offset = 0;
        const bool append = verb == "APPE";

        std::ofstream output;
        if (append) {
            output.open(resolved.value().absolute, std::ios::binary | std::ios::app);
        } else if (offset != 0) {
            // A restart write keeps the existing prefix and continues at
            // `offset`; it must not truncate what the client is resuming.
            output.open(resolved.value().absolute, std::ios::binary | std::ios::in |
                                                        std::ios::out);
            if (output) {
                output.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
            }
        } else {
            output.open(resolved.value().absolute, std::ios::binary | std::ios::trunc);
        }
        if (!output) {
            passive.reset();
            passive_port = 0;
            return send_reply(io, 550, "File unavailable");
        }

        if (passive == nullptr) {
            return send_reply(io, 425, "Use PASV or EPSV first");
        }
        const status preliminary = send_reply(io, 150, "Opening data connection");
        if (!preliminary) {
            return preliminary;
        }
        auto data = accept_data(passive, passive_port, state);
        if (!data) {
            return send_reply(io, 425, "Cannot open the data connection");
        }
        std::unique_ptr<session_io> connection = std::move(data).value();

        std::vector<std::byte> chunk(64 * 1024);
        for (;;) {
            auto got = connection->read_some(span<std::byte>(chunk.data(), chunk.size()));
            if (!got) {
                if (got.code() == rpc_code::eof) {
                    break;
                }
                flush_data(connection);
                return send_reply(io, 426, "Transfer aborted");
            }
            if (got.value() == 0) {
                continue;
            }
            output.write(reinterpret_cast<const char*>(chunk.data()),
                         static_cast<std::streamsize>(got.value()));
            if (!output) {
                flush_data(connection);
                return send_reply(io, 451, "Local write error");
            }
        }
        output.close();
        flush_data(connection);
        return send_reply(io, 226, "Transfer complete");
    }

    // --------------------------------------------------------------- lifecycle

    void session_finished() noexcept {
        sessions.fetch_sub(1, std::memory_order_acq_rel);
        idle.notify_all();
    }

    void accept_loop() {
        const std::shared_ptr<impl> self = shared_from_this();
        while (!stopping.load()) {
            auto accepted = listener->accept(std::chrono::milliseconds(
                options.poll_interval.count()));
            if (!accepted) {
                if (accepted.code() == rpc_code::shutting_down) {
                    break;
                }
                if (accepted.code() == rpc_code::deadline_exceeded) {
                    continue;
                }
                // A transient accept failure must not spin; back off a little.
                std::this_thread::sleep_for(options.poll_interval);
                continue;
            }
            if (sessions.load() >= options.max_connections) {
                net::tcp_stream& stream = *accepted.value();
                (void)stream.write(as_bytes("421 Too many connections\r\n"));
                stream.close();
                continue;
            }
            sessions.fetch_add(1, std::memory_order_acq_rel);
            std::thread worker([self, stream = std::move(accepted).value()]() mutable {
                std::unique_ptr<session_io> io(new session_io(std::move(stream)));
                self->run_session(std::move(io));
            });
            std::lock_guard<std::mutex> lock(mutex);
            workers.push_back(std::move(worker));
        }
    }

    void stop_impl() noexcept {
        if (stopping.exchange(true)) {
            return;
        }
        if (listener != nullptr) {
            listener->close();
        }
        if (accept_thread.joinable() &&
            accept_thread.get_id() != std::this_thread::get_id()) {
            accept_thread.join();
        }
        std::vector<std::thread> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.swap(workers);
        }
        for (std::thread& worker : pending) {
            if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
                worker.join();
            }
        }
    }
};

// --- server ------------------------------------------------------------------

server::server(const server_options& options) : impl_(nullptr) {
    (void)options;
}

server::~server() {
    if (impl_) {
        stop();
    }
}

result<std::unique_ptr<server>> server::start(const server_options& options) {
    if (options.root_directory.empty()) {
        return rpc_error{rpc_code::invalid_argument, "root_directory is required"};
    }
    if (options.max_line_bytes < 16) {
        return rpc_error{rpc_code::invalid_argument, "max_line_bytes is too small"};
    }
    auto root = canonical_root(options.root_directory);
    if (!root) {
        return root.error();
    }
    if (options.enable_tls || options.allow_implicit_tls) {
#if !defined(CRPC_HAVE_OPENSSL)
        return rpc_error{rpc_code::failed_precondition,
                         "FTPS needs a build with CRPC_WITH_OPENSSL=ON"};
#else
        if (options.cert_chain.empty() || options.private_key.empty()) {
            return rpc_error{rpc_code::invalid_argument,
                             "TLS needs a certificate chain and a private key"};
        }
#endif
    }

    std::unique_ptr<server> instance(new server(options));
    auto state = std::shared_ptr<impl>(
        new impl(options, std::move(root).value()));
    instance->impl_ = state;

#if defined(CRPC_HAVE_OPENSSL)
    if (options.enable_tls || options.allow_implicit_tls) {
        auto cert = pem_file::ensure(options.cert_chain, "cert");
        if (!cert) {
            return cert.error();
        }
        auto key = pem_file::ensure(options.private_key, "key");
        if (!key) {
            return key.error();
        }
        state->cert_file = std::move(cert).value();
        state->key_file = std::move(key).value();

        io::tls_config config;
        config.role = io::tls_role::server;
        config.verify = io::tls_verify_mode::none;
        config.certificate_chain = state->cert_file.path();
        config.private_key = state->key_file.path();
        config.require_alpn = false;
        config.alpn_protocols.clear();
        std::error_code ec;
        state->tls = io::tls_context::create(config, ec);
        if (!state->tls) {
            return rpc_error{rpc_code::failed_precondition,
                             "could not create the server TLS context: " +
                                 ec.message()};
        }
    }
#endif

    auto bound = net::listener::bind(
        net::address::tcp(options.address, options.port));
    if (!bound) {
        return bound.error();
    }
    state->listener = std::move(bound).value();

    // Start the accept thread only after every resource is in place: a failure
    // above must leave no thread behind.
    state->accept_thread = std::thread([state]() { state->accept_loop(); });
    return instance;
}

void server::stop() {
    if (!impl_) {
        return;
    }
    impl_->stop_impl();
}

std::uint16_t server::port() const noexcept {
    if (!impl_ || impl_->listener == nullptr) {
        return 0;
    }
    return impl_->listener->local().port();
}

std::size_t server::active_sessions() const noexcept {
    return impl_ ? impl_->sessions.load(std::memory_order_acquire) : 0;
}

} // namespace ftp
} // namespace crpc
