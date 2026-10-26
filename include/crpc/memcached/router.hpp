// CRPC -- multi-server memcached routing.
//
// `client` owns one endpoint. `router` owns several and decides which one a key
// belongs to, using the libketama ring from `hash.hpp`. Splitting the two keeps a
// single-server deployment free of ring cost and keeps placement in one place.
//
// Three rules shape the behaviour:
//
//   * placement is per key, not per request. A multi-key `get` therefore fans out
//     to every server that owns one of the keys and merges the answers.
//   * a fan-out is all-or-nothing. Returning the keys that happened to succeed
//     would look like "these keys are absent", which is indistinguishable from a
//     cache miss and would let a caller cache a negative result that is really a
//     network error. A partial failure is reported as an error.
//   * fan-out is sequential. Each call parks its coroutine, so a caller that
//     wants concurrency issues concurrent calls; the router does not open an
//     unbounded number of in-flight requests behind the caller's back.
#ifndef CRPC_MEMCACHED_ROUTER_HPP
#define CRPC_MEMCACHED_ROUTER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/memcached/client.hpp>
#include <crpc/memcached/hash.hpp>
#include <crpc/memcached/protocol.hpp>

namespace crpc {
namespace memcached {

// A keyed collection of endpoints.
class CRPC_API router {
public:
    // One endpoint. `name` is the string the ring hashes, conventionally
    // "host:port"; it must be unique and must be the same string every client in
    // the deployment uses, or the placement differs between clients.
    struct server {
        std::string name;
        std::shared_ptr<transport> endpoint;
        // Relative share of the key space. 0 removes the server from the ring
        // without removing it from the fan-out set used by `flush_all`.
        std::uint32_t weight = 1;
    };

    // `opts` is applied to every endpoint. It must not carry a key prefix that
    // differs per server: the ring hashes the key as written, so a prefix that
    // varies would place one key on different servers.
    router(std::vector<server> servers, client::options opts = {});
    ~router();

    router(const router&) = delete;
    router& operator=(const router&) = delete;
    router(router&&) = delete;
    router& operator=(router&&) = delete;

    // --- Keyed operations ----------------------------------------------------
    // Routed to the server that owns `key`. `get` accepts several keys and fans
    // out; a key on a different server is still fetched.
    [[nodiscard]] result<std::vector<value_entry>> get(const std::vector<std::string>& keys);
    [[nodiscard]] result<std::vector<value_entry>> gets(const std::vector<std::string>& keys);

    [[nodiscard]] result<store_status> store(store_kind kind, std::string_view key,
                                             std::string_view value, std::uint32_t flags = 0,
                                             std::uint64_t exptime = 0, std::uint64_t cas = 0);
    [[nodiscard]] result<store_status> erase(std::string_view key, std::uint64_t cas = 0);
    [[nodiscard]] result<std::uint64_t> increment(std::string_view key, std::uint64_t delta);
    [[nodiscard]] result<std::uint64_t> decrement(std::string_view key, std::uint64_t delta);
    [[nodiscard]] result<store_status> touch(std::string_view key, std::uint64_t exptime);

    // --- Fan-out -------------------------------------------------------------
    // Every server in the ring is contacted. A failure on any server fails the
    // call, because "flush half the cache" is not a useful success.
    [[nodiscard]] status flush_all();
    // Version of every server, in the order the servers were given.
    [[nodiscard]] result<std::vector<std::pair<std::string, std::string>>> versions();
    // Per-server stats. The outer vector follows the server order, and each entry
    // pairs the server name with its stats.
    [[nodiscard]] result<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>>
    stats(std::string_view argument = {});

    // --- Placement -----------------------------------------------------------
    // The server index a key is placed on, or -1 when the ring is empty. Exposed
    // because a caller that migrates data needs to ask the same question the
    // router answers.
    [[nodiscard]] std::ptrdiff_t owner_of(std::string_view key) const noexcept;
    [[nodiscard]] std::size_t server_count() const noexcept;
    [[nodiscard]] const std::vector<server>& servers() const noexcept;

private:
    [[nodiscard]] client& client_for(std::string_view key);
    [[nodiscard]] const client& client_for(std::string_view key) const;

    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace memcached
} // namespace crpc

#endif // CRPC_MEMCACHED_ROUTER_HPP