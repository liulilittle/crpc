// CRPC -- HTTP/2 connection state machine implementation.
#include <crpc/http2/connection.hpp>

#include <algorithm>
#include <utility>

namespace crpc::http2 {
namespace {

// Hard bound on a reassembled header block (HEADERS + CONTINUATION frames).
// A peer that keeps sending CONTINUATION forever is stopped with a connection
// error rather than growing memory without bound.
std::size_t header_block_cap(std::size_t max_header_list_size) noexcept {
    const std::size_t scaled = max_header_list_size > (1u << 24) ? max_header_list_size
                                                                 : max_header_list_size * 8u;
    return scaled < (1u << 20) ? (1u << 20) : scaled;
}

// The largest dynamic table we will let our HPACK encoder use in response to a
// peer SETTINGS_HEADER_TABLE_SIZE. The peer may allow more; using less is
// always legal and bounds our memory.
constexpr std::size_t encoder_table_cap = 65536;

bool is_odd(u32 value) noexcept {
    return (value & 1u) != 0;
}

} // namespace

const char* to_string(stream_state state) noexcept {
    switch (state) {
    case stream_state::idle:
        return "idle";
    case stream_state::reserved_local:
        return "reserved_local";
    case stream_state::reserved_remote:
        return "reserved_remote";
    case stream_state::open:
        return "open";
    case stream_state::half_closed_local:
        return "half_closed_local";
    case stream_state::half_closed_remote:
        return "half_closed_remote";
    case stream_state::closed:
        return "closed";
    }
    return "unknown";
}

status validate_setting(u16 id, u32 value) noexcept {
    switch (static_cast<setting_id>(id)) {
    case setting_id::enable_push:
        return value <= 1u ? status{} : status(rpc_code::protocol_error);
    case setting_id::initial_window_size:
        return value <= max_window_size ? status{} : status(rpc_code::protocol_error);
    case setting_id::max_frame_size:
        return (value >= min_max_frame_size && value <= max_max_frame_size)
                   ? status{}
                   : status(rpc_code::protocol_error);
    case setting_id::header_table_size:
    case setting_id::max_concurrent_streams:
    case setting_id::max_header_list_size:
        return status{};
    }
    // Unknown identifiers are ignored (RFC 7540 6.5.2).
    return status{};
}

connection::connection(connection_options options)
    : role_(options.role),
      options_(options),
      local_settings_(options.local_settings),
      remote_settings_{},
      connection_send_window_(static_cast<i64>(options.initial_connection_send_window)),
      connection_recv_window_(static_cast<i64>(options.initial_connection_recv_window)),
      decoder_(options.local_settings.header_table_size),
      encoder_() {
    decoder_.set_peer_table_limit(options.local_settings.header_table_size);
    decoder_.set_max_header_list_size(options.local_settings.max_header_list_size);
}

// --- Helpers -----------------------------------------------------------------

connection::stream_entry* connection::find_stream(u32 stream_id) noexcept {
    const auto it = streams_.find(stream_id);
    return it == streams_.end() ? nullptr : &it->second;
}

const connection::stream_entry* connection::find_stream(u32 stream_id) const noexcept {
    const auto it = streams_.find(stream_id);
    return it == streams_.end() ? nullptr : &it->second;
}

h2_error connection::connection_error(error_code code) const noexcept {
    h2_error error;
    error.code = code;
    error.scope = error_scope::connection;
    return error;
}

h2_error connection::stream_error(u32 stream_id, error_code code) const noexcept {
    h2_error error;
    error.code = code;
    error.scope = error_scope::stream;
    error.stream_id = stream_id;
    return error;
}

bool connection::is_local_id(u32 stream_id) const noexcept {
    return role_ == connection_role::client ? is_odd(stream_id) : !is_odd(stream_id);
}

bool connection::is_peer_id(u32 stream_id) const noexcept {
    return role_ == connection_role::client ? !is_odd(stream_id) : is_odd(stream_id);
}

void connection::close_stream(stream_entry& entry) noexcept {
    entry.state = stream_state::closed;
    entry.local_end = true;
    entry.remote_end = true;
    entry.send_queue.clear();
}

void connection::transition_remote_end(stream_entry& entry) noexcept {
    if (entry.state == stream_state::open) {
        entry.state = stream_state::half_closed_remote;
    } else if (entry.state == stream_state::half_closed_local) {
        entry.state = stream_state::closed;
    }
}

void connection::transition_local_end(stream_entry& entry) noexcept {
    if (entry.state == stream_state::open) {
        entry.state = stream_state::half_closed_local;
    } else if (entry.state == stream_state::half_closed_remote) {
        entry.state = stream_state::closed;
    }
}

void connection::clear_last_headers() noexcept {
    last_headers_.clear();
    last_headers_present_ = false;
    last_headers_stream_ = 0;
}

// --- Introspection -----------------------------------------------------------

stream_state connection::state_of(u32 stream_id) const noexcept {
    const stream_entry* entry = find_stream(stream_id);
    return entry == nullptr ? stream_state::idle : entry->state;
}

bool connection::stream_exists(u32 stream_id) const noexcept {
    return find_stream(stream_id) != nullptr;
}

stream_snapshot connection::snapshot(u32 stream_id) const noexcept {
    stream_snapshot value;
    value.id = stream_id;
    const stream_entry* entry = find_stream(stream_id);
    if (entry == nullptr) {
        return value;
    }
    value.state = entry->state;
    value.send_window = entry->send_window;
    value.recv_window = entry->recv_window;
    value.local_end_stream = entry->local_end;
    value.remote_end_stream = entry->remote_end;
    value.queued_bytes = entry->send_queue.size();
    return value;
}

std::size_t connection::active_stream_count() const noexcept {
    std::size_t count = 0;
    for (const auto& item : streams_) {
        const stream_state state = item.second.state;
        if (state != stream_state::closed && state != stream_state::idle) {
            ++count;
        }
    }
    return count;
}

std::size_t connection::peer_open_stream_count() const noexcept {
    std::size_t count = 0;
    for (const auto& item : streams_) {
        const stream_entry& entry = item.second;
        if (entry.peer_initiated && entry.state != stream_state::closed &&
            entry.state != stream_state::idle) {
            ++count;
        }
    }
    return count;
}

bool connection::can_open_stream(u32 stream_id) const noexcept {
    if (stream_id == 0 || stream_id > max_stream_id) {
        return false;
    }
    if (!is_local_id(stream_id)) {
        return false;
    }
    if (stream_id <= last_local_stream_id_) {
        return false;
    }
    if (goaway_received_ && stream_id > goaway_last_stream_id_) {
        return false;
    }
    return true;
}

h2_error connection::open_stream(u32 stream_id, bool end_stream) {
    if (stream_id == 0 || stream_id > max_stream_id) {
        return connection_error(error_code::protocol_error);
    }
    if (!is_local_id(stream_id)) {
        return connection_error(error_code::protocol_error);
    }
    if (find_stream(stream_id) != nullptr || stream_id <= last_local_stream_id_) {
        return connection_error(error_code::protocol_error);
    }
    if (goaway_received_ && stream_id > goaway_last_stream_id_) {
        return stream_error(stream_id, error_code::refused_stream);
    }
    last_local_stream_id_ = stream_id;
    stream_entry entry;
    entry.id = stream_id;
    entry.state = end_stream ? stream_state::half_closed_local : stream_state::open;
    entry.local_end = end_stream;
    entry.send_window = static_cast<i64>(remote_settings_.initial_window_size);
    entry.recv_window = static_cast<i64>(local_settings_.initial_window_size);
    streams_.emplace(stream_id, std::move(entry));
    return h2_error{};
}

h2_error connection::local_end_stream(u32 stream_id) {
    stream_entry* entry = find_stream(stream_id);
    if (entry == nullptr) {
        return connection_error(error_code::protocol_error);
    }
    if (entry->state == stream_state::closed || entry->local_end) {
        return stream_error(stream_id, error_code::stream_closed);
    }
    if (!entry->send_queue.empty()) {
        // END_STREAM must follow every queued DATA byte.
        return stream_error(stream_id, error_code::protocol_error);
    }
    entry->local_end = true;
    transition_local_end(*entry);
    return h2_error{};
}

h2_error connection::reset_stream(u32 stream_id, error_code code) {
    stream_entry* entry = find_stream(stream_id);
    if (entry == nullptr || entry->state == stream_state::idle) {
        return connection_error(error_code::protocol_error);
    }
    last_reset_stream_ = stream_id;
    last_reset_code_ = code;
    close_stream(*entry);
    return h2_error{};
}

void connection::prune_closed_streams() {
    for (auto it = streams_.begin(); it != streams_.end();) {
        if (it->second.state == stream_state::closed && it->second.send_queue.empty()) {
            it = streams_.erase(it);
        } else {
            ++it;
        }
    }
}

// --- Send-side flow control --------------------------------------------------

i64 connection::stream_send_window(u32 stream_id) const noexcept {
    const stream_entry* entry = find_stream(stream_id);
    return entry == nullptr ? 0 : entry->send_window;
}

std::size_t connection::available_send_window(u32 stream_id) const noexcept {
    const stream_entry* entry = find_stream(stream_id);
    if (entry == nullptr) {
        return 0;
    }
    i64 window = connection_send_window_;
    if (entry->send_window < window) {
        window = entry->send_window;
    }
    if (window < 0) {
        window = 0;
    }
    std::size_t available = static_cast<std::size_t>(window);
    if (available > remote_settings_.max_frame_size) {
        available = remote_settings_.max_frame_size;
    }
    return available;
}

h2_error connection::send_data(u32 stream_id, span<const std::byte> data,
                               std::size_t& sent, bool& parked) {
    sent = 0;
    parked = false;
    stream_entry* entry = find_stream(stream_id);
    if (entry == nullptr) {
        return connection_error(error_code::protocol_error);
    }
    if (entry->state == stream_state::closed || entry->local_end) {
        return stream_error(stream_id, error_code::stream_closed);
    }
    if (entry->state == stream_state::reserved_local ||
        entry->state == stream_state::reserved_remote ||
        entry->state == stream_state::idle) {
        return stream_error(stream_id, error_code::protocol_error);
    }

    std::size_t allowance = available_send_window(stream_id);
    if (allowance > data.size()) {
        allowance = data.size();
    }
    if (allowance != 0) {
        connection_send_window_ -= static_cast<i64>(allowance);
        entry->send_window -= static_cast<i64>(allowance);
        sent = allowance;
    }
    const std::size_t remainder = data.size() - allowance;
    if (remainder != 0) {
        // The subtraction is guarded on the left: with `remainder` larger than
        // the cap the unsigned difference would wrap to a huge value and the
        // check would never fire (B-1).
        if (remainder > options_.max_send_queue_bytes ||
            entry->send_queue.size() > options_.max_send_queue_bytes - remainder) {
            return stream_error(stream_id, error_code::enhance_your_calm);
        }
        entry->send_queue.insert(entry->send_queue.end(), data.begin() + allowance,
                                 data.end());
        parked = true;
    }
    return h2_error{};
}

h2_error connection::flush_parked(u32 stream_id, std::size_t max_bytes,
                                  std::vector<std::byte>& out, bool& flushed) {
    out.clear();
    flushed = false;
    stream_entry* entry = find_stream(stream_id);
    if (entry == nullptr) {
        return connection_error(error_code::protocol_error);
    }
    if (entry->send_queue.empty()) {
        return h2_error{};
    }
    if (entry->state == stream_state::closed || entry->local_end) {
        return stream_error(stream_id, error_code::stream_closed);
    }
    std::size_t cap = max_bytes == 0 ? remote_settings_.max_frame_size : max_bytes;
    std::size_t available = available_send_window(stream_id);
    if (cap > available) {
        cap = available;
    }
    if (cap > entry->send_queue.size()) {
        cap = entry->send_queue.size();
    }
    if (cap == 0) {
        return h2_error{};
    }
    out.assign(entry->send_queue.begin(), entry->send_queue.begin() + static_cast<std::ptrdiff_t>(cap));
    entry->send_queue.erase(entry->send_queue.begin(),
                            entry->send_queue.begin() + static_cast<std::ptrdiff_t>(cap));
    connection_send_window_ -= static_cast<i64>(cap);
    entry->send_window -= static_cast<i64>(cap);
    flushed = true;
    return h2_error{};
}

std::size_t connection::parked_bytes(u32 stream_id) const noexcept {
    const stream_entry* entry = find_stream(stream_id);
    return entry == nullptr ? 0 : entry->send_queue.size();
}

std::size_t connection::total_parked_bytes() const noexcept {
    std::size_t total = 0;
    for (const auto& item : streams_) {
        total += item.second.send_queue.size();
    }
    return total;
}

bool connection::has_parked() const noexcept {
    for (const auto& item : streams_) {
        if (!item.second.send_queue.empty()) {
            return true;
        }
    }
    return false;
}

std::vector<u32> connection::parked_streams() const {
    std::vector<u32> ids;
    for (const auto& item : streams_) {
        if (!item.second.send_queue.empty()) {
            ids.push_back(item.first);
        }
    }
    return ids;
}

// --- Receive-side flow control ----------------------------------------------

i64 connection::stream_recv_window(u32 stream_id) const noexcept {
    const stream_entry* entry = find_stream(stream_id);
    return entry == nullptr ? 0 : entry->recv_window;
}

window_credit connection::release_received(u32 stream_id, std::size_t consumed) noexcept {
    window_credit credit;
    stream_entry* entry = find_stream(stream_id);
    if (entry == nullptr || consumed == 0) {
        return credit;
    }
    std::size_t restore = consumed;
    const i64 connection_space = static_cast<i64>(max_window_size) - connection_recv_window_;
    const i64 stream_space = static_cast<i64>(max_window_size) - entry->recv_window;
    i64 space = connection_space < stream_space ? connection_space : stream_space;
    if (space < 0) {
        space = 0;
    }
    if (restore > static_cast<std::size_t>(space)) {
        restore = static_cast<std::size_t>(space);
    }
    if (restore == 0) {
        return credit;
    }
    connection_recv_window_ += static_cast<i64>(restore);
    entry->recv_window += static_cast<i64>(restore);
    credit.connection = static_cast<u32>(restore);
    credit.stream = static_cast<u32>(restore);
    return credit;
}

// --- Inbound dispatch --------------------------------------------------------

h2_error connection::receive(const frame_header& header, span<const std::byte> payload) {
    ++received_frames_;
    if (header.length != payload.size()) {
        return connection_error(error_code::protocol_error);
    }
    if (header.length > local_settings_.max_frame_size) {
        return connection_error(error_code::frame_size_error);
    }
    if (continuation_pending_) {
        if (header.type != frame_type::continuation ||
            header.stream_id != continuation_stream_) {
            return connection_error(error_code::protocol_error);
        }
        return receive_continuation(header, payload);
    }
    switch (header.type) {
    case frame_type::data:
        return receive_data(header, payload);
    case frame_type::headers:
        return receive_headers(header, payload);
    case frame_type::priority:
        return receive_priority(header, payload);
    case frame_type::rst_stream:
        return receive_rst_stream(header, payload);
    case frame_type::settings:
        return receive_settings(header, payload);
    case frame_type::push_promise:
        return receive_push_promise(header, payload);
    case frame_type::ping:
        return receive_ping(header, payload);
    case frame_type::goaway:
        return receive_goaway(header, payload);
    case frame_type::window_update:
        return receive_window_update(header, payload);
    case frame_type::continuation:
        // No HEADERS/PUSH_PROMISE is open.
        return connection_error(error_code::protocol_error);
    default:
        break;
    }
    if (options_.reject_unknown_frame_types) {
        return connection_error(error_code::protocol_error);
    }
    // RFC 7540 4.1: unknown frame types are ignored.
    return h2_error{};
}

h2_error connection::receive_data(const frame_header& header, span<const std::byte> payload) {
    if (header.stream_id == 0) {
        return connection_error(error_code::protocol_error);
    }
    span<const std::byte> data;
    const status decoded = decode_data(payload, header.flags, data);
    if (!decoded) {
        return connection_error(error_code::protocol_error);
    }
    stream_entry* entry = find_stream(header.stream_id);
    if (entry == nullptr) {
        // DATA on an idle stream is a connection error.
        return connection_error(error_code::protocol_error);
    }
    if (entry->state == stream_state::half_closed_remote ||
        entry->state == stream_state::closed) {
        return stream_error(header.stream_id, error_code::stream_closed);
    }
    if (entry->state == stream_state::reserved_local ||
        entry->state == stream_state::reserved_remote) {
        return connection_error(error_code::protocol_error);
    }
    // The whole payload counts against flow control, padding included.
    const i64 charged = static_cast<i64>(payload.size());
    connection_recv_window_ -= charged;
    if (connection_recv_window_ < 0) {
        return connection_error(error_code::flow_control_error);
    }
    entry->recv_window -= charged;
    if (entry->recv_window < 0) {
        return stream_error(header.stream_id, error_code::flow_control_error);
    }
    last_data_ = data;
    last_data_stream_ = header.stream_id;
    if ((header.flags & flags::end_stream) != 0) {
        entry->remote_end = true;
        transition_remote_end(*entry);
    }
    return h2_error{};
}

h2_error connection::open_peer_stream(u32 stream_id) {
    if (stream_id == 0 || stream_id > max_stream_id) {
        return connection_error(error_code::protocol_error);
    }
    if (!is_peer_id(stream_id)) {
        return connection_error(error_code::protocol_error);
    }
    if (stream_id <= last_peer_stream_id_) {
        return connection_error(error_code::protocol_error);
    }
    if (goaway_received_) {
        return connection_error(error_code::protocol_error);
    }
    if (peer_open_stream_count() >= local_settings_.max_concurrent_streams) {
        last_peer_stream_id_ = stream_id;
        stream_entry refused;
        refused.id = stream_id;
        refused.state = stream_state::closed;
        refused.peer_initiated = true;
        streams_.emplace(stream_id, std::move(refused));
        return stream_error(stream_id, error_code::refused_stream);
    }
    last_peer_stream_id_ = stream_id;
    // A long-lived connection would otherwise keep one closed record per
    // request forever. Prune before inserting so the map stays bounded by the
    // concurrency limit plus one batch of recently closed streams; a record is
    // kept while a parked response still refers to it.
    const std::size_t prune_threshold =
        std::max<std::size_t>(64, 2u * local_settings_.max_concurrent_streams);
    if (streams_.size() >= prune_threshold) {
        prune_closed_streams();
    }
    stream_entry entry;
    entry.id = stream_id;
    entry.state = stream_state::open;
    entry.peer_initiated = true;
    entry.send_window = static_cast<i64>(remote_settings_.initial_window_size);
    entry.recv_window = static_cast<i64>(local_settings_.initial_window_size);
    streams_.emplace(stream_id, std::move(entry));
    return h2_error{};
}

h2_error connection::finish_header_block(u32 stream_id, span<const std::byte> fragment) {
    if (header_block_.size() + fragment.size() > header_block_cap(local_settings_.max_header_list_size)) {
        header_block_.clear();
        continuation_pending_ = false;
        return connection_error(error_code::enhance_your_calm);
    }
    header_block_.insert(header_block_.end(), fragment.begin(), fragment.end());
    last_headers_.clear();
    const status decoded = decoder_.decode(span<const std::byte>(header_block_.data(),
                                                                 header_block_.size()),
                                           last_headers_);
    header_block_.clear();
    continuation_pending_ = false;
    if (!decoded) {
        if (decoded.code() == rpc_code::resource_exhausted) {
            // The table is still in sync because the decoder processed the
            // whole block; only this stream is rejected.
            last_headers_.clear();
            return stream_error(stream_id, error_code::enhance_your_calm);
        }
        last_headers_.clear();
        return connection_error(error_code::compression_error);
    }
    last_headers_stream_ = stream_id;
    last_headers_present_ = true;
    return h2_error{};
}

h2_error connection::receive_headers(const frame_header& header, span<const std::byte> payload) {
    if (header.stream_id == 0) {
        return connection_error(error_code::protocol_error);
    }
    headers_payload decoded;
    const status parse = decode_headers(payload, header.flags, decoded);
    if (!parse) {
        return connection_error(error_code::protocol_error);
    }
    stream_entry* entry = find_stream(header.stream_id);
    if (entry == nullptr) {
        const h2_error opened = open_peer_stream(header.stream_id);
        if (!opened.ok()) {
            return opened;
        }
        entry = find_stream(header.stream_id);
        if (entry == nullptr) {
            return connection_error(error_code::internal_error);
        }
    } else if (entry->state == stream_state::closed ||
               entry->state == stream_state::half_closed_remote) {
        return stream_error(header.stream_id, error_code::stream_closed);
    } else if (entry->state == stream_state::reserved_remote) {
        entry->state = stream_state::half_closed_local;
    } else if (entry->state == stream_state::idle) {
        entry->state = stream_state::open;
    }

    if (decoded.end_stream && !entry->remote_end) {
        entry->remote_end = true;
        transition_remote_end(*entry);
    }
    if (decoded.end_headers) {
        return finish_header_block(header.stream_id, decoded.header_block);
    }
    if (header_block_.size() + decoded.header_block.size() >
        header_block_cap(local_settings_.max_header_list_size)) {
        return connection_error(error_code::enhance_your_calm);
    }
    continuation_pending_ = true;
    continuation_stream_ = header.stream_id;
    header_block_.assign(decoded.header_block.begin(), decoded.header_block.end());
    return h2_error{};
}

h2_error connection::receive_continuation(const frame_header& header,
                                          span<const std::byte> payload) {
    if (header.stream_id != continuation_stream_) {
        return connection_error(error_code::protocol_error);
    }
    if (header_block_.size() + payload.size() >
        header_block_cap(local_settings_.max_header_list_size)) {
        header_block_.clear();
        continuation_pending_ = false;
        return connection_error(error_code::enhance_your_calm);
    }
    header_block_.insert(header_block_.end(), payload.begin(), payload.end());
    if ((header.flags & flags::end_headers) != 0) {
        return finish_header_block(header.stream_id, span<const std::byte>());
    }
    return h2_error{};
}

h2_error connection::receive_push_promise(const frame_header& header,
                                          span<const std::byte> payload) {
    if (header.stream_id == 0) {
        return connection_error(error_code::protocol_error);
    }
    if (role_ == connection_role::server) {
        // A client cannot push.
        return connection_error(error_code::protocol_error);
    }
    if (!local_settings_.enable_push) {
        return connection_error(error_code::protocol_error);
    }
    push_promise_payload decoded;
    const status parse = decode_push_promise(payload, header.flags, decoded);
    if (!parse) {
        return connection_error(error_code::protocol_error);
    }
    const u32 promised = decoded.promised_stream_id;
    if (promised == 0 || is_odd(promised) || promised <= last_peer_stream_id_) {
        return connection_error(error_code::protocol_error);
    }
    last_peer_stream_id_ = promised;
    stream_entry entry;
    entry.id = promised;
    entry.peer_initiated = true;
    entry.state = stream_state::reserved_remote;
    entry.send_window = static_cast<i64>(remote_settings_.initial_window_size);
    entry.recv_window = static_cast<i64>(local_settings_.initial_window_size);
    streams_.emplace(promised, std::move(entry));

    if ((header.flags & flags::end_headers) != 0) {
        return finish_header_block(header.stream_id, decoded.header_block);
    }
    if (header_block_.size() + decoded.header_block.size() >
        header_block_cap(local_settings_.max_header_list_size)) {
        return connection_error(error_code::enhance_your_calm);
    }
    continuation_pending_ = true;
    continuation_stream_ = header.stream_id;
    header_block_.assign(decoded.header_block.begin(), decoded.header_block.end());
    return h2_error{};
}

h2_error connection::receive_settings(const frame_header& header,
                                      span<const std::byte> payload) {
    if (header.stream_id != 0) {
        return connection_error(error_code::protocol_error);
    }
    if ((header.flags & flags::ack) != 0) {
        if (header.length != 0) {
            return connection_error(error_code::frame_size_error);
        }
        return h2_error{};
    }
    if (header.length % 6 != 0) {
        return connection_error(error_code::frame_size_error);
    }
    std::vector<setting> parsed;
    const status decode = decode_settings(payload, parsed);
    if (!decode) {
        return connection_error(error_code::frame_size_error);
    }
    for (const setting& item : parsed) {
        const status valid = validate_setting(item.id, item.value);
        if (!valid) {
            return connection_error(error_code::protocol_error);
        }
        if (static_cast<setting_id>(item.id) == setting_id::enable_push &&
            role_ == connection_role::client) {
            // ENABLE_PUSH is client-to-server only.
            return connection_error(error_code::protocol_error);
        }
    }
    for (const setting& item : parsed) {
        switch (static_cast<setting_id>(item.id)) {
        case setting_id::header_table_size: {
            remote_settings_.header_table_size = item.value;
            const std::size_t capped = item.value < encoder_table_cap
                                           ? static_cast<std::size_t>(item.value)
                                           : encoder_table_cap;
            encoder_.set_max_table_size(capped);
            break;
        }
        case setting_id::enable_push:
            remote_settings_.enable_push = item.value != 0;
            break;
        case setting_id::max_concurrent_streams:
            remote_settings_.max_concurrent_streams = item.value;
            break;
        case setting_id::initial_window_size: {
            const i64 delta = static_cast<i64>(item.value) -
                              static_cast<i64>(remote_settings_.initial_window_size);
            if (delta != 0) {
                for (auto& item_stream : streams_) {
                    item_stream.second.send_window += delta;
                    if (item_stream.second.send_window > static_cast<i64>(max_window_size)) {
                        return connection_error(error_code::flow_control_error);
                    }
                }
            }
            remote_settings_.initial_window_size = item.value;
            break;
        }
        case setting_id::max_frame_size:
            remote_settings_.max_frame_size = item.value;
            break;
        case setting_id::max_header_list_size:
            remote_settings_.max_header_list_size = item.value;
            break;
        default:
            break;
        }
    }
    pending_settings_ack_ = true;
    return h2_error{};
}

h2_error connection::receive_ping(const frame_header& header,
                                  span<const std::byte> payload) {
    if (header.stream_id != 0) {
        return connection_error(error_code::protocol_error);
    }
    if (header.length != 8) {
        return connection_error(error_code::frame_size_error);
    }
    ping_opaque opaque{};
    if (!decode_ping(payload, opaque)) {
        return connection_error(error_code::frame_size_error);
    }
    if ((header.flags & flags::ack) != 0) {
        ping_acknowledged_ = true;
        return h2_error{};
    }
    last_ping_ = opaque;
    pending_ping_ack_ = true;
    return h2_error{};
}

h2_error connection::receive_goaway(const frame_header& header,
                                    span<const std::byte> payload) {
    if (header.stream_id != 0) {
        return connection_error(error_code::protocol_error);
    }
    if (header.length < 8) {
        return connection_error(error_code::frame_size_error);
    }
    goaway_payload decoded;
    if (!decode_goaway(payload, decoded)) {
        return connection_error(error_code::frame_size_error);
    }
    goaway_received_ = true;
    goaway_last_stream_id_ = decoded.last_stream_id;
    goaway_error_ = decoded.code;
    for (auto& item : streams_) {
        stream_entry& entry = item.second;
        if (!entry.peer_initiated && entry.id > decoded.last_stream_id &&
            entry.state != stream_state::closed) {
            close_stream(entry);
        }
    }
    return h2_error{};
}

h2_error connection::receive_window_update(const frame_header& header,
                                           span<const std::byte> payload) {
    if (header.length != 4) {
        return connection_error(error_code::frame_size_error);
    }
    u32 increment = 0;
    if (!decode_window_update(payload, increment)) {
        return connection_error(error_code::frame_size_error);
    }
    if (increment == 0) {
        if (header.stream_id == 0) {
            return connection_error(error_code::protocol_error);
        }
        return stream_error(header.stream_id, error_code::protocol_error);
    }
    if (header.stream_id == 0) {
        if (connection_send_window_ > static_cast<i64>(max_window_size - increment)) {
            return connection_error(error_code::flow_control_error);
        }
        connection_send_window_ += static_cast<i64>(increment);
        return h2_error{};
    }
    stream_entry* entry = find_stream(header.stream_id);
    if (entry == nullptr) {
        // WINDOW_UPDATE on an idle stream is a connection error.
        return connection_error(error_code::protocol_error);
    }
    if (entry->state == stream_state::closed) {
        return stream_error(header.stream_id, error_code::stream_closed);
    }
    if (entry->send_window > static_cast<i64>(max_window_size - increment)) {
        return stream_error(header.stream_id, error_code::flow_control_error);
    }
    entry->send_window += static_cast<i64>(increment);
    return h2_error{};
}

h2_error connection::receive_rst_stream(const frame_header& header,
                                        span<const std::byte> payload) {
    if (header.stream_id == 0) {
        return connection_error(error_code::protocol_error);
    }
    if (header.length != 4) {
        return connection_error(error_code::frame_size_error);
    }
    error_code code = error_code::no_error;
    if (!decode_rst_stream(payload, code)) {
        return connection_error(error_code::frame_size_error);
    }
    stream_entry* entry = find_stream(header.stream_id);
    if (entry == nullptr || entry->state == stream_state::idle) {
        // RST_STREAM on an idle stream is a connection error.
        return connection_error(error_code::protocol_error);
    }
    last_reset_stream_ = header.stream_id;
    last_reset_code_ = code;
    close_stream(*entry);
    return h2_error{};
}

h2_error connection::receive_priority(const frame_header& header,
                                      span<const std::byte> payload) {
    if (header.stream_id == 0) {
        return connection_error(error_code::protocol_error);
    }
    if (header.length != 5) {
        return stream_error(header.stream_id, error_code::frame_size_error);
    }
    priority_payload decoded;
    if (!decode_priority(payload, decoded)) {
        return stream_error(header.stream_id, error_code::frame_size_error);
    }
    if (decoded.stream_dependency == header.stream_id) {
        return stream_error(header.stream_id, error_code::protocol_error);
    }
    return h2_error{};
}

} // namespace crpc::http2
