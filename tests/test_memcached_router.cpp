// CRPC -- memcached router tests.
//
// Two properties matter and both are testable without a server: a key must go to
// the server the ring chose, and a fan-out must never turn a failure into a
// silent miss.
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/memcached/client.hpp>
#include <crpc/memcached/hash.hpp>
#include <crpc/memcached/protocol.hpp>
#include <crpc/memcached/router.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::memcached::client;
using crpc::memcached::router;
using crpc::memcached::store_kind;
using crpc::memcached::store_status;
using crpc::memcached::text_parser;
using crpc::memcached::text_reply;
using crpc::memcached::transport;

[[nodiscard]] crpc::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return crpc::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                       text.size());
}

[[nodiscard]] crpc::result<text_reply> decode(std::string_view text) {
    text_parser parser;
    parser.feed(as_bytes(text));
    text_reply reply;
    if (parser.next(reply) != text_parser::result::reply_ready) {
        return crpc::rpc_error{crpc::rpc_code::protocol_error, "test reply did not decode"};
    }
    return reply;
}

class recording_transport final : public transport {
public:
    std::vector<std::string> requests;
    std::deque<std::string> replies;
    // One-shot failures, consumed before the scripted replies.
    std::deque<crpc::rpc_code> failures;
    // When set every exchange fails; used to model a node that is down.
    bool fail_always = false;
    crpc::rpc_code fail_code = crpc::rpc_code::connection_refused;
    std::string fail_message = "scripted transport failure";

    crpc::result<text_reply> exchange(crpc::span<const std::byte> request) override {
        requests.emplace_back(reinterpret_cast<const char*>(request.data()), request.size());
        if (fail_always) {
            return crpc::rpc_error{fail_code, fail_message};
        }
        if (!failures.empty()) {
            const crpc::rpc_code code = failures.front();
            failures.pop_front();
            return crpc::rpc_error{code, fail_message};
        }
        if (replies.empty()) {
            return crpc::rpc_error{crpc::rpc_code::internal, "no scripted reply left"};
        }
        std::string next = std::move(replies.front());
        replies.pop_front();
        return decode(next);
    }

    crpc::status close() noexcept override { return crpc::status{}; }
};

// A router over three scripted endpoints, with the endpoint handles kept so a
// case can assert which one received a request.
struct rig {
    std::vector<std::shared_ptr<recording_transport>> endpoints;
    std::unique_ptr<router> instance;

    rig() : rig(crpc::memcached::failover_options{}) {}

    explicit rig(crpc::memcached::failover_options failover) {
        std::vector<router::server> servers;
        const char* names[] = {"10.0.0.1:11211", "10.0.0.2:11211", "10.0.0.3:11211"};
        for (const char* name : names) {
            auto endpoint = std::make_shared<recording_transport>();
            endpoints.push_back(endpoint);
            servers.push_back(router::server{name, endpoint, 1});
        }
        instance.reset(new router(std::move(servers), {}, failover));
    }

    // Routes `key` the way the router does, so a case can script the right
    // endpoint without duplicating the ring.
    [[nodiscard]] std::size_t owner(std::string_view key) const {
        return static_cast<std::size_t>(instance->owner_of(key));
    }

    [[nodiscard]] recording_transport& at(std::size_t index) { return *endpoints[index]; }

    [[nodiscard]] int requests_sent() const {
        int total = 0;
        for (const auto& endpoint : endpoints) {
            total += static_cast<int>(endpoint->requests.size());
        }
        return total;
    }
};

// Finds a key the ring currently places on `server`, without duplicating the
// ketama implementation in the test.
[[nodiscard]] std::string key_owned_by(const rig& r, std::size_t server,
                                       const char* prefix) {
    for (int i = 0; i < 20000; ++i) {
        std::string candidate = std::string(prefix) + std::to_string(i);
        const std::ptrdiff_t owner = r.instance->owner_of(candidate);
        if (owner >= 0 && static_cast<std::size_t>(owner) == server) {
            return candidate;
        }
    }
    return {};
}

