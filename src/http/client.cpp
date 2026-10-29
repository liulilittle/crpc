// CRPC -- HTTP client implementation (HTTP/1.1 default, HTTP/2 by negotiation).
#include <crpc/http/client.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <system_error>
#include <utility>

#include <crpc/db/stream_channel.hpp>
#include <crpc/io/tls.hpp>

#include "h2_session.hpp"

namespace crpc {
namespace http {
namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// A CRLF-terminated line reader over the channel. Reading one byte at a time would be
// correct but slow; this reads in chunks and keeps the remainder, which is what the body
// reader then consumes.
class line_reader {
public:
    explicit line_reader(db::channel& link) noexcept : link_(&link) {}

    // Reads one line without its CRLF. `too_long` is set when the line exceeds `limit`
    // before a terminator was found, so a peer cannot send an endless line.
    [[nodiscard]] result<std::string> line(std::size_t limit) {
        std::string out;
        for (;;) {
            const std::size_t newline = buffer_.find('\n', position_);
            if (newline != std::string::npos) {
                std::size_t end = newline;
                if (end > position_ && buffer_[end - 1] == '\r') {
                    --end;
                }
                out.assign(buffer_, position_, end - position_);
                position_ = newline + 1;
                if (out.size() > limit) {
                    return rpc_error{rpc_code::resource_exhausted, "an HTTP line exceeds its limit"};
                }
                return out;
            }
            if (buffer_.size() - position_ > limit) {
                return rpc_error{rpc_code::resource_exhausted, "an HTTP line exceeds its limit"};
            }
            result<std::size_t> got = fill();
            if (!got) {
                return got.error();
            }
            if (got.value() == 0) {
                return rpc_error{rpc_code::eof, "the peer closed before the response was complete"};
            }
        }
    }

    // Reads exactly `count` bytes, including anything already buffered.
    [[nodiscard]] result<std::string> exact(std::size_t count) {
        std::string out;
        out.reserve(count);
        const std::size_t buffered = buffer_.size() - position_;
        const std::size_t take = std::min(buffered, count);
        out.append(buffer_, position_, take);
        position_ += take;
        while (out.size() < count) {
            result<std::size_t> got = fill();
            if (!got) {
                return got.error();
            }
            if (got.value() == 0) {
                return rpc_error{rpc_code::eof, "the peer closed before the body was complete"};
            }
            const std::size_t want = count - out.size();
            const std::size_t available = buffer_.size() - position_;
            const std::size_t chunk = std::min(want, available);
            out.append(buffer_, position_, chunk);
            position_ += chunk;
        }
        return out;
    }

    // Reads until the peer closes. Used when the response has neither a length nor chunking,
    // which HTTP/1.1 allows: the close *is* the delimiter.
    //
    // A close is therefore the success condition, not an error. Returning it as one is what
    // made a perfectly good `201 Created` look like a failed request.
    [[nodiscard]] result<std::string> rest(std::size_t limit) {
        std::string out;
        out.append(buffer_, position_, buffer_.size() - position_);
        position_ = buffer_.size();
        for (;;) {
            result<std::size_t> got = fill();
            if (!got) {
                if (got.error().code == rpc_code::eof) {
                    return out;
                }
                return got.error();
            }
            if (got.value() == 0) {
                return out;
            }
            out.append(buffer_, position_, buffer_.size() - position_);
            position_ = buffer_.size();
            if (out.size() > limit) {
                return rpc_error{rpc_code::resource_exhausted, "an HTTP body exceeds its limit"};
            }
        }
    }

