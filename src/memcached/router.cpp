// CRPC -- multi-server memcached routing implementation.
#include <crpc/memcached/router.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace crpc {
namespace memcached {
namespace {

using steady_clock = std::chrono::steady_clock;

// A node is ejected only when the failure says the endpoint is unreachable.
// A reply - even SERVER_ERROR or a protocol violation - proves the process is
// answering, so it must not be counted as a dead node.
[[nodiscard]] bool is_transport_failure(rpc_code code) noexcept {
    switch (code) {
    case rpc_code::unavailable:
    case rpc_code::connection_refused:
    case rpc_code::connection_reset:
    case rpc_code::broken_pipe:
    case rpc_code::eof:
    case rpc_code::deadline_exceeded:
    case rpc_code::not_acknowledged:
    case rpc_code::shutting_down:
        return true;
    default:
        return false;
    }
}

} // namespace

struct router::impl {
    struct node_state {
        bool alive = true;
        bool probing = false;
        std::uint32_t consecutive_failures = 0;
        rpc_code last_error = rpc_code::ok;
        std::string last_error_message;
        steady_clock::time_point dead_since{};
    };

    std::vector<server> servers;
    std::vector<std::unique_ptr<client>> clients;
    failover_options failover;
    mutable std::mutex mutex;
    ketama_ring ring;
    // Ring node index -> server index. The ring drops weight-0 nodes and dead
    // nodes, so the two indices are not interchangeable.
    std::vector<std::size_t> ring_to_server;
    std::vector<node_state> states;

    impl(std::vector<server> list, client::options opts, failover_options policy)
        : servers(std::move(list)), clients(), failover(policy), ring(),
          ring_to_server(), states(servers.size()) {
        clients.reserve(servers.size());
        for (const server& entry : servers) {
            clients.push_back(std::unique_ptr<client>(new client(opts, entry.endpoint)));
        }
        rebuild_locked();
    }