[[nodiscard]] std::string value_reply(const std::string& key, const std::string& value) {
    return "VALUE " + key + " 0 " + std::to_string(value.size()) + "\r\n" + value +
           "\r\nEND\r\n";
}

[[nodiscard]] crpc::memcached::failover_options failover(std::uint32_t threshold,
                                                         std::chrono::milliseconds cooldown,
                                                         bool retry_reads = false) {
    crpc::memcached::failover_options options;
    options.failure_threshold = threshold;
    options.retry_cooldown = cooldown;
    options.retry_reads = retry_reads;
    return options;
}

} // namespace

CRPC_TEST(router_places_each_key_on_one_server) {
    rig r;
    CRPC_CHECK_EQ(std::size_t{3}, r.instance->server_count());

    // A store goes to exactly one endpoint: the one the ring names.
    const std::string key = "user:42";
    const std::size_t owner = r.owner(key);
    r.at(owner).replies.push_back("STORED\r\n");
    const auto stored = r.instance->store(store_kind::set, key, "v");
    CRPC_CHECK(stored.ok());
    CRPC_CHECK_EQ(1, r.requests_sent());
    CRPC_CHECK_EQ(std::string("set user:42 0 0 1\r\nv\r\n"), r.at(owner).requests.back());
}

CRPC_TEST(router_get_fans_out_and_merges) {
    rig r;
    // Three keys chosen to land on three different servers, found by asking the
    // router itself rather than by hard-coding hashes.
    std::vector<std::string> keys;
    std::vector<std::size_t> owners;
    for (int i = 0; i < 200 && keys.size() < 3; ++i) {
        const std::string candidate = "k" + std::to_string(i);
        const std::size_t owner = r.owner(candidate);
        bool have = false;
        for (const std::size_t existing : owners) {
            if (existing == owner) {
                have = true;
            }
        }
        if (!have) {
            keys.push_back(candidate);
            owners.push_back(owner);
        }
    }
    CRPC_CHECK_EQ(std::size_t{3}, keys.size());
    if (keys.size() != 3) {
        return;
    }

    for (std::size_t i = 0; i < keys.size(); ++i) {
        const std::string reply = "VALUE " + keys[i] + " 0 " +
                                  std::to_string(keys[i].size()) + "\r\n" + keys[i] +
                                  "\r\nEND\r\n";
        r.at(owners[i]).replies.push_back(reply);
    }

    const auto values = r.instance->get(keys);
    CRPC_CHECK(values.ok());
    if (!values.ok()) {
        return;
    }
    // One request per server, and one merged entry per key.
    CRPC_CHECK_EQ(3, r.requests_sent());
    CRPC_CHECK_EQ(std::size_t{3}, values.value().size());
    for (const auto& entry : values.value()) {
        CRPC_CHECK_EQ(entry.key, entry.data);
    }
}

CRPC_TEST(router_get_fans_out_only_for_keys_present) {
    rig r;
    // Two keys on the same server must produce one request, not two.
    const std::string first = "same-a";
    const std::string second = "same-b";
    const std::size_t owner = r.owner(first);
    // Find a second key with the same owner.
    std::string partner;
    for (int i = 0; i < 500; ++i) {
        const std::string candidate = "p" + std::to_string(i);
        if (r.owner(candidate) == owner) {
            partner = candidate;
            break;
        }
    }
    CRPC_CHECK(!partner.empty());
    if (partner.empty()) {
        return;
    }
    (void)second;

    r.at(owner).replies.push_back("VALUE " + first + " 0 1\r\nx\r\nEND\r\n");
    const auto values = r.instance->get({first, partner});
    CRPC_CHECK(values.ok());
    CRPC_CHECK_EQ(1, r.requests_sent());
}

