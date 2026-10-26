// CRPC -- multi-server memcached routing implementation.
#include <crpc/memcached/router.hpp>

#include <utility>

namespace crpc {
namespace memcached {

struct router::impl {
    std::vector<server> servers;
    std::vector<std::unique_ptr<client>> clients;
    ketama_ring ring;

    impl(std::vector<server> list, client::options opts)
        : servers(std::move(list)), ring(make_nodes()), clients() {
        clients.reserve(servers.size());
        for (const server& entry : servers) {
            clients.push_back(std::unique_ptr<client>(
                new client(opts, entry.endpoint)));
        }
    }

    [[nodiscard]] std::vector<ring_node> make_nodes() const {
        std::vector<ring_node> nodes;
        nodes.reserve(servers.size());
        for (const server& entry : servers) {
            nodes.push_back(ring_node{entry.name, entry.weight});
        }
        return nodes;
    }
};

router::router(std::vector<server> servers, client::options opts)
    : impl_(new impl(std::move(servers), std::move(opts))) {}

router::~router() = default;

std::ptrdiff_t router::owner_of(std::string_view key) const noexcept {
    return impl_->ring.locate(key);
}

std::size_t router::server_count() const noexcept { return impl_->servers.size(); }

const std::vector<router::server>& router::servers() const noexcept {
    return impl_->servers;
}

client& router::client_for(std::string_view key) {
    const std::ptrdiff_t index = impl_->ring.locate(key);
    // Every caller checks `ring.empty()` first, so `index` is never negative
    // here. A negative index would be a bug in this file, and the only safe
    // response is to keep it out of the index arithmetic rather than to read
    // clients[0] for a key no server owns.
    const std::size_t bucket =
        index < 0 ? 0 : static_cast<std::size_t>(index);
    return *impl_->clients[bucket];
}

const client& router::client_for(std::string_view key) const {
    return const_cast<router*>(this)->client_for(key);
}

result<std::vector<value_entry>> router::get(const std::vector<std::string>& keys) {
    if (keys.empty()) {
        return rpc_error{rpc_code::invalid_argument, "memcached retrieval needs at least one key"};
    }
    if (impl_->ring.empty()) {
        return rpc_error{rpc_code::failed_precondition, "memcached router has no reachable server"};
    }

    // Group the keys by the server that owns each one, keeping the caller's order
    // inside each group so the merged result is stable.
    std::vector<std::vector<std::string>> buckets(impl_->servers.size());
    for (const std::string& key : keys) {
        const std::ptrdiff_t index = impl_->ring.locate(key);
        if (index < 0) {
            return rpc_error{rpc_code::failed_precondition,
                             "memcached router has no reachable server"};
        }
        buckets[static_cast<std::size_t>(index)].push_back(key);
    }

    std::vector<value_entry> merged;
    for (std::size_t index = 0; index < buckets.size(); ++index) {
        if (buckets[index].empty()) {
            continue;
        }
        result<std::vector<value_entry>> part = impl_->clients[index]->get(buckets[index]);
        if (!part) {
            // Partial data is not reported as a miss; the whole call fails so the
            // caller cannot mistake a network error for absent keys.
            return part.error();
        }
        for (value_entry& entry : part.value()) {
            merged.push_back(std::move(entry));
        }
    }
    return merged;
}

result<std::vector<value_entry>> router::gets(const std::vector<std::string>& keys) {
    if (keys.empty()) {
        return rpc_error{rpc_code::invalid_argument, "memcached retrieval needs at least one key"};
    }
    if (impl_->ring.empty()) {
        return rpc_error{rpc_code::failed_precondition, "memcached router has no reachable server"};
    }

    std::vector<std::vector<std::string>> buckets(impl_->servers.size());
    for (const std::string& key : keys) {
        const std::ptrdiff_t index = impl_->ring.locate(key);
        if (index < 0) {
            return rpc_error{rpc_code::failed_precondition,
                             "memcached router has no reachable server"};
        }
        buckets[static_cast<std::size_t>(index)].push_back(key);
    }

    std::vector<value_entry> merged;
    for (std::size_t index = 0; index < buckets.size(); ++index) {
        if (buckets[index].empty()) {
            continue;
        }
        result<std::vector<value_entry>> part = impl_->clients[index]->gets(buckets[index]);
        if (!part) {
            return part.error();
        }
        for (value_entry& entry : part.value()) {
            merged.push_back(std::move(entry));
        }
    }
    return merged;
}

result<store_status> router::store(store_kind kind, std::string_view key,
                                   std::string_view value, std::uint32_t flags,
                                   std::uint64_t exptime, std::uint64_t cas) {
    if (impl_->ring.empty()) {
        return rpc_error{rpc_code::failed_precondition, "memcached router has no reachable server"};
    }
    return client_for(key).store(kind, key, value, flags, exptime, cas);
}

result<store_status> router::erase(std::string_view key, std::uint64_t cas) {
    if (impl_->ring.empty()) {
        return rpc_error{rpc_code::failed_precondition, "memcached router has no reachable server"};
    }
    return client_for(key).erase(key, cas);
}

result<std::uint64_t> router::increment(std::string_view key, std::uint64_t delta) {
    if (impl_->ring.empty()) {
        return rpc_error{rpc_code::failed_precondition, "memcached router has no reachable server"};
    }
    return client_for(key).increment(key, delta);
}

result<std::uint64_t> router::decrement(std::string_view key, std::uint64_t delta) {
    if (impl_->ring.empty()) {
        return rpc_error{rpc_code::failed_precondition, "memcached router has no reachable server"};
    }
    return client_for(key).decrement(key, delta);
}

result<store_status> router::touch(std::string_view key, std::uint64_t exptime) {
    if (impl_->ring.empty()) {
        return rpc_error{rpc_code::failed_precondition, "memcached router has no reachable server"};
    }
    return client_for(key).touch(key, exptime);
}

status router::flush_all() {
    if (impl_->servers.empty()) {
        return status{rpc_code::failed_precondition, "memcached router has no server"};
    }
    for (std::size_t index = 0; index < impl_->servers.size(); ++index) {
        // A server with weight 0 is out of the ring but still holds data, so it is
        // still flushed. Leaving it out would be a silent cache split.
        const char command[] = "flush_all\r\n";
        const result<text_reply> reply = impl_->clients[index]->execute(
            span<const std::byte>(reinterpret_cast<const std::byte*>(command),
                                  sizeof(command) - 1));
        if (!reply) {
            return reply.error();
        }
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
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(impl_->servers.size());
    for (std::size_t index = 0; index < impl_->servers.size(); ++index) {
        result<std::string> version = impl_->clients[index]->version();
        if (!version) {
            return version.error();
        }
        out.emplace_back(impl_->servers[index].name, std::move(version.value()));
    }
    return out;
}

result<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>>
router::stats(std::string_view argument) {
    if (impl_->servers.empty()) {
        return rpc_error{rpc_code::failed_precondition, "memcached router has no server"};
    }
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> out;
    out.reserve(impl_->servers.size());
    for (std::size_t index = 0; index < impl_->servers.size(); ++index) {
        result<std::vector<std::pair<std::string, std::string>>> part =
            impl_->clients[index]->stats(argument);
        if (!part) {
            return part.error();
        }
        out.emplace_back(impl_->servers[index].name, std::move(part.value()));
    }
    return out;
}

} // namespace memcached
} // namespace crpc