    // The lock must be held. Rebuilds the ring from the live nodes only, which
    // is what makes an ejected node stop owning keys.
    void rebuild_locked() {
        std::vector<ring_node> nodes;
        std::vector<std::size_t> mapping;
        nodes.reserve(servers.size());
        mapping.reserve(servers.size());
        for (std::size_t index = 0; index < servers.size(); ++index) {
            if (!states[index].alive) {
                continue;
            }
            if (servers[index].weight == 0 || servers[index].name.empty()) {
                continue;
            }
            bool duplicate = false;
            for (const ring_node& kept : nodes) {
                if (kept.name == servers[index].name) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            nodes.push_back(ring_node{servers[index].name, servers[index].weight});
            mapping.push_back(index);
        }
        ring = ketama_ring(std::move(nodes));
        ring_to_server = std::move(mapping);
    }

    // The lock must be held.
    [[nodiscard]] std::ptrdiff_t locate_locked(std::string_view key) const {
        const std::ptrdiff_t node = ring.locate(key);
        if (node < 0) {
            return -1;
        }
        return static_cast<std::ptrdiff_t>(
            ring_to_server[static_cast<std::size_t>(node)]);
    }

    // First live ring node for `key` whose server index is not excluded, walking
    // clockwise from the key's position. Used for read failover; with an empty
    // `excluded` it is the ordinary owner.
    [[nodiscard]] std::ptrdiff_t successor_locked(
        std::string_view key, const std::vector<char>& excluded) const {
        const std::vector<ketama_ring::point>& points = ring.points();
        if (points.empty()) {
            return -1;
        }
        const std::uint32_t hash = digest_u32(md5(key), 0);
        const auto found = std::lower_bound(
            points.begin(), points.end(), hash,
            [](const ketama_ring::point& candidate, std::uint32_t value) {
                return candidate.hash < value;
            });
        const std::size_t start =
            found == points.end()
                ? 0
                : static_cast<std::size_t>(std::distance(points.begin(), found));
        for (std::size_t step = 0; step < points.size(); ++step) {
            const std::size_t node =
                static_cast<std::size_t>(points[(start + step) % points.size()].node);
            const std::size_t server = ring_to_server[node];
            if (server >= excluded.size() || excluded[server] == 0) {
                return static_cast<std::ptrdiff_t>(server);
            }
        }
        return -1;
    }

    // The lock must be held. Any completed exchange, including an application
    // error, proves the endpoint is reachable.
    void mark_reachable_locked(std::size_t index) {
        node_state& state = states[index];
        state.consecutive_failures = 0;
        if (!state.alive) {
            state.alive = true;
            state.dead_since = steady_clock::time_point{};
            rebuild_locked();
        }
    }

    // The lock must be held. Returns true when this failure ejected the node.
    bool mark_failure_locked(std::size_t index, const rpc_error& error,
                             steady_clock::time_point now) {
        if (!is_transport_failure(error.code)) {
            mark_reachable_locked(index);
            return false;
        }
        node_state& state = states[index];
        state.last_error = error.code;
        state.last_error_message = error.message;
        if (state.consecutive_failures !=
            std::numeric_limits<std::uint32_t>::max()) {
            ++state.consecutive_failures;
        }
        if (state.alive && failover.failure_threshold != 0 &&
            state.consecutive_failures >= failover.failure_threshold) {
            state.alive = false;
            state.dead_since = now;
            rebuild_locked();
            return true;
        }
        if (!state.alive) {
            // A failed probe starts a fresh cooldown: a permanently down node is
            // not probed on every subsequent call.
            state.dead_since = now;
        }
        return false;
    }

    void note_reachable(std::size_t index) {
        std::lock_guard<std::mutex> lock(mutex);
        mark_reachable_locked(index);
    }

    void note_failure(std::size_t index, const rpc_error& error) {
        std::lock_guard<std::mutex> lock(mutex);
        mark_failure_locked(index, error, steady_clock::now());
    }

    template <typename T>
    void note_outcome(std::size_t index, const result<T>& outcome) {
        if (outcome) {
            note_reachable(index);
        } else {
            note_failure(index, outcome.error());
        }
    }

    // Probes every dead node whose cooldown elapsed. Probing happens outside the
    // health lock because it performs a network round trip; a `probing` flag
    // keeps two callers from probing the same node at once.
    void maybe_probe() {
        std::vector<std::size_t> due;
        const steady_clock::time_point now = steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (std::size_t index = 0; index < states.size(); ++index) {
                node_state& state = states[index];
                if (state.alive || state.probing) {
                    continue;
                }
                const std::chrono::milliseconds elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - state.dead_since);
                if (elapsed < failover.retry_cooldown) {
                    continue;
                }
                state.probing = true;
                due.push_back(index);
            }
        }
        for (const std::size_t index : due) {
            result<std::string> reply = clients[index]->version();
            std::lock_guard<std::mutex> lock(mutex);
            states[index].probing = false;
            if (!reply) {
                mark_failure_locked(index, reply.error(), steady_clock::now());
                continue;
            }
            mark_reachable_locked(index);
        }
    }

    [[nodiscard]] rpc_error no_live_error() const {
        if (servers.empty()) {
            return rpc_error{rpc_code::failed_precondition,
                             "memcached router has no server"};
        }
        return rpc_error{rpc_code::unavailable,
                         "memcached router has no live server"};
    }

    template <typename Call>
    auto call_keyed(std::string_view key, Call&& call)
        -> decltype(call(*clients.front())) {
        using outcome_type = decltype(call(*clients.front()));
        if (servers.empty()) {
            return outcome_type{rpc_error{rpc_code::failed_precondition,
                                          "memcached router has no server"}};
        }
        maybe_probe();

        std::ptrdiff_t index = -1;
        {
            std::lock_guard<std::mutex> lock(mutex);
            index = locate_locked(key);
        }
        if (index < 0) {
            return outcome_type{no_live_error()};
        }
        outcome_type outcome = call(*clients[static_cast<std::size_t>(index)]);
        note_outcome(static_cast<std::size_t>(index), outcome);
        return outcome;
    }

