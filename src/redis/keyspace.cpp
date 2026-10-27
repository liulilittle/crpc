// CRPC -- Redis keyspace notifications.
#include <crpc/redis/keyspace.hpp>

#include <algorithm>
#include <string>

namespace crpc {
namespace redis {
namespace {

constexpr std::string_view keyspace_prefix = "__keyspace@";
constexpr std::string_view keyevent_prefix = "__keyevent@";

// Maps a flag character to its bit. 'A' means "every class except m and n", which
// Redis treats as a shortcut rather than a stored bit; it is expanded here so a
// round trip through build/parse is stable.
[[nodiscard]] bool flag_bit(char c, unsigned& out) noexcept {
    switch (c) {
    case 'K': out = bit(notify_class::keyspace); return true;
    case 'E': out = bit(notify_class::keyevent); return true;
    case 'g': out = bit(notify_class::generic); return true;
    case '$': out = bit(notify_class::string); return true;
    case 'l': out = bit(notify_class::list); return true;
    case 's': out = bit(notify_class::set); return true;
    case 'h': out = bit(notify_class::hash); return true;
    case 'z': out = bit(notify_class::zset); return true;
    case 't': out = bit(notify_class::stream); return true;
    case 'x': out = bit(notify_class::expired); return true;
    case 'e': out = bit(notify_class::evicted); return true;
    case 'm': out = bit(notify_class::key_miss); return true;
    case 'n': out = bit(notify_class::new_key); return true;
    case 'd': out = bit(notify_class::module); return true;
    default: return false;
    }
}

// Parses the "<db>__:<subject>" tail after the family prefix.
[[nodiscard]] status split_channel(std::string_view channel, std::string_view prefix,
                                   std::uint32_t& database, std::string& subject) {
    if (channel.size() <= prefix.size() || channel.compare(0, prefix.size(), prefix) != 0) {
        return status{rpc_code::invalid_argument, "not a keyspace notification channel"};
    }
    const std::string_view rest = channel.substr(prefix.size());
    const std::size_t separator = rest.find("__:");
    if (separator == std::string_view::npos || separator == 0) {
        return status{rpc_code::invalid_argument, "a notification channel has no database or subject"};
    }
    std::uint64_t value = 0;
    for (const char c : rest.substr(0, separator)) {
        if (c < '0' || c > '9') {
            return status{rpc_code::invalid_argument, "a notification channel has a non-numeric database"};
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > 0xFFFFFFFFull / 10) {
            return status{rpc_code::invalid_argument, "a notification database overflows"};
        }
        value = value * 10 + digit;
    }
    if (value > 0xFFFFFFFFull) {
        return status{rpc_code::invalid_argument, "a notification database overflows"};
    }
    database = static_cast<std::uint32_t>(value);
    subject.assign(rest.substr(separator + 3).data(), rest.substr(separator + 3).size());
    if (subject.empty()) {
        return status{rpc_code::invalid_argument, "a notification channel has no subject"};
    }
    return status{};
}

} // namespace

result<std::string> build_notify_flags(unsigned classes) {
    const unsigned family = classes & (bit(notify_class::keyspace) | bit(notify_class::keyevent));
    if (family == 0) {
        // Without K or E the server delivers nothing; the caller would subscribe and
        // wait forever, which is the failure this check exists to prevent.
        return rpc_error{rpc_code::invalid_argument,
                         "notify flags need K (keyspace) or E (keyevent)"};
    }

    std::string flags;
    if (classes & bit(notify_class::keyspace)) {
        flags.push_back('K');
    }
    if (classes & bit(notify_class::keyevent)) {
        flags.push_back('E');
    }
    struct entry {
        notify_class value;
        char letter;
    };
    // Redis requires the class letters in a canonical order and rejects a string
    // with an unknown letter, so they are emitted from one table.
    static const entry order[] = {
        {notify_class::generic, 'g'}, {notify_class::string, '$'}, {notify_class::list, 'l'},
        {notify_class::set, 's'},     {notify_class::hash, 'h'},   {notify_class::zset, 'z'},
        {notify_class::stream, 't'},  {notify_class::expired, 'x'}, {notify_class::evicted, 'e'},
        {notify_class::key_miss, 'm'}, {notify_class::new_key, 'n'}, {notify_class::module, 'd'}};
    bool any_class = false;
    for (const entry& item : order) {
        if (classes & bit(item.value)) {
            flags.push_back(item.letter);
            any_class = true;
        }
    }
    // No class letter means "every class" to Redis, which is a legitimate request
    // but is easy to conflate with "nothing". It is expressed as A so the string
    // cannot be misread.
    if (!any_class) {
        flags.push_back('A');
    }
    return flags;
}

result<unsigned> parse_notify_flags(std::string_view flags) {
    unsigned classes = 0;
    for (const char c : flags) {
        if (c == 'A') {
            classes |= bit(notify_class::generic) | bit(notify_class::string) |
                       bit(notify_class::list) | bit(notify_class::set) |
                       bit(notify_class::hash) | bit(notify_class::zset) |
                       bit(notify_class::stream) | bit(notify_class::expired) |
                       bit(notify_class::evicted) | bit(notify_class::module);
            continue;
        }
        unsigned value = 0;
        if (!flag_bit(c, value)) {
            return rpc_error{rpc_code::protocol_error,
                             std::string("unknown notify-keyspace-events flag: ") + c};
        }
        classes |= value;
    }
    return classes;
}

result<notification> parse_notification(std::string_view channel, std::string_view payload) {
    notification out;
    std::uint32_t database = 0;
    std::string subject;
    if (channel.compare(0, keyspace_prefix.size(), keyspace_prefix) == 0) {
        const status split = split_channel(channel, keyspace_prefix, database, subject);
        if (!split) {
            return split.error();
        }
        out.from_keyevent = false;
    } else if (channel.compare(0, keyevent_prefix.size(), keyevent_prefix) == 0) {
        const status split = split_channel(channel, keyevent_prefix, database, subject);
        if (!split) {
            return split.error();
        }
        out.from_keyevent = true;
    } else {
        return rpc_error{rpc_code::invalid_argument, "channel is not a keyspace notification"};
    }
    out.database = database;
    out.subject = std::move(subject);
    out.payload.assign(payload.data(), payload.size());
    return out;
}

std::string keyspace_channel(std::uint32_t database, std::string_view key) {
    std::string channel = "__keyspace@";
    channel += std::to_string(database);
    channel += "__:";
    channel.append(key.data(), key.size());
    return channel;
}

std::string keyevent_channel(std::uint32_t database, std::string_view event) {
    std::string channel = "__keyevent@";
    channel += std::to_string(database);
    channel += "__:";
    channel.append(event.data(), event.size());
    return channel;
}

} // namespace redis
} // namespace crpc
