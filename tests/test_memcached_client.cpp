// CRPC -- memcached client tests.
//
// The client is written against `transport`, so every case here drives it with a
// scripted endpoint: the test asserts the exact bytes the client writes and
// supplies the exact bytes the server would answer. No socket, no server, no
// timing — a command-shaping or reply-interpretation bug fails deterministically.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/memcached/client.hpp>
#include <crpc/memcached/protocol.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::memcached::client;
using crpc::memcached::store_kind;
using crpc::memcached::store_status;
using crpc::memcached::text_parser;
using crpc::memcached::text_reply;
using crpc::memcached::transport;

[[nodiscard]] crpc::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return crpc::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                       text.size());
}

// Decodes exactly one reply from `text`; the test data is always a whole reply.
[[nodiscard]] crpc::result<text_reply> decode(std::string_view text) {
    text_parser parser;
    parser.feed(as_bytes(text));
    text_reply reply;
    if (parser.next(reply) != text_parser::result::reply_ready) {
        return crpc::rpc_error{crpc::rpc_code::protocol_error, "test reply did not decode"};
    }
    return reply;
}

// Records every request and answers from a queue of scripted replies.
class scripted_transport final : public transport {
public:
    std::vector<std::string> requests;
    std::deque<std::string> replies;
    // When set, every exchange fails with this code instead of answering.
    crpc::rpc_code fail_with = crpc::rpc_code::ok;
    int closed = 0;

    crpc::result<text_reply> exchange(crpc::span<const std::byte> request) override {
        requests.emplace_back(reinterpret_cast<const char*>(request.data()), request.size());
        if (fail_with != crpc::rpc_code::ok) {
            return crpc::rpc_error{fail_with, "scripted transport failure"};
        }
        if (replies.empty()) {
            return crpc::rpc_error{crpc::rpc_code::internal, "no scripted reply left"};
        }
        std::string next = std::move(replies.front());
        replies.pop_front();
        return decode(next);
    }

    crpc::status close() noexcept override {
        ++closed;
        return crpc::status{};
    }
};

struct harness {
    std::shared_ptr<scripted_transport> endpoint = std::make_shared<scripted_transport>();
    client instance;

    explicit harness(client::options opts = {})
        : instance(std::move(opts), std::static_pointer_cast<transport>(endpoint)) {}

    void expect(std::string reply) { endpoint->replies.push_back(std::move(reply)); }
    [[nodiscard]] const std::string& last_request() const { return endpoint->requests.back(); }
};

} // namespace

CRPC_TEST(client_get_single_key) {
    harness h;
    h.expect("VALUE user:1 7 5\r\nhello\r\nEND\r\n");
    const auto values = h.instance.get({"user:1"});
    CRPC_CHECK(values.ok());
    if (!values.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("get user:1\r\n"), h.last_request());
    CRPC_CHECK_EQ(std::size_t{1}, values.value().size());
    CRPC_CHECK_EQ(std::string("user:1"), values.value()[0].key);
    CRPC_CHECK_EQ(std::uint32_t{7}, values.value()[0].flags);
    CRPC_CHECK_EQ(std::string("hello"), values.value()[0].data);
}

CRPC_TEST(client_get_multiple_keys_and_miss) {
    harness h;
    // Two keys, one missing: the reply simply omits the miss.
    h.expect("VALUE a 0 1\r\nx\r\nEND\r\n");
    const auto values = h.instance.get({"a", "b", "c"});
    CRPC_CHECK(values.ok());
    if (!values.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("get a b c\r\n"), h.last_request());
    CRPC_CHECK_EQ(std::size_t{1}, values.value().size());
    CRPC_CHECK_EQ(std::string("a"), values.value()[0].key);
}

CRPC_TEST(client_gets_carries_cas) {
    harness h;
    h.expect("VALUE k 0 2 99\r\nhi\r\nEND\r\n");
    const auto values = h.instance.gets({"k"});
    CRPC_CHECK(values.ok());
    if (!values.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("gets k\r\n"), h.last_request());
    CRPC_CHECK_EQ(std::uint64_t{99}, values.value()[0].cas);
}

