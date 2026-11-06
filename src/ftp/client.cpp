// CRPC -- FTP/FTPS client.  See include/crpc/ftp/client.hpp for the contract.
//
// The data path is deliberately explicit: `arm_transfer` performs REST and then
// PASV/EPSV (or EPRT/PORT), `transfer` drives exactly one flow, and every exit
// closes the data connection before the final completion reply is read.  That
// ordering (RFC 959 section 5.2) is what keeps the control stream in sync after
// a failed or refused transfer.
//
// TLS wiring
// ----------
// `io::tls_stream` is driven synchronously here: the socket is read in the
// calling thread and the ciphertext is handed to `tls_stream::on_data`, while
// `tls_stream::push` produces ciphertext through the small `socket_sink` adapter
// below.  There is no runtime, no callback thread and no hidden buffering, so
// every TLS failure is reported on the same call that caused it.
#include <crpc/ftp/client.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <crpc/fs/file.hpp>
#include <crpc/io/tls.hpp>
#include <crpc/net/connector.hpp>
#include <crpc/net/listener.hpp>

namespace crpc {
namespace ftp {
namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

[[nodiscard]] std::error_code to_error_code(rpc_code code) noexcept {
    return std::error_code(static_cast<int>(code), rpc_error_category());
}

// The socket as an `io::sink`: one synchronous write, one completion.  This is
// the only bridge `io::tls_stream` needs to run over `net::tcp_stream`.
class socket_sink final : public io::sink {
public:
    explicit socket_sink(net::tcp_stream& stream) noexcept : stream_(stream) {}

    void push(span<const std::byte> data, io::completion* c) noexcept override {
        const status written = stream_.write(data);
        if (c != nullptr) {
            if (written.ok()) {
                c->complete(io::io_result{{}, data.size()});
            } else {
                c->complete(io::io_result{to_error_code(written.code()), 0});
            }
        }
    }

private:
    net::tcp_stream& stream_;
};

// Turns PEM text into a file path for the TLS configuration and removes the
// file when the session ends.  An existing path is used verbatim.
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
        const std::string name = "crpc-ftp-" + std::string(tag) + "-" +
                                 std::to_string(stamp) + "-" +
                                 std::to_string(sequence.fetch_add(1)) + ".pem";
        const std::filesystem::path target = std::filesystem::current_path() / name;
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            return rpc_error{rpc_code::failed_precondition,
                             "could not materialise the PEM file"};
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

} // namespace

namespace detail {

// --- detail::transport -------------------------------------------------------

struct transport::impl {
    std::unique_ptr<net::tcp_stream> stream;
    std::unique_ptr<socket_sink> sink_adapter;
    std::unique_ptr<io::tls_stream> tls;
    bool secure = false;

    // Decrypted bytes produced by the TLS layer, and its terminal state.
    std::mutex mutex;
    std::string plaintext;
    std::size_t plaintext_offset = 0;
    bool eof = false;
    std::error_code error;

