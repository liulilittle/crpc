// CRPC -- synchronous AMQP 0-9-1 client implementation.
#include <crpc/mq/amqp/connection.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>
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

std::vector<std::size_t> node_visit_order(std::size_t count,
                                          std::size_t last_success) noexcept {
    std::vector<std::size_t> order;
    if (count == 0) {
        return order;
    }
    order.reserve(count);
    if (last_success < count) {
        order.push_back(last_success);
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (i != last_success) {
            order.push_back(i);
        }
    }
    return order;
}

std::chrono::milliseconds node_backoff_delay(std::size_t attempt,
                                             std::chrono::milliseconds base,
                                             std::chrono::milliseconds cap,
                                             std::uint64_t jitter_basis) noexcept {
    const auto base_ms = base.count() > 0 ? static_cast<std::uint64_t>(base.count()) : 0u;
    if (base_ms == 0) {
        return std::chrono::milliseconds(0);
    }
    std::uint64_t backoff = base_ms;
    for (std::size_t i = 0; i < attempt && backoff < std::numeric_limits<std::uint64_t>::max() / 2; ++i) {
        backoff *= 2;
    }
    const std::uint64_t cap_ms =
        cap.count() > 0 ? static_cast<std::uint64_t>(cap.count()) : backoff;
    const std::uint64_t bounded = std::min(backoff, cap_ms);
    // Jitter draws from [0, base) so a fleet of clients does not stampede in
    // lockstep after a shared outage.
    const std::uint64_t jitter = jitter_basis % base_ms;
    const std::uint64_t total = bounded + jitter;
    return std::chrono::milliseconds(static_cast<std::int64_t>(
        total > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            : total));
}

// --- connection --------------------------------------------------------------

connection::connection(connection_options options, std::unique_ptr<db::channel> link)
    : options_(std::move(options)), link_(std::move(link)) {
    if (options_.nodes.empty()) {
        options_.nodes.push_back({options_.host, options_.port});
    }
    nodes_ = options_.nodes;
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
    std::vector<node_address> candidates = options.nodes;
    if (candidates.empty()) {
        candidates.push_back({options.host, options.port});
    }

    rpc_error last_error{rpc_code::unavailable,
                         "every broker in the node list refused the connection"};
    for (const std::size_t index : node_visit_order(candidates.size(), 0)) {
        connection_options attempt = options;
        attempt.host = candidates[index].host;
        attempt.port = candidates[index].port;

        crpc::net::connection_config config;
        config.ep = crpc::endpoint::tcp(attempt.host, attempt.port);
        config.operation_timeout = attempt.operation_timeout;
        // An AMQP link cannot be reconnected behind the caller's back: the
        // handshake and every channel's state would be gone. A close is
        // terminal instead, and failover happens at this client's level.
        config.reconnect_on_eof = false;

        auto link = std::unique_ptr<db::channel>(
            new db::socket_channel(std::move(config), backend, shard));
        std::unique_ptr<connection> conn(
            new connection(std::move(attempt), std::move(link)));
        if (conn->is_open()) {
            conn->backend_ = &backend;
            conn->shard_ = shard;
            conn->nodes_ = std::move(candidates);
            conn->set_node(index);
            return std::move(conn);
        }
        last_error = conn->connect_error();
    }
    return last_error;
}

void connection::fail() noexcept {
    open_.store(false, std::memory_order_release);
    if (link_ != nullptr) {
        link_->close();
    }
}

const node_address& connection::active_node() const noexcept {
    if (node_index_ < nodes_.size()) {
        return nodes_[node_index_];
    }
    static const node_address none;
    return none;
}

void connection::set_node(std::size_t index) noexcept {
    node_index_ = index;
    last_success_index_ = index;
}

status connection::ensure_connected() {
    if (user_closed_) {
        return status{rpc_code::shutting_down, "the AMQP connection is closed"};
    }
    if (open_.load(std::memory_order_acquire) && link_ != nullptr) {
        return status{};
    }
    return reconnect_with_backoff();
}

