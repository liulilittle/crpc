// CRPC -- G19 step2: registry client.
//
// Thread model
// ------------
// `registry_client` is thread-safe by contract. Every calling thread keeps its
// OWN anchor (`thread_local`); a shared anchor would make one thread's commit
// invalidate another thread's in-flight observation, which is the two-order
// two-state defect VERDICT section 5.2 measured (33% of writes rejected).
//
// Retry: three layers, all required (VERDICT section 5.3)
// ------------------------------------------------------
//   1. transport: `rpc::dedup` (the caller sets `call_context::idempotent`);
//   2. service: a persisted `last_anchor` per key, recognised by the request
//      carrying `client_request_id`;
//   3. client: the dual anchor `(authority_epoch, index)`.
//
// A `stale_anchor` answer re-anchors from `rpc_error::metadata["registry.index"]`
// and retries exactly once; a second failure is returned to the caller.
#ifndef CRPC_CLUSTER_REGISTRY_CLIENT_HPP
#define CRPC_CLUSTER_REGISTRY_CLIENT_HPP

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/rpc/client.hpp>

#include <crpc/cluster/registry.hpp>
#include <crpc/cluster/registry_wire.hpp>

namespace crpc {
namespace cluster {

// The dual anchor `(authority_epoch, index)` is defined in registry_wire.hpp;
// `not_stale` is defined there too so the client and the service share one
// dictionary-order predicate.

struct registry_client_options {
    // The bearer token. Required: an empty token is always denied.
    std::string token;
    // When the token was issued, in **system_clock** milliseconds (Unix epoch).
    // The service compares it against its own system clock, so the client and
    // the service must agree on the epoch AND on the wall clock. The resulting
    // dependency on NTP synchronisation is documented in docs/migration.md and
    // is UNVERIFIED. The default stamps "now" at construction, so an ordinary
    // caller is inside the window; `set_token_issued_at` overrides it (tests
    // use that to move outside the window).
    std::chrono::milliseconds token_issued_at{
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())};
    // Per-call timeout applied when the caller sets none.
    std::chrono::milliseconds default_timeout{2000};
    // How many times a `stale_anchor` answer re-anchors and retries. One retry
    // is the contract; a larger value only hides a broken anchor.
    int stale_retries = 1;
};

class CRPC_API registry_client {
public:
    // The transport may be shared: the client owns no state that conflicts
    // with another client on the same connection, and a test that needs both a
    // legitimate and an impersonating caller uses one connection for both.
    explicit registry_client(std::shared_ptr<rpc::client> transport,
                             registry_client_options options = registry_client_options{});
    // Moves an owned transport into the client.
    explicit registry_client(std::unique_ptr<rpc::client> transport,
                             registry_client_options options = registry_client_options{});
    ~registry_client();

    registry_client(const registry_client&) = delete;
    registry_client& operator=(const registry_client&) = delete;
    registry_client(registry_client&&) = delete;
    registry_client& operator=(registry_client&&) = delete;

    // --- Writes ----------------------------------------------------------------
    //
    // Each write is preceded by a `next_fence` allocation on the same key, so
    // the presented token is an authority issuance (registry L-b) rather than a
    // caller-invented number.

    // The request identity for one logical write. A stale-anchor retry reuses
    // it, because the service keys its idempotency map on it; deriving a fresh
    // id per attempt would make the retry unrecognisable.
    void set_request_identity(const std::string& identity);

    // Test/diagnostic seam: how many times a `stale_anchor` answer re-anchors
    // and retries. Zero disables the retry so a caller can observe the raw
    // service answer.
    void set_stale_retries(int retries) noexcept;

    // Test seam: overrides the token issue time the client puts on the wire.
    // The value is system_clock milliseconds.
    void set_token_issued_at(std::chrono::milliseconds issued_at) noexcept;

    [[nodiscard]] result<registry_write_response> register_instance(
        const service_instance& instance, u64 incarnation, u64 writer_epoch);

    // Internal: the retry depth. A `stale_anchor` answer re-anchors and retries
    // this many times AT MOST; the counter must travel with the call, because a
    // config value alone is not decremented by the recursive call and would
    // retry forever (a stack overflow, not a bounded retry).
    [[nodiscard]] result<registry_write_response> register_instance(
        const service_instance& instance, u64 incarnation, u64 writer_epoch,
        int stale_retries_left);

    [[nodiscard]] result<registry_write_response> deregister(
        const std::string& service, const std::string& node_id, u64 incarnation,
        u64 writer_epoch);
    [[nodiscard]] result<registry_write_response> set_state(
        const std::string& service, const std::string& node_id, instance_state state,
        u64 incarnation, u64 writer_epoch);

    // --- Reads -----------------------------------------------------------------

    [[nodiscard]] result<registry_query_response> query(const std::string& service,
                                                        const std::string& node_id = "",
                                                        bool passing_only = false);
    [[nodiscard]] result<registry_anchor> anchor();
    [[nodiscard]] result<u64> next_fence(const std::string& service,
                                         const std::string& node_id);
    [[nodiscard]] result<registry_anchor> ping();

    // The calling thread's anchor, as last observed. Exposed for tests.
    [[nodiscard]] registry_anchor local_anchor() const;

    // The calling thread's anchor for one key. Exposed for tests so a case can
    // assert that two threads observe two independent anchors.
    [[nodiscard]] registry_anchor local_anchor_for(const std::string& key) const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_REGISTRY_CLIENT_HPP
