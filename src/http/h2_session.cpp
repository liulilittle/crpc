// CRPC -- HTTP/2 client session implementation (see h2_session.hpp).
#include "h2_session.hpp"

#include <algorithm>
#include <utility>

namespace crpc {
namespace http {
namespace detail {
namespace {

constexpr std::string_view h2_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

[[nodiscard]] std::string lowercase(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

// Fields that name the connection itself are forbidden on an h2 stream
// (RFC 9113 8.2.2); sending one is a protocol error.
[[nodiscard]] bool is_connection_specific(std::string_view name) noexcept {
    return name == "connection" || name == "keep-alive" ||
           name == "proxy-connection" || name == "transfer-encoding" ||
           name == "upgrade";
}

// Field values must not carry line breaks or NUL: HPACK would transport them,
// and the receiving side would see a smuggled field boundary.
[[nodiscard]] bool valid_field_value(std::string_view value) noexcept {
    for (const char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return false;
        }
    }
    return true;
}

} // namespace

h2_session::h2_session(db::channel& link, h2_session_config config)
    : link_(&link), config_(std::move(config)),
      h2_([&config = config_] {
          h2::connection_options options;
          options.role = h2::connection_role::client;
          options.local_settings.enable_push = false;
          options.local_settings.initial_window_size = config.initial_window_size;
          options.local_settings.max_frame_size = config.max_frame_size;
          options.local_settings.max_header_list_size =
              static_cast<u32>(config.max_header_bytes);
          options.max_send_queue_bytes = config.max_send_queue_bytes;
          return options;
      }()) {}

void h2_session::close() noexcept {
    if (link_ != nullptr) {
        link_->close();
    }
}

status h2_session::write_all(span<const std::byte> bytes) {
    if (connection_dead_) {
        return dead_error_;
    }
    return link_->write(bytes);
}

status h2_session::send_frame(h2::frame_type type, u8 flags, u32 stream_id,
                              span<const std::byte> payload) {
    h2::frame_header header;
    header.type = type;
    header.flags = flags;
    header.stream_id = stream_id;
    header.length = static_cast<u32>(payload.size());
    std::string frame(h2::frame_header_size + payload.size(), '\0');
    std::size_t written = 0;
    const status encoded = h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header, payload, written);
    if (!encoded.ok()) {
        return encoded.error();
    }
    frame.resize(written);
    return write_all(as_bytes(frame));
}

// Encodes one HPACK block as HEADERS (+ CONTINUATION frames when the block
// exceeds the peer's max frame size). END_STREAM stays on the HEADERS frame,
// END_HEADERS on the frame that completes the block (RFC 7540 6.2).
status h2_session::send_header_block(u32 stream_id, const std::string& block,
                                     bool end_stream) {
    const std::size_t max_frame = h2_.remote_settings().max_frame_size;
    const std::size_t first = std::min(block.size(), max_frame);
    const bool whole = first == block.size();
    const u8 head_flags = static_cast<u8>(
        (whole ? h2::flags::end_headers : static_cast<u8>(0)) |
        (end_stream ? h2::flags::end_stream : static_cast<u8>(0)));
    status sent = send_frame(h2::frame_type::headers, head_flags, stream_id,
                             as_bytes(std::string_view(block.data(), first)));
    if (!sent.ok()) {
        return sent;
    }
    std::size_t offset = first;
    while (offset < block.size()) {
        const std::size_t chunk = std::min(block.size() - offset, max_frame);
        const bool last = offset + chunk == block.size();
        sent = send_frame(h2::frame_type::continuation,
                          last ? h2::flags::end_headers : static_cast<u8>(0), stream_id,
                          as_bytes(std::string_view(block.data() + offset, chunk)));
        if (!sent.ok()) {
            return sent;
        }
        offset += chunk;
    }
    return status{};
}

status h2_session::start() {
    if (started_) {
        return status{};
    }
    std::string out;
    out.reserve(h2_preface.size() + 4 * 6 + h2::frame_header_size);
    out.append(h2_preface);
    h2::setting entries[4];
    entries[0].id = static_cast<u16>(h2::setting_id::enable_push);
    entries[0].value = 0; // server push is not implemented; say so up front
    entries[1].id = static_cast<u16>(h2::setting_id::initial_window_size);
    entries[1].value = config_.initial_window_size;
    entries[2].id = static_cast<u16>(h2::setting_id::max_frame_size);
    entries[2].value = config_.max_frame_size;
    entries[3].id = static_cast<u16>(h2::setting_id::max_header_list_size);
    entries[3].value = static_cast<u32>(config_.max_header_bytes);
    std::string payload(4 * 6, '\0');
    std::size_t written = 0;
    const status encoded = h2::encode_settings(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        span<const h2::setting>(entries, 4), written);
    if (!encoded.ok()) {
        return encoded.error();
    }
    payload.resize(written);
    h2::frame_header header;
    header.type = h2::frame_type::settings;
    header.flags = 0;
    header.stream_id = 0;
    header.length = static_cast<u32>(payload.size());
    std::string frame(h2::frame_header_size + payload.size(), '\0');
    std::size_t count = 0;
    const status framed = h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header, as_bytes(payload), count);
    if (!framed.ok()) {
        return framed.error();
    }
    frame.resize(count);
    out.append(frame);
    const status wrote = write_all(as_bytes(out));
    if (!wrote.ok()) {
        return wrote;
    }
    started_ = true;
    return status{};
}

// --- Inbound -----------------------------------------------------------------

status h2_session::pump_read() {
    if (connection_dead_) {
        return dead_error_;
    }
    std::byte chunk[16 * 1024];
    result<std::size_t> got = link_->read_some(span<std::byte>(chunk, sizeof chunk));
    if (!got) {
        // A dead link is terminal for every stream that has not completed.
        fail_everything(got.error().code, got.error().message);
        return got.error();
    }
    read_buf_.append(reinterpret_cast<const char*>(chunk), got.value());
    return process_buffer();
}

status h2_session::process_buffer() {
    for (;;) {
        if (read_buf_.size() < h2::frame_header_size) {
            return status{};
        }
        h2::frame_header header;
        const status decoded = h2::decode_frame_header(
            span<const std::byte>(reinterpret_cast<const std::byte*>(read_buf_.data()),
                                  h2::frame_header_size),
            header, config_.max_frame_size);
        if (!decoded.ok()) {
            fail_everything(rpc_code::protocol_error,
                            "an HTTP/2 frame header is malformed");
            return status{rpc_code::protocol_error,
                          "an HTTP/2 frame header is malformed"};
        }
        const std::size_t total = h2::frame_header_size + header.length;
        if (read_buf_.size() < total) {
            return status{};
        }
        const span<const std::byte> payload(
            reinterpret_cast<const std::byte*>(read_buf_.data()) + h2::frame_header_size,
            header.length);
        const status handled = process_frame(header, payload);
        read_buf_.erase(0, static_cast<std::size_t>(total));
        if (!handled.ok()) {
            return handled;
        }
        if (connection_dead_) {
            return dead_error_;
        }
    }
}

status h2_session::process_frame(const h2::frame_header& header,
                                 span<const std::byte> payload) {
    // DATA borrows the read buffer; take the bytes out before anything can
    // invalidate the view.
    std::string data;
    if (header.type == h2::frame_type::data && header.length != 0) {
        span<const std::byte> body;
        const status stripped = h2::decode_data(payload, header.flags, body);
        if (!stripped.ok()) {
            fail_everything(rpc_code::protocol_error, "a DATA frame is malformed");
            return status{rpc_code::protocol_error, "a DATA frame is malformed"};
        }
        data.assign(reinterpret_cast<const char*>(body.data()), body.size());
    }

    const h2::h2_error error = h2_.receive(header, payload);
    if (!error.ok()) {
        if (error.scope == h2::error_scope::connection) {
            const std::string message =
                std::string("the HTTP/2 connection failed: ") + h2::to_string(error.code);
            fail_everything(error.rpc(), message);
            return status{error.rpc(), message};
        }
        // Stream scope: answer with RST_STREAM and fail only that stream.
        (void)send_frame(h2::frame_type::rst_stream, 0, error.stream_id,
                         span<const std::byte>());
        fail_stream(error.stream_id, error.rpc(),
                    std::string("the HTTP/2 stream failed: ") + h2::to_string(error.code));
        return status{};
    }

    switch (header.type) {
    case h2::frame_type::settings:
        if (!header.has_flag(h2::flags::ack) && h2_.pending_settings_ack()) {
            h2_.settings_acknowledge();
            const status acked = send_frame(h2::frame_type::settings, h2::flags::ack, 0,
                                            span<const std::byte>());
            if (!acked.ok()) {
                return acked;
            }
        }
        break;
    case h2::frame_type::ping:
        if (!header.has_flag(h2::flags::ack) && h2_.pending_ping_ack()) {
            h2::ping_opaque opaque = h2_.last_ping();
            h2_.ping_acknowledge();
            std::string echo(8, '\0');
            std::size_t written = 0;
            const status encoded = h2::encode_ping(
                span<std::byte>(reinterpret_cast<std::byte*>(echo.data()), echo.size()),
                opaque, written);
            if (!encoded.ok()) {
                return encoded.error();
            }
            echo.resize(written);
            const status acked = send_frame(h2::frame_type::ping, h2::flags::ack, 0,
                                            as_bytes(echo));
            if (!acked.ok()) {
                return acked;
            }
        }
        break;
    case h2::frame_type::headers:
    case h2::frame_type::continuation:
        if (h2_.last_headers_present()) {
            const u32 stream_id = h2_.last_headers_stream();
            const bool end_stream = h2_.snapshot(stream_id).remote_end_stream;
            const http::header_list fields = h2_.last_headers();
            h2_.clear_last_headers();
            auto slot = inflight_.find(stream_id);
            if (slot == inflight_.end() || slot->second.stage == stage::failed) {
                // A late block for a stream we already finished (or never
                // tracked): nothing to route it to.
                break;
            }
            if (slot->second.stage == stage::body && end_stream) {
                const status trailers = handle_response_trailers(stream_id, fields);
                if (!trailers.ok()) {
                    return trailers;
                }
                break;
            }
            const status head = handle_response_headers(stream_id, fields, end_stream);
            if (!head.ok()) {
                return head;
            }
        }
        break;
    case h2::frame_type::data: {
        const status received =
            handle_response_data(header.stream_id, as_bytes(data), header.length,
                                 header.has_flag(h2::flags::end_stream));
        if (!received.ok()) {
            return received;
        }
        break;
    }
    case h2::frame_type::rst_stream:
        if (h2_.last_reset_stream() != 0) {
            const u32 stream_id = h2_.last_reset_stream();
            const rpc_code code = h2::to_rpc_code(h2_.last_reset_code());
            fail_stream(stream_id, code,
                        std::string("the server reset the stream: ") +
                            h2::to_string(h2_.last_reset_code()));
        }
        break;
    case h2::frame_type::goaway:
        if (h2_.goaway_received()) {
            const u32 last = h2_.goaway_last_stream_id();
            std::vector<u32> doomed;
            for (const auto& item : inflight_) {
                if (item.first > last) {
                    doomed.push_back(item.first);
                }
            }
            for (const u32 stream_id : doomed) {
                fail_stream(stream_id, rpc_code::unavailable,
                            "the server is shutting down the connection before this "
                            "stream was answered");
            }
        }
        break;
    default:
        break;
    }
    return status{};
}

// --- Response assembly -------------------------------------------------------

status h2_session::handle_response_headers(u32 stream_id, const http::header_list& fields,
                                           bool end_stream) {
    assembly& slot = inflight_[stream_id];
    slot.response = http::response{};
    slot.response.version = http_version::http_2;
    bool have_status = false;
    bool pseudo_done = false;
    bool malformed = false;
    std::string malformed_why;
    for (const http::header& field : fields) {
        if (!field.name.empty() && field.name.front() == ':') {
            if (pseudo_done) {
                malformed = true; // pseudo-headers must precede regular ones
                malformed_why = "a pseudo-header follows a regular header";
                break;
            }
            // The only valid response pseudo-header is :status.
            if (field.name == ":status") {
                if (have_status || field.value.size() != 3) {
                    malformed = true;
                    malformed_why = "the :status pseudo-header is malformed";
                    break;
                }
                int code = 0;
                for (const char c : field.value) {
                    if (c < '0' || c > '9') {
                        malformed = true;
                        malformed_why = "the :status pseudo-header is not numeric";
                        break;
                    }
                    code = code * 10 + (c - '0');
                }
                if (malformed) {
                    break;
                }
                slot.response.status_code = code;
                slot.response.reason.clear();
                have_status = true;
            } else {
                malformed = true;
                malformed_why = "an unknown response pseudo-header: " + field.name;
                break;
            }
        } else {
            pseudo_done = true;
            const std::string name = lowercase(field.name);
            if (name != field.name || name.empty() || !http::is_token(name)) {
                malformed = true;
                malformed_why = "a response field name is not lower-case: " + field.name;
                break;
            }
            if (!valid_field_value(field.value)) {
                malformed = true;
                malformed_why = "a response field value carries a line break";
                break;
            }
            if (name == "content-length") {
                // Parsed, not trusted: the DATA count decides.
                std::size_t declared = 0;
                bool numeric = !field.value.empty();
                for (const char c : field.value) {
                    if (c < '0' || c > '9') {
                        numeric = false;
                        break;
                    }
                    if (declared > (SIZE_MAX - static_cast<std::size_t>(c - '0')) / 10) {
                        numeric = false;
                        break;
                    }
                    declared = declared * 10 + static_cast<std::size_t>(c - '0');
                }
                if (!numeric) {
                    malformed = true;
                    malformed_why = "the content-length response field is malformed";
                    break;
                }
                slot.have_length = true;
                slot.declared = declared;
                slot.response.headers.add(name, field.value);
            } else if (is_connection_specific(name)) {
                // Forbidden on an h2 stream; a server sending one is broken,
                // but dropping is the gentler and RFC-blessed reading for a
                // *response* (RFC 9113 8.2.2 says the message is malformed;
                // failing the stream is equally correct). Be strict.
                malformed = true;
                malformed_why = "a connection-specific field: " + name;
                break;
            } else {
                slot.response.headers.add(name, field.value);
            }
        }
    }

    if (!malformed && !have_status) {
        malformed = true;
        malformed_why = "the response has no :status pseudo-header";
    }
    if (malformed) {
        const std::string message = "the HTTP/2 response is malformed: " + malformed_why;
        fail_stream(stream_id, rpc_code::protocol_error, message);
        return status{rpc_code::protocol_error, message};
    }

    // Informational responses are interim: the final response follows on the
    // same stream (RFC 9110 15.2). Keep the assembly waiting for it.
    if (slot.response.status_code >= 100 && slot.response.status_code < 200) {
        if (end_stream) {
            const std::string message =
                "an informational HTTP/2 response cannot end the stream";
            fail_stream(stream_id, rpc_code::protocol_error, message);
            return status{rpc_code::protocol_error, message};
        }
        slot.stage = stage::headers;
        slot.have_length = false;
        slot.declared = 0;
        slot.received = 0;
        return status{};
    }

    if (end_stream) {
        complete_stream(stream_id);
    } else {
        slot.stage = stage::body;
    }
    return status{};
}

status h2_session::handle_response_trailers(u32 stream_id, const http::header_list& fields) {
    assembly& slot = inflight_[stream_id];
    for (const http::header& field : fields) {
        if (!field.name.empty() && field.name.front() == ':') {
            const std::string message =
                "the HTTP/2 trailers carry a pseudo-header: " + field.name;
            fail_stream(stream_id, rpc_code::protocol_error, message);
            return status{rpc_code::protocol_error, message};
        }
        const std::string name = lowercase(field.name);
        if (name != field.name || !http::is_token(name) ||
            !valid_field_value(field.value)) {
            const std::string message = "the HTTP/2 trailers are malformed: " + field.name;
            fail_stream(stream_id, rpc_code::protocol_error, message);
            return status{rpc_code::protocol_error, message};
        }
        slot.response.trailers.add(name, field.value);
    }
    complete_stream(stream_id);
    return status{};
}

status h2_session::handle_response_data(u32 stream_id, span<const std::byte> data,
                                        u32 charged, bool end_stream) {
    // The receive windows must be restored for every DATA frame, even for a
    // stream this session no longer tracks (defensive against late frames).
    auto slot = inflight_.find(stream_id);
    if (slot != inflight_.end() && slot->second.stage == stage::body) {
        assembly& target = slot->second;
        if (target.received + data.size() > config_.max_body_bytes) {
            const std::string message = "the HTTP/2 response body exceeds its limit";
            fail_stream(stream_id, rpc_code::resource_exhausted, message);
            (void)send_frame(h2::frame_type::rst_stream, 0, stream_id,
                             span<const std::byte>());
            return status{rpc_code::resource_exhausted, message};
        }
        if (target.have_length && target.received + data.size() > target.declared) {
            const std::string message =
                "the HTTP/2 response body exceeds its declared content-length";
            fail_stream(stream_id, rpc_code::protocol_error, message);
            (void)send_frame(h2::frame_type::rst_stream, 0, stream_id,
                             span<const std::byte>());
            return status{rpc_code::protocol_error, message};
        }
        target.received += data.size();
        target.response.body.append(reinterpret_cast<const char*>(data.data()),
                                    data.size());
        if (end_stream) {
            complete_stream(stream_id);
        }
    }
    return advertise_window(stream_id, charged);
}

status h2_session::advertise_window(u32 stream_id, u32 charged) {
    const h2::window_credit credit = h2_.release_received(stream_id, charged);
    if (credit.connection != 0) {
        std::string payload(4, '\0');
        std::size_t written = 0;
        const status encoded = h2::encode_window_update(
            span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), 4),
            credit.connection, written);
        if (!encoded.ok()) {
            return encoded.error();
        }
        payload.resize(written);
        const status sent = send_frame(h2::frame_type::window_update, 0, 0,
                                       as_bytes(payload));
        if (!sent.ok()) {
            return sent;
        }
    }
    if (credit.stream != 0) {
        std::string payload(4, '\0');
        std::size_t written = 0;
        const status encoded = h2::encode_window_update(
            span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), 4),
            credit.stream, written);
        if (!encoded.ok()) {
            return encoded.error();
        }
        payload.resize(written);
        const status sent = send_frame(h2::frame_type::window_update, 0, stream_id,
                                       as_bytes(payload));
        if (!sent.ok()) {
            return sent;
        }
    }
    return status{};
}

