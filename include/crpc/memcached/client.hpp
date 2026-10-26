// CRPC -- asynchronous memcached client.
//
// One `client` talks to one memcached endpoint. Multi-server placement is a
// separate concern (`router`, built on the ketama ring in `hash.hpp`), so a
// single-server deployment pays for no ring and a sharded one places keys
// deterministically.
//
// The wire format lives in `protocol.hpp`; this layer is command shaping, reply
// interpretation and connection reuse. Two properties matter most:
//
//   * a failure is a value. `CLIENT_ERROR`, `SERVER_ERROR` and a bare `ERROR`
//     all come back as an `rpc_error` carrying the server's own message, so a
//     caller never has to parse a diagnostic string;
//   * `NOT_STORED`/`NOT_FOUND`/`EXISTS` is not an error. It is a normal outcome
//     reported as `store_status`, because `add` on an existing key and `delete`
//     on a missing one are routine.
//
// What is deliberately not here: retries. Whether a retry is safe depends on the
// command and on whether the link failed before or after the request was
// written, so the policy belongs to the caller.
#ifndef CRPC_MEMCACHED_CLIENT_HPP
#define CRPC_MEMCACHED_CLIENT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/memcached/protocol.hpp>
#include <crpc/net/connection.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace memcached {

// One endpoint's request/reply exchange. The client is written against this
// interface rather than a socket, so command shaping and reply interpretation
// are testable with a scripted endpoint and no server.
class CRPC_API transport {
public:
    virtual ~transport() = default;

    // Sends one complete request and returns the one reply it produces. The
    // request must already be a whole command, because a memcached text command
    // is not self-delimiting once written.
    [[nodiscard]] virtual result<text_reply> exchange(span<const std::byte> request) = 0;

    // Terminal close. Idempotent.
    [[nodiscard]] virtual status close() noexcept = 0;
};

// Tunables for the real transport.
struct CRPC_API connection_transport_config {
    // Pool policy plus the endpoint: `pool.connection.ep` names the server.
    net::pool_config pool{};

    // Ceilings applied to the reply parser. A reply above either limit fails the
    // exchange with `resource_exhausted` instead of growing memory.
    std::size_t max_value_size = default_max_value_size;
    std::size_t max_line_size = default_max_line_size;
};

// The real transport: a pool of `crpc::net::connection`s.
//
// Each exchange builds a fresh reply parser from the request it is about to
// write. That is deliberate: a parser carried across exchanges would have to
// prove that the previous exchange consumed exactly one reply, and the only way
// to prove it is to check for leftover bytes — which this does, and then treats a
// leftover as a protocol failure. On any failure the connection is closed and
// dropped instead of being returned to the pool, so a desynchronised stream is
// never handed to the next caller.
class CRPC_API connection_transport final : public transport {
public:
    connection_transport(connection_transport_config config, crpc::backend_factory& backend,
                         runtime* shard = nullptr);
    ~connection_transport() override;

    connection_transport(const connection_transport&) = delete;
    connection_transport& operator=(const connection_transport&) = delete;
    connection_transport(connection_transport&&) = delete;
    connection_transport& operator=(connection_transport&&) = delete;

    [[nodiscard]] result<text_reply> exchange(span<const std::byte> request) override;
    [[nodiscard]] status close() noexcept override;

    [[nodiscard]] net::connection_pool& pool() noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// The storage commands that share one request shape.
enum class store_kind : std::uint8_t { set, add, replace, append, prepend, cas_cmd };

// An asynchronous client for one memcached endpoint.
//
// Thread safety: every method may be called from any thread, and concurrency is
// bounded by the pool inside the transport. Calls are independent: each borrows
// a connection, writes one command, reads one reply and returns the connection.
class CRPC_API client {
public:
    struct options {
        // Pool policy and the endpoint. For the transport-injected constructor
        // only the limits below are meaningful.
        net::pool_config pool{};

        // Reply ceilings. The client also refuses to send a value larger than
        // `max_value_size`, so a deployment with a raised server limit must raise
        // this too.
        std::size_t max_value_size = default_max_value_size;
        std::size_t max_line_size = default_max_line_size;

