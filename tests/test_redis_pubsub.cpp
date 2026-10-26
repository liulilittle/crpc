// CRPC Redis -- Pub/Sub pushes, acknowledgements and glob matching.
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <crpc/redis/pubsub.hpp>

#include "redis_test_support.hpp"
#include "test_framework.hpp"

using namespace redis_test;

namespace {

int kind_of(const crpc::redis::pubsub_message& message) {
    return static_cast<int>(message.kind);
}

} // namespace

CRPC_TEST(pubsub_glob_matches_literals_and_wildcards) {
    CRPC_CHECK(crpc::redis::pubsub::matches("", ""));
    CRPC_CHECK(crpc::redis::pubsub::matches("*", ""));
    CRPC_CHECK(crpc::redis::pubsub::matches("*", "anything"));
    CRPC_CHECK(crpc::redis::pubsub::matches("news.*", "news.tech"));
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("news.*", "sports.tech"));
    CRPC_CHECK(crpc::redis::pubsub::matches("h?llo", "hello"));
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("h?llo", "hllo"));
    CRPC_CHECK(crpc::redis::pubsub::matches("a*b*c", "aXbYc"));
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("a*b*c", "aXbY"));
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("", "x"));
}

CRPC_TEST(pubsub_glob_matches_character_classes) {
    CRPC_CHECK(crpc::redis::pubsub::matches("h[ae]llo", "hello"));
    CRPC_CHECK(crpc::redis::pubsub::matches("h[ae]llo", "hallo"));
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("h[ae]llo", "hillo"));
    CRPC_CHECK(crpc::redis::pubsub::matches("h[^e]llo", "hallo"));
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("h[^e]llo", "hello"));
    // Redis only treats '^' as negation; '!' is an ordinary class member.
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("h[!e]llo", "hallo"));
    CRPC_CHECK(crpc::redis::pubsub::matches("h[!e]llo", "h!llo"));
    CRPC_CHECK(crpc::redis::pubsub::matches("h[!e]llo", "hello"));
    CRPC_CHECK(crpc::redis::pubsub::matches("h[a-c]llo", "hbllo"));
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("h[a-c]llo", "hdllo"));
    // A reversed range is accepted and its endpoints are swapped.
    CRPC_CHECK(crpc::redis::pubsub::matches("[z-a]", "m"));
    CRPC_CHECK(crpc::redis::pubsub::matches("[z-a]", "z"));
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("[z-a]", "0"));
    // ']' right after '[' closes the class (empty class), it is not a member.
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("[]a]", "]"));
    // '[a-]' is the range ']'..'a' (swapped), so '-' is not a member.
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("[a-]", "-"));
    CRPC_CHECK(crpc::redis::pubsub::matches("[a-]", "]"));
    // An escaped ']' inside a class is a literal member.
    CRPC_CHECK(crpc::redis::pubsub::matches("[\\]]", "]"));
    CRPC_CHECK(crpc::redis::pubsub::matches("a\\*b", "a*b"));
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("a\\*b", "axb"));
    // An unterminated class never matches.
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("a[", "a["));
    CRPC_CHECK_FALSE(crpc::redis::pubsub::matches("[", "]"));
}

CRPC_TEST(pubsub_parses_message_and_pmessage) {
    crpc::result<crpc::redis::pubsub_message> message =
        crpc::redis::pubsub::parse(
            array({text("message"), text("channel"), text("payload")}));
    CRPC_CHECK(message.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::redis::pubsub_kind::message),
                  kind_of(message.value()));
    CRPC_CHECK_EQ(std::string("channel"), message.value().channel);
    CRPC_CHECK_EQ(std::string("payload"), message.value().payload);
    CRPC_CHECK(message.value().pattern.empty());

    crpc::result<crpc::redis::pubsub_message> pattern =
        crpc::redis::pubsub::parse(
            push({text("pmessage"), text("news.*"), text("news.tech"),
                  text("payload")}));
    CRPC_CHECK(pattern.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::redis::pubsub_kind::pmessage),
                  kind_of(pattern.value()));
    CRPC_CHECK_EQ(std::string("news.*"), pattern.value().pattern);
    CRPC_CHECK_EQ(std::string("news.tech"), pattern.value().channel);
    CRPC_CHECK_EQ(std::string("payload"), pattern.value().payload);
}