    struct plaintext_sink final : io::source {
        impl& owner;
        explicit plaintext_sink(impl& value) noexcept : owner(value) {}
        std::size_t on_data(span<const std::byte> data) noexcept override {
            std::lock_guard<std::mutex> lock(owner.mutex);
            owner.plaintext.append(reinterpret_cast<const char*>(data.data()),
                                   data.size());
            return data.size();
        }
        void on_eof() noexcept override {
            std::lock_guard<std::mutex> lock(owner.mutex);
            owner.eof = true;
        }
        void on_error(std::error_code ec) noexcept override {
            std::lock_guard<std::mutex> lock(owner.mutex);
            owner.error = ec;
        }
    };
    std::unique_ptr<plaintext_sink> plain;
};

transport::transport(std::unique_ptr<net::tcp_stream> stream) : impl_(new impl) {
    impl_->stream = std::move(stream);
}

transport::~transport() {
    close();
}

result<std::unique_ptr<transport>> transport::upgrade(
    std::unique_ptr<transport> plain, io::tls_context& context,
    std::string server_name, std::chrono::milliseconds handshake_timeout) {
    if (!plain || plain->impl_->tls != nullptr) {
        return rpc_error{rpc_code::invalid_argument,
                         "transport is already protected"};
    }
    if (!context.valid()) {
        return rpc_error{rpc_code::failed_precondition,
                         "the TLS context is not usable"};
    }

    impl& self = *plain->impl_;
    self.sink_adapter.reset(new socket_sink(*self.stream));
    self.plain.reset(new impl::plaintext_sink(self));

    std::error_code ec;
    self.tls = io::tls_stream::create(*self.sink_adapter, context,
                                      std::move(server_name), ec);
    if (!self.tls) {
        self.sink_adapter.reset();
        return rpc_error{rpc_code::failed_precondition,
                         "TLS setup failed: " + ec.message()};
    }
    self.tls->bind_source(self.plain.get());
    self.secure = true;

    // Drive the handshake by moving ciphertext from the socket into the TLS
    // layer until it settles.  A bounded wait rather than a read timeout: the
    // socket timeout applies per read, and a peer that trickles one byte at a
    // time must still not hold the caller forever.
    const auto deadline = std::chrono::steady_clock::now() + handshake_timeout;
    std::vector<std::byte> chunk(16 * 1024);
    while (!self.tls->handshake_complete()) {
        if (self.tls->handshake_failed()) {
            const std::error_code failure = self.tls->error();
            return rpc_error{rpc_code::failed_precondition,
                             "TLS handshake failed: " + failure.message()};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return rpc_error{rpc_code::deadline_exceeded,
                             "TLS handshake did not complete in time"};
        }
        auto got = self.stream->read_some(span<std::byte>(chunk.data(), chunk.size()));
        if (!got) {
            self.tls->on_eof();
            if (self.tls->handshake_complete()) {
                break;
            }
            return rpc_error{rpc_code::failed_precondition,
                             "connection closed during the TLS handshake"};
        }
        (void)self.tls->on_data(span<const std::byte>(chunk.data(), got.value()));
    }
    return plain;
}

result<std::vector<std::byte>> transport::read_exact(std::size_t count) {
    std::vector<std::byte> out;
    try {
        out.resize(count);
    } catch (...) {
        return rpc_error{rpc_code::resource_exhausted, "read buffer allocation failed"};
    }
    std::size_t got = 0;
    while (got < count) {
        auto part = read_some(span<std::byte>(out.data() + got, count - got));
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

result<std::size_t> transport::read_some(span<std::byte> buffer) {
    if (buffer.empty()) {
        return std::size_t{0};
    }
    impl& self = *impl_;
    if (self.tls == nullptr) {
        return self.stream->read_some(buffer);
    }
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(self.mutex);
            if (self.plaintext_offset < self.plaintext.size()) {
                const std::size_t available =
                    self.plaintext.size() - self.plaintext_offset;
                const std::size_t take = std::min(available, buffer.size());
                std::memcpy(buffer.data(),
                            self.plaintext.data() + self.plaintext_offset, take);
                self.plaintext_offset += take;
                if (self.plaintext_offset == self.plaintext.size()) {
                    self.plaintext.clear();
                    self.plaintext_offset = 0;
                }
                return take;
            }
            if (self.error) {
                const std::error_code ec = self.error;
                return rpc_error{net::classify_stream_error(ec.value()),
                                 "TLS transport error: " + ec.message()};
            }
            if (self.eof) {
                return rpc_error{rpc_code::eof, "peer closed the TLS session"};
            }
        }
        std::vector<std::byte> chunk(16 * 1024);
        auto got = self.stream->read_some(span<std::byte>(chunk.data(), chunk.size()));
        if (!got) {
            if (got.code() == rpc_code::eof) {
                self.tls->on_eof();
                continue; // drain decrypted bytes, then report eof
            }
            // A deadline is not terminal for the TLS session; hand it to the
            // caller without poisoning the TLS state.
            return got.error();
        }
        (void)self.tls->on_data(span<const std::byte>(chunk.data(), got.value()));
    }
}

status transport::write(span<const std::byte> data) {
    impl& self = *impl_;
    if (self.tls == nullptr) {
        return self.stream->write(data);
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

    self.tls->push(data, &completion);
    if (!completion.done) {
        // The only way a push parks here is a transport that refused to
        // complete synchronously; the socket sink never does.  One resume is
        // enough to let a retained tail drain, and a still-parked write is a
        // hard failure rather than a silent partial frame.
        self.tls->resume_read();
    }
    if (!completion.done) {
        return status{rpc_code::internal, "TLS write did not complete"};
    }
    if (completion.result.ec) {
        return status{net::classify_stream_error(completion.result.ec.value()),
                      "TLS write failed: " + completion.result.ec.message()};
    }
    if (completion.result.bytes != data.size()) {
        return status{rpc_code::broken_pipe, "TLS write was truncated"};
    }
    return status{};
}

void transport::close() noexcept {
    if (!impl_) {
        return;
    }
    if (impl_->tls != nullptr) {
        impl_->tls->close();
        impl_->tls.reset();
    }
    if (impl_->stream != nullptr) {
        impl_->stream->close();
    }
}

bool transport::is_secure() const noexcept {
    return impl_ != nullptr && impl_->secure;
}

net::address transport::local() const {
    return impl_ != nullptr && impl_->stream != nullptr ? impl_->stream->local()
                                                        : net::address();
}

net::address transport::peer() const {
    return impl_ != nullptr && impl_->stream != nullptr ? impl_->stream->peer()
                                                        : net::address();
}

// --- detail::line_reader -----------------------------------------------------

line_reader::line_reader(net::channel& channel, std::size_t max_line_bytes)
    : channel_(&channel), max_line_bytes_(max_line_bytes) {}

result<std::string> line_reader::read_line() {
    for (;;) {
        const std::size_t newline = buffer_.find('\n', offset_);
        if (newline != std::string::npos) {
            const std::size_t end = newline + 1;
            std::string line = buffer_.substr(offset_, end - offset_);
            offset_ = end;
            if (offset_ == buffer_.size()) {
                buffer_.clear();
                offset_ = 0;
            }
            return line;
        }
        if (buffer_.size() - offset_ > max_line_bytes_) {
            channel_->close();
            return rpc_error{rpc_code::protocol_error,
                             "control line exceeds max_line_bytes"};
        }
        std::vector<std::byte> chunk(8 * 1024);
        auto got = channel_->read_some(span<std::byte>(chunk.data(), chunk.size()));
        if (!got) {
            return got.error();
        }
        if (got.value() == 0) {
            continue;
        }
        buffer_.append(reinterpret_cast<const char*>(chunk.data()), got.value());
        if (buffer_.size() - offset_ > max_line_bytes_) {
            channel_->close();
            return rpc_error{rpc_code::protocol_error,
                             "control line exceeds max_line_bytes"};
        }
    }
}

void line_reader::rebind(net::channel& channel) {
    channel_ = &channel;
}

// `229 Entering Extended Passive Mode (|||port|)`: RFC 2429 fixes the delimiter
// to four '|' characters, so the port is the digits between the third and the
// fourth one.  Returns 0 for anything that is not that exact shape (including a
// literal port 0, which no caller can dial anyway) so the caller can fall back
// to PASV.
[[nodiscard]] int parse_epsv_port(std::string_view text) noexcept {
    const std::size_t open = text.find('(');
    const std::size_t close = text.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos ||
        close <= open) {
        return 0;
    }
    const std::string_view inside = text.substr(open + 1, close - open - 1);
    if (inside.size() < 4 || inside[0] != '|' || inside[1] != '|' ||
        inside[2] != '|' || inside.back() != '|') {
        return 0;
    }
    const std::string_view digits = inside.substr(3, inside.size() - 4);
    int port = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return 0;
        }
        port = port * 10 + (c - '0');
        if (port > 65535) {
            return 0;
        }
    }
    return port;
}

} // namespace detail