CRPC_TEST(client_get_and_touch) {
    harness h;
    h.expect("VALUE k 0 1\r\nx\r\nEND\r\n");
    const auto values = h.instance.get_and_touch({"k"}, 120, false);
    CRPC_CHECK(values.ok());
    CRPC_CHECK_EQ(std::string("gat 120 k\r\n"), h.last_request());

    harness with_cas;
    with_cas.expect("VALUE k 0 1 5\r\nx\r\nEND\r\n");
    const auto cas_values = with_cas.instance.get_and_touch({"k"}, 60, true);
    CRPC_CHECK(cas_values.ok());
    CRPC_CHECK_EQ(std::string("gats 60 k\r\n"), with_cas.last_request());
}

CRPC_TEST(client_store_commands) {
    struct shape {
        store_kind kind;
        std::uint64_t cas;
        const char* expected;
    };
    const shape shapes[] = {
        {store_kind::set, 0, "set k 3 60 5\r\nhello\r\n"},
        {store_kind::add, 0, "add k 3 60 5\r\nhello\r\n"},
        {store_kind::replace, 0, "replace k 3 60 5\r\nhello\r\n"},
        {store_kind::append, 0, "append k 3 60 5\r\nhello\r\n"},
        {store_kind::prepend, 0, "prepend k 3 60 5\r\nhello\r\n"},
        {store_kind::cas_cmd, 4242, "cas k 3 60 5 4242\r\nhello\r\n"},
    };
    for (const shape& item : shapes) {
        harness h;
        h.expect("STORED\r\n");
        const auto stored = h.instance.store(item.kind, "k", "hello", 3, 60, item.cas);
        CRPC_CHECK(stored.ok());
        if (stored.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(store_status::stored),
                          static_cast<int>(stored.value()));
        }
        CRPC_CHECK_EQ(std::string(item.expected), h.last_request());
    }
}

CRPC_TEST(client_store_outcomes) {
    harness h;
    h.expect("NOT_STORED\r\n");
    const auto not_stored = h.instance.store(store_kind::add, "k", "v");
    CRPC_CHECK(not_stored.ok());
    if (not_stored.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(store_status::not_stored),
                      static_cast<int>(not_stored.value()));
    }

    harness exists;
    exists.expect("EXISTS\r\n");
    const auto conflict = exists.instance.store(store_kind::cas_cmd, "k", "v", 0, 0, 7);
    CRPC_CHECK(conflict.ok());
    if (conflict.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(store_status::exists),
                      static_cast<int>(conflict.value()));
    }
}

CRPC_TEST(client_store_rejects_bad_arguments) {
    {
        // A cas without a token would make the server read the value length
        // differently, so it is rejected before anything is written.
        harness h;
        const auto result = h.instance.store(store_kind::cas_cmd, "k", "v", 0, 0, 0);
        CRPC_CHECK_FALSE(result.ok());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(result.error().code));
        CRPC_CHECK(h.endpoint->requests.empty());
    }
    {
        // A value above the ceiling is refused locally, not sent and rejected.
        client::options opts;
        opts.max_value_size = 4;
        harness h(opts);
        const auto result = h.instance.store(store_kind::set, "k", "12345");
        CRPC_CHECK_FALSE(result.ok());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                      static_cast<int>(result.error().code));
        CRPC_CHECK(h.endpoint->requests.empty());
    }
    {
        // A key above 250 bytes (after the prefix) is refused.
        client::options opts;
        opts.key_prefix = "ns:";
        harness h(opts);
        const std::string long_key(249, 'k');
        const auto result = h.instance.store(store_kind::set, long_key, "v");
        CRPC_CHECK_FALSE(result.ok());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(result.error().code));
    }
}

CRPC_TEST(client_key_prefix) {
    client::options opts;
    opts.key_prefix = "app1:";
    harness h(opts);
    h.expect("STORED\r\n");
    const auto stored = h.instance.store(store_kind::set, "user:1", "v");
    CRPC_CHECK(stored.ok());
    CRPC_CHECK_EQ(std::string("set app1:user:1 0 0 1\r\nv\r\n"), h.last_request());

    h.expect("VALUE app1:user:1 0 1\r\nv\r\nEND\r\n");
    const auto values = h.instance.get({"user:1"});
    CRPC_CHECK(values.ok());
    CRPC_CHECK_EQ(std::string("get app1:user:1\r\n"), h.last_request());
    if (values.ok()) {
        CRPC_CHECK_EQ(std::string("app1:user:1"), values.value()[0].key);
    }
}