    // Bytes read from the channel but not consumed by a message. On a
    // keep-alive connection this must be zero once a response is complete:
    // anything left belongs to no framed message and would pollute the next
    // response, which is exactly the classic keep-alive bug.
    [[nodiscard]] std::size_t buffered() const noexcept {
        return buffer_.size() - position_;
    }

private:
    // Moves more bytes into the buffer and returns how many arrived.
    [[nodiscard]] result<std::size_t> fill() {
        // Compact before reading so the buffer does not grow without bound on a long
        // response.
        if (position_ != 0) {
            buffer_.erase(0, position_);
            position_ = 0;
        }
        std::byte chunk[16 * 1024];
        result<std::size_t> got = link_->read_some(span<std::byte>(chunk, sizeof chunk));
        if (!got) {
            return got.error();
        }
        if (got.value() == 0) {
            return std::size_t{0};
        }
        buffer_.append(reinterpret_cast<const char*>(chunk), got.value());
        return got;
    }

    db::channel* link_;
    std::string buffer_;
    std::size_t position_ = 0;
};

[[nodiscard]] bool is_chunked(const header_list& headers) {
    const std::string* value = headers.find("transfer-encoding");
    if (value == nullptr) {
        return false;
    }
    return lower(*value).find("chunked") != std::string::npos;
}

// True when a comma-separated Connection value carries the token. HTTP
// regulates persistence through this one field: `close` retires the
// connection, `keep-alive` requests (or confirms) its reuse.
[[nodiscard]] bool connection_has_token(const header_list& headers,
                                        std::string_view token) {
    const std::string* value = headers.find("connection");
    if (value == nullptr) {
        return false;
    }
    std::size_t start = 0;
    const std::string text = lower(*value);
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        std::string item = text.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
            item.erase(item.begin());
        }
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
            item.pop_back();
        }
        if (item == token) {
            return true;
        }
        if (comma == std::string::npos) {
            return false;
        }
        start = comma + 1;
    }
    return false;
}

// Whether the response retires the connection. `Connection: close` always
// does. An HTTP/1.0 reply is a one-shot protocol: it keeps the connection
// alive only when it says `keep-alive` explicitly, which is exactly the
// reverse of HTTP/1.1, where persistence is the default and `close` is the
// opt-out.
[[nodiscard]] bool response_retires_connection(const response& out) {
    if (connection_has_token(out.headers, "close")) {
        return true;
    }
    if (out.version == http_version::http_1_0) {
        return !connection_has_token(out.headers, "keep-alive");
    }
    return false;
}

// Reports whether the header was present, separately from its value: `Content-Length: 0` means
// an empty body, while a missing header means the body runs to end of connection. Treating the
// two the same makes the client block waiting for a close that never comes -- which is exactly
// how this hung against the HBase gateway.
struct length_field {
    bool present = false;
    std::size_t value = 0;
};

[[nodiscard]] result<length_field> content_length(const header_list& headers) {
    const std::string* value = headers.find("content-length");
    if (value == nullptr) {
        return length_field{};
    }
    if (value->empty()) {
        return rpc_error{rpc_code::protocol_error, "an empty Content-Length"};
    }
    std::size_t length = 0;
    for (const char c : *value) {
        if (c < '0' || c > '9') {
            return rpc_error{rpc_code::protocol_error, "a non-numeric Content-Length"};
        }
        const std::size_t digit = static_cast<std::size_t>(c - '0');
        if (length > (SIZE_MAX - digit) / 10) {
            return rpc_error{rpc_code::protocol_error, "a Content-Length that overflows"};
        }
        length = length * 10 + digit;
    }
    return length_field{true, length};
}

} // namespace

struct client::impl {
    client_options options;
    std::unique_ptr<db::channel> link;
    rpc_error setup_error;
    // One-shot bookkeeping for `keep_alive == false`: a client built that way
    // sends `Connection: close`, so a second `send` would write into a socket
    // the server has retired. With keep-alive on, the flag never gates.
    bool used = false;
    // Set when a response left bytes on the connection beyond its framing:
    // the response was well-formed and is delivered, but the connection can
    // never be trusted for another response, so every later `send` fails with
    // this stored reason instead of parsing the leftover bytes.
    rpc_error poisoned;
    // Set by make_client when the link is the TLS stream channel: the
    // negotiated ALPN decides the protocol, and the result is only known
    // after the handshake, so the decision is taken at the first send().
    db::stream_channel* tls_link = nullptr;
    bool negotiated = false;
    bool negotiated_h2 = false;
    // Created on demand for an h2 connection (negotiated or h2c prior
    // knowledge).
    std::unique_ptr<detail::h2_session> h2;