// --- client::impl ------------------------------------------------------------

struct client::impl {
    client_options options;
    std::unique_ptr<detail::transport> control;
    std::unique_ptr<detail::line_reader> reader;
    std::unique_ptr<io::tls_context> tls;
    pem_file ca_file;
    ftp_reply greeting;
    bool open = false;
    bool secure = false;
    bool data_secure = false;
    std::mutex mutex;

    // REST state: set by a transfer with a non-zero offset, consumed by the next
    // transfer command.
    bool restart_pending = false;
    std::uint64_t restart_offset = 0;

    // The one-shot data endpoints.  `passive_stream` is connected by PASV/EPSV;
    // `active_listener` is armed by EPRT/PORT and accepts exactly one peer.
    std::unique_ptr<net::tcp_stream> passive_stream;
    std::unique_ptr<net::listener> active_listener;

    [[nodiscard]] result<ftp_reply> read_reply() {
        std::string buffer;
        for (;;) {
            auto line = reader->read_line();
            if (!line) {
                return line.error();
            }
            buffer += line.value();
            if (buffer.size() > options.max_line_bytes * 64) {
                return rpc_error{rpc_code::protocol_error, "FTP reply is too long"};
            }
            reply_parse_result parsed = parse_reply(buffer);
            if (parsed.state == reply_state::incomplete) {
                continue;
            }
            if (parsed.state == reply_state::malformed) {
                return rpc_error{rpc_code::protocol_error,
                                 "malformed FTP reply: " + parsed.diagnostic};
            }
            return std::move(parsed.reply);
        }
    }

    [[nodiscard]] result<ftp_reply> send(std::string_view verb,
                                         std::string_view argument) {
        auto wire = encode_command(verb, argument);
        if (!wire) {
            return wire.error();
        }
        const status written = control->write(as_bytes(wire.value()));
        if (!written) {
            return written.error();
        }
        return read_reply();
    }

    [[nodiscard]] result<ftp_reply> send_raw(std::string_view line) {
        if (contains_injection(line)) {
            return rpc_error{rpc_code::invalid_argument,
                             "FTP command contains CR, LF or NUL"};
        }
        const std::string wire = std::string(line) + "\r\n";
        const status written = control->write(as_bytes(wire));
        if (!written) {
            return written.error();
        }
        return read_reply();
    }

    [[nodiscard]] static result<ftp_reply> expect_code(ftp_reply reply, int code,
                                                       const char* what) {
        if (reply.code == code) {
            return reply;
        }
        rpc_error error{rpc_code::application,
                        std::string(what) + ": server answered " +
                            std::to_string(reply.code) +
                            (reply.text.empty() ? "" : " " + reply.text)};
        error.metadata.set("ftp.code", std::to_string(reply.code));
        error.metadata.set("ftp.reply", reply.text);
        return error;
    }

    [[nodiscard]] static result<ftp_reply> expect_class(ftp_reply reply, int klass,
                                                        const char* what) {
        if (reply.code / 100 == klass) {
            return reply;
        }
        rpc_error error{rpc_code::application,
                        std::string(what) + ": server answered " +
                            std::to_string(reply.code) +
                            (reply.text.empty() ? "" : " " + reply.text)};
        error.metadata.set("ftp.code", std::to_string(reply.code));
        error.metadata.set("ftp.reply", reply.text);
        return error;
    }

    [[nodiscard]] result<ftp_reply> require(std::string_view verb,
                                            std::string_view argument, int code) {
        auto reply = send(verb, argument);
        if (!reply) {
            return reply.error();
        }
        if (reply.value().code != code) {
            return expect_code(std::move(reply).value(), code, verb.data());
        }
        return reply;
    }

    // --- client-side path screening -----------------------------------------