CRPC_TEST(router_get_fails_whole_call_on_partial_error) {
    rig r;
    std::vector<std::string> keys;
    std::vector<std::size_t> owners;
    for (int i = 0; i < 200 && keys.size() < 2; ++i) {
        const std::string candidate = "e" + std::to_string(i);
        const std::size_t owner = r.owner(candidate);
        bool have = false;
        for (const std::size_t existing : owners) {
            if (existing == owner) {
                have = true;
            }
        }
        if (!have) {
            keys.push_back(candidate);
            owners.push_back(owner);
        }
    }
    CRPC_CHECK_EQ(std::size_t{2}, keys.size());
    if (keys.size() != 2) {
        return;
    }

    // The first server answers, the second reports a server error. A partial
    // result would be indistinguishable from a cache miss for that key.
    r.at(owners[0]).replies.push_back("VALUE " + keys[0] + " 0 1\r\nx\r\nEND\r\n");
    r.at(owners[1]).replies.push_back("SERVER_ERROR broken\r\n");
    const auto values = r.instance->get(keys);
    CRPC_CHECK_FALSE(values.ok());
    if (!values.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::internal),
                      static_cast<int>(values.error().code));
    }
}

CRPC_TEST(router_counters_and_delete_are_keyed) {
    rig r;
    const std::string key = "counter";
    const std::size_t owner = r.owner(key);
    r.at(owner).replies.push_back("7\r\n");
    const auto incremented = r.instance->increment(key, 2);
    CRPC_CHECK(incremented.ok());
    CRPC_CHECK_EQ(1, r.requests_sent());
    if (incremented.ok()) {
        CRPC_CHECK_EQ(std::uint64_t{7}, incremented.value());
    }

    r.at(owner).replies.push_back("DELETED\r\n");
    const auto erased = r.instance->erase(key);
    CRPC_CHECK(erased.ok());
    if (erased.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(store_status::deleted),
                      static_cast<int>(erased.value()));
    }
}

CRPC_TEST(router_flush_all_covers_every_server) {
    rig r;
    // Even a server with weight 0 still holds data, so it must be flushed.
    for (auto& endpoint : r.endpoints) {
        endpoint->replies.push_back("OK\r\n");
    }
    CRPC_CHECK(r.instance->flush_all().ok());
    CRPC_CHECK_EQ(3, r.requests_sent());
    for (const auto& endpoint : r.endpoints) {
        CRPC_CHECK_EQ(std::string("flush_all\r\n"), endpoint->requests.back());
    }
}

CRPC_TEST(router_flush_all_reports_failure) {
    rig r;
    r.at(0).replies.push_back("OK\r\n");
    r.at(1).replies.push_back("SERVER_ERROR cannot flush\r\n");
    const crpc::status result = r.instance->flush_all();
    CRPC_CHECK_FALSE(result.ok());
}

CRPC_TEST(router_versions_follow_server_order) {
    rig r;
    r.at(0).replies.push_back("VERSION 1.6.21\r\n");
    r.at(1).replies.push_back("VERSION 1.6.22\r\n");
    r.at(2).replies.push_back("VERSION 1.6.23\r\n");
    const auto versions = r.instance->versions();
    CRPC_CHECK(versions.ok());
    if (!versions.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{3}, versions.value().size());
    CRPC_CHECK_EQ(std::string("10.0.0.1:11211"), versions.value()[0].first);
    CRPC_CHECK_EQ(std::string("1.6.21"), versions.value()[0].second);
    CRPC_CHECK_EQ(std::string("1.6.23"), versions.value()[2].second);
}

CRPC_TEST(router_empty_key_list_is_rejected) {
    rig r;
    const auto values = r.instance->get({});
    CRPC_CHECK_FALSE(values.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(values.error().code));
    CRPC_CHECK_EQ(0, r.requests_sent());
}

CRPC_TEST(router_without_servers_reports_precondition) {
    router empty(std::vector<router::server>{});
    CRPC_CHECK_EQ(std::size_t{0}, empty.server_count());
    CRPC_CHECK_EQ(std::ptrdiff_t{-1}, empty.owner_of("k"));

    const auto stored = empty.store(store_kind::set, "k", "v");
    CRPC_CHECK_FALSE(stored.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(stored.error().code));

    const auto values = empty.get({"k"});
    CRPC_CHECK_FALSE(values.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(values.error().code));
}