CRPC_TEST(pubsub_parses_acknowledgements_and_pong) {
    crpc::result<crpc::redis::pubsub_message> ack =
        crpc::redis::pubsub::parse(
            array({text("subscribe"), text("channel"), integer(2)}));
    CRPC_CHECK(ack.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::redis::pubsub_kind::subscribe),
                  kind_of(ack.value()));
    CRPC_CHECK_EQ(std::string("channel"), ack.value().channel);
    CRPC_CHECK_EQ(std::int64_t{2}, ack.value().count);

    crpc::result<crpc::redis::pubsub_message> all =
        crpc::redis::pubsub::parse(
            array({text("unsubscribe"), null_value(), integer(0)}));
    CRPC_CHECK(all.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::redis::pubsub_kind::unsubscribe),
                  kind_of(all.value()));
    CRPC_CHECK(all.value().channel.empty());
    CRPC_CHECK_EQ(std::int64_t{0}, all.value().count);

    crpc::result<crpc::redis::pubsub_message> pong =
        crpc::redis::pubsub::parse(array({text("pong"), text("")}));
    CRPC_CHECK(pong.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::redis::pubsub_kind::pong),
                  kind_of(pong.value()));
}

CRPC_TEST(pubsub_parse_rejects_errors_and_malformed_pushes) {
    crpc::result<crpc::redis::pubsub_message> error_reply =
        crpc::redis::pubsub::parse(error("ERR bad"));
    CRPC_CHECK_FALSE(error_reply.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                  static_cast<int>(error_reply.code()));

    crpc::result<crpc::redis::pubsub_message> not_array =
        crpc::redis::pubsub::parse(text("message"));
    CRPC_CHECK_FALSE(not_array.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(not_array.code()));

    crpc::result<crpc::redis::pubsub_message> short_push =
        crpc::redis::pubsub::parse(array({text("message")}));
    CRPC_CHECK_FALSE(short_push.ok());

    crpc::result<crpc::redis::pubsub_message> unknown =
        crpc::redis::pubsub::parse(
            array({text("mystery"), text("channel"), integer(1)}));
    CRPC_CHECK_FALSE(unknown.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(unknown.code()));

    crpc::result<crpc::redis::pubsub_message> bad_count =
        crpc::redis::pubsub::parse(
            array({text("subscribe"), text("channel"), text("not-an-integer")}));
    CRPC_CHECK_FALSE(bad_count.ok());

    crpc::result<crpc::redis::pubsub_message> bad_message =
        crpc::redis::pubsub::parse(
            array({text("message"), text("channel")}));
    CRPC_CHECK_FALSE(bad_message.ok());
}

CRPC_TEST(pubsub_subscribe_consumes_acks_and_sends_command) {
    scripted_executor executor;
    executor.incoming.push_back(
        array({text("subscribe"), text("a"), integer(1)}));
    executor.incoming.push_back(
        array({text("subscribe"), text("b"), integer(2)}));

    crpc::redis::pubsub subscriber(executor);
    CRPC_CHECK(subscriber.subscribe({"a", "b"}).ok());
    CRPC_CHECK_EQ(std::size_t{1}, executor.sent.size());
    CRPC_CHECK_EQ(std::string("SUBSCRIBE"), executor.sent[0][0]);
    CRPC_CHECK_EQ(std::string("a"), executor.sent[0][1]);
    CRPC_CHECK_EQ(std::string("b"), executor.sent[0][2]);
    CRPC_CHECK(executor.incoming.empty());
}