    // The client refuses the path shapes that only make sense to attack the
    // server's filesystem confinement before they are put on the wire: absolute
    // forms, drive/ADS colons, control bytes and `..` components.  The server
    // re-checks everything; this is defence in depth with better error codes.
    [[nodiscard]] static status validate_path(std::string_view path,
                                              bool allow_empty) {
        if (path.empty()) {
            if (allow_empty) {
                return status{};
            }
            return rpc_error{rpc_code::invalid_argument, "FTP path is empty"};
        }
        for (const char c : path) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 0x20 || uc == 0x7f) {
                return rpc_error{rpc_code::invalid_argument,
                                 "FTP path contains a control byte"};
            }
            if (c == ':') {
                return rpc_error{rpc_code::invalid_argument,
                                 "FTP path must not contain a colon"};
            }
        }
        if (path.front() == '/' || path.front() == '\\') {
            return rpc_error{rpc_code::invalid_argument,
                             "absolute FTP paths are not accepted"};
        }
        std::string_view rest = path;
        while (!rest.empty()) {
            const std::size_t cut = rest.find_first_of("/\\");
            const std::string_view part =
                cut == std::string_view::npos ? rest : rest.substr(0, cut);
            rest = cut == std::string_view::npos
                       ? std::string_view()
                       : rest.substr(cut + 1);
            if (part == "..") {
                return rpc_error{rpc_code::invalid_argument,
                                 "parent traversal is not accepted"};
            }
        }
        return status{};
    }

    // --- data endpoints -----------------------------------------------------

    // A TLS 1.3 server sends post-handshake session tickets right after the
    // data-connection handshake.  On an upload the client only writes, so those
    // records would still be unread when the client closes: Windows answers an
    // unread receive buffer with RST, the server loses the buffered
    // close_notify and its read fails with error 10053 mid-transfer.  The
    // client therefore drains the data connection before closing, and for
    // uploads the data stream carries a short read deadline so that drain ends
    // right after the tickets are consumed instead of waiting for a server FIN
    // that only comes after the client's own close.
    [[nodiscard]] std::chrono::nanoseconds data_read_deadline(bool upload) const
        noexcept {
        if (!upload) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                options.timeout);
        }
        const std::chrono::nanoseconds upload_cap{std::chrono::seconds(1)};
        const std::chrono::nanoseconds wanted =
            std::chrono::duration_cast<std::chrono::nanoseconds>(options.timeout);
        return wanted < upload_cap ? wanted : upload_cap;
    }

    [[nodiscard]] status arm_passive(bool upload) {
        auto epsv = send("EPSV", "");
        if (!epsv) {
            return epsv.error();
        }
        if (epsv.value().code == 229) {
            const int port = detail::parse_epsv_port(epsv.value().text);
            if (port > 0) {
                net::connect_options connect_options;
                connect_options.timeout =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        options.timeout);
                connect_options.stream.rw_timeout = data_read_deadline(upload);
                auto connected = net::connect(
                    net::address::tcp(control->peer().host(),
                                      static_cast<u16>(port)),
                    connect_options);
                if (!connected) {
                    return connected.error();
                }
                passive_stream = std::move(connected).value();
                return status{};
            }
            // A 229 with an unparseable port is a broken server: fall back to
            // PASV once rather than failing the transfer outright.
        }

        auto pasv = send("PASV", "");
        if (!pasv) {
            return pasv.error();
        }
        if (pasv.value().code != 227) {
            return expect_code(std::move(pasv).value(), 227, "PASV").error();
        }
        std::vector<int> parts;
        if (!parse_pasv_host_port(pasv.value().text, parts)) {
            return status{rpc_code::protocol_error, "malformed PASV reply"};
        }
        const std::string host =
            options.use_pasv_address
                ? std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." +
                      std::to_string(parts[2]) + "." + std::to_string(parts[3])
                : control->peer().host();
        const u16 port = static_cast<u16>(parts[4] * 256 + parts[5]);
        net::connect_options connect_options;
        connect_options.timeout =
            std::chrono::duration_cast<std::chrono::nanoseconds>(options.timeout);
        connect_options.stream.rw_timeout =
            std::chrono::duration_cast<std::chrono::nanoseconds>(options.timeout);
        auto connected = net::connect(net::address::tcp(host, port), connect_options);
        if (!connected) {
            return connected.error();
        }
        passive_stream = std::move(connected).value();
        return status{};
    }

    [[nodiscard]] static bool parse_pasv_host_port(const std::string& text,
                                                   std::vector<int>& parts) {
        const std::size_t open = text.find('(');
        const std::size_t close = open == std::string::npos
                                      ? std::string::npos
                                      : text.find(')', open);
        if (open == std::string::npos || close == std::string::npos || close <= open) {
            return false;
        }
        std::size_t pos = open + 1;
        while (pos <= close && parts.size() < 6) {
            const std::size_t comma = text.find(',', pos);
            const std::size_t end =
                comma == std::string::npos || comma > close ? close : comma;
            if (end == pos) {
                return false;
            }
            int value = 0;
            for (std::size_t i = pos; i < end; ++i) {
                if (text[i] < '0' || text[i] > '9') {
                    return false;
                }
                value = value * 10 + (text[i] - '0');
                if (value > 255) {
                    return false;
                }
            }
            parts.push_back(value);
            if (end == close) {
                break;
            }
            pos = end + 1;
        }
        return parts.size() == 6;
    }

    [[nodiscard]] static bool is_ipv4_literal(const std::string& host) noexcept {
        int dots = 0;
        bool digit_seen = false;
        for (const char c : host) {
            if (c == '.') {
                ++dots;
                digit_seen = false;
            } else if (c >= '0' && c <= '9') {
                digit_seen = true;
            } else {
                return false;
            }
        }
        return dots == 3 && digit_seen;
    }

    [[nodiscard]] status arm_active(bool upload) {
        net::listen_options listen;
        listen.stream.rw_timeout = data_read_deadline(upload);
        auto bound =
            net::listener::bind(net::address::tcp(control->local().host(), 0), listen);
        if (!bound) {
            return bound.error();
        }
        active_listener = std::move(bound).value();
        const u16 port = active_listener->local().port();
        const std::string host = active_listener->local().host();

        const std::string eprt =
            "|1|" + host + "|" + std::to_string(port) + "|";
        auto eprt_reply = send("EPRT", eprt);
        if (eprt_reply.ok() && eprt_reply.value().code / 100 == 2) {
            return status{};
        }
        if (!is_ipv4_literal(host)) {
            active_listener.reset();
            return status{rpc_code::failed_precondition,
                          "the server refused EPRT and the address is not IPv4"};
        }
        std::string comma;
        std::size_t pos = 0;
        int octets = 0;
        while (pos < host.size() && octets < 4) {
            const std::size_t dot = host.find('.', pos);
            const std::size_t end = dot == std::string::npos ? host.size() : dot;
            comma += host.substr(pos, end - pos);
            comma += ",";
            ++octets;
            if (dot == std::string::npos) {
                break;
            }
            pos = dot + 1;
        }
        if (octets != 4) {
            active_listener.reset();
            return status{rpc_code::failed_precondition,
                          "cannot build a PORT argument"};
        }
        comma += std::to_string(port / 256) + "," + std::to_string(port % 256);
        auto port_reply = send("PORT", comma);
        if (!port_reply) {
            active_listener.reset();
            return port_reply.error();
        }
        if (port_reply.value().code / 100 != 2) {
            const result<ftp_reply> failure =
                expect_class(std::move(port_reply).value(), 2, "PORT");
            active_listener.reset();
            return failure.error();
        }
        return status{};
    }

    [[nodiscard]] result<std::unique_ptr<detail::transport>> open_data() {
        if (options.passive) {
            if (passive_stream == nullptr) {
                return rpc_error{rpc_code::protocol_error,
                                 "PASV did not produce a data connection"};
            }
            std::unique_ptr<net::tcp_stream> stream = std::move(passive_stream);
            passive_stream.reset();
            return wrap_data(std::move(stream));
        }
        if (active_listener == nullptr) {
            return rpc_error{rpc_code::protocol_error,
                             "PORT/EPRT did not produce a data listener"};
        }
        auto accepted = active_listener->accept(
            std::chrono::duration_cast<std::chrono::nanoseconds>(options.timeout));
        active_listener.reset();
        if (!accepted) {
            return accepted.error();
        }
        return wrap_data(std::move(accepted).value());
    }

    [[nodiscard]] result<std::unique_ptr<detail::transport>> wrap_data(
        std::unique_ptr<net::tcp_stream> stream) {
        std::unique_ptr<detail::transport> plain(
            new detail::transport(std::move(stream)));
        if (!data_secure) {
            return plain;
        }
        if (tls == nullptr) {
            return rpc_error{rpc_code::failed_precondition,
                             "PROT P needs a TLS context"};
        }
        if (!options.passive) {
            // Active-mode FTPS would need the client to present a server
            // certificate on the data connection.  Refusing is honest; sending
            // it in the clear would not be.
            return rpc_error{rpc_code::failed_precondition,
                             "protected FTPS is only supported in passive mode"};
        }
        return detail::transport::upgrade(std::move(plain), *tls, options.host,
                                          options.timeout);
    }

    // REST is per-transfer state: it is cleared on every transfer attempt, so a
    // failed transfer cannot silently offset the next one.
    [[nodiscard]] status apply_restart() {
        if (!restart_pending) {
            return status{};
        }
        restart_pending = false;
        const std::uint64_t offset = restart_offset;
        restart_offset = 0;
        if (offset == 0) {
            return status{};
        }
        auto reply = send("REST", std::to_string(offset));
        if (!reply) {
            return reply.error();
        }
        if (reply.value().code != 350) {
            return expect_code(std::move(reply).value(), 350, "REST").error();
        }
        return status{};
    }

    [[nodiscard]] status arm_transfer(bool upload) {
        const status restarted = apply_restart();
        if (!restarted) {
            return restarted;
        }
        return options.passive ? arm_passive(upload) : arm_active(upload);
    }

    [[nodiscard]] result<std::string> drain(detail::transport& data) {
        std::string out;
        std::vector<std::byte> chunk(64 * 1024);
        for (;;) {
            auto got = data.read_some(span<std::byte>(chunk.data(), chunk.size()));
            if (!got) {
                if (got.code() == rpc_code::eof) {
                    break;
                }
                return got.error();
            }
            if (got.value() == 0) {
                continue;
            }
            out.append(reinterpret_cast<const char*>(chunk.data()), got.value());
        }
        return out;
    }

    [[nodiscard]] status pump(detail::transport& data, std::string_view payload) {
        if (payload.empty()) {
            return status{};
        }
        return data.write(as_bytes(payload));
    }

    template <class Fn>
    [[nodiscard]] result<ftp_reply> transfer(std::string_view verb,
                                             std::string_view argument, Fn&& body) {
        // A transfer that is refused before any bytes move leaves the REST
        // state consumed; that is intentional: the caller asked for this
        // transfer and must issue the offset again for a retry.
        const bool upload = verb == "STOR" || verb == "APPE";
        const status armed = arm_transfer(upload);
        if (!armed) {
            return armed.error();
        }
        auto preliminary = send(verb, argument);
        if (!preliminary) {
            if (passive_stream != nullptr) {
                passive_stream->close();
                passive_stream.reset();
            }
            active_listener.reset();
            return preliminary.error();
        }
        if (preliminary.value().code != 150 && preliminary.value().code != 125) {
            if (passive_stream != nullptr) {
                passive_stream->close();
                passive_stream.reset();
            }
            active_listener.reset();
            return preliminary;
        }
        auto data = open_data();
        if (!data) {
            (void)read_reply();
            return data.error();
        }
        std::unique_ptr<detail::transport> connection = std::move(data).value();
        const status body_status = std::forward<Fn>(body)(*connection);
        // Drain the data connection to the server's closing FIN before the
        // socket is closed.  Closing with unread bytes in the receive buffer
        // makes Windows abort the connection (RST), which the server sees as
        // error 10053 mid-transfer even though the upload was complete.  The
        // drain also consumes post-handshake TLS records such as a session
        // ticket after an upload, and is a no-op for downloads: their bodies
        // already read to EOF, so the first drain read returns eof.
        std::vector<std::byte> drain_chunk(4 * 1024);
        for (;;) {
            auto tail = connection->read_some(
                span<std::byte>(drain_chunk.data(), drain_chunk.size()));
            if (!tail) {
                break; // eof, deadline or reset: the completion reply decides
            }
            if (tail.value() == 0) {
                continue;
            }
        }
        connection->close();
        connection.reset();

        auto completion = read_reply();
        if (!completion) {
            return completion.error();
        }
        if (!body_status) {
            return body_status.error();
        }
        return completion;
    }

    [[nodiscard]] result<std::vector<std::string>> listing(std::string_view verb,
                                                           std::string_view path) {
        std::vector<std::string> lines;
        auto reply = transfer(verb, path, [&](detail::transport& data) {
            auto body = drain(data);
            if (!body) {
                return status{body.error()};
            }
            std::string_view text = body.value();
            while (!text.empty()) {
                const std::size_t newline = text.find('\n');
                std::string line = newline == std::string_view::npos
                                       ? std::string(text)
                                       : std::string(text.substr(0, newline));
                text = newline == std::string_view::npos ? std::string_view()
                                                         : text.substr(newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    lines.push_back(std::move(line));
                }
            }
            return status{};
        });
        if (!reply) {
            return reply.error();
        }
        if (reply.value().code / 100 != 2) {
            return expect_class(std::move(reply).value(), 2, verb.data()).error();
        }
        return lines;
    }

    void close_all() noexcept {
        if (passive_stream != nullptr) {
            passive_stream->close();
            passive_stream.reset();
        }
        if (active_listener != nullptr) {
            active_listener->close();
            active_listener.reset();
        }
        if (control != nullptr) {
            control->close();
        }
        open = false;
    }
};