// --- Node health, ejection and recovery -------------------------------------

CRPC_TEST(router_ejects_node_after_consecutive_failures) {
    rig r(failover(2, std::chrono::seconds(60)));
    const std::string key = key_owned_by(r, 1, "eject");
    CRPC_CHECK(!key.empty());
    if (key.empty()) {
        return;
    }

    r.at(1).fail_always = true;

    const auto first = r.instance->store(store_kind::set, key, "v");
    CRPC_CHECK_FALSE(first.ok());
    if (!first.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_refused),
                      static_cast<int>(first.error().code));
    }
    std::vector<crpc::memcached::node_health> health = r.instance->health();
    CRPC_CHECK_EQ(std::size_t{3}, health.size());
    CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::alive),
                  static_cast<int>(health[1].condition));
    CRPC_CHECK_EQ(std::uint32_t{1}, health[1].consecutive_failures);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_refused),
                  static_cast<int>(health[1].last_error));

    const auto second = r.instance->store(store_kind::set, key, "v");
    CRPC_CHECK_FALSE(second.ok());
    health = r.instance->health();
    CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::dead),
                  static_cast<int>(health[1].condition));
    CRPC_CHECK_EQ(std::uint32_t{2}, health[1].consecutive_failures);
    CRPC_CHECK_EQ(std::string("10.0.0.2:11211"), health[1].name);
    CRPC_CHECK(!health[1].last_error_message.empty());
    CRPC_CHECK(health[1].retry_in.count() > 0);
    // The failed writes are reported, never silently retried on another node.
    CRPC_CHECK_EQ(2, static_cast<int>(r.at(1).requests.size()));
    // ... and after the eject, the ring no longer contains that node at all.
    const std::ptrdiff_t owner = r.instance->owner_of(key);
    CRPC_CHECK(owner >= 0);
    CRPC_CHECK(owner != 1);
    for (int i = 0; i < 500; ++i) {
        CRPC_CHECK(r.instance->owner_of("post-eject-" + std::to_string(i)) != 1);
    }
}

CRPC_TEST(router_does_not_retry_reads_by_default) {
    rig r(failover(5, std::chrono::seconds(60)));
    const std::string key = key_owned_by(r, 1, "noretry");
    CRPC_CHECK(!key.empty());
    if (key.empty()) {
        return;
    }

    r.at(1).fail_always = true;
    const auto values = r.instance->get({key});
    CRPC_CHECK_FALSE(values.ok());
    if (!values.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_refused),
                      static_cast<int>(values.error().code));
    }
    // Exactly one request: the failing owner. No other node was asked, because
    // a hit on another node would not be the value that was stored here.
    CRPC_CHECK_EQ(1, r.requests_sent());
    CRPC_CHECK_EQ(std::size_t{1}, r.at(1).requests.size());
    const auto health = r.instance->health();
    CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::alive),
                  static_cast<int>(health[1].condition));
    CRPC_CHECK_EQ(std::uint32_t{1}, health[1].consecutive_failures);
}

CRPC_TEST(router_retries_reads_to_next_live_node_when_enabled) {
    rig r(failover(5, std::chrono::seconds(60), true));
    const std::string key = key_owned_by(r, 1, "retry");
    CRPC_CHECK(!key.empty());
    if (key.empty()) {
        return;
    }

    r.at(1).fail_always = true;
    for (std::size_t i = 0; i < r.endpoints.size(); ++i) {
        if (i != 1) {
            r.at(i).replies.push_back(value_reply(key, "v"));
        }
    }

    const auto values = r.instance->get({key});
    CRPC_CHECK(values.ok());
    if (values.ok()) {
        CRPC_CHECK_EQ(std::size_t{1}, values.value().size());
        if (values.value().size() == 1) {
            CRPC_CHECK_EQ(std::string("v"), values.value()[0].data);
        }
    }
    CRPC_CHECK_EQ(2, r.requests_sent());
    CRPC_CHECK_EQ(std::size_t{1}, r.at(1).requests.size());
    int retried_on = 0;
    for (std::size_t i = 0; i < r.endpoints.size(); ++i) {
        if (i != 1 && r.at(i).requests.size() == 1) {
            ++retried_on;
            CRPC_CHECK_EQ(std::string("get " + key + "\r\n"), r.at(i).requests[0]);
        }
    }
    CRPC_CHECK_EQ(1, retried_on);
}