CRPC_TEST(pubsub_message_between_acks_is_kept_for_next) {
    scripted_executor executor;
    executor.incoming.push_back(
        array({text("subscribe"), text("a"), integer(1)}));
    executor.incoming.push_back(
        push({text("message"), text("a"), text("early")}));
    executor.incoming.push_back(
        array({text("subscribe"), text("b"), integer(2)}));

    crpc::redis::pubsub subscriber(executor);
    CRPC_CHECK(subscriber.subscribe({"a", "b"}).ok());
    CRPC_CHECK(executor.incoming.empty());

    crpc::result<crpc::redis::pubsub_message> message = subscriber.next();
    CRPC_CHECK(message.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::redis::pubsub_kind::message),
                  kind_of(message.value()));
    CRPC_CHECK_EQ(std::string("a"), message.value().channel);
    CRPC_CHECK_EQ(std::string("early"), message.value().payload);
}

CRPC_TEST(pubsub_subscribe_requires_a_channel) {
    scripted_executor executor;
    crpc::redis::pubsub subscriber(executor);
    crpc::status subscribed = subscriber.subscribe({});
    CRPC_CHECK_FALSE(subscribed.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(subscribed.code()));
    CRPC_CHECK(executor.sent.empty());
}

CRPC_TEST(pubsub_unsubscribe_all_reads_until_zero) {
    scripted_executor executor;
    executor.incoming.push_back(
        array({text("unsubscribe"), text("a"), integer(1)}));
    executor.incoming.push_back(
        array({text("unsubscribe"), text("b"), integer(0)}));

    crpc::redis::pubsub subscriber(executor);
    CRPC_CHECK(subscriber.unsubscribe({}).ok());
    CRPC_CHECK_EQ(std::size_t{1}, executor.sent.size());
    CRPC_CHECK_EQ(std::size_t{1}, executor.sent[0].size());
    CRPC_CHECK_EQ(std::string("UNSUBSCRIBE"), executor.sent[0][0]);
    CRPC_CHECK(executor.incoming.empty());
}

CRPC_TEST(pubsub_unsubscribe_explicit_reads_one_ack_per_name) {
    scripted_executor executor;
    executor.incoming.push_back(
        array({text("unsubscribe"), text("a"), integer(0)}));

    crpc::redis::pubsub subscriber(executor);
    CRPC_CHECK(subscriber.unsubscribe({"a"}).ok());
    CRPC_CHECK_EQ(std::size_t{2}, executor.sent[0].size());
    CRPC_CHECK_EQ(std::string("UNSUBSCRIBE"), executor.sent[0][0]);
    CRPC_CHECK_EQ(std::string("a"), executor.sent[0][1]);
}

CRPC_TEST(pubsub_pattern_subscriptions_use_their_own_commands) {
    scripted_executor executor;
    executor.incoming.push_back(
        array({text("psubscribe"), text("news.*"), integer(1)}));
    executor.incoming.push_back(
        array({text("punsubscribe"), text("news.*"), integer(0)}));

    crpc::redis::pubsub subscriber(executor);
    CRPC_CHECK(subscriber.psubscribe({"news.*"}).ok());
    CRPC_CHECK_EQ(std::string("PSUBSCRIBE"), executor.sent[0][0]);
    CRPC_CHECK(subscriber.punsubscribe({"news.*"}).ok());
    CRPC_CHECK_EQ(std::string("PUNSUBSCRIBE"), executor.sent[1][0]);
}

CRPC_TEST(pubsub_next_returns_the_first_push) {
    scripted_executor executor;
    executor.incoming.push_back(
        push({text("message"), text("channel"), text("payload")}));

    crpc::redis::pubsub subscriber(executor);
    crpc::result<crpc::redis::pubsub_message> message = subscriber.next();
    CRPC_CHECK(message.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::redis::pubsub_kind::message),
                  kind_of(message.value()));
    CRPC_CHECK_EQ(std::string("channel"), message.value().channel);
    CRPC_CHECK_EQ(std::string("payload"), message.value().payload);
}