status connection::reconnect() {
    if (user_closed_) {
        return status{rpc_code::shutting_down, "the AMQP connection is closed"};
    }
    if (open_.load(std::memory_order_acquire) && link_ != nullptr) {
        return status{};
    }
    return reconnect_with_backoff();
}

status connection::reconnect_with_backoff() {
    if (backend_ == nullptr) {
        return status{rpc_code::unavailable,
                      "this connection cannot reconnect: it was not created by "
                      "connection::open, so no transport backend is available"};
    }
    if (nodes_.empty()) {
        return status{rpc_code::internal, "the connection has no configured broker"};
    }

    std::uint64_t jitter_basis = 0;
    for (std::size_t attempt = 0; attempt <= options_.reconnect_attempts; ++attempt) {
        for (const std::size_t index : node_visit_order(nodes_.size(), last_success_index_)) {
            const status attached = attach_to_node(index);
            if (attached.ok()) {
                connect_error_ = rpc_error{};
                const status replayed = replay_channels();
                if (!replayed) {
                    last_reconnect_error_ = replayed.error();
                    fail();
                    return replayed;
                }
                return status{};
            }
            last_reconnect_error_ = attached.error();
        }
        if (attempt < options_.reconnect_attempts) {
            const auto delay = node_backoff_delay(attempt, options_.reconnect_delay_base,
                                                  options_.reconnect_delay_cap,
                                                  jitter_basis++);
            if (delay.count() > 0) {
                std::this_thread::sleep_for(delay);
            }
        }
    }
    return last_reconnect_error_;
}

status connection::attach_to_node(std::size_t index) {
    if (index >= nodes_.size()) {
        return status{rpc_code::invalid_argument, "the node index is out of range"};
    }
    crpc::net::connection_config config;
    config.ep = crpc::endpoint::tcp(nodes_[index].host, nodes_[index].port);
    config.operation_timeout = options_.operation_timeout;
    config.reconnect_on_eof = false;

    auto new_link = std::unique_ptr<db::channel>(
        new db::socket_channel(std::move(config), *backend_, shard_));
    {
        // The heartbeat writer is gated on `open_` (false here), but a frame it
        // started before the text dropped could still be mid-write; the same
        // mutex serialises the transport swap with writers.
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (link_ != nullptr) {
            link_->close();
        }
        link_ = std::move(new_link);
        open_.store(false, std::memory_order_release);
    }
    last_write_ = std::chrono::steady_clock::now();
    reset_protocol_state();

    const status handshake = perform_handshake();
    if (!handshake) {
        if (link_ != nullptr) {
            link_->close();
        }
        return handshake;
    }
    open_.store(true, std::memory_order_release);
    set_node(index);
    return status{};
}

void connection::reset_protocol_state() noexcept {
    pending_methods_.clear();
    deliveries_.clear();
    returns_.clear();
    closed_channels_.clear();
    channel_errors_.clear();
    confirm_events_.clear();
    consumer_cancels_.clear();
    confirm_results_.clear();
    next_channel_ = 1;
    {
        // The heartbeat thread reads the interval every cycle; publish the
        // reset under the same mutex to keep the access race-free.
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        heartbeat_ = 0;
    }
    channel_max_ = 0;
    frame_max_ = default_frame_max;
}

void connection::register_channel(channel* ch) {
    channels_.push_back(ch);
    channels_by_id_[ch->id_] = ch;
}

void connection::unregister_channel(channel* ch) {
    const auto it = std::find(channels_.begin(), channels_.end(), ch);
    if (it != channels_.end()) {
        channels_.erase(it);
    }
    const auto by_id = channels_by_id_.find(ch->id_);
    if (by_id != channels_by_id_.end() && by_id->second == ch) {
        channels_by_id_.erase(by_id);
    }
}

channel* connection::channel_with_id(std::uint16_t id) noexcept {
    const auto it = channels_by_id_.find(id);
    return it != channels_by_id_.end() ? it->second : nullptr;
}