CRPC_TEST(router_write_failure_is_reported_and_not_retried) {
    rig r(failover(1, std::chrono::seconds(60)));
    const std::string key = key_owned_by(r, 1, "write");
    CRPC_CHECK(!key.empty());
    if (key.empty()) {
        return;
    }

    r.at(1).fail_always = true;
    const auto failed = r.instance->store(store_kind::set, key, "v");
    CRPC_CHECK_FALSE(failed.ok());
    CRPC_CHECK_EQ(1, r.requests_sent());
    CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::dead),
                  static_cast<int>(r.instance->health()[1].condition));

    // A later write is a new operation and follows the new ring: it is not a
    // retry of the failed one, and the remapping is the documented consequence
    // of ejecting a node.
    const std::ptrdiff_t remapped = r.instance->owner_of(key);
    CRPC_CHECK(remapped >= 0);
    CRPC_CHECK(remapped != 1);
    if (remapped >= 0 && remapped != 1) {
        r.at(static_cast<std::size_t>(remapped)).replies.push_back("STORED\r\n");
        const auto stored = r.instance->store(store_kind::set, key, "v");
        CRPC_CHECK(stored.ok());
    }
}

CRPC_TEST(router_explicit_probe_recovers_a_dead_node) {
    rig r(failover(1, std::chrono::milliseconds(0)));
    const std::string key = key_owned_by(r, 1, "probe");
    CRPC_CHECK(!key.empty());
    if (key.empty()) {
        return;
    }

    r.at(1).fail_always = true;
    CRPC_CHECK_FALSE(r.instance->store(store_kind::set, key, "v").ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::dead),
                  static_cast<int>(r.instance->health()[1].condition));

    r.at(1).fail_always = false;
    r.at(1).replies.push_back("VERSION 1.6.24\r\n");
    const auto probed = r.instance->probe_node(1);
    CRPC_CHECK(probed.ok());
    if (probed.ok()) {
        CRPC_CHECK(probed.value());
    }
    const auto health = r.instance->health();
    CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::alive),
                  static_cast<int>(health[1].condition));
    CRPC_CHECK_EQ(std::uint32_t{0}, health[1].consecutive_failures);
    CRPC_CHECK_EQ(std::string("version\r\n"), r.at(1).requests.back());
}

CRPC_TEST(router_auto_probes_a_dead_node_after_cooldown) {
    rig r(failover(1, std::chrono::milliseconds(0)));
    const std::string key = key_owned_by(r, 1, "auto");
    CRPC_CHECK(!key.empty());
    if (key.empty()) {
        return;
    }

    r.at(1).fail_always = true;
    CRPC_CHECK_FALSE(r.instance->store(store_kind::set, key, "v").ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::dead),
                  static_cast<int>(r.instance->health()[1].condition));

    // The cooldown is zero, so the next public call probes first. The probe
    // consumes one VERSION on the recovered node; versions() then asks each
    // node once.
    r.at(1).fail_always = false;
    for (std::size_t i = 0; i < r.endpoints.size(); ++i) {
        r.at(i).replies.push_back("VERSION 1.6.24\r\n");
    }
    r.at(1).replies.push_back("VERSION 1.6.24\r\n");
    const auto versions = r.instance->versions();
    CRPC_CHECK(versions.ok());
    if (versions.ok()) {
        CRPC_CHECK_EQ(std::size_t{3}, versions.value().size());
    }
    CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::alive),
                  static_cast<int>(r.instance->health()[1].condition));
    // Request 0 is the failing set that ejected the node; request 1 is the
    // automatic probe, before versions() asks it again.
    CRPC_CHECK(r.at(1).requests.size() >= 2);
    if (r.at(1).requests.size() >= 2) {
        CRPC_CHECK_EQ(std::string("version\r\n"), r.at(1).requests[1]);
    }
}