void h2_session::complete_stream(u32 stream_id) {
    auto slot = inflight_.find(stream_id);
    if (slot == inflight_.end()) {
        return;
    }
    assembly& done = slot->second;
    if (done.have_length && done.received != done.declared) {
        const std::string message = "the HTTP/2 response content-length does not match "
                                    "the body: declared " +
                                    std::to_string(done.declared) + ", received " +
                                    std::to_string(done.received);
        inflight_.erase(slot);
        // The mismatch is the server's fault; reset the stream so it cannot
        // keep feeding us bytes.
        (void)send_frame(h2::frame_type::rst_stream, 0, stream_id, span<const std::byte>());
        fail_stream(stream_id, rpc_code::protocol_error, message);
        return;
    }
    completed_.emplace(stream_id, std::move(done.response));
    inflight_.erase(slot);
    (void)h2_.local_end_stream(stream_id);
    // Deliberately no prune here: a late RST_STREAM for this stream must hit
    // a recorded (closed) entry, not an unknown id, or the connection layer
    // would escalate a benign race to a connection error.
}

void h2_session::fail_stream(u32 stream_id, rpc_code code, std::string message) {
    if (completed_.find(stream_id) != completed_.end()) {
        // The response was already won; a late reset cannot revoke it.
        return;
    }
    inflight_.erase(stream_id);
    failures_.erase(stream_id);
    failures_.emplace(stream_id, rpc_error{code, std::move(message)});
}

