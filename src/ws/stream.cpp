// CRPC -- WebSocket session implementation (RFC 6455 sections 5.4-7).
#include <crpc/ws/stream.hpp>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#include <crpc/endian.hpp>

namespace crpc {
namespace ws {
namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

} // namespace

struct stream::impl {
    impl(std::unique_ptr<db::channel> link_value, const stream_options& opt,
         span<const std::byte> leftover)
        : link(std::move(link_value)),
          options(opt),
          decoder(frame_decoder::options{!opt.is_client, opt.max_message_bytes}) {
        if (!leftover.empty()) {
            decoded_frame ignored;
            (void)decoder.feed(leftover, ignored);
        }
    }

    std::unique_ptr<db::channel> link;
    stream_options options;
    frame_decoder decoder;
    std::array<std::byte, 16 * 1024> read_chunk{};

    bool close_sent = false;
    bool close_received = false;
    bool aborted = false;

    bool in_fragment = false;
    opcode fragment_type = opcode::binary;
    std::string fragment;

    std::optional<event> next_event;
};

stream::stream(std::unique_ptr<db::channel> link, const stream_options& options)
    : stream(std::move(link), options, span<const std::byte>()) {}

stream::stream(std::unique_ptr<db::channel> link, const stream_options& options,
               span<const std::byte> leftover)
    : impl_(new impl(std::move(link), options, leftover)) {}

stream::~stream() { abort(); }

result<decoded_frame> stream::next_frame() {
    for (;;) {
        decoded_frame frame;
        frame_status status =
            impl_->decoder.feed(span<const std::byte>(), frame);
        if (status == frame_status::frame) {
            return frame;
        }
        if (status == frame_status::error) {
            const rpc_error failure = impl_->decoder.error();
            if (failure.code == rpc_code::resource_exhausted) {
                (void)send_close(close_code::message_too_big);
            } else {
                (void)send_close(close_code::protocol_error);
            }
            return failure;
        }
        if (impl_->link == nullptr) {
            return rpc_error{rpc_code::shutting_down,
                             "the WebSocket session has no link"};
        }
        result<std::size_t> got =
            impl_->link->read_some(span<std::byte>(impl_->read_chunk.data(),
                                                   impl_->read_chunk.size()));
        if (!got) {
            return got.error();
        }
        if (got.value() == 0) {
            continue;
        }
        status = impl_->decoder.feed(
            span<const std::byte>(impl_->read_chunk.data(), got.value()), frame);
        if (status == frame_status::frame || status == frame_status::error) {
            if (status == frame_status::error) {
                const rpc_error failure = impl_->decoder.error();
                if (failure.code == rpc_code::resource_exhausted) {
                    (void)send_close(close_code::message_too_big);
                } else {
                    (void)send_close(close_code::protocol_error);
                }
                return failure;
            }
            return frame;
        }
    }
}

result<bool> stream::handle_frame(const decoded_frame& frame) {
    if (is_control(frame.op)) {
        if (frame.op == opcode::ping) {
            // RFC 6455 section 5.5.2: the Pong carries the Ping's payload.
            (void)send_pong(as_bytes(frame.payload));
            event ev;
            ev.type = event::kind::ping;
            ev.data = frame.payload;
            impl_->next_event = std::move(ev);
            return true;
        }
        if (frame.op == opcode::pong) {
            event ev;
            ev.type = event::kind::pong;
            ev.data = frame.payload;
            impl_->next_event = std::move(ev);
            return true;
        }
        // close
        close_code code = close_code::normal;
        bool had_code = false;
        std::string reason;
        if (frame.payload.size() >= 2) {
            const auto high = static_cast<std::uint8_t>(
                static_cast<unsigned char>(frame.payload[0]));
            const auto low = static_cast<std::uint8_t>(
                static_cast<unsigned char>(frame.payload[1]));
            const auto raw = static_cast<std::uint16_t>((high << 8) | low);
            if (!is_valid_close_code(raw)) {
                (void)send_close(close_code::protocol_error);
                return rpc_error{rpc_code::protocol_error,
                                 "a Close frame carried a reserved status code"};
            }
            code = static_cast<close_code>(raw);
            had_code = true;
            reason = frame.payload.substr(2);
            if (!is_valid_utf8(as_bytes(reason))) {
                (void)send_close(close_code::invalid_payload);
                return rpc_error{rpc_code::protocol_error,
                                 "a Close reason is not valid UTF-8"};
            }
        } else if (frame.payload.size() == 1) {
            (void)send_close(close_code::protocol_error);
            return rpc_error{rpc_code::protocol_error,
                             "a Close frame with a one-byte payload"};
        }
        if (!impl_->close_sent) {
            (void)send_close(code, reason);
        }
        impl_->close_received = true;
        impl_->in_fragment = false;
        impl_->fragment.clear();
        event ev;
        ev.type = event::kind::closed;
        ev.code = had_code ? static_cast<std::uint16_t>(code)
                           : static_cast<std::uint16_t>(close_code::no_status_received);
        ev.had_code = had_code;
        ev.data = std::move(reason);
        impl_->next_event = std::move(ev);
        return true;
    }

    // Data frame.
    if (impl_->close_sent || impl_->close_received) {
        (void)send_close(close_code::protocol_error);
        return rpc_error{rpc_code::protocol_error,
                         "a data frame arrived after a Close"};
    }

    if (frame.op == opcode::continuation) {
        if (!impl_->in_fragment) {
            (void)send_close(close_code::protocol_error);
            return rpc_error{rpc_code::protocol_error,
                             "a continuation frame has no message to continue"};
        }
        if (impl_->fragment.size() + frame.payload.size() >
            impl_->options.max_message_bytes) {
            (void)send_close(close_code::message_too_big);
            return rpc_error{rpc_code::resource_exhausted,
                             "a fragmented message exceeds max_message_bytes"};
        }
        impl_->fragment.append(frame.payload);
        if (!frame.fin) {
            return false;
        }
        const opcode type = impl_->fragment_type;
        std::string data = std::move(impl_->fragment);
        impl_->fragment.clear();
        impl_->in_fragment = false;
        if (type == opcode::text && !is_valid_utf8(as_bytes(data))) {
            (void)send_close(close_code::invalid_payload);
            return rpc_error{rpc_code::protocol_error,
                             "a fragmented text message is not valid UTF-8"};
        }
        event ev;
        ev.type = event::kind::message;
        ev.op = type;
        ev.data = std::move(data);
        impl_->next_event = std::move(ev);
        return true;
    }

    // A new text/binary frame.
    if (impl_->in_fragment) {
        (void)send_close(close_code::protocol_error);
        return rpc_error{rpc_code::protocol_error,
                         "a new data frame arrived while a message was fragmented"};
    }
    if (frame.payload.size() > impl_->options.max_message_bytes) {
        (void)send_close(close_code::message_too_big);
        return rpc_error{rpc_code::resource_exhausted,
                         "a message exceeds max_message_bytes"};
    }
    if (frame.fin) {
        if (frame.op == opcode::text && !is_valid_utf8(as_bytes(frame.payload))) {
            (void)send_close(close_code::invalid_payload);
            return rpc_error{rpc_code::protocol_error,
                             "a text message is not valid UTF-8"};
        }
        event ev;
        ev.type = event::kind::message;
        ev.op = frame.op;
        ev.data = frame.payload;
        impl_->next_event = std::move(ev);
        return true;
    }
    // Start of a fragmented message.
    impl_->in_fragment = true;
    impl_->fragment_type = frame.op;
    impl_->fragment.assign(frame.payload);
    return false;
}

result<event> stream::recv() {
    if (!impl_ || !impl_->link) {
        return rpc_error{rpc_code::shutting_down, "the WebSocket session has no link"};
    }
    if (impl_->aborted) {
        return rpc_error{rpc_code::shutting_down, "the WebSocket session was aborted"};
    }
    if (impl_->close_received) {
        return rpc_error{rpc_code::eof, "the WebSocket session is closed"};
    }
    for (;;) {
        if (impl_->next_event.has_value()) {
            event ev = std::move(*impl_->next_event);
            impl_->next_event.reset();
            return ev;
        }
        result<decoded_frame> frame = next_frame();
        if (!frame) {
            return frame.error();
        }
        result<bool> staged = handle_frame(frame.value());
        if (!staged) {
            return staged.error();
        }
    }
}

status stream::send_text(std::string_view text) {
    if (!is_valid_utf8(text)) {
        return status{rpc_code::invalid_argument,
                      "a text message must be valid UTF-8"};
    }
    return send_data(opcode::text, as_bytes(text));
}

status stream::send_binary(span<const std::byte> data) {
    return send_data(opcode::binary, data);
}

status stream::send_data(opcode op, span<const std::byte> data) {
    if (!impl_ || !impl_->link) {
        return status{rpc_code::shutting_down, "the WebSocket session has no link"};
    }
    if (impl_->aborted) {
        return status{rpc_code::shutting_down, "the WebSocket session was aborted"};
    }
    if (impl_->close_sent || impl_->close_received) {
        return status{rpc_code::failed_precondition,
                      "no data frame may follow a Close"};
    }
    const std::size_t fragment = impl_->options.max_fragment_bytes;
    if (fragment != 0 && data.size() > fragment) {
        std::size_t offset = 0;
        bool first = true;
        while (offset < data.size()) {
            const std::size_t take = std::min(fragment, data.size() - offset);
            const bool last = offset + take >= data.size();
            const opcode use = first ? op : opcode::continuation;
            first = false;
            const status sent = emit_frame(
                use, span<const std::byte>(data.data() + offset, take), last);
            if (!sent) {
                return sent;
            }
            offset += take;
        }
        return status{};
    }
    return emit_frame(op, data, true);
}

status stream::send_ping(span<const std::byte> payload) {
    if (!impl_ || !impl_->link) {
        return status{rpc_code::shutting_down, "the WebSocket session has no link"};
    }
    if (impl_->aborted) {
        return status{rpc_code::shutting_down, "the WebSocket session was aborted"};
    }
    if (impl_->close_sent || impl_->close_received) {
        return status{rpc_code::failed_precondition,
                      "no control frame may follow a Close"};
    }
    if (payload.size() > 125) {
        return status{rpc_code::invalid_argument,
                      "a control frame payload is limited to 125 bytes"};
    }
    return emit_frame(opcode::ping, payload, true);
}

status stream::send_pong(span<const std::byte> payload) {
    if (!impl_ || !impl_->link) {
        return status{rpc_code::shutting_down, "the WebSocket session has no link"};
    }
    if (impl_->aborted) {
        return status{rpc_code::shutting_down, "the WebSocket session was aborted"};
    }
    if (impl_->close_sent || impl_->close_received) {
        return status{rpc_code::failed_precondition,
                      "no control frame may follow a Close"};
    }
    if (payload.size() > 125) {
        return status{rpc_code::invalid_argument,
                      "a control frame payload is limited to 125 bytes"};
    }
    return emit_frame(opcode::pong, payload, true);
}

status stream::send_close(close_code code, std::string_view reason) {
    if (!impl_ || !impl_->link) {
        return status{rpc_code::shutting_down, "the WebSocket session has no link"};
    }
    if (impl_->aborted) {
        return status{rpc_code::shutting_down, "the WebSocket session was aborted"};
    }
    if (impl_->close_sent) {
        return status{};
    }
    const auto raw = static_cast<std::uint16_t>(code);
    if (!is_valid_close_code(raw)) {
        return status{rpc_code::invalid_argument,
                      "a Close status code must be a defined or registered value"};
    }
    if (reason.size() > 123) {
        return status{rpc_code::invalid_argument,
                      "a Close reason is limited to 123 bytes after the status"};
    }
    if (!is_valid_utf8(reason)) {
        return status{rpc_code::invalid_argument,
                      "a Close reason must be valid UTF-8"};
    }
    std::string payload(2u + reason.size(), '\0');
    crpc::endian::store_be16(payload.data(), raw);
    if (!reason.empty()) {
        std::memcpy(payload.data() + 2, reason.data(), reason.size());
    }
    const status sent = emit_frame(opcode::close, as_bytes(payload), true);
    if (!sent) {
        return sent;
    }
    impl_->close_sent = true;
    return status{};
}

status stream::close() { return send_close(close_code::normal, {}); }

void stream::abort() noexcept {
    if (!impl_) {
        return;
    }
    impl_->aborted = true;
    if (impl_->link) {
        impl_->link->close();
    }
}

bool stream::is_open() const noexcept {
    return impl_ != nullptr && !impl_->aborted && impl_->link != nullptr;
}

status stream::emit_frame(opcode op, span<const std::byte> payload, bool fin) {
    frame_header header;
    header.fin = fin;
    header.op = op;
    header.masked = impl_->options.is_client;
    std::string wire;
    const status encoded = encode_frame(header, payload, wire);
    if (!encoded) {
        return encoded;
    }
    return impl_->link->write(as_bytes(wire));
}

} // namespace ws
} // namespace crpc