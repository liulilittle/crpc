// CRPC Redis -- Pub/Sub acknowledgements, pushes and Redis glob matching.
#include <crpc/redis/pubsub.hpp>

#include <cstddef>
#include <utility>

#include "reply_util.hpp"

namespace crpc {
namespace redis {
namespace {

bool read_text(const resp_value& value, std::string& out) {
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

const std::vector<resp_value>* pubsub_items(const resp_value& value) noexcept {
    return detail::array_items(value);
}

// Matches the character class starting at `pattern[start]` (which is '[')
// against `ch`. Mirrors Redis `stringmatchlen_impl`: only '^' negates, ']'
// right after '[' closes the class, a reversed range has its endpoints
// swapped, and a class that runs off the end of the pattern keeps whatever
// matched so far. On return `next` is the index just past the class.
bool match_class(std::string_view pattern, std::size_t start, char ch,
                 std::size_t& next) noexcept {
    std::size_t i = start + 1;
    bool negate = false;
    if (i < pattern.size() && pattern[i] == '^') {
        negate = true;
        ++i;
    }

    bool matched = false;

    for (;;) {
        if (i >= pattern.size()) {
            break;
        }
        if (pattern[i] == '\\' && i + 1 < pattern.size()) {
            ++i;
            if (pattern[i] == ch) {
                matched = true;
            }
        } else if (pattern[i] == ']') {
            ++i;
            break;
        } else if (i + 2 < pattern.size() && pattern[i + 1] == '-') {
            char lo = pattern[i];
            char hi = pattern[i + 2];
            if (lo > hi) {
                const char swapped = lo;
                lo = hi;
                hi = swapped;
            }
            if (ch >= lo && ch <= hi) {
                matched = true;
            }
            i += 2;
        } else {
            if (pattern[i] == ch) {
                matched = true;
            }
        }
        ++i;
    }

    next = i;
    return negate ? !matched : matched;
}

bool glob_match(std::string_view pattern, std::string_view text) noexcept {
    std::size_t p = 0;
    std::size_t s = 0;
    std::size_t star_p = std::string_view::npos;
    std::size_t star_s = 0;

    while (s < text.size()) {
        bool advanced = false;
        if (p < pattern.size()) {
            const char pc = pattern[p];
            if (pc == '*') {
                star_p = p;
                star_s = s;
                ++p;
                advanced = true;
            } else if (pc == '?') {
                ++p;
                ++s;
                advanced = true;
            } else if (pc == '\\') {
                if (p + 1 < pattern.size()) {
                    if (pattern[p + 1] == text[s]) {
                        p += 2;
                        ++s;
                        advanced = true;
                    }
                } else if (text[s] == '\\') {
                    ++p;
                    ++s;
                    advanced = true;
                }
            } else if (pc == '[') {
                std::size_t next = 0;
                if (match_class(pattern, p, text[s], next)) {
                    p = next;
                    ++s;
                    advanced = true;
                }
            } else if (pc == text[s]) {
                ++p;
                ++s;
                advanced = true;
            }
        }

        if (advanced) {
            continue;
        }

        if (star_p != std::string_view::npos) {
            ++star_s;
            s = star_s;
            p = star_p + 1;
            continue;
        }
        return false;
    }

    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

} // namespace

const char* to_string(pubsub_kind kind) noexcept {
    switch (kind) {
    case pubsub_kind::message:
        return "message";
    case pubsub_kind::pmessage:
        return "pmessage";
    case pubsub_kind::subscribe:
        return "subscribe";
    case pubsub_kind::unsubscribe:
        return "unsubscribe";
    case pubsub_kind::psubscribe:
        return "psubscribe";
    case pubsub_kind::punsubscribe:
        return "punsubscribe";
    case pubsub_kind::pong:
        return "pong";
    }
    return "unknown";
}

pubsub::pubsub(message_channel& channel) noexcept : channel_(&channel) {}

pubsub::~pubsub() = default;

result<pubsub_message> pubsub::parse(const resp_value& value) {
    if (detail::is_error_reply(value)) {
        return detail::application_error(value);
    }

    const std::vector<resp_value>* items = pubsub_items(value);
    if (items == nullptr) {
        return detail::protocol_error("a Pub/Sub push must be an array or a push");
    }
    if (items->size() < 2 || !detail::is_text((*items)[0])) {
        return detail::protocol_error("a Pub/Sub push has no message kind");
    }

    const std::string& kind = detail::as_text((*items)[0]);
    pubsub_message message;

    if (kind == "message") {
        if (items->size() != 3 || !read_text((*items)[1], message.channel) ||
            !read_text((*items)[2], message.payload)) {
            return detail::protocol_error("malformed Pub/Sub message");
        }
        message.kind = pubsub_kind::message;
        return message;
    }
    if (kind == "pmessage") {
        if (items->size() != 4 || !read_text((*items)[1], message.pattern) ||
            !read_text((*items)[2], message.channel) ||
            !read_text((*items)[3], message.payload)) {
            return detail::protocol_error("malformed Pub/Sub pattern message");
        }
        message.kind = pubsub_kind::pmessage;
        return message;
    }
    if (kind == "pong") {
        if (items->size() != 2 || !read_text((*items)[1], message.payload)) {
            return detail::protocol_error("malformed Pub/Sub pong");
        }
        message.kind = pubsub_kind::pong;
        return message;
    }

    pubsub_kind ack_kind = pubsub_kind::subscribe;
    if (kind == "subscribe") {
        ack_kind = pubsub_kind::subscribe;
    } else if (kind == "unsubscribe") {
        ack_kind = pubsub_kind::unsubscribe;
    } else if (kind == "psubscribe") {
        ack_kind = pubsub_kind::psubscribe;
    } else if (kind == "punsubscribe") {
        ack_kind = pubsub_kind::punsubscribe;
    } else {
        return detail::protocol_error("unknown Pub/Sub message kind: " + kind);
    }

    if (items->size() != 3 || !read_text((*items)[1], message.channel) ||
        !(*items)[2].is_integer()) {
        return detail::protocol_error("malformed Pub/Sub acknowledgement");
    }
    message.kind = ack_kind;
    message.count = (*items)[2].as_integer();
    return message;
}

status pubsub::subscribe_like(const char* command_name, pubsub_kind expected,
                              const std::vector<std::string>& names) {
    command cmd;
    cmd.reserve(1 + names.size());
    cmd.emplace_back(command_name);
    for (const std::string& name : names) {
        cmd.push_back(name);
    }

    const status sent = channel_->send(cmd);
    if (!sent) {
        return sent;
    }

    std::size_t received = 0;
    for (;;) {
        result<resp_value> reply = channel_->receive();
        if (!reply) {
            return reply.error();
        }
        result<pubsub_message> message = parse(reply.value());
        if (!message) {
            return message.error();
        }

        // A message (or a PONG) can be delivered between the subscription
        // acknowledgements. Keep it for `next()` instead of treating it as a
        // protocol violation.
        if (message.value().kind == pubsub_kind::message ||
            message.value().kind == pubsub_kind::pmessage ||
            message.value().kind == pubsub_kind::pong) {
            pending_.push_back(std::move(message).value());
            continue;
        }

        if (message.value().kind != expected) {
            return status{detail::protocol_error(
                std::string("unexpected Pub/Sub acknowledgement for ") +
                command_name)};
        }
        ++received;

        if (!names.empty()) {
            if (received == names.size()) {
                break;
            }
        } else if (message.value().count == 0) {
            break;
        }
    }
    return status{};
}

status pubsub::subscribe(const std::vector<std::string>& channels) {
    if (channels.empty()) {
        return status{rpc_error{rpc_code::invalid_argument,
                                "SUBSCRIBE requires at least one channel"}};
    }
    return subscribe_like("SUBSCRIBE", pubsub_kind::subscribe, channels);
}

status pubsub::unsubscribe(const std::vector<std::string>& channels) {
    return subscribe_like("UNSUBSCRIBE", pubsub_kind::unsubscribe, channels);
}

status pubsub::psubscribe(const std::vector<std::string>& patterns) {
    if (patterns.empty()) {
        return status{rpc_error{rpc_code::invalid_argument,
                                "PSUBSCRIBE requires at least one pattern"}};
    }
    return subscribe_like("PSUBSCRIBE", pubsub_kind::psubscribe, patterns);
}

status pubsub::punsubscribe(const std::vector<std::string>& patterns) {
    return subscribe_like("PUNSUBSCRIBE", pubsub_kind::punsubscribe, patterns);
}

result<pubsub_message> pubsub::next() {
    if (!pending_.empty()) {
        pubsub_message message = std::move(pending_.front());
        pending_.pop_front();
        return message;
    }
    result<resp_value> reply = channel_->receive();
    if (!reply) {
        return reply.error();
    }
    return parse(reply.value());
}

result<std::int64_t> pubsub::publish(command_executor& executor,
                                     const std::string& channel,
                                     const std::string& message) {
    result<resp_value> reply =
        executor.execute(command{"PUBLISH", channel, message});
    if (!reply) {
        return reply.error();
    }
    if (detail::is_error_reply(reply.value())) {
        return detail::application_error(reply.value());
    }
    if (!reply.value().is_integer()) {
        return detail::protocol_error("PUBLISH did not return an integer");
    }
    return reply.value().as_integer();
}

result<std::vector<std::string>> pubsub::channels(command_executor& executor,
                                                  const std::string& pattern) {
    command cmd{"PUBSUB", "CHANNELS"};
    if (!pattern.empty()) {
        cmd.push_back(pattern);
    }
    result<resp_value> reply = executor.execute(cmd);
    if (!reply) {
        return reply.error();
    }
    if (detail::is_error_reply(reply.value())) {
        return detail::application_error(reply.value());
    }
    if (!reply.value().is_array()) {
        return detail::protocol_error("PUBSUB CHANNELS did not return an array");
    }

    std::vector<std::string> names;
    names.reserve(reply.value().as_array().size());
    for (const resp_value& item : reply.value().as_array()) {
        if (!detail::is_text(item)) {
            return detail::protocol_error(
                "PUBSUB CHANNELS returned a non-string channel");
        }
        names.push_back(detail::as_text(item));
    }
    return names;
}

result<std::vector<std::pair<std::string, std::int64_t>>> pubsub::numsub(
    command_executor& executor, const std::vector<std::string>& channels) {
    command cmd{"PUBSUB", "NUMSUB"};
    for (const std::string& channel : channels) {
        cmd.push_back(channel);
    }
    result<resp_value> reply = executor.execute(cmd);
    if (!reply) {
        return reply.error();
    }
    if (detail::is_error_reply(reply.value())) {
        return detail::application_error(reply.value());
    }
    if (reply.value().is_map()) {
        // RESP3 encodes PUBSUB NUMSUB as a map.
        std::vector<std::pair<std::string, std::int64_t>> pairs;
        pairs.reserve(reply.value().as_map().size());
        for (const auto& entry : reply.value().as_map()) {
            if (!detail::is_text(entry.first) || !entry.second.is_integer()) {
                return detail::protocol_error(
                    "PUBSUB NUMSUB returned a malformed map entry");
            }
            pairs.emplace_back(detail::as_text(entry.first),
                               entry.second.as_integer());
        }
        return pairs;
    }
    if (!reply.value().is_array() || (reply.value().as_array().size() % 2) != 0) {
        return detail::protocol_error(
            "PUBSUB NUMSUB did not return channel/count pairs");
    }

    const resp_array& flat = reply.value().as_array();
    std::vector<std::pair<std::string, std::int64_t>> pairs;
    pairs.reserve(flat.size() / 2);
    for (std::size_t i = 0; i < flat.size(); i += 2) {
        if (!detail::is_text(flat[i]) || !flat[i + 1].is_integer()) {
            return detail::protocol_error(
                "PUBSUB NUMSUB returned a malformed pair");
        }
        pairs.emplace_back(detail::as_text(flat[i]), flat[i + 1].as_integer());
    }
    return pairs;
}

result<std::int64_t> pubsub::numpat(command_executor& executor) {
    result<resp_value> reply = executor.execute(command{"PUBSUB", "NUMPAT"});
    if (!reply) {
        return reply.error();
    }
    if (detail::is_error_reply(reply.value())) {
        return detail::application_error(reply.value());
    }
    if (!reply.value().is_integer()) {
        return detail::protocol_error("PUBSUB NUMPAT did not return an integer");
    }
    return reply.value().as_integer();
}

bool pubsub::matches(std::string_view pattern, std::string_view channel) noexcept {
    return glob_match(pattern, channel);
}

} // namespace redis
} // namespace crpc