// --- client ------------------------------------------------------------------

client::client(const client_options& options) : impl_(new impl) {
    impl_->options = options;
}

client::~client() {
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->close_all();
    }
}

result<std::unique_ptr<client>> client::connect(const client_options& options) {
    if (options.host.empty()) {
        return rpc_error{rpc_code::invalid_argument, "FTP host is empty"};
    }
    if (options.port == 0) {
        return rpc_error{rpc_code::invalid_argument, "FTP port is zero"};
    }

    std::unique_ptr<client> session(new client(options));
    impl& self = *session->impl_;

    net::connect_options connect_options;
    connect_options.timeout =
        std::chrono::duration_cast<std::chrono::nanoseconds>(options.timeout);
    connect_options.stream.rw_timeout =
        std::chrono::duration_cast<std::chrono::nanoseconds>(options.timeout);
    auto connected =
        net::connect(net::address::tcp(options.host, options.port), connect_options);
    if (!connected) {
        return connected.error();
    }
    self.control.reset(new detail::transport(std::move(connected).value()));

    if (options.use_tls || options.implicit_tls) {
#if !defined(CRPC_HAVE_OPENSSL)
        return rpc_error{rpc_code::failed_precondition,
                         "FTPS needs a build with CRPC_WITH_OPENSSL=ON"};
#else
        auto ca = pem_file::ensure(options.ca_bundle, "ca");
        if (!ca) {
            return ca.error();
        }
        self.ca_file = std::move(ca).value();

        io::tls_config config;
        config.role = io::tls_role::client;
        config.verify = options.verify_peer ? io::tls_verify_mode::peer
                                            : io::tls_verify_mode::none;
        config.ca_bundle = self.ca_file.path();
        // ALPN is an HTTP mechanism.  An FTP server that does not speak it must
        // not be rejected, so ALPN is disabled explicitly here.
        config.require_alpn = false;
        config.alpn_protocols.clear();

        std::error_code ec;
        self.tls = io::tls_context::create(config, ec);
        if (!self.tls) {
            return rpc_error{rpc_code::failed_precondition,
                             "could not create the TLS context: " + ec.message()};
        }
        if (options.implicit_tls) {
            auto upgraded = detail::transport::upgrade(
                std::move(self.control), *self.tls, options.host, options.timeout);
            if (!upgraded) {
                return upgraded.error();
            }
            self.control = std::move(upgraded).value();
            self.secure = true;
        }
#endif
    }

    self.reader.reset(new detail::line_reader(*self.control, options.max_line_bytes));

    auto greeting_reply = self.read_reply();
    if (!greeting_reply) {
        return greeting_reply.error();
    }
    if (greeting_reply.value().code != 220) {
        return impl::expect_code(std::move(greeting_reply).value(), 220,
                                 "FTP greeting")
            .error();
    }
    self.greeting = greeting_reply.value();
    self.open = true;

    if (options.use_tls && !options.implicit_tls) {
#if !defined(CRPC_HAVE_OPENSSL)
        return rpc_error{rpc_code::failed_precondition,
                         "FTPS needs a build with CRPC_WITH_OPENSSL=ON"};
#else
        auto auth = self.send_raw("AUTH TLS");
        if (!auth) {
            return auth.error();
        }
        if (auth.value().code != 234) {
            return impl::expect_code(std::move(auth).value(), 234, "AUTH TLS").error();
        }
        if (self.reader->buffered() != 0) {
            return rpc_error{rpc_code::protocol_error,
                             "server sent bytes before the TLS handshake"};
        }
        auto upgraded = detail::transport::upgrade(
            std::move(self.control), *self.tls, options.host, options.timeout);
        if (!upgraded) {
            return upgraded.error();
        }
        self.control = std::move(upgraded).value();
        self.secure = true;
        self.reader->rebind(*self.control);

        // RFC 4217: PBSZ 0 precedes PROT.  A server that refuses PROT P would
        // leave the data channel cleartext; this client treats that as a
        // failure rather than silently downgrading a protected session.
        auto pbsz = self.require("PBSZ", "0", 200);
        if (!pbsz) {
            return pbsz.error();
        }
        auto prot = self.require("PROT", "P", 200);
        if (!prot) {
            return prot.error();
        }
        self.data_secure = true;
#endif
    }

    if (options.auto_login) {
        auto logged_in = session->login(options.user, options.password);
        if (!logged_in) {
            return logged_in.error();
        }
    }
    return session;
}