    // Retrieval, including the optional cross-node retry. See
    // `failover_options::retry_reads` for why this is off by default.
    result<std::vector<value_entry>> read_keys(const std::vector<std::string>& keys,
                                               bool with_cas) {
        std::vector<std::string> pending = keys;
        std::vector<value_entry> merged;
        std::vector<char> excluded(servers.size(), 0);
        rpc_error last_error{rpc_code::unavailable,
                             "memcached router has no live server"};
        bool have_error = false;

        // Each failed round excludes at least one more server, so the bound is
        // the server count; the extra iteration only observes an empty pending
        // list.
        const std::size_t max_rounds = servers.size() + 1;
        for (std::size_t round = 0; round < max_rounds && !pending.empty(); ++round) {
            std::vector<std::vector<std::string>> buckets(servers.size());
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const std::string& key : pending) {
                    const std::ptrdiff_t owner = successor_locked(key, excluded);
                    if (owner < 0) {
                        return have_error
                                   ? result<std::vector<value_entry>>{last_error}
                                   : result<std::vector<value_entry>>{no_live_error()};
                    }
                    buckets[static_cast<std::size_t>(owner)].push_back(key);
                }
            }
            pending.clear();
            bool attempted = false;
            for (std::size_t index = 0; index < buckets.size(); ++index) {
                if (buckets[index].empty()) {
                    continue;
                }
                attempted = true;
                result<std::vector<value_entry>> part =
                    with_cas ? clients[index]->gets(buckets[index])
                             : clients[index]->get(buckets[index]);
                if (part) {
                    note_reachable(index);
                    for (value_entry& entry : part.value()) {
                        merged.push_back(std::move(entry));
                    }
                    continue;
                }

                const rpc_error& error = part.error();
                note_failure(index, error);
                if (!is_transport_failure(error.code) || !failover.retry_reads) {
                    // Partial data is never reported as a miss. Without
                    // retry_reads a transport failure is also final: another
                    // node does not hold this key.
                    return part.error();
                }
                last_error = error;
                have_error = true;
                excluded[index] = 1;
                for (const std::string& key : buckets[index]) {
                    pending.push_back(key);
                }
            }
            if (!attempted) {
                return have_error
                           ? result<std::vector<value_entry>>{last_error}
                           : result<std::vector<value_entry>>{no_live_error()};
            }
        }
        if (!pending.empty()) {
            return have_error
                       ? result<std::vector<value_entry>>{last_error}
                       : result<std::vector<value_entry>>{no_live_error()};
        }
        return merged;
    }
};

router::router(std::vector<server> servers, client::options opts,
               failover_options failover)
    : impl_(new impl(std::move(servers), std::move(opts), failover)) {}

router::~router() = default;

std::ptrdiff_t router::owner_of(std::string_view key) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->locate_locked(key);
}

std::size_t router::server_count() const noexcept { return impl_->servers.size(); }

const std::vector<router::server>& router::servers() const noexcept {
    return impl_->servers;
}

std::vector<node_health> router::health() const {
    std::vector<node_health> out;
    out.reserve(impl_->servers.size());
    const steady_clock::time_point now = steady_clock::now();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (std::size_t index = 0; index < impl_->servers.size(); ++index) {
        const impl::node_state& state = impl_->states[index];
        node_health entry;
        entry.name = impl_->servers[index].name;
        entry.condition =
            state.alive ? node_condition::alive : node_condition::dead;
        entry.consecutive_failures = state.consecutive_failures;
        entry.last_error = state.last_error;
        entry.last_error_message = state.last_error_message;
        if (!state.alive) {
            const std::chrono::milliseconds elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - state.dead_since);
            if (elapsed < impl_->failover.retry_cooldown) {
                entry.retry_in = impl_->failover.retry_cooldown - elapsed;
            }
        }
        out.push_back(std::move(entry));
    }
    return out;
}

result<bool> router::probe_node(std::size_t index) {
    if (index >= impl_->servers.size()) {
        return rpc_error{rpc_code::invalid_argument,
                         "memcached router probe index is out of range"};
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->states[index].probing = true;
    }
    result<std::string> reply = impl_->clients[index]->version();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->states[index].probing = false;
    if (reply) {
        impl_->mark_reachable_locked(index);
        return true;
    }
    impl_->mark_failure_locked(index, reply.error(), steady_clock::now());
    // A transport failure is a failed probe even when ejection is disabled; a
    // server error (or any other reply) still proves the endpoint answered, and
    // mark_failure_locked has already treated it as reachability.
    return !is_transport_failure(reply.error().code);
}