    // A connection that failed mid-exchange is never handed out again: some
    // of the request or response may be on the wire, and the framing position
    // is unknowable. Closing here is what keeps the pool from ever returning
    // such a connection to its idle set.
    void retire_link() noexcept {
        if (link) {
            link->close();
        }
    }
};

client::client(client_options options, std::unique_ptr<db::channel> link)
    : impl_(new impl()) {
    impl_->options = std::move(options);
    impl_->link = std::move(link);
}

client::client(client_options options, std::unique_ptr<db::channel> link,
               rpc_error setup_error)
    : impl_(new impl()) {
    impl_->options = std::move(options);
    impl_->link = std::move(link);
    impl_->setup_error = std::move(setup_error);
}

client::~client() { close(); }

void client::close() noexcept {
    if (impl_ && impl_->link) {
        impl_->link->close();
    }
}

bool client::is_open() const noexcept {
    // A failed TLS setup has no link and is never open. A poisoned connection
    // carries bytes beyond its framing and is never open. With keep-alive on,
    // "open" means the channel still reports a usable link -- which is what a
    // pool consults before a borrow; with keep-alive off the client is
    // one-shot, so it stops being open once its request has been sent.
    if (impl_ == nullptr || impl_->link == nullptr || !impl_->setup_error.ok()) {
        return false;
    }
    if (!impl_->poisoned.ok()) {
        return false;
    }
    if (!impl_->options.keep_alive && impl_->used) {
        return false;
    }
    return impl_->link->is_open();
}