CRPC_TEST(client_delete_and_outcomes) {
    harness h;
    h.expect("DELETED\r\n");
    const auto deleted = h.instance.erase("k");
    CRPC_CHECK(deleted.ok());
    CRPC_CHECK_EQ(std::string("delete k\r\n"), h.last_request());
    if (deleted.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(store_status::deleted),
                      static_cast<int>(deleted.value()));
    }

    h.expect("NOT_FOUND\r\n");
    const auto missing = h.instance.erase("k");
    CRPC_CHECK(missing.ok());
    if (missing.ok()) {
        // Deleting a missing key is a routine outcome, not an error.
        CRPC_CHECK_EQ(static_cast<int>(store_status::not_found),
                      static_cast<int>(missing.value()));
    }

    h.expect("DELETED\r\n");
    const auto with_cas = h.instance.erase("k", 11);
    CRPC_CHECK(with_cas.ok());
    CRPC_CHECK_EQ(std::string("delete k 11\r\n"), h.last_request());
}

CRPC_TEST(client_counters) {
    harness h;
    h.expect("42\r\n");
    const auto incremented = h.instance.increment("counter", 3);
    CRPC_CHECK(incremented.ok());
    CRPC_CHECK_EQ(std::string("incr counter 3\r\n"), h.last_request());
    if (incremented.ok()) {
        CRPC_CHECK_EQ(std::uint64_t{42}, incremented.value());
    }

    h.expect("40\r\n");
    const auto decremented = h.instance.decrement("counter", 2);
    CRPC_CHECK(decremented.ok());
    CRPC_CHECK_EQ(std::string("decr counter 2\r\n"), h.last_request());
    if (decremented.ok()) {
        CRPC_CHECK_EQ(std::uint64_t{40}, decremented.value());
    }

    // A missing counter is an error, not a zero.
    h.expect("NOT_FOUND\r\n");
    const auto missing = h.instance.increment("absent", 1);
    CRPC_CHECK_FALSE(missing.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(missing.error().code));
}

CRPC_TEST(client_touch_version_stats) {
    {
        harness h;
        h.expect("TOUCHED\r\n");
        const auto touched = h.instance.touch("k", 300);
        CRPC_CHECK(touched.ok());
        CRPC_CHECK_EQ(std::string("touch k 300\r\n"), h.last_request());
    }
    {
        harness h;
        h.expect("VERSION 1.6.21\r\n");
        const auto version = h.instance.version();
        CRPC_CHECK(version.ok());
        CRPC_CHECK_EQ(std::string("version\r\n"), h.last_request());
        if (version.ok()) {
            CRPC_CHECK_EQ(std::string("1.6.21"), version.value());
        }
    }
    {
        harness h;
        h.expect("STAT curr_items 3\r\nSTAT bytes 128\r\nEND\r\n");
        const auto stats = h.instance.stats();
        CRPC_CHECK(stats.ok());
        CRPC_CHECK_EQ(std::string("stats\r\n"), h.last_request());
        if (stats.ok()) {
            CRPC_CHECK_EQ(std::size_t{2}, stats.value().size());
            CRPC_CHECK_EQ(std::string("curr_items"), stats.value()[0].first);
            CRPC_CHECK_EQ(std::string("3"), stats.value()[0].second);
        }
    }
    {
        harness h;
        h.expect("RESET\r\n");
        const auto reset = h.instance.stats("reset");
        // `stats reset` answers RESET, which is not a stat list; the client must
        // report that instead of inventing an empty list.
        CRPC_CHECK_FALSE(reset.ok());
        CRPC_CHECK_EQ(std::string("stats reset\r\n"), h.last_request());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                      static_cast<int>(reset.error().code));
    }
}