result<ftp_reply> client::command(std::string_view cmd) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    return impl_->send_raw(cmd);
}

result<ftp_reply> client::login(std::string_view user, std::string_view password) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    auto user_reply = impl_->send("USER", user);
    if (!user_reply) {
        return user_reply.error();
    }
    // 331 = password required, 230 = logged in without a password.
    if (user_reply.value().code == 230) {
        return user_reply;
    }
    if (user_reply.value().code != 331) {
        return impl::expect_code(std::move(user_reply).value(), 331, "USER").error();
    }
    auto pass_reply = impl_->send("PASS", password);
    if (!pass_reply) {
        return pass_reply.error();
    }
    if (pass_reply.value().code != 230) {
        return impl::expect_code(std::move(pass_reply).value(), 230, "PASS").error();
    }
    return pass_reply;
}

result<std::string> client::pwd() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    auto reply = impl_->send("PWD", "");
    if (!reply) {
        return reply.error();
    }
    if (reply.value().code != 257) {
        return impl::expect_code(std::move(reply).value(), 257, "PWD").error();
    }
    // `257 "/path" is the current directory`.  A doubled quote inside the name is
    // one literal quote (RFC 959 section 6.3).
    const std::string& text = reply.value().text;
    const std::size_t first = text.find('"');
    if (first == std::string::npos) {
        return rpc_error{rpc_code::protocol_error, "PWD reply is not quoted"};
    }
    std::string path;
    for (std::size_t i = first + 1; i < text.size(); ++i) {
        if (text[i] == '"') {
            if (i + 1 < text.size() && text[i + 1] == '"') {
                path.push_back('"');
                ++i;
                continue;
            }
            return path;
        }
        path.push_back(text[i]);
    }
    return rpc_error{rpc_code::protocol_error, "PWD reply has no closing quote"};
}

