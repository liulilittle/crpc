// CRPC Redis -- Redis Streams command builders and reply decoders.
#include <crpc/redis/stream.hpp>

#include <limits>
#include <utility>

#include "reply_util.hpp"

namespace crpc {
namespace redis {
namespace {

status expect_ok(const resp_value& value, const char* operation) {
    if (detail::is_error_reply(value)) {
        return status{detail::application_error(value)};
    }
    if (!detail::is_text(value) || detail::as_text(value) != "OK") {
        return status{detail::protocol_error(std::string(operation) +
                                             " did not return OK")};
    }
    return status{};
}

result<std::int64_t> as_integer(const resp_value& value, const char* operation) {
    if (detail::is_error_reply(value)) {
        return detail::application_error(value);
    }
    if (!value.is_integer()) {
        return detail::protocol_error(std::string(operation) +
                                      " did not return an integer");
    }
    return value.as_integer();
}

bool parse_u64(std::string_view text, std::uint64_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        const std::uint64_t limit = std::numeric_limits<std::uint64_t>::max();
        if (value > (limit - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

result<stream_entry> parse_entry(const resp_value& value) {
    if (!value.is_array() || value.as_array().size() != 2) {
        return detail::protocol_error("a stream entry must be a two-element array");
    }
    const resp_array& item = value.as_array();
    if (!detail::is_text(item[0])) {
        return detail::protocol_error("a stream entry ID must be a string");
    }

    stream_entry entry;
    entry.id = detail::as_text(item[0]);
    if (!stream_id::parse(entry.id, entry.parsed_id)) {
        return detail::protocol_error("a stream entry has an invalid ID: " + entry.id);
    }

    const resp_value& fields = item[1];
    if (fields.is_null()) {
        // XREADGROUP reports an entry deleted between delivery and the read as
        // [id, nil]. Keep the ID and an empty field list.
        return entry;
    }
    if (fields.is_map()) {
        // RESP3 encodes field/value pairs as a map.
        entry.fields.reserve(fields.as_map().size());
        for (const auto& pair : fields.as_map()) {
            if (!detail::is_text(pair.first) || !detail::is_text(pair.second)) {
                return detail::protocol_error(
                    "stream entry map fields must be strings");
            }
            entry.fields.emplace_back(detail::as_text(pair.first),
                                      detail::as_text(pair.second));
        }
        return entry;
    }
    if (!fields.is_array() || (fields.as_array().size() % 2) != 0) {
        return detail::protocol_error(
            "stream entry fields must be an even-sized array or a map");
    }
    const resp_array& flat = fields.as_array();
    entry.fields.reserve(flat.size() / 2);
    for (std::size_t i = 0; i < flat.size(); i += 2) {
        if (!detail::is_text(flat[i]) || !detail::is_text(flat[i + 1])) {
            return detail::protocol_error("stream entry fields must be strings");
        }
        entry.fields.emplace_back(detail::as_text(flat[i]),
                                  detail::as_text(flat[i + 1]));
    }
    return entry;
}

result<std::vector<stream_entry>> parse_entries(const resp_value& value,
                                                const char* operation) {
    if (detail::is_error_reply(value)) {
        return detail::application_error(value);
    }
    if (!value.is_array()) {
        return detail::protocol_error(std::string(operation) +
                                      " did not return an array");
    }
    std::vector<stream_entry> entries;
    entries.reserve(value.as_array().size());
    for (const resp_value& item : value.as_array()) {
        result<stream_entry> entry = parse_entry(item);
        if (!entry) {
            return entry.error();
        }
        entries.push_back(std::move(entry).value());
    }
    return entries;
}

result<std::vector<stream_read>> parse_read(const resp_value& value) {
    if (value.is_null()) {
        return std::vector<stream_read>{};
    }
    if (detail::is_error_reply(value)) {
        return detail::application_error(value);
    }
    std::vector<stream_read> reads;
    if (value.is_map()) {
        // RESP3 encodes XREAD/XREADGROUP as a map from stream key to entries.
        reads.reserve(value.as_map().size());
        for (const auto& entry : value.as_map()) {
            if (!detail::is_text(entry.first)) {
                return detail::protocol_error(
                    "an XREAD map key must be a stream name");
            }
            stream_read read;
            read.key = detail::as_text(entry.first);
            result<std::vector<stream_entry>> entries =
                parse_entries(entry.second, "XREAD entries");
            if (!entries) {
                return entries.error();
            }
            read.entries = std::move(entries).value();
            reads.push_back(std::move(read));
        }
        return reads;
    }
    if (!value.is_array()) {
        return detail::protocol_error(
            "XREAD did not return an array, a map or null");
    }

    reads.reserve(value.as_array().size());
    for (const resp_value& item : value.as_array()) {
        if (!item.is_array() || item.as_array().size() != 2) {
            return detail::protocol_error(
                "an XREAD stream item must be a two-element array");
        }
        const resp_array& pair = item.as_array();
        if (!detail::is_text(pair[0])) {
            return detail::protocol_error("an XREAD stream key must be a string");
        }
        stream_read read;
        read.key = detail::as_text(pair[0]);
        result<std::vector<stream_entry>> entries =
            parse_entries(pair[1], "XREAD entries");
        if (!entries) {
            return entries.error();
        }
        read.entries = std::move(entries).value();
        reads.push_back(std::move(read));
    }
    return reads;
}

status append_trim(command& cmd, stream_trim_kind kind, bool approximate,
                   std::uint64_t maxlen, const std::string& minid,
                   std::size_t limit) {
    switch (kind) {
    case stream_trim_kind::none:
        return status{};
    case stream_trim_kind::maxlen:
        if (maxlen == 0) {
            return status{rpc_error{rpc_code::invalid_argument,
                                    "MAXLEN requires a positive threshold"}};
        }
        cmd.emplace_back("MAXLEN");
        cmd.emplace_back(approximate ? "~" : "=");
        cmd.push_back(std::to_string(maxlen));
        break;
    case stream_trim_kind::minid:
        if (minid.empty()) {
            return status{rpc_error{rpc_code::invalid_argument,
                                    "MINID requires a threshold ID"}};
        }
        cmd.emplace_back("MINID");
        cmd.emplace_back(approximate ? "~" : "=");
        cmd.push_back(minid);
        break;
    }
    if (limit != 0) {
        if (!approximate) {
            return status{rpc_error{rpc_code::invalid_argument,
                                    "LIMIT requires approximate trimming (~)"}};
        }
        cmd.emplace_back("LIMIT");
        cmd.push_back(std::to_string(limit));
    }
    return status{};
}

bool valid_name(const std::string& value) noexcept {
    return !value.empty();
}

bool read_optional_text(const resp_value& value, std::string& out) {
    if (value.is_null()) {
        out.clear();
        return true;
    }
    if (!detail::is_text(value)) {
        return false;
    }
    out = detail::as_text(value);
    return true;
}

result<stream_entry> parse_claimed_entry(const resp_value& value) {
    if (detail::is_text(value)) {
        // JUSTID: only the entry ID is returned.
        stream_entry entry;
        entry.id = detail::as_text(value);
        if (!stream_id::parse(entry.id, entry.parsed_id)) {
            return detail::protocol_error(
                "a claimed stream entry has an invalid ID: " + entry.id);
        }
        return entry;
    }
    return parse_entry(value);
}

result<std::vector<stream_entry>> parse_claimed_entries(const resp_value& value,
                                                        const char* operation) {
    if (detail::is_error_reply(value)) {
        return detail::application_error(value);
    }
    if (!value.is_array()) {
        return detail::protocol_error(std::string(operation) +
                                      " did not return an array");
    }
    std::vector<stream_entry> entries;
    entries.reserve(value.as_array().size());
    for (const resp_value& item : value.as_array()) {
        result<stream_entry> entry = parse_claimed_entry(item);
        if (!entry) {
            return entry.error();
        }
        entries.push_back(std::move(entry).value());
    }
    return entries;
}

} // namespace

// --- stream_id ---------------------------------------------------------------

bool stream_id::parse(std::string_view text, stream_id& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const char first = text.front();
    if (first == '-' || first == '+' || first == '$' || first == '*') {
        return false;
    }

    const std::size_t dash = text.find('-');
    if (dash == std::string_view::npos) {
        stream_id parsed;
        if (!parse_u64(text, parsed.ms)) {
            return false;
        }
        out = parsed;
        return true;
    }
    if (text.find('-', dash + 1) != std::string_view::npos) {
        return false;
    }

    const std::string_view ms_text = text.substr(0, dash);
    const std::string_view seq_text = text.substr(dash + 1);
    stream_id parsed;
    if (!parse_u64(ms_text, parsed.ms) || !parse_u64(seq_text, parsed.seq)) {
        return false;
    }
    out = parsed;
    return true;
}

std::string stream_id::to_string() const {
    return std::to_string(ms) + "-" + std::to_string(seq);
}

// --- stream ------------------------------------------------------------------

stream::stream(command_executor& executor) noexcept : executor_(&executor) {}

stream::~stream() = default;

result<std::string> stream::add(
    const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& fields,
    const stream_add_options& options) {
    return add(key, fields, "*", options);
}

result<std::string> stream::add(
    const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& fields, std::string id,
    const stream_add_options& options) {
    if (!valid_name(key)) {
        return rpc_error{rpc_code::invalid_argument, "XADD requires a key"};
    }
    if (fields.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "XADD requires at least one field/value pair"};
    }
    if (id.empty()) {
        return rpc_error{rpc_code::invalid_argument, "XADD requires an ID or *"};
    }
    if (options.limit != 0 && options.trim == stream_trim_kind::none) {
        return rpc_error{rpc_code::invalid_argument,
                         "LIMIT requires MAXLEN or MINID"};
    }

    command cmd;
    cmd.reserve(10 + fields.size() * 2);
    cmd.emplace_back("XADD");
    cmd.push_back(key);
    if (options.nomkstream) {
        cmd.emplace_back("NOMKSTREAM");
    }
    const status trimmed =
        append_trim(cmd, options.trim, options.approximate, options.maxlen,
                    options.minid, options.limit);
    if (!trimmed) {
        return trimmed.error();
    }
    cmd.push_back(std::move(id));
    for (const auto& field : fields) {
        cmd.push_back(field.first);
        cmd.push_back(field.second);
    }

    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    const resp_value& value = reply.value();
    if (detail::is_error_reply(value)) {
        return detail::application_error(value);
    }
    if (value.is_null()) {
        return rpc_error{rpc_code::not_found,
                         "XADD NOMKSTREAM did not find the stream"};
    }
    if (!detail::is_text(value)) {
        return detail::protocol_error("XADD did not return an entry ID");
    }
    return detail::as_text(value);
}

result<std::vector<stream_entry>> stream::range(const std::string& key,
                                                const std::string& start,
                                                const std::string& end,
                                                std::size_t count) {
    if (!valid_name(key)) {
        return rpc_error{rpc_code::invalid_argument, "XRANGE requires a key"};
    }
    command cmd;
    cmd.reserve(5);
    cmd.emplace_back("XRANGE");
    cmd.push_back(key);
    cmd.push_back(start);
    cmd.push_back(end);
    if (count != 0) {
        cmd.emplace_back("COUNT");
        cmd.push_back(std::to_string(count));
    }

    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    return parse_entries(reply.value(), "XRANGE");
}

result<std::vector<stream_entry>> stream::reverse_range(const std::string& key,
                                                        const std::string& end,
                                                        const std::string& start,
                                                        std::size_t count) {
    if (!valid_name(key)) {
        return rpc_error{rpc_code::invalid_argument, "XREVRANGE requires a key"};
    }
    command cmd;
    cmd.reserve(5);
    cmd.emplace_back("XREVRANGE");
    cmd.push_back(key);
    cmd.push_back(end);
    cmd.push_back(start);
    if (count != 0) {
        cmd.emplace_back("COUNT");
        cmd.push_back(std::to_string(count));
    }

    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    return parse_entries(reply.value(), "XREVRANGE");
}

result<std::vector<stream_read>> stream::read(
    const std::vector<std::string>& keys, const std::vector<std::string>& ids,
    const stream_read_options& options) {
    if (keys.empty() || keys.size() != ids.size()) {
        return rpc_error{rpc_code::invalid_argument,
                         "XREAD requires one ID per key and at least one key"};
    }

    command cmd;
    cmd.reserve(6 + keys.size() * 2);
    cmd.emplace_back("XREAD");
    if (options.count != 0) {
        cmd.emplace_back("COUNT");
        cmd.push_back(std::to_string(options.count));
    }
    if (options.block.count() > 0) {
        cmd.emplace_back("BLOCK");
        cmd.push_back(std::to_string(options.block.count()));
    }
    cmd.emplace_back("STREAMS");
    for (const std::string& key : keys) {
        cmd.push_back(key);
    }
    for (const std::string& id : ids) {
        cmd.push_back(id);
    }

    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    return parse_read(reply.value());
}

result<std::vector<stream_read>> stream::read_group(
    const std::string& group, const std::string& consumer,
    const std::vector<std::string>& keys, const std::vector<std::string>& ids,
    const stream_read_options& options) {
    if (!valid_name(group) || !valid_name(consumer)) {
        return rpc_error{rpc_code::invalid_argument,
                         "XREADGROUP requires a group and a consumer"};
    }
    if (keys.empty() || keys.size() != ids.size()) {
        return rpc_error{rpc_code::invalid_argument,
                         "XREADGROUP requires one ID per key and at least one key"};
    }

    command cmd;
    cmd.reserve(10 + keys.size() * 2);
    cmd.emplace_back("XREADGROUP");
    cmd.emplace_back("GROUP");
    cmd.push_back(group);
    cmd.push_back(consumer);
    if (options.count != 0) {
        cmd.emplace_back("COUNT");
        cmd.push_back(std::to_string(options.count));
    }
    if (options.block.count() > 0) {
        cmd.emplace_back("BLOCK");
        cmd.push_back(std::to_string(options.block.count()));
    }
    if (options.noack) {
        cmd.emplace_back("NOACK");
    }
    cmd.emplace_back("STREAMS");
    for (const std::string& key : keys) {
        cmd.push_back(key);
    }
    for (const std::string& id : ids) {
        cmd.push_back(id);
    }

    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    return parse_read(reply.value());
}

result<std::int64_t> stream::length(const std::string& key) {
    if (!valid_name(key)) {
        return rpc_error{rpc_code::invalid_argument, "XLEN requires a key"};
    }
    result<resp_value> reply = executor_->execute(command{"XLEN", key});
    if (!reply) {
        return reply.error();
    }
    return as_integer(reply.value(), "XLEN");
}

result<std::int64_t> stream::del(const std::string& key,
                                 const std::vector<std::string>& ids) {
    if (!valid_name(key) || ids.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "XDEL requires a key and at least one ID"};
    }
    command cmd;
    cmd.reserve(2 + ids.size());
    cmd.emplace_back("XDEL");
    cmd.push_back(key);
    for (const std::string& id : ids) {
        cmd.push_back(id);
    }
    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    return as_integer(reply.value(), "XDEL");
}

result<std::int64_t> stream::trim(const std::string& key,
                                  const stream_trim_options& options) {
    if (!valid_name(key)) {
        return rpc_error{rpc_code::invalid_argument, "XTRIM requires a key"};
    }
    if (options.trim == stream_trim_kind::none) {
        return rpc_error{rpc_code::invalid_argument,
                         "XTRIM requires MAXLEN or MINID"};
    }
    command cmd;
    cmd.reserve(7);
    cmd.emplace_back("XTRIM");
    cmd.push_back(key);
    const status trimmed =
        append_trim(cmd, options.trim, options.approximate, options.maxlen,
                    options.minid, options.limit);
    if (!trimmed) {
        return trimmed.error();
    }
    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    return as_integer(reply.value(), "XTRIM");
}

status stream::group_create(const std::string& key, const std::string& group,
                            const std::string& id, bool mkstream) {
    if (!valid_name(key) || !valid_name(group) || id.empty()) {
        return status{rpc_error{rpc_code::invalid_argument,
                                "XGROUP CREATE requires key, group and ID"}};
    }
    command cmd{"XGROUP", "CREATE", key, group, id};
    if (mkstream) {
        cmd.emplace_back("MKSTREAM");
    }
    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    return expect_ok(reply.value(), "XGROUP CREATE");
}

result<std::int64_t> stream::group_destroy(const std::string& key,
                                           const std::string& group) {
    if (!valid_name(key) || !valid_name(group)) {
        return rpc_error{rpc_code::invalid_argument,
                         "XGROUP DESTROY requires key and group"};
    }
    result<resp_value> reply =
        executor_->execute(command{"XGROUP", "DESTROY", key, group});
    if (!reply) {
        return reply.error();
    }
    return as_integer(reply.value(), "XGROUP DESTROY");
}

result<std::int64_t> stream::group_create_consumer(const std::string& key,
                                                   const std::string& group,
                                                   const std::string& consumer) {
    if (!valid_name(key) || !valid_name(group) || !valid_name(consumer)) {
        return rpc_error{rpc_code::invalid_argument,
                         "XGROUP CREATECONSUMER requires key, group and consumer"};
    }
    result<resp_value> reply = executor_->execute(
        command{"XGROUP", "CREATECONSUMER", key, group, consumer});
    if (!reply) {
        return reply.error();
    }
    return as_integer(reply.value(), "XGROUP CREATECONSUMER");
}

result<std::int64_t> stream::group_del_consumer(const std::string& key,
                                                const std::string& group,
                                                const std::string& consumer) {
    if (!valid_name(key) || !valid_name(group) || !valid_name(consumer)) {
        return rpc_error{rpc_code::invalid_argument,
                         "XGROUP DELCONSUMER requires key, group and consumer"};
    }
    result<resp_value> reply = executor_->execute(
        command{"XGROUP", "DELCONSUMER", key, group, consumer});
    if (!reply) {
        return reply.error();
    }
    return as_integer(reply.value(), "XGROUP DELCONSUMER");
}

result<std::int64_t> stream::ack(const std::string& key, const std::string& group,
                                 const std::vector<std::string>& ids) {
    if (!valid_name(key) || !valid_name(group) || ids.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "XACK requires key, group and at least one ID"};
    }
    command cmd;
    cmd.reserve(3 + ids.size());
    cmd.emplace_back("XACK");
    cmd.push_back(key);
    cmd.push_back(group);
    for (const std::string& id : ids) {
        cmd.push_back(id);
    }
    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    return as_integer(reply.value(), "XACK");
}

result<stream_pending_summary> stream::pending(const std::string& key,
                                               const std::string& group) {
    if (!valid_name(key) || !valid_name(group)) {
        return rpc_error{rpc_code::invalid_argument,
                         "XPENDING requires key and group"};
    }
    result<resp_value> reply =
        executor_->execute(command{"XPENDING", key, group});
    if (!reply) {
        return reply.error();
    }
    const resp_value& value = reply.value();
    if (detail::is_error_reply(value)) {
        return detail::application_error(value);
    }
    if (!value.is_array() || value.as_array().size() != 4 ||
        !value.as_array()[0].is_integer()) {
        return detail::protocol_error("XPENDING returned a malformed summary");
    }

    stream_pending_summary summary;
    summary.total = value.as_array()[0].as_integer();
    if (!read_optional_text(value.as_array()[1], summary.smallest_id) ||
        !read_optional_text(value.as_array()[2], summary.greatest_id)) {
        return detail::protocol_error("XPENDING returned malformed range bounds");
    }

    const resp_value& consumers = value.as_array()[3];
    if (consumers.is_null()) {
        return summary;
    }
    if (!consumers.is_array()) {
        return detail::protocol_error(
            "XPENDING returned a malformed consumer list");
    }
    summary.consumers.reserve(consumers.as_array().size());
    for (const resp_value& item : consumers.as_array()) {
        if (!item.is_array() || item.as_array().size() != 2 ||
            !detail::is_text(item.as_array()[0]) ||
            !item.as_array()[1].is_integer()) {
            return detail::protocol_error(
                "XPENDING returned a malformed consumer entry");
        }
        summary.consumers.emplace_back(detail::as_text(item.as_array()[0]),
                                       item.as_array()[1].as_integer());
    }
    return summary;
}

result<std::vector<stream_pending_entry>> stream::pending_range(
    const std::string& key, const std::string& group, const std::string& start,
    const std::string& end, std::size_t count, const std::string& consumer,
    std::chrono::milliseconds idle) {
    if (!valid_name(key) || !valid_name(group)) {
        return rpc_error{rpc_code::invalid_argument,
                         "XPENDING requires key and group"};
    }
    if (count == 0) {
        return rpc_error{rpc_code::invalid_argument,
                         "XPENDING range form requires a positive count"};
    }
    if (idle.count() < 0) {
        return rpc_error{rpc_code::invalid_argument,
                         "XPENDING IDLE cannot be negative"};
    }

    command cmd;
    cmd.reserve(8);
    cmd.emplace_back("XPENDING");
    cmd.push_back(key);
    cmd.push_back(group);
    if (idle.count() > 0) {
        cmd.emplace_back("IDLE");
        cmd.push_back(std::to_string(idle.count()));
    }
    cmd.push_back(start);
    cmd.push_back(end);
    cmd.push_back(std::to_string(count));
    if (!consumer.empty()) {
        cmd.push_back(consumer);
    }

    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    const resp_value& value = reply.value();
    if (detail::is_error_reply(value)) {
        return detail::application_error(value);
    }
    if (!value.is_array()) {
        return detail::protocol_error(
            "XPENDING range form did not return an array");
    }

    std::vector<stream_pending_entry> entries;
    entries.reserve(value.as_array().size());
    for (const resp_value& item : value.as_array()) {
        if (!item.is_array() || item.as_array().size() != 4 ||
            !detail::is_text(item.as_array()[0]) ||
            !detail::is_text(item.as_array()[1]) ||
            !item.as_array()[2].is_integer() ||
            !item.as_array()[3].is_integer()) {
            return detail::protocol_error(
                "XPENDING returned a malformed pending entry");
        }
        stream_pending_entry entry;
        entry.id = detail::as_text(item.as_array()[0]);
        entry.consumer = detail::as_text(item.as_array()[1]);
        entry.idle = std::chrono::milliseconds(item.as_array()[2].as_integer());
        entry.deliveries = item.as_array()[3].as_integer();
        entries.push_back(std::move(entry));
    }
    return entries;
}

result<std::vector<stream_entry>> stream::claim(
    const std::string& key, const std::string& group,
    const std::string& consumer, std::chrono::milliseconds min_idle,
    const std::vector<std::string>& ids, const stream_claim_options& options) {
    if (!valid_name(key) || !valid_name(group) || !valid_name(consumer) ||
        ids.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "XCLAIM requires key, group, consumer and IDs"};
    }
    if (min_idle.count() < 0) {
        return rpc_error{rpc_code::invalid_argument,
                         "XCLAIM min-idle-time cannot be negative"};
    }

