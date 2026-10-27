// CRPC -- Redis keyspace notifications.
//
// Redis publishes key events as ordinary Pub/Sub messages on two channel families:
//
//   __keyspace@<db>__:<key>    payload is the event name ("set", "expired", ...)
//   __keyevent@<db>__:<event>  payload is the key
//
// Two conventions bite here. First, notifications are **off by default** and are
// enabled per server with `CONFIG SET notify-keyspace-events`, a flag string in
// which `K` or `E` must be present or nothing is delivered at all — a client that
// subscribes without enabling sees an empty stream forever. Second, the channel and
// the payload swap roles between the two families, so a handler that assumes one
// shape reads the event name as a key.
//
// The flag string and the parsing live here; the subscription itself uses the
// existing Pub/Sub channel.
#ifndef CRPC_REDIS_KEYSPACE_HPP
#define CRPC_REDIS_KEYSPACE_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace redis {

// Event classes that can be enabled. `key_miss` and `new_key` generate extra
// traffic for every lookup and every first write, so they are separate flags rather
// than part of "all classes".
enum class notify_class : unsigned {
    keyspace = 1u << 0,  // K: __keyspace@ channels
    keyevent = 1u << 1,  // E: __keyevent@ channels
    generic = 1u << 2,   // g
    string = 1u << 3,    // $
    list = 1u << 4,      // l
    set = 1u << 5,       // s
    hash = 1u << 6,      // h
    zset = 1u << 7,      // z
    stream = 1u << 8,    // t
    expired = 1u << 9,   // x
    evicted = 1u << 10,  // e
    key_miss = 1u << 11, // m
    new_key = 1u << 12,  // n
    module = 1u << 13,   // d
    all_classes = 1u << 14 // A
};

[[nodiscard]] constexpr unsigned bit(notify_class value) noexcept {
    return static_cast<unsigned>(value);
}

// Builds the `notify-keyspace-events` argument from a bitmask.
//
// Returns `invalid_argument` when neither K nor E is set: subscribing would
// otherwise deliver nothing, which looks exactly like a broken subscription.
[[nodiscard]] CRPC_API result<std::string> build_notify_flags(unsigned classes);

// Parses the flag string a server reports (`CONFIG GET`), so a caller can verify
// what is actually enabled instead of assuming its own request took effect.
[[nodiscard]] CRPC_API result<unsigned> parse_notify_flags(std::string_view flags);

// One decoded notification.
struct notification {
    // Which family the message arrived on.
    bool from_keyevent = false;
    // The database number from the channel.
    std::uint32_t database = 0;
    // For a keyspace message, the key; for a keyevent message, the event.
    std::string subject;
    // The value carried in the payload.
    std::string payload;
};

// Decodes one notification. A channel that is not a notification channel is
// `invalid_argument`: it belongs to a different subscription and must not be
// silently reinterpreted.
[[nodiscard]] CRPC_API result<notification> parse_notification(std::string_view channel,
                                                             std::string_view payload);

// Builds the channel pattern for one key, matching either family.
[[nodiscard]] CRPC_API std::string keyspace_channel(std::uint32_t database,
                                                   std::string_view key);

// Builds the channel pattern for one event, matching either family.
[[nodiscard]] CRPC_API std::string keyevent_channel(std::uint32_t database,
                                                   std::string_view event);

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_KEYSPACE_HPP