result<ftp_reply> client::cwd(std::string_view path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(path, false); !valid) {
        return valid.error();
    }
    auto reply = impl_->send("CWD", path);
    if (!reply) {
        return reply;
    }
    if (reply.value().code / 100 != 2) {
        return impl::expect_class(std::move(reply).value(), 2, "CWD");
    }
    return reply;
}

result<ftp_reply> client::cdup() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    auto reply = impl_->send("CDUP", "");
    if (!reply) {
        return reply;
    }
    if (reply.value().code / 100 != 2) {
        return impl::expect_class(std::move(reply).value(), 2, "CDUP");
    }
    return reply;
}

result<ftp_reply> client::mkdir(std::string_view path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(path, false); !valid) {
        return valid.error();
    }
    auto reply = impl_->send("MKD", path);
    if (!reply) {
        return reply;
    }
    if (reply.value().code / 100 != 2) {
        return impl::expect_class(std::move(reply).value(), 2, "MKD");
    }
    return reply;
}

result<ftp_reply> client::rmdir(std::string_view path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(path, false); !valid) {
        return valid.error();
    }
    auto reply = impl_->send("RMD", path);
    if (!reply) {
        return reply;
    }
    if (reply.value().code / 100 != 2) {
        return impl::expect_class(std::move(reply).value(), 2, "RMD");
    }
    return reply;
}

result<ftp_reply> client::remove(std::string_view path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(path, false); !valid) {
        return valid.error();
    }
    auto reply = impl_->send("DELE", path);
    if (!reply) {
        return reply;
    }
    if (reply.value().code / 100 != 2) {
        return impl::expect_class(std::move(reply).value(), 2, "DELE");
    }
    return reply;
}

result<ftp_reply> client::rename(std::string_view from, std::string_view to) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(from, false); !valid) {
        return valid.error();
    }
    if (const status valid = impl::validate_path(to, false); !valid) {
        return valid.error();
    }
    auto first = impl_->send("RNFR", from);
    if (!first) {
        return first.error();
    }
    if (first.value().code != 350) {
        return impl::expect_code(std::move(first).value(), 350, "RNFR");
    }
    auto second = impl_->send("RNTO", to);
    if (!second) {
        return second;
    }
    if (second.value().code / 100 != 2) {
        return impl::expect_class(std::move(second).value(), 2, "RNTO");
    }
    return second;
}

result<std::uint64_t> client::size(std::string_view path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(path, false); !valid) {
        return valid.error();
    }
    auto reply = impl_->send("SIZE", path);
    if (!reply) {
        return reply.error();
    }
    if (reply.value().code != 213) {
        return impl::expect_code(std::move(reply).value(), 213, "SIZE").error();
    }
    const std::string& text = reply.value().text;
    if (text.empty()) {
        return rpc_error{rpc_code::protocol_error, "SIZE reply has no number"};
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return rpc_error{rpc_code::protocol_error, "SIZE reply is not numeric"};
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10) {
            return rpc_error{rpc_code::protocol_error, "SIZE reply overflows"};
        }
        value = value * 10 + digit;
    }
    return value;
}

result<std::string> client::mdtm(std::string_view path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(path, false); !valid) {
        return valid.error();
    }
    auto reply = impl_->send("MDTM", path);
    if (!reply) {
        return reply.error();
    }
    if (reply.value().code != 213) {
        return impl::expect_code(std::move(reply).value(), 213, "MDTM").error();
    }
    return reply.value().text;
}

result<std::vector<std::string>> client::list(std::string_view path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(path, true); !valid) {
        return valid.error();
    }
    return impl_->listing("LIST", path);
}