CRPC_TEST(client_meta_get_hit_and_miss) {
    {
        harness h;
        h.expect("VA 5 f0 c12\r\nhello\r\n");
        const auto reply = h.instance.meta_get("k", {"v", "f", "c"});
        CRPC_CHECK(reply.ok());
        CRPC_CHECK_EQ(std::string("mg k v f c\r\n"), h.last_request());
        if (reply.ok()) {
            CRPC_CHECK_EQ(std::string("hello"), reply.value().value);
            CRPC_CHECK_EQ(std::uint64_t{12}, reply.value().meta_cas);
        }
    }
    {
        harness h;
        h.expect("EN\r\n");
        const auto reply = h.instance.meta_get("absent");
        CRPC_CHECK(reply.ok());
        if (reply.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(text_reply::kind::meta_miss),
                          static_cast<int>(reply.value().type));
        }
    }
}

CRPC_TEST(client_meta_set_and_delete) {
    {
        harness h;
        h.expect("HD\r\n");
        const auto stored = h.instance.meta_set("k", "hello", {"T60"});
        CRPC_CHECK(stored.ok());
        CRPC_CHECK_EQ(std::string("ms k 5 T60\r\nhello\r\n"), h.last_request());
        if (stored.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(store_status::stored),
                          static_cast<int>(stored.value()));
        }
    }
    {
        harness h;
        h.expect("NS\r\n");
        const auto stored = h.instance.meta_set("k", "v", {"ME"});
        CRPC_CHECK(stored.ok());
        if (stored.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(store_status::not_stored),
                          static_cast<int>(stored.value()));
        }
    }
    {
        harness h;
        h.expect("HD\r\n");
        const auto erased = h.instance.meta_delete("k");
        CRPC_CHECK(erased.ok());
        CRPC_CHECK_EQ(std::string("md k\r\n"), h.last_request());
        if (erased.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(store_status::deleted),
                          static_cast<int>(erased.value()));
        }
    }
    {
        harness h;
        h.expect("NF\r\n");
        const auto erased = h.instance.meta_delete("k");
        CRPC_CHECK(erased.ok());
        if (erased.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(store_status::not_found),
                          static_cast<int>(erased.value()));
        }
    }
    {
        harness h;
        h.expect("MN\r\n");
        CRPC_CHECK(h.instance.meta_noop().ok());
        CRPC_CHECK_EQ(std::string("mn\r\n"), h.last_request());
    }
}

CRPC_TEST(client_maps_server_errors) {
    {
        // A malformed request is the caller's fault.
        harness h;
        h.expect("CLIENT_ERROR bad command line format\r\n");
        const auto result = h.instance.store(store_kind::set, "k", "v");
        CRPC_CHECK_FALSE(result.ok());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(result.error().code));
        CRPC_CHECK_EQ(std::string("bad command line format"), result.error().message);
    }
    {
        // A server-side failure is not.
        harness h;
        h.expect("SERVER_ERROR out of memory storing object\r\n");
        const auto result = h.instance.get({"k"});
        CRPC_CHECK_FALSE(result.ok());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::internal),
                      static_cast<int>(result.error().code));
        CRPC_CHECK_EQ(std::string("out of memory storing object"), result.error().message);
    }
    {
        // A bare ERROR is a framing violation.
        harness h;
        h.expect("ERROR\r\n");
        const auto result = h.instance.get({"k"});
        CRPC_CHECK_FALSE(result.ok());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                      static_cast<int>(result.error().code));
    }
}

CRPC_TEST(client_rejects_wrong_reply_kind) {
    harness h;
    // A get answered by a status word means the streams are desynchronised.
    h.expect("STORED\r\n");
    const auto result = h.instance.get({"k"});
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(result.error().code));
}

CRPC_TEST(client_propagates_transport_failure) {
    harness h;
    h.endpoint->fail_with = crpc::rpc_code::connection_reset;
    const auto result = h.instance.get({"k"});
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_reset),
                  static_cast<int>(result.error().code));
}

CRPC_TEST(client_execute_passthrough) {
    harness h;
    h.expect("VERSION 9.9.9\r\n");
    const std::string request = "version\r\n";
    const auto reply = h.instance.execute(as_bytes(request));
    CRPC_CHECK(reply.ok());
    CRPC_CHECK_EQ(request, h.last_request());
    if (reply.ok()) {
        CRPC_CHECK_EQ(std::string("9.9.9"), reply.value().text);
    }
}

CRPC_TEST(client_rejects_empty_key_list) {
    harness h;
    const auto result = h.instance.get({});
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(result.error().code));
    CRPC_CHECK(h.endpoint->requests.empty());
}

CRPC_TEST_MAIN()