result<std::vector<value_entry>> router::get(const std::vector<std::string>& keys) {
    if (keys.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "memcached retrieval needs at least one key"};
    }
    if (impl_->servers.empty()) {
        return rpc_error{rpc_code::failed_precondition,
                         "memcached router has no server"};
    }
    impl_->maybe_probe();
    return impl_->read_keys(keys, false);
}

result<std::vector<value_entry>> router::gets(const std::vector<std::string>& keys) {
    if (keys.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "memcached retrieval needs at least one key"};
    }
    if (impl_->servers.empty()) {
        return rpc_error{rpc_code::failed_precondition,
                         "memcached router has no server"};
    }
    impl_->maybe_probe();
    return impl_->read_keys(keys, true);
}

result<store_status> router::store(store_kind kind, std::string_view key,
                                   std::string_view value, std::uint32_t flags,
                                   std::uint64_t exptime, std::uint64_t cas) {
    return impl_->call_keyed(key, [&](client& endpoint) {
        return endpoint.store(kind, key, value, flags, exptime, cas);
    });
}

result<store_status> router::erase(std::string_view key, std::uint64_t cas) {
    return impl_->call_keyed(key, [&](client& endpoint) {
        return endpoint.erase(key, cas);
    });
}

result<std::uint64_t> router::increment(std::string_view key, std::uint64_t delta) {
    return impl_->call_keyed(key, [&](client& endpoint) {
        return endpoint.increment(key, delta);
    });
}

result<std::uint64_t> router::decrement(std::string_view key, std::uint64_t delta) {
    return impl_->call_keyed(key, [&](client& endpoint) {
        return endpoint.decrement(key, delta);
    });
}

result<store_status> router::touch(std::string_view key, std::uint64_t exptime) {
    return impl_->call_keyed(key, [&](client& endpoint) {
        return endpoint.touch(key, exptime);
    });
}

status router::flush_all() {
    if (impl_->servers.empty()) {
        return status{rpc_code::failed_precondition, "memcached router has no server"};
    }
    impl_->maybe_probe();
    for (std::size_t index = 0; index < impl_->servers.size(); ++index) {
        // A server with weight 0 is out of the ring but still holds data, so it is
        // still flushed. Leaving it out would be a silent cache split.
        const char command[] = "flush_all\r\n";
        const result<text_reply> reply = impl_->clients[index]->execute(
            span<const std::byte>(reinterpret_cast<const std::byte*>(command),
                                  sizeof(command) - 1));
        if (!reply) {
            impl_->note_failure(index, reply.error());
            return reply.error();
        }
        // The server answered, so it is reachable even if it refused the flush.
        impl_->note_reachable(index);
        const status failed = reply_error(reply.value());
        if (!failed) {
            return failed;
        }
    }
    return status{};
}

result<std::vector<std::pair<std::string, std::string>>> router::versions() {
    if (impl_->servers.empty()) {
        return rpc_error{rpc_code::failed_precondition, "memcached router has no server"};
    }
    impl_->maybe_probe();
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(impl_->servers.size());
    for (std::size_t index = 0; index < impl_->servers.size(); ++index) {
        result<std::string> version = impl_->clients[index]->version();
        if (!version) {
            impl_->note_failure(index, version.error());
            return version.error();
        }
        impl_->note_reachable(index);
        out.emplace_back(impl_->servers[index].name, std::move(version.value()));
    }
    return out;
}

result<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>>
router::stats(std::string_view argument) {
    if (impl_->servers.empty()) {
        return rpc_error{rpc_code::failed_precondition, "memcached router has no server"};
    }
    impl_->maybe_probe();
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> out;
    out.reserve(impl_->servers.size());
    for (std::size_t index = 0; index < impl_->servers.size(); ++index) {
        result<std::vector<std::pair<std::string, std::string>>> part =
            impl_->clients[index]->stats(argument);
        if (!part) {
            impl_->note_failure(index, part.error());
            return part.error();
        }
        impl_->note_reachable(index);
        out.emplace_back(impl_->servers[index].name, std::move(part.value()));
    }
    return out;
}

} // namespace memcached
} // namespace crpc