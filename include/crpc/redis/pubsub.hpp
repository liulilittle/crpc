// CRPC Redis -- Pub/Sub.
//
// A subscribed Redis connection leaves request/response mode: after SUBSCRIBE
// the server pushes messages at any time, and the only commands it accepts are
// SUBSCRIBE/UNSUBSCRIBE/PSUBSCRIBE/PUNSUBSCRIBE/PING/QUIT. The usual design is
// therefore one dedicated connection for the subscription and a normal
// (possibly pooled) connection for PUBLISH.
//
// `pubsub` takes a `message_channel` -- normally a `command_channel` over its own
// connection. `subscribe()` consumes the acknowledgement pushes so the caller's
// `next()` starts at the first real message. `publish()` and the PUBSUB
// introspection helpers are static because they run on a different executor.
//
// The pattern matcher mirrors Redis `stringmatchlen` exactly: `*`, `?`,
// `[...]` (with ranges, `^` negation and `\` escapes), and `\` outside a class.
// `!` is an ordinary class member, a reversed range has its endpoints swapped,
// a `]` right after `[` closes the class, and a class that runs off the end of
// the pattern never matches. It is the same matcher the server uses for
// PSUBSCRIBE, so a client-side filter agrees with what the server delivers.
#ifndef CRPC_REDIS_PUBSUB_HPP
#define CRPC_REDIS_PUBSUB_HPP

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/redis/pipeline.hpp>

namespace crpc {
namespace redis {

enum class pubsub_kind : std::uint8_t {
    message,
    pmessage,
    subscribe,
    unsubscribe,
    psubscribe,
    punsubscribe,
    pong,
};

[[nodiscard]] CRPC_API const char* to_string(pubsub_kind kind) noexcept;

struct pubsub_message {
    pubsub_kind kind = pubsub_kind::message;

    // Set for `message`, `pmessage`, and the subscription acknowledgements.
    std::string channel;

    // Set for `pmessage` only.
    std::string pattern;

    // Set for `message`, `pmessage` and `pong`.
    std::string payload;

    // Subscription count reported by an acknowledgement; zero for messages.
    std::int64_t count = 0;
};

class CRPC_API pubsub {
public:
    explicit pubsub(message_channel& channel) noexcept;
    ~pubsub();

    pubsub(const pubsub&) = delete;
    pubsub& operator=(const pubsub&) = delete;

    // SUBSCRIBE channel ... Consumes one acknowledgement per channel.
    [[nodiscard]] status subscribe(const std::vector<std::string>& channels);

    // UNSUBSCRIBE [channel ...]. With no channels, unsubscribes everything and
    // consumes acknowledgements until the remaining count reaches zero.
    [[nodiscard]] status unsubscribe(const std::vector<std::string>& channels = {});

    // PSUBSCRIBE pattern ...
    [[nodiscard]] status psubscribe(const std::vector<std::string>& patterns);

    // PUNSUBSCRIBE [pattern ...]
    [[nodiscard]] status punsubscribe(const std::vector<std::string>& patterns = {});

    // Waits for the next push and decodes it. Subscription acknowledgements that
    // were not consumed by subscribe/unsubscribe are returned too, so a caller
    // that manages subscriptions manually can see them.
    [[nodiscard]] result<pubsub_message> next();

    // PUBLISH channel message -> number of receivers.
    [[nodiscard]] static result<std::int64_t> publish(command_executor& executor,
                                                      const std::string& channel,
                                                      const std::string& message);

    // PUBSUB CHANNELS [pattern] -> active channels.
    [[nodiscard]] static result<std::vector<std::string>> channels(
        command_executor& executor, const std::string& pattern = "*");

    // PUBSUB NUMSUB channel ... -> (channel, subscriber count) pairs.
    [[nodiscard]] static result<std::vector<std::pair<std::string, std::int64_t>>>
    numsub(command_executor& executor, const std::vector<std::string>& channels);

    // PUBSUB NUMPAT -> number of pattern subscriptions.
    [[nodiscard]] static result<std::int64_t> numpat(command_executor& executor);

    // Redis glob match, used by PSUBSCRIBE on the server.
    [[nodiscard]] static bool matches(std::string_view pattern,
                                      std::string_view channel) noexcept;

    // Decodes a push/array into a `pubsub_message`. A non-Pub/Sub value or a
    // malformed shape fails with `rpc_code::protocol_error`; a RESP error value
    // fails with `rpc_code::application`.
    [[nodiscard]] static result<pubsub_message> parse(const resp_value& value);

private:
    [[nodiscard]] status subscribe_like(const char* command_name,
                                        pubsub_kind expected,
                                        const std::vector<std::string>& names);

    message_channel* channel_;

    // Messages that arrived while subscribe/unsubscribe was waiting for its
    // acknowledgements. Redis may interleave them, and dropping them would lose
    // data, so `next()` drains this queue first.
    std::deque<pubsub_message> pending_;
};

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_PUBSUB_HPP