result<response> client::send(method verb, std::string_view target, span<const std::byte> body,
                              span<const header> extra_headers) {
    if (impl_ && !impl_->setup_error.ok()) {
        // HTTPS requested without the TLS backend, an unusable CA file, or no
        // host name for SNI: report the stored reason rather than pretending a
        // connection exists.
        return impl_->setup_error;
    }
    if (!impl_ || !impl_->link) {
        return rpc_error{rpc_code::shutting_down, "the HTTP client has no link"};
    }
    if (!impl_->options.keep_alive && impl_->used) {
        // One request per connection: the request asked for `Connection: close`, so a
        // second request would be written into a socket the server has already closed.
        return rpc_error{rpc_code::failed_precondition,
                         "this HTTP client has already sent its request"};
    }
    if (!impl_->poisoned.ok()) {
        // A previous response left bytes on the wire beyond its framing. The
        // leftover bytes are not part of any message this client sent, so
        // they must never be parsed as one: refusing here is the guarantee
        // that a broken server cannot make the next response out of garbage.
        return impl_->poisoned;
    }
    if (!impl_->options.keep_alive) {
        impl_->used = true;
    }

    // TLS negotiation: the handshake settles on backend callbacks, so wait for
    // it here (bounded by the request budget) and read the ALPN result. An
    // h2-capable server selects "h2"; anything else -- http/1.1, or no ALPN
    // extension at all -- keeps this client on HTTP/1.1. Cleartext connections
    // have no negotiation: h2c prior knowledge is an explicit option.
    if (impl_->tls_link != nullptr && !impl_->negotiated) {
        const std::chrono::milliseconds budget =
            impl_->options.timeout_seconds == 0
                ? std::chrono::milliseconds(0)
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::seconds(impl_->options.timeout_seconds));
        const status handshook = impl_->tls_link->wait_handshake(budget);
        if (!handshook.ok()) {
            return handshook.error();
        }
        impl_->negotiated = true;
        impl_->negotiated_h2 = impl_->tls_link->tls_alpn_protocol() == "h2";
    }
    const bool use_h2 =
        impl_->tls_link != nullptr ? impl_->negotiated_h2 : impl_->options.h2c;
    if (!impl_->link->is_open()) {
        // The connection was retired (by a `Connection: close` response, a
        // close-delimited body, the peer, a failed handshake, or
        // `client::close()`), and this client has no way to dial a
        // replacement -- that is what `client_pool` is for. The check sits
        // after the handshake on purpose: a handshake failure must surface as
        // its TLS error, not as a generic precondition.
        return rpc_error{rpc_code::failed_precondition,
                         "the HTTP connection is closed; open a new client or "
                         "borrow one from a client_pool"};
    }

    if (use_h2) {
        if (!impl_->h2) {
            detail::h2_session_config config;
            config.scheme = impl_->options.tls ? "https" : "http";
            config.authority = impl_->options.host;
            const std::uint16_t default_port = impl_->options.tls ? 443 : 80;
            if (impl_->options.port != default_port) {
                config.authority += ':';
                config.authority += std::to_string(impl_->options.port);
            }
            config.default_headers = impl_->options.default_headers;
            config.max_header_bytes = impl_->options.max_header_bytes;
            config.max_body_bytes = impl_->options.max_body_bytes;
            impl_->h2.reset(new detail::h2_session(*impl_->link, std::move(config)));
        }
        const result<std::uint32_t> opened =
            impl_->h2->begin(verb, target, body, extra_headers);
        if (!opened) {
            return opened.error();
        }
        return impl_->h2->finish(opened.value());
    }

    std::string request;
    request.reserve(128 + target.size() + body.size());
    request += to_token(verb);
    request += ' ';
    request += target;
    request += " HTTP/1.1\r\nHost: ";
    request += impl_->options.host;
    // The default port is part of the scheme: 443 under TLS, 80 otherwise. It is
    // omitted from Host only when the connection uses that scheme default.
    const std::uint16_t default_port = impl_->options.tls ? 443 : 80;
    if (impl_->options.port != default_port) {
        request += ':';
        request += std::to_string(impl_->options.port);
    }
    // Persistence is regulated through this one field. The caller may spell it
    // (a caller that sends `Connection: close` gets the one-shot exchange it
    // asked for); otherwise the option decides: keep-alive announces that this
    // connection may carry the next request, close retires it after this one.
    bool caller_sent_connection = impl_->options.default_headers.contains("connection");
    for (const header& item : extra_headers) {
        if (lower(item.name) == "connection") {
            caller_sent_connection = true;
        }
    }
    if (!caller_sent_connection) {
        request += "\r\nConnection: ";
        request += impl_->options.keep_alive ? "keep-alive" : "close";
        request += "\r\n";
    } else {
        request += "\r\n";
    }
    for (const header& item : impl_->options.default_headers) {
        request += item.name;
        request += ": ";
        request += item.value;
        request += "\r\n";
    }
    for (const header& item : extra_headers) {
        request += item.name;
        request += ": ";
        request += item.value;
        request += "\r\n";
    }
    if (!body.empty()) {
        request += "Content-Length: ";
        request += std::to_string(body.size());
        request += "\r\n";
    }
    request += "\r\n";

    // From here on the request is on its way to the kernel. Every failure
    // after this point leaves the framing position unknowable: the connection
    // is retired (never reused), and a transport-shaped failure is reported
    // as `unknown_side_effect` -- the request may have been executed, so a
    // non-idempotent caller must not blind-retry.
    const auto fail_after_request = [this](const rpc_error& original) {
        impl_->retire_link();
        switch (original.code) {
        case rpc_code::eof:
        case rpc_code::connection_reset:
        case rpc_code::broken_pipe:
        case rpc_code::unavailable:
        case rpc_code::deadline_exceeded:
            return rpc_error{rpc_code::unknown_side_effect,
                             "the request was written but no complete response "
                             "arrived: " +
                                 original.message};
        default:
            return original;
        }
    };

    status written = impl_->link->write(as_bytes(request));
    if (!written) {
        // Nothing of this request was accepted by the kernel, so the raw
        // failure stands (a refused connect stays `connection_refused`).
        impl_->retire_link();
        return written.error();
    }
    if (!body.empty()) {
        written = impl_->link->write(body);
        if (!written) {
            return fail_after_request(written.error());
        }
    }

    line_reader reader(*impl_->link);
    response out;
    for (;;) {
        // The status line, then the header block. An interim 1xx response is
        // skipped: the real response follows on the same connection, and
        // stopping at the interim would leave its successors unparsed. 101 is
        // terminal: the protocol switches and this client speaks none of the
        // upgraded ones.
        result<std::string> status_line = reader.line(8u * 1024u);
        if (!status_line) {
            return fail_after_request(status_line.error());
        }
        // "HTTP/1.1 200 OK": the version, a space, three digits, then an optional reason.
        const std::string& line = status_line.value();
        const std::size_t first_space = line.find(' ');
        if (first_space == std::string::npos || line.size() < first_space + 4) {
            return fail_after_request(
                rpc_error{rpc_code::protocol_error, "a malformed HTTP status line"});
        }
        out = response{};
        const std::string_view version = std::string_view(line).substr(0, first_space);
        out.version =
            version == "HTTP/1.0" ? http_version::http_1_0 : http_version::http_1_1;
        const std::string_view code_text = std::string_view(line).substr(first_space + 1, 3);
        int code = 0;
        for (const char c : code_text) {
            if (c < '0' || c > '9') {
                return fail_after_request(
                    rpc_error{rpc_code::protocol_error, "a non-numeric HTTP status code"});
            }
            code = code * 10 + (c - '0');
        }
        out.status_code = code;
        if (line.size() > first_space + 4) {
            out.reason = line.substr(first_space + 4);
        }

        std::size_t header_bytes = 0;
        for (;;) {
            result<std::string> field = reader.line(8u * 1024u);
            if (!field) {
                return fail_after_request(field.error());
            }
            if (field.value().empty()) {
                break;
            }
            header_bytes += field.value().size() + 2;
            if (header_bytes > impl_->options.max_header_bytes) {
                return fail_after_request(rpc_error{rpc_code::resource_exhausted,
                                                    "the response headers exceed their limit"});
            }
            const std::size_t colon = field.value().find(':');
            if (colon == std::string::npos) {
                return fail_after_request(
                    rpc_error{rpc_code::protocol_error, "a malformed HTTP header field"});
            }
            const std::string name = field.value().substr(0, colon);
            std::string_view value = std::string_view(field.value()).substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.remove_suffix(1);
            }
            // header_list has `add`, not `append`; it copies the strings.
            out.headers.add(name, value);
        }

        if (out.status_code == 101) {
            impl_->retire_link();
            return out;
        }
        // 1xx interim responses carry no body and no framing of their own.
        if (out.status_code >= 100 && out.status_code < 200) {
            continue;
        }
        break;
    }

    // What follows is body framing, and it decides how the connection may be
    // reused: the response must be read to its very last framed byte before
    // another request goes out, or the leftover bytes become the next
    // response -- the classic keep-alive corruption.
    bool close_delimited = false;
    // A HEAD response's framing headers describe the body a GET would have
    // carried; the body itself never follows. 204 and 304 carry no body.
    const bool bodyless = verb == method::head || out.status_code == 204 ||
                          out.status_code == 304;
    if (!bodyless && is_chunked(out.headers)) {
        // The chunk grammar is read here rather than through `chunked_decoder` because the
        // decoder expects bytes and this reader owns the buffered remainder; feeding it
        // would mean re-implementing the same buffer hand-off twice.
        for (;;) {
            result<std::string> size_line = reader.line(8u * 1024u);
            if (!size_line) {
                return fail_after_request(size_line.error());
            }
            std::string_view text = size_line.value();
            const std::size_t semicolon = text.find(';');
            if (semicolon != std::string_view::npos) {
                text = text.substr(0, semicolon);
            }
            std::size_t size = 0;
            bool saw_digit = false;
            for (const char c : text) {
                int digit = -1;
                if (c >= '0' && c <= '9') {
                    digit = c - '0';
                } else if (c >= 'a' && c <= 'f') {
                    digit = c - 'a' + 10;
                } else if (c >= 'A' && c <= 'F') {
                    digit = c - 'A' + 10;
                } else if (c == ' ' || c == '\t') {
                    continue;
                } else {
                    return fail_after_request(
                        rpc_error{rpc_code::protocol_error, "a malformed chunk size"});
                }
                if (digit < 0) {
                    return fail_after_request(
                        rpc_error{rpc_code::protocol_error, "a malformed chunk size"});
                }
                saw_digit = true;
                size = size * 16 + static_cast<std::size_t>(digit);
            }
            if (!saw_digit) {
                return fail_after_request(
                    rpc_error{rpc_code::protocol_error, "an empty chunk size"});
            }
            if (size == 0) {
                // Trailers, terminated by an empty line.
                for (;;) {
                    result<std::string> trailer = reader.line(8u * 1024u);
                    if (!trailer) {
                        return fail_after_request(trailer.error());
                    }
                    if (trailer.value().empty()) {
                        break;
                    }
                }
                break;
            }
            if (out.body.size() + size > impl_->options.max_body_bytes) {
                return fail_after_request(rpc_error{rpc_code::resource_exhausted,
                                                    "the response body exceeds its limit"});
            }
            result<std::string> chunk = reader.exact(size);
            if (!chunk) {
                return fail_after_request(chunk.error());
            }
            out.body += chunk.value();
            // The chunk is followed by its own CRLF.
            result<std::string> terminator = reader.line(2u * 1024u);
            if (!terminator) {
                return fail_after_request(terminator.error());
            }
            if (!terminator.value().empty()) {
                return fail_after_request(
                    rpc_error{rpc_code::protocol_error, "a chunk is not followed by CRLF"});
            }
        }
    } else if (!bodyless) {
        result<length_field> length = content_length(out.headers);
        if (!length) {
            return fail_after_request(length.error());
        }
        if (length.value().value > impl_->options.max_body_bytes) {
            return fail_after_request(rpc_error{rpc_code::resource_exhausted,
                                                "the response body exceeds its limit"});
        }
        if (length.value().present) {
            // A declared length, including zero, is authoritative: an empty body is a body of no
            // bytes, not "read until the peer closes".
            if (length.value().value != 0) {
                result<std::string> body_text = reader.exact(length.value().value);
                if (!body_text) {
                    return fail_after_request(body_text.error());
                }
                out.body = std::move(body_text.value());
            }
        } else {
            // No Content-Length and no chunking: the body runs to the end of
            // the connection. The close *is* the delimiter, so this response
            // consumes the connection with it.
            result<std::string> rest = reader.rest(impl_->options.max_body_bytes);
            if (!rest) {
                return fail_after_request(rest.error());
            }
            out.body = std::move(rest.value());
            close_delimited = true;
        }
    }

    // The exchange is complete; decide what the connection is worth now.
    if (close_delimited || response_retires_connection(out)) {
        impl_->retire_link();
    } else if (reader.buffered() != 0) {
        // The server wrote bytes beyond the response's framing. This response
        // is well-formed and is delivered below; the connection is not: the
        // extra bytes belong to no message this client sent, so parsing them
        // as the next response could only fabricate one. Poison, and refuse
        // every later exchange on this client with the stored reason.
        impl_->poisoned = rpc_error{
            rpc_code::protocol_error,
            "the server sent " + std::to_string(reader.buffered()) +
                " byte(s) beyond the response framing"};
        impl_->retire_link();
    } else if (!impl_->options.keep_alive) {
        // The historical one-shot behaviour: one exchange per connection.
        impl_->retire_link();
    }
    return out;
}