        // Prepended to every key. Empty by default. The prefix counts against the
        // 250-byte key limit, because that is what the server sees.
        std::string key_prefix;
    };

    client(options opts, crpc::backend_factory& backend, runtime* shard = nullptr);
    // Injected transport: used by tests, and by a caller that supplies its own
    // connection management (a TLS or Unix-socket wrapper, for instance).
    client(options opts, std::shared_ptr<transport> endpoint);
    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;

    // --- Retrieval -----------------------------------------------------------
    // One server, several keys. A missing key is simply absent from the result,
    // which is why the return is a list of entries rather than of optionals:
    // order follows the reply, not the request.
    [[nodiscard]] result<std::vector<value_entry>> get(const std::vector<std::string>& keys);

    // With the CAS token of each entry. Pass that token to
    // `store(store_kind::cas_cmd, ...)` for an optimistic update.
    [[nodiscard]] result<std::vector<value_entry>> gets(const std::vector<std::string>& keys);

    // `gat`/`gats`: fetch and reset the expiry in one round trip.
    [[nodiscard]] result<std::vector<value_entry>> get_and_touch(
        const std::vector<std::string>& keys, std::uint64_t exptime, bool with_cas);

    // --- Storage -------------------------------------------------------------
    [[nodiscard]] result<store_status> store(store_kind kind, std::string_view key,
                                             std::string_view value, std::uint32_t flags = 0,
                                             std::uint64_t exptime = 0, std::uint64_t cas = 0);

    // --- Removal and arithmetic ----------------------------------------------
    [[nodiscard]] result<store_status> erase(std::string_view key, std::uint64_t cas = 0);
    [[nodiscard]] result<std::uint64_t> increment(std::string_view key, std::uint64_t delta);
    [[nodiscard]] result<std::uint64_t> decrement(std::string_view key, std::uint64_t delta);
    [[nodiscard]] result<store_status> touch(std::string_view key, std::uint64_t exptime);

    // --- Server commands -----------------------------------------------------
    [[nodiscard]] result<std::string> version();
    [[nodiscard]] result<std::vector<std::pair<std::string, std::string>>> stats(
        std::string_view argument = {});

    // --- Meta protocol -------------------------------------------------------
    // `mg <key> <flags...>`. Flags are passed through verbatim (for example
    // {"v", "f", "t"}), and the reply carries the hit or miss plus its tokens.
    [[nodiscard]] result<text_reply> meta_get(std::string_view key,
                                              const std::vector<std::string>& flags = {});

    // `ms <key> <bytes> <flags...>`. Returns the server's single-word outcome.
    [[nodiscard]] result<store_status> meta_set(std::string_view key, std::string_view value,
                                                const std::vector<std::string>& flags = {});

    // `md <key> <flags...>`.
    [[nodiscard]] result<store_status> meta_delete(std::string_view key,
                                                   const std::vector<std::string>& flags = {});

    // `mn`: end a meta pipeline and flush the responses.
    [[nodiscard]] status meta_noop();

    // --- Escape hatch --------------------------------------------------------
    // Writes `request` verbatim and returns the decoded reply, so a command this
    // class does not model yet (a new meta flag, a server extension) is reachable
    // without a library change. The request must be a whole command with its CRLF.
    [[nodiscard]] result<text_reply> execute(span<const std::byte> request);

    [[nodiscard]] transport& endpoint() noexcept;
    [[nodiscard]] std::size_t pool_idle() const noexcept;
    [[nodiscard]] std::size_t pool_in_use() const noexcept;

private:
    [[nodiscard]] std::string prefixed(std::string_view key) const;
    [[nodiscard]] status check_value_size(std::string_view value) const;
    [[nodiscard]] result<text_reply> exchange(span<const std::byte> request);

    struct impl;
    std::unique_ptr<impl> impl_;
};

// Maps a reply that is one of the three error kinds to an `rpc_error`, keeping
// the server's message. Returns `ok` when the reply is not an error.
[[nodiscard]] CRPC_API status reply_error(const text_reply& reply);

} // namespace memcached
} // namespace crpc

#endif // CRPC_MEMCACHED_CLIENT_HPP