    command cmd;
    cmd.reserve(8 + ids.size());
    cmd.emplace_back("XCLAIM");
    cmd.push_back(key);
    cmd.push_back(group);
    cmd.push_back(consumer);
    cmd.push_back(std::to_string(min_idle.count()));
    for (const std::string& id : ids) {
        cmd.push_back(id);
    }
    if (options.idle.count() > 0) {
        cmd.emplace_back("IDLE");
        cmd.push_back(std::to_string(options.idle.count()));
    }
    if (options.time.count() > 0) {
        cmd.emplace_back("TIME");
        cmd.push_back(std::to_string(options.time.count()));
    }
    if (options.retry_count >= 0) {
        cmd.emplace_back("RETRYCOUNT");
        cmd.push_back(std::to_string(options.retry_count));
    }
    if (options.force) {
        cmd.emplace_back("FORCE");
    }
    if (options.justid) {
        cmd.emplace_back("JUSTID");
    }
    if (!options.last_id.empty()) {
        cmd.emplace_back("LASTID");
        cmd.push_back(options.last_id);
    }

    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    return parse_claimed_entries(reply.value(), "XCLAIM");
}

result<stream_autoclaim_result> stream::auto_claim(
    const std::string& key, const std::string& group,
    const std::string& consumer, std::chrono::milliseconds min_idle,
    const std::string& start, std::size_t count, bool justid) {
    if (!valid_name(key) || !valid_name(group) || !valid_name(consumer)) {
        return rpc_error{rpc_code::invalid_argument,
                         "XAUTOCLAIM requires key, group and consumer"};
    }
    if (min_idle.count() < 0) {
        return rpc_error{rpc_code::invalid_argument,
                         "XAUTOCLAIM min-idle-time cannot be negative"};
    }

    command cmd;
    cmd.reserve(8);
    cmd.emplace_back("XAUTOCLAIM");
    cmd.push_back(key);
    cmd.push_back(group);
    cmd.push_back(consumer);
    cmd.push_back(std::to_string(min_idle.count()));
    cmd.push_back(start);
    if (count != 0) {
        cmd.emplace_back("COUNT");
        cmd.push_back(std::to_string(count));
    }
    if (justid) {
        cmd.emplace_back("JUSTID");
    }

    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    const resp_value& value = reply.value();
    if (detail::is_error_reply(value)) {
        return detail::application_error(value);
    }
    if (!value.is_array() ||
        (value.as_array().size() != 2 && value.as_array().size() != 3) ||
        !detail::is_text(value.as_array()[0])) {
        return detail::protocol_error("XAUTOCLAIM returned a malformed reply");
    }

    stream_autoclaim_result output;
    output.next_id = detail::as_text(value.as_array()[0]);

    result<std::vector<stream_entry>> claimed =
        parse_claimed_entries(value.as_array()[1], "XAUTOCLAIM");
    if (!claimed) {
        return claimed.error();
    }
    output.entries = std::move(claimed).value();

    if (value.as_array().size() == 3) {
        const resp_value& deleted = value.as_array()[2];
        if (!deleted.is_array()) {
            return detail::protocol_error(
                "XAUTOCLAIM returned a malformed deleted-ID list");
        }
        output.deleted_ids.reserve(deleted.as_array().size());
        for (const resp_value& item : deleted.as_array()) {
            if (!detail::is_text(item)) {
                return detail::protocol_error(
                    "XAUTOCLAIM deleted IDs must be strings");
            }
            output.deleted_ids.push_back(detail::as_text(item));
        }
    }
    return output;
}

} // namespace redis
} // namespace crpc