void h2_session::fail_everything(rpc_code code, std::string message) {
    if (connection_dead_) {
        return;
    }
    connection_dead_ = true;
    dead_error_ = rpc_error{code, message};
    std::vector<u32> doomed;
    doomed.reserve(inflight_.size());
    for (const auto& item : inflight_) {
        doomed.push_back(item.first);
    }
    for (const u32 stream_id : doomed) {
        fail_stream(stream_id, code, message);
    }
    if (link_ != nullptr) {
        link_->close();
    }
}

// --- Request path ------------------------------------------------------------

result<u32> h2_session::begin(method verb, std::string_view target,
                              span<const std::byte> body,
                              span<const header> extra_headers) {
    if (connection_dead_) {
        return dead_error_;
    }
    const status boot = start();
    if (!boot.ok()) {
        return boot.error();
    }
    h2_.prune_closed_streams();

    // Concurrency: a zero limit means the server takes no streams at all;
    // otherwise the peer's SETTINGS_MAX_CONCURRENT_STREAMS bounds the streams
    // we may have open at once. Until the peer's first SETTINGS arrives the
    // RFC default (100) applies, so the first request never stalls on this
    // check.
    const u32 limit = h2_.remote_settings().max_concurrent_streams;
    if (limit == 0) {
        return rpc_error{rpc_code::failed_precondition,
                         "the server allows no concurrent HTTP/2 streams"};
    }
    if (h2_.active_stream_count() >= limit) {
        return rpc_error{rpc_code::resource_exhausted,
                         "the server's HTTP/2 max-concurrent-streams limit is reached"};
    }
    if (h2_.goaway_received()) {
        return rpc_error{rpc_code::unavailable,
                         "the server is shutting down this HTTP/2 connection"};
    }

    if (verb == method::connect || verb == method::extension) {
        return rpc_error{rpc_code::invalid_argument,
                         "this HTTP/2 client does not support CONNECT or extension "
                         "methods"};
    }
    if (target.empty() || target.front() != '/') {
        return rpc_error{rpc_code::invalid_argument,
                         "an HTTP/2 request target must be origin-form (start with '/')"};
    }

    // Build the request field list: pseudo-headers first, in the fixed order
    // method/scheme/path/authority, then regular fields lower-cased.
    std::vector<h2::header_field> fields;
    fields.reserve(4 + config_.default_headers.size() + extra_headers.size() + 1);
    fields.push_back({":method", to_token(verb)});
    fields.push_back({":scheme", config_.scheme});
    fields.push_back({":path", std::string(target)});
    fields.push_back({":authority", config_.authority});

    const auto add_regular = [&](const header& item) -> status {
        const std::string name = lowercase(item.name);
        if (name.empty() || name.front() == ':' || !http::is_token(name)) {
            return status{rpc_code::invalid_argument,
                          "an HTTP/2 request field name is malformed: " + item.name};
        }
        if (!valid_field_value(item.value)) {
            return status{rpc_code::invalid_argument,
                          "an HTTP/2 request field value carries a line break"};
        }
        if (is_connection_specific(name)) {
            // Forbidden in HTTP/2 (RFC 9113 8.2.2). Dropping is what every
            // client does; sending one would reset the stream.
            return status{};
        }
        if (name == "host") {
            // :authority replaces it; a differing Host would be a protocol
            // violation.
            return status{};
        }
        if (name == "te" && lowercase(item.value) != "trailers") {
            // Only `TE: trailers` may cross an h2 hop.
            return status{};
        }
        if (name == "content-length") {
            // Recomputed from the body below.
            return status{};
        }
        fields.push_back({name, item.value});
        return status{};
    };
    for (const header& item : config_.default_headers) {
        const status added = add_regular(item);
        if (!added.ok()) {
            return added.error();
        }
    }
    for (const header& item : extra_headers) {
        const status added = add_regular(item);
        if (!added.ok()) {
            return added.error();
        }
    }
    if (!body.empty()) {
        fields.push_back({"content-length", std::to_string(body.size())});
    }

    std::string block;
    const status encoded = h2_.encoder().encode(
        span<const h2::header_field>(fields.data(), fields.size()), block);
    if (!encoded.ok()) {
        return encoded.error();
    }

    // Pick the next odd stream id. GOAWAY may forbid ids beyond its last
    // stream; `can_open_stream` knows.
    u32 stream_id = next_stream_id_;
    while (!h2_.can_open_stream(stream_id)) {
        if (stream_id > h2::max_stream_id - 2u) {
            return rpc_error{rpc_code::resource_exhausted,
                             "the HTTP/2 stream identifiers are exhausted"};
        }
        stream_id += 2;
    }
    next_stream_id_ = stream_id + 2;
    const h2::h2_error opened = h2_.open_stream(stream_id, body.empty());
    if (!opened.ok()) {
        return rpc_error{opened.rpc(),
                         std::string("the HTTP/2 stream cannot be opened: ") +
                             h2::to_string(opened.code)};
    }
    inflight_.emplace(stream_id, assembly{});

    const status head = send_header_block(stream_id, block, body.empty());
    if (!head.ok()) {
        fail_stream(stream_id, head.code(), head.message());
        return head.error();
    }

    if (body.empty()) {
        return stream_id;
    }

    // Send the body, honouring both send windows. The loop offers only what
    // the windows allow right now, so no byte is ever parked in the
    // connection's send queue: one representation on the wire, none waiting
    // (a queue plus a remaining-span would be two, and flushing both is how
    // bytes get sent twice). Pumping reads the WINDOW_UPDATEs that open the
    // windows, plus any response that arrived in the meantime.
    span<const std::byte> remaining = body;
    while (!remaining.empty()) {
        const std::size_t allowance = h2_.available_send_window(stream_id);
        if (allowance == 0) {
            const status pumped = pump_read();
            if (!pumped.ok()) {
                return pumped.error();
            }
            if (completed_.find(stream_id) != completed_.end() ||
                failures_.find(stream_id) != failures_.end()) {
                // The server answered (or refused) before consuming the whole
                // body: stop pushing, `finish` reports the outcome.
                return stream_id;
            }
            continue;
        }
        const std::size_t chunk = std::min(remaining.size(), allowance);
        std::size_t sent = 0;
        bool parked = false;
        const h2::h2_error offered = h2_.send_data(
            stream_id, remaining.first(chunk), sent, parked);
        if (!offered.ok()) {
            fail_stream(stream_id, offered.rpc(),
                        std::string("the HTTP/2 stream failed while sending: ") +
                            h2::to_string(offered.code));
            return rpc_error{offered.rpc(),
                             std::string("the HTTP/2 stream failed while sending: ") +
                                 h2::to_string(offered.code)};
        }
        // allowance > 0 and chunk <= allowance: everything was charged.
        const status wrote = send_frame(
            h2::frame_type::data, 0, stream_id,
            span<const std::byte>(remaining.data(), chunk));
        if (!wrote.ok()) {
            return wrote.error();
        }
        remaining = remaining.subspan(chunk);
    }
    (void)h2_.local_end_stream(stream_id);
    // An empty DATA frame carries END_STREAM; a zero-length final DATA frame
    // is how the library's server engine expects the stream to end.
    const status ended =
        send_frame(h2::frame_type::data, h2::flags::end_stream, stream_id,
                   span<const std::byte>());
    if (!ended.ok()) {
        return ended.error();
    }
    return stream_id;
}