result<response> client::send_text(method verb, std::string_view target, std::string_view body,
                                   std::string_view content_type) {
    header fields[1];
    if (!content_type.empty()) {
        fields[0].name = "Content-Type";
        fields[0].value = std::string(content_type);
    }
    return send(verb, target, as_bytes(body),
                span<const header>(fields, content_type.empty() ? 0 : 1));
}

std::unique_ptr<client> make_client(client_options options, crpc::backend_factory& backend,
                                    crpc::endpoint endpoint, crpc::runtime* shard) {
    if (!options.tls) {
        crpc::net::connection_config config;
        config.ep = std::move(endpoint);
        // A response body may be delimited by the close itself, so a close must surface as
        // eof rather than a silent reconnect.
        config.reconnect_on_eof = false;
        if (options.timeout_seconds != 0) {
            config.operation_timeout = std::chrono::seconds(options.timeout_seconds);
        }
        auto link = std::unique_ptr<db::channel>(
            new db::socket_channel(std::move(config), backend, shard));
        return std::unique_ptr<client>(new client(std::move(options), std::move(link)));
    }

#if !defined(CRPC_HAVE_OPENSSL)
    return std::unique_ptr<client>(new client(
        std::move(options), nullptr,
        rpc_error{rpc_code::failed_precondition,
                  "https:// requires the OpenSSL TLS backend: rebuild the library with "
                  "-DCRPC_WITH_OPENSSL=ON"}));
#else
    const std::string sni =
        options.tls_server_name.empty() ? options.host : options.tls_server_name;
    if (sni.empty()) {
        return std::unique_ptr<client>(new client(
            std::move(options), nullptr,
            rpc_error{rpc_code::failed_precondition,
                      "https:// requires a host name for SNI and certificate "
                      "verification"}));
    }

    io::tls_config tls;
    tls.role = io::tls_role::client;
    // Verification is the default; turning it off is an explicit caller decision.
    tls.verify = options.tls_verify_peer ? io::tls_verify_mode::peer
                                         : io::tls_verify_mode::none;
    tls.ca_bundle = options.tls_ca_bundle;
    // Advertise the caller's list (default h2 then http/1.1). Empty disables
    // the ALPN extension entirely. A server that does not implement ALPN is
    // still a valid HTTP/1.1 server: the client reads the negotiated value
    // after the handshake and falls back to HTTP/1.1 unless it says "h2".
    tls.alpn_protocols = options.tls_alpn;
    tls.require_alpn = false;

    std::error_code ec;
    std::unique_ptr<io::tls_context> context = io::tls_context::create(tls, ec);
    if (!context || ec) {
        return std::unique_ptr<client>(new client(
            std::move(options), nullptr,
            rpc_error{rpc_code::failed_precondition,
                      "HTTPS needs a usable TLS context: " +
                          (ec ? ec.message() : std::string("unknown error"))}));
    }

    const std::chrono::milliseconds budget =
        options.timeout_seconds == 0
            ? std::chrono::milliseconds(0)
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::seconds(options.timeout_seconds));
    crpc::connect_options connect;
    if (options.timeout_seconds != 0) {
        connect.connect_timeout = std::chrono::seconds(options.timeout_seconds);
    }
    // TLS needs the raw io::connection (the sink/source pair the record layer
    // decorates); net::connection hides that pair behind its own framing.
    std::unique_ptr<crpc::connection> raw = backend.connect(endpoint, connect);
    if (!raw) {
        return std::unique_ptr<client>(new client(
            std::move(options), nullptr,
            rpc_error{rpc_code::unavailable,
                      "the HTTPS transport refused to connect"}));
    }
    auto* stream = new db::stream_channel(std::move(raw), *context, sni, budget);
    std::unique_ptr<client> made(new client(std::move(options),
                                            std::unique_ptr<db::channel>(stream)));
    // The negotiated ALPN is only known after the handshake; hand the concrete
    // channel to the client so the first send() can wait and decide.
    made->impl_->tls_link = stream;
    // `context` may be released here: the TLS stream retains the shared context
    // state for the lifetime of the channel.
    return made;
#endif
}

} // namespace http
} // namespace crpc