CRPC_TEST(router_failed_probe_keeps_node_dead) {
    rig r(failover(1, std::chrono::milliseconds(0)));
    const std::string key = key_owned_by(r, 1, "stilldead");
    CRPC_CHECK(!key.empty());
    if (key.empty()) {
        return;
    }

    r.at(1).fail_always = true;
    CRPC_CHECK_FALSE(r.instance->store(store_kind::set, key, "v").ok());
    const auto probed = r.instance->probe_node(1);
    CRPC_CHECK(probed.ok());
    if (probed.ok()) {
        CRPC_CHECK_FALSE(probed.value());
    }
    const auto health = r.instance->health();
    CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::dead),
                  static_cast<int>(health[1].condition));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_refused),
                  static_cast<int>(health[1].last_error));
    // The cooldown restarts after a failed probe, so a probe of a still-down
    // node cannot spin.
    CRPC_CHECK(health[1].retry_in.count() >= 0);
    const std::ptrdiff_t owner = r.instance->owner_of(key);
    CRPC_CHECK(owner >= 0);
    CRPC_CHECK(owner != 1);
}

CRPC_TEST(router_probe_with_ejection_disabled_reports_failure_but_keeps_alive) {
    // failure_threshold 0 means "observe, never eject". A failed probe must still
    // be reported as a failure even though the node stays in the ring.
    rig r(failover(0, std::chrono::seconds(60)));
    r.at(0).fail_always = true;
    const auto probed = r.instance->probe_node(0);
    CRPC_CHECK(probed.ok());
    if (probed.ok()) {
        CRPC_CHECK_FALSE(probed.value());
    }
    const auto health = r.instance->health();
    CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::alive),
                  static_cast<int>(health[0].condition));
    CRPC_CHECK_EQ(std::uint32_t{1}, health[0].consecutive_failures);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_refused),
                  static_cast<int>(health[0].last_error));
}

CRPC_TEST(router_probe_rejects_an_out_of_range_index) {
    rig r;
    const auto probed = r.instance->probe_node(7);
    CRPC_CHECK_FALSE(probed.ok());
    if (!probed.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(probed.error().code));
    }
    CRPC_CHECK_EQ(0, r.requests_sent());
}

CRPC_TEST(router_all_dead_reports_unavailable_without_looping) {
    // A long cooldown keeps the automatic probes out of this case: every node
    // is ejected and the router must return a definite error, not spin.
    rig r(failover(1, std::chrono::seconds(60)));
    for (auto& endpoint : r.endpoints) {
        endpoint->fail_always = true;
    }

    for (std::size_t server = 0; server < r.endpoints.size(); ++server) {
        const std::string key = key_owned_by(r, server, "alldead");
        CRPC_CHECK(!key.empty());
        if (key.empty()) {
            return;
        }
        const auto stored = r.instance->store(store_kind::set, key, "v");
        CRPC_CHECK_FALSE(stored.ok());
    }

    const auto health = r.instance->health();
    CRPC_CHECK_EQ(std::size_t{3}, health.size());
    for (const auto& node : health) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::dead),
                      static_cast<int>(node.condition));
    }
    CRPC_CHECK_EQ(std::ptrdiff_t{-1}, r.instance->owner_of("any-key"));

    const auto values = r.instance->get({"any-key"});
    CRPC_CHECK_FALSE(values.ok());
    if (!values.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                      static_cast<int>(values.error().code));
    }
    const std::size_t before = static_cast<std::size_t>(r.requests_sent());
    const auto stored = r.instance->store(store_kind::set, "any-key", "v");
    CRPC_CHECK_FALSE(stored.ok());
    if (!stored.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                      static_cast<int>(stored.error().code));
    }
    // With every node dead and no probe due, neither call touched the network.
    CRPC_CHECK_EQ(before, static_cast<std::size_t>(r.requests_sent()));
}

CRPC_TEST_MAIN()