CRPC_TEST(pubsub_next_propagates_eof_and_error_replies) {
    scripted_executor empty;
    crpc::redis::pubsub subscriber(empty);
    crpc::result<crpc::redis::pubsub_message> eof = subscriber.next();
    CRPC_CHECK_FALSE(eof.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::eof),
                  static_cast<int>(eof.code()));

    scripted_executor failing;
    failing.incoming.push_back(error("ERR push failure"));
    crpc::redis::pubsub failing_subscriber(failing);
    crpc::result<crpc::redis::pubsub_message> failed = failing_subscriber.next();
    CRPC_CHECK_FALSE(failed.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                  static_cast<int>(failed.code()));
}

CRPC_TEST(pubsub_publish_and_introspection) {
    command last;
    resp_value reply = integer(3);
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return reply;
    });

    crpc::result<std::int64_t> published =
        crpc::redis::pubsub::publish(executor, "channel", "payload");
    CRPC_CHECK(published.ok());
    CRPC_CHECK_EQ(std::int64_t{3}, published.value());
    CRPC_CHECK_EQ(std::string("PUBLISH"), last[0]);
    CRPC_CHECK_EQ(std::string("channel"), last[1]);
    CRPC_CHECK_EQ(std::string("payload"), last[2]);

    reply = array({text("a"), text("b")});
    crpc::result<std::vector<std::string>> channels =
        crpc::redis::pubsub::channels(executor, "news.*");
    CRPC_CHECK(channels.ok());
    CRPC_CHECK_EQ(std::size_t{2}, channels.value().size());
    CRPC_CHECK_EQ(std::string("a"), channels.value()[0]);
    CRPC_CHECK_EQ(std::string("PUBSUB"), last[0]);
    CRPC_CHECK_EQ(std::string("CHANNELS"), last[1]);
    CRPC_CHECK_EQ(std::string("news.*"), last[2]);

    reply = array({text("a"), integer(2), text("b"), integer(0)});
    crpc::result<std::vector<std::pair<std::string, std::int64_t>>> counts =
        crpc::redis::pubsub::numsub(executor, {"a", "b"});
    CRPC_CHECK(counts.ok());
    CRPC_CHECK_EQ(std::size_t{2}, counts.value().size());
    CRPC_CHECK_EQ(std::string("a"), counts.value()[0].first);
    CRPC_CHECK_EQ(std::int64_t{2}, counts.value()[0].second);

    reply = map({{text("a"), integer(2)}, {text("b"), integer(0)}});
    crpc::result<std::vector<std::pair<std::string, std::int64_t>>> mapped =
        crpc::redis::pubsub::numsub(executor, {"a", "b"});
    CRPC_CHECK(mapped.ok());
    CRPC_CHECK_EQ(std::size_t{2}, mapped.value().size());
    CRPC_CHECK_EQ(std::int64_t{0}, mapped.value()[1].second);

    reply = integer(4);
    crpc::result<std::int64_t> patterns = crpc::redis::pubsub::numpat(executor);
    CRPC_CHECK(patterns.ok());
    CRPC_CHECK_EQ(std::int64_t{4}, patterns.value());
    CRPC_CHECK_EQ(std::string("NUMPAT"), last[1]);
}

CRPC_TEST(pubsub_introspection_errors_are_propagated) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return error("ERR introspection failed");
    });
    crpc::result<std::vector<std::string>> channels =
        crpc::redis::pubsub::channels(executor);
    CRPC_CHECK_FALSE(channels.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                  static_cast<int>(channels.code()));

    scripted_executor malformed([](const command&) -> crpc::result<resp_value> {
        return text("not-an-array");
    });
    crpc::result<std::int64_t> numpat =
        crpc::redis::pubsub::numpat(malformed);
    CRPC_CHECK_FALSE(numpat.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(numpat.code()));
}

CRPC_TEST_MAIN()
