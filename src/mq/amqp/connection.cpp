// CRPC -- synchronous AMQP 0-9-1 client implementation.
#include <crpc/mq/amqp/connection.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include <crpc/net/connection.hpp>

namespace crpc {
namespace mq {
namespace amqp {
namespace {

[[nodiscard]] span<const std::byte> span_of(const std::vector<std::byte>& data) noexcept {
    return span<const std::byte>(data.data(), data.size());
}

[[nodiscard]] span<const std::byte> span_of(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

[[nodiscard]] std::vector<std::byte> copy_bytes(span<const std::byte> data) {
    if (data.empty()) {
        return {};
    }
    return std::vector<std::byte>(data.begin(), data.end());
}

[[nodiscard]] bool method_in_list(std::initializer_list<std::uint16_t> methods,
                                  std::uint16_t method) noexcept {
    for (const std::uint16_t candidate : methods) {
        if (candidate == method) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint8_t bits_of(std::initializer_list<bool> values) noexcept {
    std::uint8_t bits = 0;
    std::uint8_t bit = 0;
    for (const bool value : values) {
        if (value) {
            bits = static_cast<std::uint8_t>(bits | (1u << bit));
        }
        ++bit;
    }
    return bits;
}

struct close_info {
    std::uint16_t reply_code = 0;
    std::string reply_text;
    std::uint16_t class_id = 0;
    std::uint16_t method_id = 0;
};

[[nodiscard]] bool parse_close(span<const std::byte> arguments, close_info& out) {
    reader in(arguments);
    out.reply_code = in.u16();
    out.reply_text = std::string(in.short_string());
    out.class_id = in.u16();
    out.method_id = in.u16();
    return in.ok();
}

[[nodiscard]] rpc_error close_error(const char* what, const close_info& info) {
    std::string message(what);
    message += " (";
    message += std::to_string(info.reply_code);
    message += "): ";
    message += info.reply_text;
    return rpc_error{map_reply_code(info.reply_code), std::move(message)};
}

// The locale list is space separated. "en_US" is what RabbitMQ offers; a
// different server may offer another first locale, which is equally valid.
[[nodiscard]] std::string pick_locale(std::string_view locales) {
    if (locales.find("en_US") != std::string_view::npos) {
        return "en_US";
    }
    const std::size_t end = locales.find(' ');
    const std::string_view first =
        end == std::string_view::npos ? locales : locales.substr(0, end);
    return first.empty() ? std::string("en_US") : std::string(first);
}

void append_client_properties(writer& out, const connection_options& options) {
    field_table capabilities;
    capabilities.set("publisher_confirms", field_value::make_bool(true));
    capabilities.set("basic.nack", field_value::make_bool(true));
    capabilities.set("consumer_cancel_notify", field_value::make_bool(true));
    capabilities.set("authentication_failure_close", field_value::make_bool(true));

    field_table properties;
    properties.set("product", field_value::make_string(options.client_product));
    properties.set("version", field_value::make_string(options.client_version));
    properties.set("platform", field_value::make_string("crpc"));
    properties.set("information", field_value::make_string("CRPC AMQP 0-9-1 client"));
    properties.set("capabilities", field_value::make_table(std::move(capabilities)));
    out.table(properties);
}

} // namespace

// --- connection --------------------------------------------------------------

connection::connection(connection_options options, std::unique_ptr<db::channel> link)
    : options_(std::move(options)), link_(std::move(link)) {
    last_write_ = std::chrono::steady_clock::now();
    if (link_ == nullptr) {
        connect_error_ = rpc_error{rpc_code::shutting_down, "no channel was supplied"};
        return;
    }
    const status opened = perform_handshake();
    if (!opened) {
        connect_error_ = opened.error();
        fail();
        return;
    }
    open_.store(true, std::memory_order_release);
    start_heartbeat();
}

connection::~connection() {
    stop_heartbeat();
    if (link_ != nullptr) {
        link_->close();
    }
}

result<std::unique_ptr<connection>> connection::open(connection_options options,
                                                    crpc::backend_factory& backend,
                                                    crpc::runtime* shard) {
    crpc::net::connection_config config;
    config.ep = crpc::endpoint::tcp(options.host, options.port);
    config.operation_timeout = options.operation_timeout;
    // An AMQP link cannot be reconnected behind the caller's back: the handshake
    // and every channel's state would be gone. A close is terminal instead.
    config.reconnect_on_eof = false;

    auto link = std::unique_ptr<db::channel>(
        new db::socket_channel(std::move(config), backend, shard));
    std::unique_ptr<connection> conn(
        new connection(std::move(options), std::move(link)));
    if (!conn->is_open()) {
        return conn->connect_error();
    }
    return std::move(conn);
}

void connection::fail() noexcept {
    open_.store(false, std::memory_order_release);
    if (link_ != nullptr) {
        link_->close();
    }
}

status connection::send_bytes(span<const std::byte> data) {
    if (link_ == nullptr) {
        return status{rpc_code::shutting_down, "the AMQP connection has no channel"};
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    const status sent = link_->write(data);
    if (!sent) {
        fail();
        return sent;
    }
    last_write_ = std::chrono::steady_clock::now();
    return status{};
}

status connection::send_method(std::uint16_t channel_id, std::uint16_t method_class,
                               std::uint16_t method, span<const std::byte> arguments) {
    std::vector<std::byte> frame_data;
    frame_data.reserve(frame_overhead + 4 + arguments.size());
    append_method_frame(frame_data, channel_id, method_class, method, arguments);
    return send_bytes(span_of(frame_data));
}

status connection::perform_handshake() {
    const status header = send_bytes(
        span<const std::byte>(protocol_header.data(), protocol_header.size()));
    if (!header) {
        return header;
    }

    // connection.start
    auto start_arguments = await_method(0, class_id::connection, {method_id::start}, nullptr);
    if (!start_arguments) {
        return status{start_arguments.error()};
    }
    reader start(span_of(start_arguments.value()));
    const std::uint8_t version_major = start.u8();
    const std::uint8_t version_minor = start.u8();
    if (!start.read_table(server_properties_)) {
        return status{start.error(), "the server properties are not a valid field table"};
    }
    const std::string_view mechanisms = start.long_string();
    const std::string_view locales = start.long_string();
    if (!start.ok()) {
        return status{start.error(), "Connection.Start arguments are malformed"};
    }
    if (version_major != 0 || version_minor != 9) {
        return status{rpc_code::protocol_error, "the broker did not start with AMQP 0-9"};
    }
    if (mechanisms.find("PLAIN") == std::string_view::npos) {
        return status{rpc_code::no_such_method, "the broker does not offer SASL PLAIN"};
    }

    const field_value* product = server_properties_.find("product");
    const field_value* version = server_properties_.find("version");
    server_version_.clear();
    if (product != nullptr) {
        server_version_ = product->text;
    }
    if (version != nullptr) {
        if (!server_version_.empty()) {
            server_version_.push_back(' ');
        }
        server_version_ += version->text;
    }

    // connection.start-ok: plain SASL response is NUL user NUL password.
    writer start_ok;
    append_client_properties(start_ok, options_);
    start_ok.short_string("PLAIN");
    std::string response;
    response.push_back('\0');
    response += options_.user;
    response.push_back('\0');
    response += options_.password;
    start_ok.long_string(response);
    start_ok.short_string(pick_locale(locales));
    if (!start_ok.ok()) {
        return status{start_ok.error(), "Connection.StartOk arguments do not fit"};
    }
    const status start_ok_sent = send_method(0, class_id::connection,
                                             method_id::start_ok, span_of(start_ok.data()));
    if (!start_ok_sent) {
        return start_ok_sent;
    }

    // connection.tune
    auto tune_arguments = await_method(0, class_id::connection, {method_id::tune}, nullptr);
    if (!tune_arguments) {
        return status{tune_arguments.error()};
    }
    reader tune(span_of(tune_arguments.value()));
    const std::uint16_t server_channel_max = tune.u16();
    const std::uint32_t server_frame_max = tune.u32();
    const std::uint16_t server_heartbeat = tune.u16();
    if (!tune.ok()) {
        return status{tune.error(), "Connection.Tune arguments are malformed"};
    }

    // Zero on the server side means "no limit"; the client's own value is a cap,
    // not a request the server can ignore.
    const std::uint16_t unlimited_channels = 65535;
    if (options_.channel_max != 0) {
        channel_max_ = server_channel_max == 0
                           ? options_.channel_max
                           : std::min(server_channel_max, options_.channel_max);
    } else {
        channel_max_ = server_channel_max == 0 ? unlimited_channels : server_channel_max;
    }

    const std::uint32_t unlimited_frame = default_frame_max;
    if (options_.frame_max != 0) {
        frame_max_ = server_frame_max == 0
                         ? options_.frame_max
                         : std::min(server_frame_max, options_.frame_max);
    } else {
        frame_max_ = server_frame_max == 0 ? unlimited_frame : server_frame_max;
    }
    if (frame_max_ <= frame_overhead) {
        return status{rpc_code::failed_precondition,
                      "the negotiated frame_max cannot hold a frame"};
    }

    if (server_heartbeat == 0) {
        heartbeat_ = 0;
    } else if (options_.heartbeat == 0) {
        heartbeat_ = server_heartbeat;
    } else {
        heartbeat_ = std::min(server_heartbeat, options_.heartbeat);
    }

    writer tune_ok;
    tune_ok.u16(channel_max_);
    tune_ok.u32(frame_max_);
    tune_ok.u16(heartbeat_);
    const status tune_ok_sent = send_method(0, class_id::connection,
                                            method_id::tune_ok, span_of(tune_ok.data()));
    if (!tune_ok_sent) {
        return tune_ok_sent;
    }

    // connection.open
    writer open;
    open.short_string(options_.virtual_host);
    open.short_string("");    // reserved-1
    open.bits(0, 1);          // reserved-2: insist = false
    if (!open.ok()) {
        return status{open.error(), "Connection.Open arguments do not fit"};
    }
    const status open_sent = send_method(0, class_id::connection, method_id::open,
                                         span_of(open.data()));
    if (!open_sent) {
        return open_sent;
    }
    auto open_ok = await_method(0, class_id::connection, {method_id::open_ok}, nullptr);
    if (!open_ok) {
        return status{open_ok.error()};
    }
    return status{};
}

result<frame> connection::read_frame() {
    if (link_ == nullptr) {
        return rpc_error{rpc_code::shutting_down, "the AMQP connection has no channel"};
    }
    auto header_bytes = link_->read_exact(7);
    if (!header_bytes) {
        if (header_bytes.error().code != rpc_code::deadline_exceeded) {
            fail();
        }
        return header_bytes.error();
    }
    frame_header header;
    const status decoded = decode_frame_header(
        span<const std::byte>(header_bytes.value().data(), header_bytes.value().size()),
        header);
    if (!decoded) {
        fail();
        return decoded.error();
    }
    if (static_cast<std::size_t>(header.size) > options_.max_inbound_frame) {
        fail();
        return rpc_error{rpc_code::resource_exhausted,
                         "an inbound AMQP frame exceeds max_inbound_frame"};
    }
    auto payload = link_->read_exact(static_cast<std::size_t>(header.size));
    if (!payload) {
        if (payload.error().code != rpc_code::deadline_exceeded) {
            fail();
        }
        return payload.error();
    }
    auto end = link_->read_exact(1);
    if (!end) {
        if (end.error().code != rpc_code::deadline_exceeded) {
            fail();
        }
        return end.error();
    }
    if (end.value().empty() || end.value()[0] != frame_end_octet) {
        fail();
        return rpc_error{rpc_code::protocol_error, "the AMQP frame-end octet is not 0xCE"};
    }
    frame out;
    out.type = header.type;
    out.channel = header.channel;
    out.payload = std::move(payload.value());
    return out;
}

result<bool> connection::handle_incoming(std::uint16_t frame_channel,
                                         std::uint16_t method_class,
                                         std::uint16_t method,
                                         span<const std::byte> arguments) {
    if (method_class == class_id::connection && method == method_id::close) {
        close_info info;
        if (!parse_close(arguments, info)) {
            fail();
            return rpc_error{rpc_code::protocol_error, "Connection.Close arguments are malformed"};
        }
        // The reply is required even though the link is about to end.
        (void)send_method(0, class_id::connection, method_id::close_ok,
                          span<const std::byte>());
        open_.store(false, std::memory_order_release);
        if (link_ != nullptr) {
            link_->close();
        }
        return close_error("the broker closed the connection", info);
    }

    if (method_class == class_id::channel && method == method_id::channel_close) {
        close_info info;
        if (!parse_close(arguments, info)) {
            fail();
            return rpc_error{rpc_code::protocol_error, "Channel.Close arguments are malformed"};
        }
        (void)send_method(frame_channel, class_id::channel, method_id::channel_close_ok,
                          span<const std::byte>());
        closed_channels_.insert(frame_channel);
        channel_errors_[frame_channel] = close_error("the broker closed the channel", info);
        return true;
    }

    if (method_class == class_id::basic && method == method_id::basic_deliver) {
        reader in(arguments);
        delivery delivered;
        delivered.consumer_tag = std::string(in.short_string());
        delivered.delivery_tag = in.u64();
        std::uint8_t flags = 0;
        if (!in.bits(1, flags)) {
            fail();
            return rpc_error{rpc_code::protocol_error, "Basic.Deliver flags are malformed"};
        }
        delivered.redelivered = (flags & 0x01u) != 0;
        delivered.exchange = std::string(in.short_string());
        delivered.routing_key = std::string(in.short_string());
        if (!in.ok()) {
            fail();
            return rpc_error{rpc_code::protocol_error, "Basic.Deliver arguments are malformed"};
        }
        const status content =
            read_content(frame_channel, delivered.properties, delivered.body);
        if (!content) {
            return content.error();
        }
        delivered.present = true;
        deliveries_[frame_channel].push_back(std::move(delivered));
        return true;
    }

    if (method_class == class_id::basic && method == method_id::basic_return) {
        reader in(arguments);
        returned_message returned;
        returned.reply_code = in.u16();
        returned.reply_text = std::string(in.short_string());
        returned.exchange = std::string(in.short_string());
        returned.routing_key = std::string(in.short_string());
        if (!in.ok()) {
            fail();
            return rpc_error{rpc_code::protocol_error, "Basic.Return arguments are malformed"};
        }
        const status content =
            read_content(frame_channel, returned.properties, returned.body);
        if (!content) {
            return content.error();
        }
        returned.present = true;
        returns_.push_back(std::move(returned));
        return true;
    }

    if (method_class == class_id::basic && method == method_id::basic_cancel) {
        // A server-initiated cancel (consumer_cancel_notify). This client does not
        // advertise a handler, but consuming the frame keeps the stream in sync.
        return true;
    }

    if (method_class == class_id::connection &&
        (method == 60 || method == 61)) { // blocked / unblocked
        return true;
    }
    return false;
}

result<std::vector<std::byte>> connection::await_method(
    std::uint16_t channel_id, std::uint16_t method_class,
    std::initializer_list<std::uint16_t> methods, std::uint16_t* matched) {
    for (;;) {
        for (auto it = pending_methods_.begin(); it != pending_methods_.end(); ++it) {
            if (it->channel == channel_id && it->class_id == method_class &&
                method_in_list(methods, it->method_id)) {
                if (matched != nullptr) {
                    *matched = it->method_id;
                }
                std::vector<std::byte> arguments = std::move(it->arguments);
                pending_methods_.erase(it);
                return arguments;
            }
        }

        auto incoming = read_frame();
        if (!incoming) {
            return incoming.error();
        }
        if (incoming.value().type == frame_type::heartbeat) {
            continue;
        }
        if (incoming.value().type != frame_type::method) {
            fail();
            return rpc_error{rpc_code::protocol_error,
                             "expected a method frame while waiting for a reply"};
        }
        const span<const std::byte> payload = span_of(incoming.value().payload);
        if (payload.size() < 4) {
            fail();
            return rpc_error{rpc_code::protocol_error, "a method frame is shorter than its header"};
        }
        reader head(payload);
        const std::uint16_t incoming_class = head.u16();
        const std::uint16_t incoming_method = head.u16();
        if (!head.ok()) {
            fail();
            return head.error();
        }
        const span<const std::byte> arguments = payload.subspan(4);
        auto handled = handle_incoming(incoming.value().channel, incoming_class,
                                       incoming_method, arguments);
        if (!handled) {
            return handled.error();
        }
        if (handled.value()) {
            const auto error = channel_errors_.find(channel_id);
            if (error != channel_errors_.end()) {
                return error->second;
            }
            continue;
        }
        if (incoming.value().channel == channel_id && incoming_class == method_class &&
            method_in_list(methods, incoming_method)) {
            if (matched != nullptr) {
                *matched = incoming_method;
            }
            return copy_bytes(arguments);
        }
        queued_method queued;
        queued.channel = incoming.value().channel;
        queued.class_id = incoming_class;
        queued.method_id = incoming_method;
        queued.arguments = copy_bytes(arguments);
        pending_methods_.push_back(std::move(queued));
    }
}

status connection::read_content(std::uint16_t expected_channel,
                                message_properties& properties,
                                std::vector<std::byte>& body) {
    for (;;) {
        auto incoming = read_frame();
        if (!incoming) {
            return status{incoming.error()};
        }
        if (incoming.value().type == frame_type::heartbeat) {
            continue;
        }
        if (incoming.value().type != frame_type::header) {
            fail();
            return status{rpc_code::protocol_error,
                          "expected a content header frame after a method frame"};
        }
        if (incoming.value().channel != expected_channel) {
            fail();
            return status{rpc_code::protocol_error,
                          "the content header is on a different channel"};
        }
        reader in(span_of(incoming.value().payload));
        const std::uint16_t content_class = in.u16();
        const std::uint16_t weight = in.u16();
        const std::uint64_t body_size = in.u64();
        if (!in.ok()) {
            fail();
            return status{in.error(), "the content header is malformed"};
        }
        if (content_class != class_id::basic) {
            fail();
            return status{rpc_code::protocol_error,
                          "the content header class is not Basic"};
        }
        if (weight != 0) {
            fail();
            return status{rpc_code::protocol_error, "the content weight is not zero"};
        }
        if (!in.read_properties(properties)) {
            fail();
            return status{in.error(), "the content properties are malformed"};
        }
        if (body_size > static_cast<std::uint64_t>(options_.max_body_size)) {
            fail();
            return status{rpc_code::resource_exhausted,
                          "the content body exceeds max_body_size"};
        }
        body.clear();
        body.reserve(static_cast<std::size_t>(
            std::min<std::uint64_t>(body_size, 1024u * 1024u)));
        while (static_cast<std::uint64_t>(body.size()) < body_size) {
            auto part = read_frame();
            if (!part) {
                return status{part.error()};
            }
            if (part.value().type == frame_type::heartbeat) {
                continue;
            }
            if (part.value().type != frame_type::body ||
                part.value().channel != expected_channel) {
                fail();
                return status{rpc_code::protocol_error,
                              "a body frame is missing or on the wrong channel"};
            }
            if (static_cast<std::uint64_t>(body.size() + part.value().payload.size()) >
                body_size) {
                fail();
                return status{rpc_code::protocol_error,
                              "the content body is longer than its header announced"};
            }
            body.insert(body.end(), part.value().payload.begin(),
                        part.value().payload.end());
        }
        return status{};
    }
}

bool connection::channel_closed(std::uint16_t id) const noexcept {
    return closed_channels_.find(id) != closed_channels_.end();
}

result<delivery> connection::wait_delivery(std::uint16_t id,
                                           std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto pending = deliveries_.find(id);
        if (pending != deliveries_.end() && !pending->second.empty()) {
            delivery next = std::move(pending->second.front());
            pending->second.pop_front();
            return next;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            delivery none;
            return none;
        }
        auto incoming = read_frame();
        if (!incoming) {
            if (incoming.error().code == rpc_code::deadline_exceeded) {
                continue;
            }
            return incoming.error();
        }
        if (incoming.value().type == frame_type::heartbeat) {
            continue;
        }
        if (incoming.value().type != frame_type::method) {
            fail();
            return rpc_error{rpc_code::protocol_error,
                             "expected a method frame while waiting for a delivery"};
        }
        const span<const std::byte> payload = span_of(incoming.value().payload);
        if (payload.size() < 4) {
            fail();
            return rpc_error{rpc_code::protocol_error, "a method frame is shorter than its header"};
        }
        reader head(payload);
        const std::uint16_t incoming_class = head.u16();
        const std::uint16_t incoming_method = head.u16();
        auto handled = handle_incoming(incoming.value().channel, incoming_class,
                                       incoming_method, payload.subspan(4));
        if (!handled) {
            return handled.error();
        }
        if (handled.value()) {
            const auto error = channel_errors_.find(id);
            if (error != channel_errors_.end()) {
                return error->second;
            }
            continue;
        }
        queued_method queued;
        queued.channel = incoming.value().channel;
        queued.class_id = incoming_class;
        queued.method_id = incoming_method;
        queued.arguments = copy_bytes(payload.subspan(4));
        pending_methods_.push_back(std::move(queued));
    }
}

result<returned_message> connection::wait_return(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (!returns_.empty()) {
            returned_message next = std::move(returns_.front());
            returns_.pop_front();
            return next;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            returned_message none;
            return none;
        }
        auto incoming = read_frame();
        if (!incoming) {
            if (incoming.error().code == rpc_code::deadline_exceeded) {
                continue;
            }
            return incoming.error();
        }
        if (incoming.value().type == frame_type::heartbeat) {
            continue;
        }
        if (incoming.value().type != frame_type::method) {
            fail();
            return rpc_error{rpc_code::protocol_error,
                             "expected a method frame while waiting for a return"};
        }
        const span<const std::byte> payload = span_of(incoming.value().payload);
        if (payload.size() < 4) {
            fail();
            return rpc_error{rpc_code::protocol_error, "a method frame is shorter than its header"};
        }
        reader head(payload);
        const std::uint16_t incoming_class = head.u16();
        const std::uint16_t incoming_method = head.u16();
        auto handled = handle_incoming(incoming.value().channel, incoming_class,
                                       incoming_method, payload.subspan(4));
        if (!handled) {
            return handled.error();
        }
        if (!handled.value()) {
            queued_method queued;
            queued.channel = incoming.value().channel;
            queued.class_id = incoming_class;
            queued.method_id = incoming_method;
            queued.arguments = copy_bytes(payload.subspan(4));
            pending_methods_.push_back(std::move(queued));
        }
    }
}

void connection::start_heartbeat() {
    if (heartbeat_ == 0) {
        return;
    }
    // AMQP peers send a heartbeat after roughly half the negotiated timeout;
    // RabbitMQ closes a peer that misses two intervals.
    const std::int64_t half_ms =
        std::max<std::int64_t>(100, static_cast<std::int64_t>(heartbeat_) * 1000 / 2);
    const std::chrono::milliseconds period(half_ms);
    heartbeat_stop_ = false;
    heartbeat_thread_ = std::thread(&connection::heartbeat_loop, this, period);
}

void connection::stop_heartbeat() noexcept {
    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        heartbeat_stop_ = true;
    }
    heartbeat_cv_.notify_all();
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

void connection::heartbeat_loop(std::chrono::milliseconds period) {
    std::unique_lock<std::mutex> lock(heartbeat_mutex_);
    for (;;) {
        if (heartbeat_cv_.wait_for(lock, period, [this] { return heartbeat_stop_; })) {
            return;
        }
        lock.unlock();
        if (open_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> write_lock(write_mutex_);
            if (link_ != nullptr && open_.load(std::memory_order_acquire) &&
                std::chrono::steady_clock::now() - last_write_ >= period) {
                const std::vector<std::byte> heartbeat = encode_frame(
                    frame_type::heartbeat, 0, span<const std::byte>());
                const status sent =
                    link_->write(span<const std::byte>(heartbeat.data(), heartbeat.size()));
                if (sent) {
                    last_write_ = std::chrono::steady_clock::now();
                }
            }
        }
        lock.lock();
    }
}

status connection::close() {
    if (link_ == nullptr) {
        return status{};
    }
    stop_heartbeat();
    if (!open_.load(std::memory_order_acquire)) {
        link_->close();
        return status{};
    }
    writer arguments;
    arguments.u16(200); // reply success
    arguments.short_string("");
    arguments.u16(0);
    arguments.u16(0);
    status closed = send_method(0, class_id::connection, method_id::close,
                                span_of(arguments.data()));
    if (closed) {
        auto reply = await_method(0, class_id::connection, {method_id::close_ok}, nullptr);
        if (!reply) {
            closed = status{reply.error()};
        }
    }
    open_.store(false, std::memory_order_release);
    link_->close();
    return closed;
}

status connection::close_channel(std::uint16_t id) {
    if (!open_.load(std::memory_order_acquire) || channel_closed(id)) {
        closed_channels_.insert(id);
        return status{};
    }
    writer arguments;
    arguments.u16(200);
    arguments.short_string("");
    arguments.u16(0);
    arguments.u16(0);
    status closed = send_method(id, class_id::channel, method_id::channel_close,
                                span_of(arguments.data()));
    if (closed) {
        auto reply = await_method(id, class_id::channel, {method_id::channel_close_ok}, nullptr);
        if (!reply) {
            closed = status{reply.error()};
        }
    }
    closed_channels_.insert(id);
    return closed;
}

result<std::unique_ptr<channel>> connection::open_channel() {
    if (!open_.load(std::memory_order_acquire)) {
        return rpc_error{rpc_code::shutting_down, "the AMQP connection is closed"};
    }
    if (channel_max_ != 0 && next_channel_ > channel_max_) {
        return rpc_error{rpc_code::resource_exhausted, "the broker's channel limit is reached"};
    }
    const std::uint16_t id = next_channel_;
    ++next_channel_;

    writer arguments;
    arguments.short_string(""); // reserved-1
    const status sent = send_method(id, class_id::channel, method_id::channel_open,
                                    span_of(arguments.data()));
    if (!sent) {
        return sent.error();
    }
    auto opened = await_method(id, class_id::channel, {method_id::channel_open_ok}, nullptr);
    if (!opened) {
        return opened.error();
    }
    return std::unique_ptr<channel>(new channel(this, id));
}

// --- channel -----------------------------------------------------------------

channel::channel(connection* owner, std::uint16_t id) noexcept : owner_(owner), id_(id) {}

channel::~channel() {
    if (open_ && owner_ != nullptr && owner_->is_open()) {
        (void)owner_->close_channel(id_);
    }
}

bool channel::is_open() const noexcept {
    return open_ && owner_ != nullptr && owner_->is_open() &&
           !owner_->channel_closed(id_);
}

result<queue_declare_result> channel::queue_declare(std::string_view queue, bool passive,
                                                    bool durable, bool exclusive,
                                                    bool auto_delete,
                                                    const field_table& arguments) {
    if (passive && queue.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "a passive queue.declare needs a queue name"};
    }
    writer out;
    // AMQP 0-9-1 keeps the deprecated access `ticket` short in every Queue and
    // Exchange method and in Basic.Publish/Consume/Get. RabbitMQ's decoder
    // requires it; omitting it shifts the whole argument list and the broker
    // answers with a frame error rather than a method error.
    out.u16(0);
    out.short_string(queue);
    out.bits(bits_of({passive, durable, exclusive, auto_delete, false}), 5);
    out.table(arguments);
    if (!out.ok()) {
        return rpc_error{out.error(), "Queue.Declare arguments do not fit"};
    }
    const status sent = owner_->send_method(id_, class_id::queue, method_id::queue_declare,
                                            span_of(out.data()));
    if (!sent) {
        return sent.error();
    }
    auto reply = owner_->await_method(id_, class_id::queue, {method_id::queue_declare_ok},
                                      nullptr);
    if (!reply) {
        return reply.error();
    }
    reader in(span_of(reply.value()));
    queue_declare_result result;
    result.queue = std::string(in.short_string());
    result.message_count = in.u32();
    result.consumer_count = in.u32();
    if (!in.ok()) {
        return rpc_error{rpc_code::protocol_error, "Queue.DeclareOk arguments are malformed"};
    }
    return result;
}

status channel::queue_delete(std::string_view queue, bool if_unused, bool if_empty) {
    writer out;
    out.u16(0); // ticket
    out.short_string(queue);
    out.bits(bits_of({if_unused, if_empty, false}), 3);
    if (!out.ok()) {
        return status{out.error(), "Queue.Delete arguments do not fit"};
    }
    const status sent = owner_->send_method(id_, class_id::queue, method_id::queue_delete,
                                            span_of(out.data()));
    if (!sent) {
        return sent;
    }
    auto reply = owner_->await_method(id_, class_id::queue, {method_id::queue_delete_ok},
                                      nullptr);
    if (!reply) {
        return status{reply.error()};
    }
    return status{};
}

status channel::queue_bind(std::string_view queue, std::string_view exchange,
                           std::string_view routing_key, const field_table& arguments) {
    writer out;
    out.u16(0); // ticket
    out.short_string(queue);
    out.short_string(exchange);
    out.short_string(routing_key);
    out.bits(0, 1); // no-wait = false
    out.table(arguments);
    if (!out.ok()) {
        return status{out.error(), "Queue.Bind arguments do not fit"};
    }
    const status sent = owner_->send_method(id_, class_id::queue, method_id::queue_bind,
                                            span_of(out.data()));
    if (!sent) {
        return sent;
    }
    auto reply = owner_->await_method(id_, class_id::queue, {method_id::queue_bind_ok},
                                      nullptr);
    if (!reply) {
        return status{reply.error()};
    }
    return status{};
}

status channel::exchange_declare(std::string_view exchange, std::string_view type,
                                 bool passive, bool durable, bool auto_delete,
                                 bool internal, const field_table& arguments) {
    writer out;
    out.u16(0); // ticket
    out.short_string(exchange);
    out.short_string(type);
    out.bits(bits_of({passive, durable, auto_delete, internal, false}), 5);
    out.table(arguments);
    if (!out.ok()) {
        return status{out.error(), "Exchange.Declare arguments do not fit"};
    }
    const status sent = owner_->send_method(id_, class_id::exchange,
                                            method_id::exchange_declare,
                                            span_of(out.data()));
    if (!sent) {
        return sent;
    }
    auto reply = owner_->await_method(id_, class_id::exchange,
                                      {method_id::exchange_declare_ok}, nullptr);
    if (!reply) {
        return status{reply.error()};
    }
    return status{};
}

status channel::basic_publish(std::string_view exchange, std::string_view routing_key,
                              const message_properties& properties,
                              span<const std::byte> body, bool mandatory,
                              bool immediate) {
    writer arguments;
    arguments.u16(0); // ticket
    arguments.short_string(exchange);
    arguments.short_string(routing_key);
    arguments.bits(bits_of({mandatory, immediate}), 2);
    if (!arguments.ok()) {
        return status{arguments.error(), "Basic.Publish arguments do not fit"};
    }

    // One buffer: a concurrent heartbeat must not land between the method frame
    // and its content frames.
    std::vector<std::byte> frames;
    append_method_frame(frames, id_, class_id::basic, method_id::basic_publish,
                        span_of(arguments.data()));
    const status content = append_content_frames(frames, id_, class_id::basic,
                                                 properties, body, owner_->frame_max());
    if (!content) {
        return content;
    }
    return owner_->send_bytes(span_of(frames));
}

result<get_result> channel::basic_get(std::string_view queue, bool no_ack) {
    writer out;
    out.u16(0); // ticket
    out.short_string(queue);
    out.bits(bits_of({no_ack}), 1);
    if (!out.ok()) {
        return rpc_error{out.error(), "Basic.Get arguments do not fit"};
    }
    const status sent = owner_->send_method(id_, class_id::basic, method_id::basic_get,
                                            span_of(out.data()));
    if (!sent) {
        return sent.error();
    }
    std::uint16_t matched = 0;
    auto reply = owner_->await_method(
        id_, class_id::basic, {method_id::basic_get_ok, method_id::basic_get_empty},
        &matched);
    if (!reply) {
        return reply.error();
    }
    if (matched == method_id::basic_get_empty) {
        get_result empty;
        empty.empty = true;
        return empty;
    }

    reader in(span_of(reply.value()));
    get_result result;
    result.empty = false;
    result.delivery_tag = in.u64();
    std::uint8_t flags = 0;
    if (!in.bits(1, flags)) {
        return rpc_error{rpc_code::protocol_error, "Basic.GetOk flags are malformed"};
    }
    result.redelivered = (flags & 0x01u) != 0;
    result.exchange = std::string(in.short_string());
    result.routing_key = std::string(in.short_string());
    result.message_count = in.u32();
    if (!in.ok()) {
        return rpc_error{rpc_code::protocol_error, "Basic.GetOk arguments are malformed"};
    }
    const status content =
        owner_->read_content(id_, result.properties, result.body);
    if (!content) {
        return content.error();
    }
    return result;
}

result<std::string> channel::basic_consume(std::string_view queue,
                                           std::string_view consumer_tag, bool no_ack,
                                           const field_table& arguments) {
    writer out;
    out.u16(0); // ticket
    out.short_string(queue);
    out.short_string(consumer_tag);
    // no-local, no-ack, exclusive, no-wait. RabbitMQ rejects no-local=true, so it
    // is always false here.
    out.bits(bits_of({false, no_ack, false, false}), 4);
    out.table(arguments);
    if (!out.ok()) {
        return rpc_error{out.error(), "Basic.Consume arguments do not fit"};
    }
    const status sent = owner_->send_method(id_, class_id::basic, method_id::basic_consume,
                                            span_of(out.data()));
    if (!sent) {
        return sent.error();
    }
    auto reply = owner_->await_method(id_, class_id::basic,
                                      {method_id::basic_consume_ok}, nullptr);
    if (!reply) {
        return reply.error();
    }
    reader in(span_of(reply.value()));
    const std::string tag(in.short_string());
    if (!in.ok()) {
        return rpc_error{rpc_code::protocol_error, "Basic.ConsumeOk is malformed"};
    }
    return tag;
}

status channel::basic_cancel(std::string_view consumer_tag, bool no_wait) {
    writer out;
    out.short_string(consumer_tag);
    out.bits(bits_of({no_wait}), 1);
    if (!out.ok()) {
        return status{out.error(), "Basic.Cancel arguments do not fit"};
    }
    const status sent = owner_->send_method(id_, class_id::basic, method_id::basic_cancel,
                                            span_of(out.data()));
    if (!sent || no_wait) {
        return sent;
    }
    auto reply = owner_->await_method(id_, class_id::basic, {method_id::basic_cancel_ok},
                                      nullptr);
    if (!reply) {
        return status{reply.error()};
    }
    return status{};
}

status channel::basic_ack(std::uint64_t delivery_tag, bool multiple) {
    writer out;
    out.u64(delivery_tag);
    out.bits(bits_of({multiple}), 1);
    return owner_->send_method(id_, class_id::basic, method_id::basic_ack,
                               span_of(out.data()));
}

status channel::basic_nack(std::uint64_t delivery_tag, bool multiple, bool requeue) {
    writer out;
    out.u64(delivery_tag);
    out.bits(bits_of({multiple, requeue}), 2);
    return owner_->send_method(id_, class_id::basic, method_id::basic_nack,
                               span_of(out.data()));
}

status channel::basic_reject(std::uint64_t delivery_tag, bool requeue) {
    writer out;
    out.u64(delivery_tag);
    out.bits(bits_of({requeue}), 1);
    return owner_->send_method(id_, class_id::basic, method_id::basic_reject,
                               span_of(out.data()));
}

status channel::basic_qos(std::uint16_t prefetch_count, std::uint32_t prefetch_size,
                          bool global) {
    writer out;
    out.u32(prefetch_size);
    out.u16(prefetch_count);
    out.bits(bits_of({global}), 1);
    const status sent = owner_->send_method(id_, class_id::basic, method_id::basic_qos,
                                            span_of(out.data()));
    if (!sent) {
        return sent;
    }
    auto reply = owner_->await_method(id_, class_id::basic, {method_id::basic_qos_ok},
                                      nullptr);
    if (!reply) {
        return status{reply.error()};
    }
    return status{};
}

result<delivery> channel::next_delivery(std::chrono::milliseconds timeout) {
    if (owner_ == nullptr) {
        return rpc_error{rpc_code::shutting_down, "the channel has no connection"};
    }
    return owner_->wait_delivery(id_, timeout);
}

result<returned_message> channel::next_return(std::chrono::milliseconds timeout) {
    if (owner_ == nullptr) {
        return rpc_error{rpc_code::shutting_down, "the channel has no connection"};
    }
    return owner_->wait_return(timeout);
}

status channel::close() {
    if (owner_ == nullptr) {
        open_ = false;
        return status{};
    }
    if (!open_) {
        return status{};
    }
    open_ = false;
    return owner_->close_channel(id_);
}

} // namespace amqp
} // namespace mq
} // namespace crpc