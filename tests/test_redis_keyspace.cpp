// CRPC -- Redis keyspace notification tests.
//
// Two mistakes here are silent: enabling notifications without K or E delivers
// nothing at all, and a handler that assumes one channel family reads the event name
// as a key. Both are pinned.
#include "test_framework.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/error.hpp>
#include <crpc/redis/keyspace.hpp>

namespace {

using crpc::redis::bit;
using crpc::redis::build_notify_flags;
using crpc::redis::keyevent_channel;
using crpc::redis::keyspace_channel;
using crpc::redis::notify_class;
using crpc::redis::parse_notification;
using crpc::redis::parse_notify_flags;

} // namespace

CRPC_TEST(notify_flags_require_a_channel_family) {
    // Without K or E the server delivers nothing, so the request is refused rather
    // than producing a subscription that never fires.
    const auto none = build_notify_flags(bit(notify_class::string));
    CRPC_CHECK_FALSE(none.ok());
    if (!none.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(none.error().code));
    }
}

CRPC_TEST(notify_flags_are_canonical) {
    const auto flags = build_notify_flags(bit(notify_class::keyevent) |
                                          bit(notify_class::string) |
                                          bit(notify_class::expired));
    CRPC_CHECK(flags.ok());
    if (flags.ok()) {
        CRPC_CHECK_EQ(std::string("E$x"), flags.value());
    }

    // Neither family letter, but both requested: K first, then E.
    const auto both = build_notify_flags(bit(notify_class::keyspace) |
                                         bit(notify_class::keyevent) |
                                         bit(notify_class::hash));
    CRPC_CHECK(both.ok());
    if (both.ok()) {
        CRPC_CHECK_EQ(std::string("KEh"), both.value());
    }
}

CRPC_TEST(notify_flags_without_classes_mean_all) {
    // K/E alone means "every class" to Redis; it is written with A so the string
    // cannot be confused with "nothing".
    const auto flags = build_notify_flags(bit(notify_class::keyspace));
    CRPC_CHECK(flags.ok());
    if (flags.ok()) {
        CRPC_CHECK_EQ(std::string("KA"), flags.value());
    }
}

CRPC_TEST(notify_flags_round_trip) {
    const unsigned classes = bit(notify_class::keyspace) | bit(notify_class::keyevent) |
                             bit(notify_class::list) | bit(notify_class::expired);
    const auto flags = build_notify_flags(classes);
    CRPC_CHECK(flags.ok());
    if (!flags.ok()) {
        return;
    }
    const auto parsed = parse_notify_flags(flags.value());
    CRPC_CHECK(parsed.ok());
    if (parsed.ok()) {
        CRPC_CHECK_EQ(classes, parsed.value());
    }
}

CRPC_TEST(notify_flags_reject_unknown_letters) {
    CRPC_CHECK_FALSE(parse_notify_flags("Kq").ok());
}

CRPC_TEST(notification_parses_both_families) {
    {
        // Keyspace family: channel carries the key, payload carries the event.
        const auto message = parse_notification("__keyspace@2__:user:7", "set");
        CRPC_CHECK(message.ok());
        if (message.ok()) {
            CRPC_CHECK_FALSE(message.value().from_keyevent);
            CRPC_CHECK_EQ(std::uint32_t{2}, message.value().database);
            CRPC_CHECK_EQ(std::string("user:7"), message.value().subject);
            CRPC_CHECK_EQ(std::string("set"), message.value().payload);
        }
    }
    {
        // Keyevent family: channel carries the event, payload carries the key. A
        // handler that assumed the other shape would read "expired" as a key.
        const auto message = parse_notification("__keyevent@0__:expired", "session:42");
        CRPC_CHECK(message.ok());
        if (message.ok()) {
            CRPC_CHECK(message.value().from_keyevent);
            CRPC_CHECK_EQ(std::uint32_t{0}, message.value().database);
            CRPC_CHECK_EQ(std::string("expired"), message.value().subject);
            CRPC_CHECK_EQ(std::string("session:42"), message.value().payload);
        }
    }
}

CRPC_TEST(notification_rejects_foreign_channels) {
    CRPC_CHECK_FALSE(parse_notification("news", "x").ok());
    CRPC_CHECK_FALSE(parse_notification("__keyspace@__:k", "set").ok());
    CRPC_CHECK_FALSE(parse_notification("__keyspace@x__:k", "set").ok());
    CRPC_CHECK_FALSE(parse_notification("__keyspace@1__:", "set").ok());
    CRPC_CHECK_FALSE(parse_notification("__keyspace@1__k", "set").ok());
}

CRPC_TEST(notification_channels_are_built_correctly) {
    CRPC_CHECK_EQ(std::string("__keyspace@3__:user:7"), keyspace_channel(3, "user:7"));
    CRPC_CHECK_EQ(std::string("__keyevent@0__:expired"), keyevent_channel(0, "expired"));
}

CRPC_TEST_MAIN()