result<std::vector<std::string>> client::nlst(std::string_view path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(path, true); !valid) {
        return valid.error();
    }
    return impl_->listing("NLST", path);
}

result<ftp_reply> client::type_binary() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->require("TYPE", "I", 200);
}

result<ftp_reply> client::type_ascii() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->require("TYPE", "A", 200);
}

result<std::string> client::retr(std::string_view path, std::uint64_t offset) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(path, false); !valid) {
        return valid.error();
    }
    impl_->restart_pending = offset != 0;
    impl_->restart_offset = offset;

    std::string payload;
    auto reply = impl_->transfer("RETR", path, [&](detail::transport& data) {
        auto body = impl_->drain(data);
        if (!body) {
            return status{body.error()};
        }
        payload = std::move(body).value();
        return status{};
    });
    if (!reply) {
        return reply.error();
    }
    if (reply.value().code / 100 != 2) {
        return impl::expect_class(std::move(reply).value(), 2, "RETR").error();
    }
    return payload;
}

result<ftp_reply> client::stor(std::string_view path, std::string_view data) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(path, false); !valid) {
        return valid.error();
    }
    impl_->restart_pending = false;
    impl_->restart_offset = 0;
    auto reply = impl_->transfer("STOR", path, [&](detail::transport& connection) {
        return impl_->pump(connection, data);
    });
    if (!reply) {
        return reply;
    }
    if (reply.value().code / 100 != 2) {
        return impl::expect_class(std::move(reply).value(), 2, "STOR");
    }
    return reply;
}

result<ftp_reply> client::appe(std::string_view path, std::string_view data) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(path, false); !valid) {
        return valid.error();
    }
    impl_->restart_pending = false;
    impl_->restart_offset = 0;
    auto reply = impl_->transfer("APPE", path, [&](detail::transport& connection) {
        return impl_->pump(connection, data);
    });
    if (!reply) {
        return reply;
    }
    if (reply.value().code / 100 != 2) {
        return impl::expect_class(std::move(reply).value(), 2, "APPE");
    }
    return reply;
}

result<std::string> client::retr_to_file(std::string_view remote,
                                         const std::filesystem::path& local,
                                         std::uint64_t offset) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(remote, false); !valid) {
        return valid.error();
    }
    impl_->restart_pending = offset != 0;
    impl_->restart_offset = offset;

    std::ofstream out(local, offset == 0 ? (std::ios::binary | std::ios::trunc)
                                        : (std::ios::binary | std::ios::app));
    if (!out) {
        return rpc_error{rpc_code::permission_denied,
                         "cannot open the local file for writing: " + local.string()};
    }

    auto reply = impl_->transfer("RETR", remote, [&](detail::transport& data) {
        std::vector<std::byte> chunk(64 * 1024);
        for (;;) {
            auto got = data.read_some(span<std::byte>(chunk.data(), chunk.size()));
            if (!got) {
                if (got.code() == rpc_code::eof) {
                    return status{};
                }
                return status{got.error()};
            }
            if (got.value() == 0) {
                continue;
            }
            out.write(reinterpret_cast<const char*>(chunk.data()),
                      static_cast<std::streamsize>(got.value()));
            if (!out) {
                return status{rpc_code::resource_exhausted,
                              "local write failed: " + local.string()};
            }
        }
    });
    out.close();
    if (!reply) {
        return reply.error();
    }
    if (reply.value().code / 100 != 2) {
        return impl::expect_class(std::move(reply).value(), 2, "RETR").error();
    }
    return local.string();
}

result<ftp_reply> client::stor_from_file(std::string_view remote,
                                         const std::filesystem::path& local,
                                         bool append) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    if (const status valid = impl::validate_path(remote, false); !valid) {
        return valid.error();
    }
    std::ifstream in(local, std::ios::binary);
    if (!in) {
        return rpc_error{rpc_code::not_found,
                         "cannot open the local file for reading: " + local.string()};
    }
    impl_->restart_pending = false;
    impl_->restart_offset = 0;
    auto reply = impl_->transfer(append ? "APPE" : "STOR", remote,
                                 [&](detail::transport& data) {
        std::vector<std::byte> chunk(64 * 1024);
        for (;;) {
            in.read(reinterpret_cast<char*>(chunk.data()),
                    static_cast<std::streamsize>(chunk.size()));
            const std::streamsize got = in.gcount();
            if (got <= 0) {
                break;
            }
            const status written =
                data.write(span<const std::byte>(chunk.data(),
                                                 static_cast<std::size_t>(got)));
            if (!written) {
                return written;
            }
        }
        if (in.bad()) {
            return status{rpc_code::internal, "local read failed: " + local.string()};
        }
        return status{};
    });
    if (!reply) {
        return reply;
    }
    if (reply.value().code / 100 != 2) {
        return impl::expect_class(std::move(reply).value(), 2,
                                  append ? "APPE" : "STOR");
    }
    return reply;
}

result<ftp_reply> client::quit() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open) {
        return rpc_error{rpc_code::failed_precondition, "FTP session is closed"};
    }
    auto reply = impl_->send("QUIT", "");
    impl_->close_all();
    return reply;
}

bool client::is_open() const noexcept {
    return impl_ != nullptr && impl_->open;
}

bool client::is_secure() const noexcept {
    return impl_ != nullptr && impl_->secure;
}

bool client::data_is_secure() const noexcept {
    return impl_ != nullptr && impl_->data_secure;
}

const ftp_reply& client::greeting() const noexcept {
    return impl_->greeting;
}

const client_options& client::options() const noexcept {
    return impl_->options;
}

} // namespace ftp
} // namespace crpc