status connection::replay_channels() {
    channels_by_id_.clear();
    for (channel* ch : channels_) {
        if (ch == nullptr || !ch->open_ || ch->dead_) {
            continue;
        }
        const std::uint16_t id = next_channel_;
        if (channel_max_ != 0 && id > channel_max_) {
            return status{rpc_code::resource_exhausted,
                          "the broker's channel limit is reached during reconnect"};
        }
        ++next_channel_;

        writer arguments;
        arguments.short_string("");  // reserved-1
        const status sent = send_method(id, class_id::channel, method_id::channel_open,
                                        span_of(arguments.data()));
        if (!sent) {
            return sent;
        }
        auto opened = await_method(id, class_id::channel, {method_id::channel_open_ok},
                                   nullptr);
        if (!opened) {
            return status{opened.error()};
        }

        const std::uint16_t old_id = ch->id_;
        ch->id_ = id;
        channels_by_id_[id] = ch;
        if (old_id != id) {
            channels_by_id_.erase(old_id);
        }
        const status replayed = ch->replay_topology();
        if (!replayed) {
            return replayed;
        }
    }
    return status{};
}

result<connection::confirm_event> connection::wait_confirm(
    std::uint16_t id, std::uint64_t target_tag, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto& results = confirm_results_[id];
    for (;;) {
        auto& events = confirm_events_[id];
        while (!events.empty()) {
            confirm_event event = events.front();
            events.pop_front();
            if ((event.multiple && event.delivery_tag >= target_tag) ||
                (!event.multiple && event.delivery_tag == target_tag)) {
                // `multiple` settles every outstanding tag up to its own; older
                // events settle nothing this wait is interested in and are
                // dropped (the confirm sequence only moves forward).
                results.clear();
                results[target_tag] = event.acknowledged;
            }
        }
        const auto found = results.find(target_tag);
        if (found != results.end()) {
            confirm_event resolved;
            resolved.delivery_tag = target_tag;
            resolved.acknowledged = found->second;
            results.clear();
            return resolved;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return rpc_error{rpc_code::deadline_exceeded,
                             "timed out waiting for the broker's publisher confirm"};
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
                             "expected a method frame while waiting for a confirm"};
        }
        const span<const std::byte> payload = span_of(incoming.value().payload);
        if (payload.size() < 4) {
            fail();
            return rpc_error{rpc_code::protocol_error,
                             "a method frame is shorter than its header"};
        }
        reader head(payload);
        const std::uint16_t incoming_class = head.u16();
        const std::uint16_t incoming_method = head.u16();
        if (!head.ok()) {
            fail();
            return head.error();
        }
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

result<consumer_cancel> connection::wait_cancel(std::uint16_t id,
                                                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto pending = consumer_cancels_.find(id);
        if (pending != consumer_cancels_.end() && !pending->second.empty()) {
            consumer_cancel out;
            out.present = true;
            out.consumer_tag = std::move(pending->second.front());
            pending->second.pop_front();
            return out;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return consumer_cancel{};
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
                             "expected a method frame while waiting for a cancel"};
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
    server_properties_.clear();
    server_version_.clear();
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

    // The heartbeat thread recomputes its period from this member every cycle,
    // so the negotiated value is published under the same mutex it reads under.
    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        if (server_heartbeat == 0) {
            heartbeat_ = 0;
        } else if (options_.heartbeat == 0) {
            heartbeat_ = server_heartbeat;
        } else {
            heartbeat_ = std::min(server_heartbeat, options_.heartbeat);
        }
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
        // A channel the broker closed is permanent: reconnect must not re-open it.
        if (channel* ch = channel_with_id(frame_channel); ch != nullptr) {
            ch->dead_ = true;
        }
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
        // A server-initiated cancel (consumer_cancel_notify), e.g. because the
        // queue was deleted. Record it per channel so `next_cancel` can report
        // that consumption stopped instead of letting it die silently.
        reader in(arguments);
        const std::string tag(in.short_string());
        if (!in.ok()) {
            fail();
            return rpc_error{rpc_code::protocol_error, "Basic.Cancel arguments are malformed"};
        }
        consumer_cancels_[frame_channel].push_back(tag);
        return true;
    }

    if (method_class == class_id::basic &&
        (method == method_id::basic_ack || method == method_id::basic_nack)) {
        // Publisher-confirm outcome for one or more published messages.
        reader in(arguments);
        confirm_event event;
        event.delivery_tag = in.u64();
        std::uint8_t flags = 0;
        if (!in.bits(method == method_id::basic_ack ? 1u : 2u, flags)) {
            fail();
            return rpc_error{rpc_code::protocol_error,
                             "Basic.Ack/Basic.Nack flags are malformed"};
        }
        event.multiple = (flags & 0x01u) != 0;
        event.acknowledged = method == method_id::basic_ack;
        if (!in.ok()) {
            fail();
            return rpc_error{rpc_code::protocol_error,
                             "Basic.Ack/Basic.Nack arguments are malformed"};
        }
        confirm_events_[frame_channel].push_back(event);
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
    heartbeat_stop_ = false;
    heartbeat_thread_ = std::thread(&connection::heartbeat_loop, this);
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

void connection::heartbeat_loop() {
    std::unique_lock<std::mutex> lock(heartbeat_mutex_);
    for (;;) {
        // The interval is a property of the *current* node and may change after
        // a reconnect, so it is recomputed every iteration instead of captured
        // once. A node that proposes no heartbeat gets a conservative idle poll.
        const std::int64_t asked_ms = static_cast<std::int64_t>(heartbeat_) * 1000 / 2;
        const std::int64_t half_ms =
            heartbeat_ == 0 ? 1000 : (std::max<std::int64_t>(100, asked_ms));
        const std::chrono::milliseconds period(half_ms);
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
    user_closed_ = true;
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
    const status ready = ensure_connected();
    if (!ready) {
        return ready.error();
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
    std::unique_ptr<channel> result(new channel(this, id));
    register_channel(result.get());
    return result;
}

// --- channel -----------------------------------------------------------------

channel::channel(connection* owner, std::uint16_t id) noexcept : owner_(owner), id_(id) {}

channel::~channel() {
    if (owner_ != nullptr) {
        if (open_ && owner_->is_open()) {
            (void)owner_->close_channel(id_);
        }
        owner_->unregister_channel(this);
    }
}

bool channel::is_open() const noexcept {
    return open_ && !dead_ && owner_ != nullptr && owner_->is_open() &&
           !owner_->channel_closed(id_);
}

status channel::ensure_ready() {
    if (owner_ == nullptr) {
        return status{rpc_code::shutting_down, "the channel has no connection"};
    }
    if (!open_) {
        return status{rpc_code::shutting_down, "the channel is closed"};
    }
    if (dead_) {
        return status{rpc_code::connection_reset,
                      "the broker closed this channel; open a new one"};
    }
    return owner_->ensure_connected();
}

status channel::require_live() {
    if (owner_ == nullptr || !open_ || dead_) {
        return status{rpc_code::shutting_down, "the channel is closed"};
    }
    if (!owner_->is_open()) {
        return status{rpc_code::connection_reset,
                      "the connection is down; a delivery from before the drop "
                      "cannot be acknowledged for the new connection generation"};
    }
    return status{};
}

// Sends one method on this channel's current id and waits for one of `replies`.
// Used by the public call and by the reconnect replay, so the wire encoding of
// a declaration stays identical in both paths.
result<std::vector<std::byte>> channel::exchange_method(
    std::uint16_t method_class, std::uint16_t method,
    span<const std::byte> arguments, std::initializer_list<std::uint16_t> replies,
    std::uint16_t* matched) {
    const status sent =
        owner_->send_method(id_, method_class, method, arguments);
    if (!sent) {
        return sent.error();
    }
    return owner_->await_method(id_, method_class, replies, matched);
}

// Builds and writes the Basic.Publish method + content frames in one buffer.
// Does not touch the confirm sequence; the caller advances it once the write
// went out.
status channel::send_publish(std::string_view exchange, std::string_view routing_key,
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

result<queue_declare_result> channel::queue_declare(std::string_view queue, bool passive,
                                                    bool durable, bool exclusive,
                                                    bool auto_delete,
                                                    const field_table& arguments) {
    const status ready = ensure_ready();
    if (!ready) {
        return ready.error();
    }
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
    auto reply = exchange_method(class_id::queue, method_id::queue_declare,
                                 span_of(out.data()), {method_id::queue_declare_ok});
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
    if (!result.queue.empty()) {
        // The resolved name (server-generated names are stored here) is what the
        // reconnect replay declares, so a broker-generated queue keeps its
        // identity across a failover. A `passive` declare is replayed as a real
        // declare: its only meaning was "this queue must exist", and after a
        // broker restart the fastest way to make that true is to declare it.
        queue_state state;
        state.name = result.queue;
        state.passive = passive;
        state.durable = durable;
        state.exclusive = exclusive;
        state.auto_delete = auto_delete;
        state.arguments = arguments;
        queues_[result.queue] = std::move(state);
    }
    return result;
}

status channel::queue_delete(std::string_view queue, bool if_unused, bool if_empty) {
    const status ready = ensure_ready();
    if (!ready) {
        return ready;
    }
    writer out;
    out.u16(0); // ticket
    out.short_string(queue);
    out.bits(bits_of({if_unused, if_empty, false}), 3);
    if (!out.ok()) {
        return status{out.error(), "Queue.Delete arguments do not fit"};
    }
    auto reply = exchange_method(class_id::queue, method_id::queue_delete,
                                 span_of(out.data()), {method_id::queue_delete_ok});
    if (!reply) {
        return status{reply.error()};
    }
    // A deleted queue must not be re-created by a reconnect.
    queues_.erase(std::string(queue));
    return status{};
}

status channel::queue_bind(std::string_view queue, std::string_view exchange,
                           std::string_view routing_key, const field_table& arguments) {
    const status ready = ensure_ready();
    if (!ready) {
        return ready;
    }
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
    auto reply = exchange_method(class_id::queue, method_id::queue_bind,
                                 span_of(out.data()), {method_id::queue_bind_ok});
    if (!reply) {
        return status{reply.error()};
    }
    bind_state state;
    state.queue = std::string(queue);
    state.exchange = std::string(exchange);
    state.routing_key = std::string(routing_key);
    state.arguments = arguments;
    binds_.push_back(std::move(state));
    return status{};
}

status channel::exchange_declare(std::string_view exchange, std::string_view type,
                                 bool passive, bool durable, bool auto_delete,
                                 bool internal, const field_table& arguments) {
    const status ready = ensure_ready();
    if (!ready) {
        return ready;
    }
    writer out;
    out.u16(0); // ticket
    out.short_string(exchange);
    out.short_string(type);
    out.bits(bits_of({passive, durable, auto_delete, internal, false}), 5);
    out.table(arguments);
    if (!out.ok()) {
        return status{out.error(), "Exchange.Declare arguments do not fit"};
    }
    auto reply = exchange_method(class_id::exchange, method_id::exchange_declare,
                                 span_of(out.data()), {method_id::exchange_declare_ok});
    if (!reply) {
        return status{reply.error()};
    }
    exchange_state state;
    state.name = std::string(exchange);
    state.type = std::string(type);
    state.passive = passive;
    state.durable = durable;
    state.auto_delete = auto_delete;
    state.internal = internal;
    state.arguments = arguments;
    exchanges_[state.name] = std::move(state);
    return status{};
}

status channel::basic_publish(std::string_view exchange, std::string_view routing_key,
                              const message_properties& properties,
                              span<const std::byte> body, bool mandatory,
                              bool immediate) {
    const status ready = ensure_ready();
    if (!ready) {
        return ready;
    }
    const status sent = send_publish(exchange, routing_key, properties, body,
                                     mandatory, immediate);
    if (sent && confirm_mode_) {
        // Every Basic.Publish on a confirm-mode channel consumes a sequence
        // number on the broker, whether or not the caller waits for it.
        ++next_publish_tag_;
    }
    return sent;
}

result<publish_confirm> channel::basic_publish_confirm(
    std::string_view exchange, std::string_view routing_key,
    const message_properties& properties, span<const std::byte> body, bool mandatory,
    bool immediate, std::chrono::milliseconds timeout) {
    const status ready = ensure_ready();
    if (!ready) {
        return ready.error();
    }
    if (!confirm_mode_) {
        return rpc_error{rpc_code::failed_precondition,
                         "call confirm_select() before basic_publish_confirm()"};
    }
    const std::uint64_t tag = next_publish_tag_;
    const status sent = send_publish(exchange, routing_key, properties, body,
                                     mandatory, immediate);
    if (!sent) {
        return sent.error();
    }
    ++next_publish_tag_;

    auto resolved = owner_->wait_confirm(id_, tag, timeout);
    if (!resolved) {
        return resolved.error();
    }
    publish_confirm result;
    result.acknowledged = resolved.value().acknowledged;
    result.delivery_tag = tag;
    return result;
}

result<get_result> channel::basic_get(std::string_view queue, bool no_ack) {
    const status ready = ensure_ready();
    if (!ready) {
        return ready.error();
    }
    writer out;
    out.u16(0); // ticket
    out.short_string(queue);
    out.bits(bits_of({no_ack}), 1);
    if (!out.ok()) {
        return rpc_error{out.error(), "Basic.Get arguments do not fit"};
    }
    std::uint16_t matched = 0;
    auto reply = exchange_method(
        class_id::basic, method_id::basic_get, span_of(out.data()),
        {method_id::basic_get_ok, method_id::basic_get_empty}, &matched);
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
    const status ready = ensure_ready();
    if (!ready) {
        return ready.error();
    }
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
    auto reply = exchange_method(class_id::basic, method_id::basic_consume,
                                 span_of(out.data()), {method_id::basic_consume_ok});
    if (!reply) {
        return reply.error();
    }
    reader in(span_of(reply.value()));
    const std::string tag(in.short_string());
    if (!in.ok()) {
        return rpc_error{rpc_code::protocol_error, "Basic.ConsumeOk is malformed"};
    }
    // The granted tag (the broker's generated one when the caller left it
    // empty) is what a reconnect must reuse to re-register this consumer.
    consume_state state;
    state.queue = std::string(queue);
    state.consumer_tag = tag;
    state.no_ack = no_ack;
    state.arguments = arguments;
    consumes_[tag] = std::move(state);
    return tag;
}

status channel::basic_cancel(std::string_view consumer_tag, bool no_wait) {
    const status ready = ensure_ready();
    if (!ready) {
        return ready;
    }
    writer out;
    out.short_string(consumer_tag);
    out.bits(bits_of({no_wait}), 1);
    if (!out.ok()) {
        return status{out.error(), "Basic.Cancel arguments do not fit"};
    }
    if (no_wait) {
        const status sent =
            owner_->send_method(id_, class_id::basic, method_id::basic_cancel,
                                span_of(out.data()));
        if (sent) {
            // A no-wait cancel removes the consumer from the replay set too; the
            // broker has already received the cancel.
            consumes_.erase(std::string(consumer_tag));
        }
        return sent;
    }
    auto reply = exchange_method(class_id::basic, method_id::basic_cancel,
                                 span_of(out.data()), {method_id::basic_cancel_ok});
    if (!reply) {
        return status{reply.error()};
    }
    // A cancelled consumer must not be re-registered by a reconnect.
    consumes_.erase(std::string(consumer_tag));
    return status{};
}

status channel::basic_ack(std::uint64_t delivery_tag, bool multiple) {
    const status live = require_live();
    if (!live) {
        return live;
    }
    writer out;
    out.u64(delivery_tag);
    out.bits(bits_of({multiple}), 1);
    return owner_->send_method(id_, class_id::basic, method_id::basic_ack,
                               span_of(out.data()));
}

status channel::basic_nack(std::uint64_t delivery_tag, bool multiple, bool requeue) {
    const status live = require_live();
    if (!live) {
        return live;
    }
    writer out;
    out.u64(delivery_tag);
    out.bits(bits_of({multiple, requeue}), 2);
    return owner_->send_method(id_, class_id::basic, method_id::basic_nack,
                               span_of(out.data()));
}

status channel::basic_reject(std::uint64_t delivery_tag, bool requeue) {
    const status live = require_live();
    if (!live) {
        return live;
    }
    writer out;
    out.u64(delivery_tag);
    out.bits(bits_of({requeue}), 1);
    return owner_->send_method(id_, class_id::basic, method_id::basic_reject,
                               span_of(out.data()));
}

status channel::basic_qos(std::uint16_t prefetch_count, std::uint32_t prefetch_size,
                          bool global) {
    const status ready = ensure_ready();
    if (!ready) {
        return ready;
    }
    writer out;
    out.u32(prefetch_size);
    out.u16(prefetch_count);
    out.bits(bits_of({global}), 1);
    auto reply = exchange_method(class_id::basic, method_id::basic_qos,
                                 span_of(out.data()), {method_id::basic_qos_ok});
    if (!reply) {
        return status{reply.error()};
    }
    qos_applied_ = true;
    qos_.prefetch_count = prefetch_count;
    qos_.prefetch_size = prefetch_size;
    qos_.global = global;
    return status{};
}

result<delivery> channel::next_delivery(std::chrono::milliseconds timeout) {
    const status ready = ensure_ready();
    if (!ready) {
        return ready.error();
    }
    if (owner_ == nullptr) {
        return rpc_error{rpc_code::shutting_down, "the channel has no connection"};
    }
    return owner_->wait_delivery(id_, timeout);
}

result<returned_message> channel::next_return(std::chrono::milliseconds timeout) {
    const status ready = ensure_ready();
    if (!ready) {
        return ready.error();
    }
    if (owner_ == nullptr) {
        return rpc_error{rpc_code::shutting_down, "the channel has no connection"};
    }
    return owner_->wait_return(timeout);
}

status channel::confirm_select() {
    const status ready = ensure_ready();
    if (!ready) {
        return ready;
    }
    writer out;
    out.bits(0, 1); // no-wait = false
    if (!out.ok()) {
        return status{out.error(), "Confirm.Select arguments do not fit"};
    }
    auto reply = exchange_method(class_id::confirm, method_id::confirm_select,
                                 span_of(out.data()), {method_id::confirm_select_ok});
    if (!reply) {
        return status{reply.error()};
    }
    confirm_mode_ = true;
    next_publish_tag_ = 1;
    return status{};
}

result<consumer_cancel> channel::next_cancel(std::chrono::milliseconds timeout) {
    const status ready = ensure_ready();
    if (!ready) {
        return ready.error();
    }
    return owner_->wait_cancel(id_, timeout);
}

status channel::replay_topology() {
    // A fresh broker channel starts outside confirm mode; re-enter it first so
    // the confirm sequence restarts at one.
    if (confirm_mode_) {
        writer out;
        out.bits(0, 1); // no-wait = false
        auto reply = exchange_method(class_id::confirm, method_id::confirm_select,
                                     span_of(out.data()),
                                     {method_id::confirm_select_ok});
        if (!reply) {
            return status{reply.error()};
        }
        next_publish_tag_ = 1;
    }

    if (qos_applied_) {
        writer out;
        out.u32(qos_.prefetch_size);
        out.u16(qos_.prefetch_count);
        out.bits(bits_of({qos_.global}), 1);
        auto reply = exchange_method(class_id::basic, method_id::basic_qos,
                                     span_of(out.data()), {method_id::basic_qos_ok});
        if (!reply) {
            return status{reply.error()};
        }
    }

    // Exchanges and queues first: bindings and consumers reference them.
    for (const auto& entry : exchanges_) {
        const exchange_state& ex = entry.second;
        writer out;
        out.u16(0); // ticket
        out.short_string(ex.name);
        out.short_string(ex.type);
        // A passive declare is replayed as a real one: its meaning was "this
        // must exist", and after a broker restart declaring is what makes it
        // exist again.
        out.bits(bits_of({false, ex.durable, ex.auto_delete, ex.internal, false}), 5);
        out.table(ex.arguments);
        if (!out.ok()) {
            return status{out.error(), "Exchange.Declare arguments do not fit"};
        }
        auto reply = exchange_method(class_id::exchange, method_id::exchange_declare,
                                     span_of(out.data()),
                                     {method_id::exchange_declare_ok});
        if (!reply) {
            return status{reply.error()};
        }
    }

    for (const auto& entry : queues_) {
        const queue_state& state = entry.second;
        writer out;
        out.u16(0); // ticket
        out.short_string(state.name);
        out.bits(bits_of({false, state.durable, state.exclusive, state.auto_delete,
                          false}), 5);
        out.table(state.arguments);
        if (!out.ok()) {
            return status{out.error(), "Queue.Declare arguments do not fit"};
        }
        auto reply = exchange_method(class_id::queue, method_id::queue_declare,
                                     span_of(out.data()),
                                     {method_id::queue_declare_ok});
        if (!reply) {
            return status{reply.error()};
        }
    }

    for (const bind_state& bind : binds_) {
        writer out;
        out.u16(0); // ticket
        out.short_string(bind.queue);
        out.short_string(bind.exchange);
        out.short_string(bind.routing_key);
        out.bits(0, 1); // no-wait = false
        out.table(bind.arguments);
        if (!out.ok()) {
            return status{out.error(), "Queue.Bind arguments do not fit"};
        }
        auto reply = exchange_method(class_id::queue, method_id::queue_bind,
                                     span_of(out.data()), {method_id::queue_bind_ok});
        if (!reply) {
            return status{reply.error()};
        }
    }

    for (const auto& entry : consumes_) {
        const consume_state& state = entry.second;
        writer out;
        out.u16(0); // ticket
        out.short_string(state.queue);
        out.short_string(state.consumer_tag);
        out.bits(bits_of({false, state.no_ack, false, false}), 4);
        out.table(state.arguments);
        if (!out.ok()) {
            return status{out.error(), "Basic.Consume arguments do not fit"};
        }
        auto reply = exchange_method(class_id::basic, method_id::basic_consume,
                                     span_of(out.data()),
                                     {method_id::basic_consume_ok});
        if (!reply) {
            return status{reply.error()};
        }
        // A broker-generated tag stays what it was; re-consuming with the same
        // explicit tag on the fresh channel keeps the identity stable.
        reader in(span_of(reply.value()));
        const std::string granted(in.short_string());
        if (!in.ok() || granted.empty()) {
            return status{rpc_code::protocol_error, "Basic.ConsumeOk is malformed"};
        }
        if (granted != state.consumer_tag) {
            consumes_.erase(state.consumer_tag);
            consume_state updated = state;
            updated.consumer_tag = granted;
            consumes_[granted] = std::move(updated);
        }
    }
    return status{};
}

status channel::close() {
    if (owner_ == nullptr) {
        open_ = false;
        return status{};
    }
    if (!open_ || dead_) {
        open_ = false;
        return status{};
    }
    open_ = false;
    owner_->unregister_channel(this);
    if (owner_->is_open()) {
        return owner_->close_channel(id_);
    }
    return status{};
}

} // namespace amqp
} // namespace mq
} // namespace crpc