result<response> h2_session::finish(u32 stream_id) {
    if (connection_dead_) {
        // Whatever completed before the death is still collectable.
        const auto done = completed_.find(stream_id);
        if (done != completed_.end()) {
            result<response> out{std::move(done->second)};
            completed_.erase(done);
            return out;
        }
        const auto failed = failures_.find(stream_id);
        if (failed != failures_.end()) {
            rpc_error error{failed->second};
            failures_.erase(failed);
            return result<response>{std::move(error)};
        }
        return result<response>{dead_error_};
    }

    const auto done = completed_.find(stream_id);
    if (done != completed_.end()) {
        result<response> out{std::move(done->second)};
        completed_.erase(done);
        return out;
    }
    const auto failed = failures_.find(stream_id);
    if (failed != failures_.end()) {
        rpc_error error{failed->second};
        failures_.erase(failed);
        return result<response>{std::move(error)};
    }
    if (inflight_.find(stream_id) == inflight_.end()) {
        return rpc_error{rpc_code::invalid_argument,
                         "finish() was called for a stream this client never opened"};
    }

    for (;;) {
        const status pumped = pump_read();
        if (!pumped.ok()) {
            return pumped.error();
        }
        const auto ready = completed_.find(stream_id);
        if (ready != completed_.end()) {
            result<response> out{std::move(ready->second)};
            completed_.erase(ready);
            return out;
        }
        const auto failure = failures_.find(stream_id);
        if (failure != failures_.end()) {
            rpc_error error{failure->second};
            failures_.erase(failure);
            return result<response>{std::move(error)};
        }
        if (connection_dead_) {
            return result<response>{dead_error_};
        }
    }
}

} // namespace detail
} // namespace http
} // namespace crpc
