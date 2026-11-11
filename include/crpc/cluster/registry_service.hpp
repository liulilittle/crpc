// CRPC -- G19 step2: the registry service (server side).
//
// The service is the only writer to the catalog log. `registry` and
// `registry_log` stay untouched (docs/abi.md section 1: this adds symbols only).
//
// Responsibilities
// ----------------
//   * ACL: the presented bearer token is matched against a static table; the
//     principal is injected by the service and never taken from the client.
//   * `epoch_store`: a process-local writer-epoch counter persisted in its own
//     file so a restarted service never hands out an epoch at or below one a
//     surviving writer already used.
//   * Single-instance protection: a named mutex, an in-lock base check against
//     the log, and a truncate to the last CRC-valid record.
//   * Six handlers: register / deregister / set_state / query / next_fence /
//     ping.
//
// Thread safety: every handler is safe to call concurrently (`rpc::server`
// runs a handler pool). The registry is thread-safe; the service's own state
// (ACL table, epoch store, anchor water-mark, idempotency map) is guarded by
// `mutex_`.
#ifndef CRPC_CLUSTER_REGISTRY_SERVICE_HPP
#define CRPC_CLUSTER_REGISTRY_SERVICE_HPP

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <crpc/buffer.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/fs/path.hpp>
#include <crpc/frame.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/server.hpp>
#include <crpc/span.hpp>

#include <crpc/cluster/fence_authority.hpp>
#include <crpc/cluster/kv_store.hpp>
#include <crpc/cluster/registry_acl.hpp>
#include <crpc/cluster/registry_log.hpp>
#include <crpc/cluster/registry.hpp>
#include <crpc/cluster/registry_wire.hpp>

namespace crpc {
namespace cluster {

class raft_node;

// The result of an ACL check. A rule is returned by value (a copy) so the
// caller cannot hold a reference into the table while another thread mutates
// it; the table is read-only after `start()` in practice, but the copy keeps
// that a property of the type rather than of the deployment.
struct acl_decision {
    bool allowed = false;
    acl_rule rule{};
    std::string reason;
};

// The service's configuration. Nothing here grows a registry type: the
// service owns its own state.
struct registry_service_config {
    // Directory for the catalog log and the epoch file. Both live in it.
    fs::path state_dir;
    // The listener. A service without a listener prepares and restores the
    // catalog but binds no port, which is what the single-instance test needs.
    crpc::endpoint listen = crpc::endpoint::tcp("127.0.0.1", 0);
    // The backend must outlive the service (the server holds a borrowed
    // pointer). A service without a backend is allowed only when no listener
    // is configured.
    crpc::backend_factory* backend = nullptr;
    // The static ACL table. A token hash of 0 is never valid.
    std::vector<acl_rule> acl;
    // Replay window for the idempotency map, kept equal to `rpc::dedup`'s.
    std::chrono::milliseconds idempotency_ttl{300000};
    // Bounded idempotency map size. Zero disables the map (the transport-level
    // `rpc::dedup` still applies).
    std::size_t max_idempotency_entries = 4096;
    // registry_config forwarded to the registry.
    registry_config registry{};
    // The fence authority. Step2 uses the local registry allocator; step3
    // replaces it with a leader-committed one and the service does not change.
    fence_authority authority{};
    // The KV store. May be null; when null every KV method returns
    // `failed_precondition` (single-authority mode has no KV data plane).
    kv_store* kv = nullptr;
    // The Raft node used to commit KV writes. May be null: a null raft means
    // KV writes apply directly (single-authority mode).
    raft_node* raft = nullptr;
};

// A persisted write result for the idempotency layer (VERDICT section 5.3).
struct idempotency_record {
    bool completed = false;
    rpc_code code = rpc_code::ok;
    std::string reason;
    registry_write_response response{};
    // When the record was written. The map is bounded by BOTH this timestamp
    // and `max_idempotency_entries`: an expired record is dropped, and when the
    // cap is reached the OLDEST record is evicted one at a time. A `clear()`
    // would drop every in-flight protection at once, which is precisely the
    // replay the second layer exists to prevent.
    std::chrono::steady_clock::time_point recorded_at{};
};

// The persisted `last_anchor` water-mark (VERDICT section 5.1: "persisted next
// to the epoch file"). Without it a restart starts with an empty map, and an
// empty map means "no anchor on record", which `anchor_allows` treats as
// permission -- so a client could replay a pre-restart anchor at a freshly
// started service.
//
// This is SECURITY STATE, so a file that exists but cannot be parsed is a
// startup failure, never a silent empty map: clearing it is exactly the bypass
// it exists to prevent.
class CRPC_API anchor_store {
public:
    [[nodiscard]] static fs::path file_path(const fs::path& state_dir);

    // An absent file is a clean "nothing persisted". A truncated file, a bad
    // key length or trailing bytes are all `failed_precondition` with
    // `registry_reason_anchor_unreadable`.
    [[nodiscard]] static status load(
        const fs::path& state_dir,
        std::map<std::pair<std::string, std::string>, registry_anchor>& out);

    // Atomic write, so a crash mid-write cannot leave a half-written map.
    [[nodiscard]] static status save(
        const fs::path& state_dir,
        const std::map<std::pair<std::string, std::string>, registry_anchor>& value);
};

class CRPC_API registry_service {
public:
    explicit registry_service(registry_service_config config = registry_service_config{});
    ~registry_service();

    registry_service(const registry_service&) = delete;
    registry_service& operator=(const registry_service&) = delete;
    registry_service(registry_service&&) = delete;
    registry_service& operator=(registry_service&&) = delete;

    // Acquires the single-instance lease, opens the log, restores the catalog,
    // seeds the epoch store and registers the six handlers on `builder`.
    // Returns `failed_precondition` (and does NOT bind any port) when another
    // instance already owns the state directory.
    [[nodiscard]] status prepare(server::builder& builder);

    // Starts listening. `prepare` must have succeeded.
    [[nodiscard]] status start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

    // The endpoint the service actually bound, valid after a successful
    // `start()`. When the configured port was 0 the OS chose one; the caller
    // must read it from here rather than assume the configured value.
    [[nodiscard]] crpc::endpoint bound_endpoint() const noexcept;

    // Takes ownership of a built and started server. Used when the caller
    // builds the server itself (the tests do, so they can bind a chosen port);
    // `start()` uses this internally after `build()`.
    void set_server(std::unique_ptr<server> owned);

    // The ACL verdict for a token. Exposed for tests; handlers use it directly.
    // `issued_at` and `has_issued_at` come from the request DTO; a missing
    // issue time is treated as outside the window (fail closed). `now` is the
    // **system** clock: the window is cross-process, and `steady_clock` has a
    // per-process epoch that cannot be compared across a process boundary.
    [[nodiscard]] acl_decision check_acl(const std::string& token, u64 method,
                                         const std::string& service,
                                         const std::string& node_id,
                                         std::chrono::system_clock::time_point now,
                                         std::chrono::milliseconds issued_at,
                                         bool has_issued_at) const;

    // The current authority anchor. Exposed for tests.
    [[nodiscard]] registry_anchor anchor() const;

    // Runs one method against the service exactly as the wire handlers do.
    //
    // This is the seam the HTTP facade uses. It takes the SAME encoded DTO body
    // the binary channel would carry and runs the SAME handler, so the ACL
    // check, the epoch issue, the anchor water-mark, the catalog log append and
    // the idempotency map cannot drift between the two channels: there is one
    // implementation, reached two ways.
    //
    // `body` must be a complete registry DTO frame (the 6-byte prefix plus
    // tags), exactly what `registry_client` puts on the wire. On success
    // `response` holds the encoded response frame. A malformed body is the
    // handler's own decode error, never a special case here.
    //
    // `method_id` selects the handler; an id with no handler is
    // `rpc_code::no_such_method` rather than a silent success.
    [[nodiscard]] status dispatch(u64 method_id, span<const std::byte> body,
                                  buffer& response);

private:
    // The six RPC handlers. They match `rpc::method_handler` exactly.
    static status on_register(void* self, const call_envelope& envelope,
                              span<const std::byte> body, buffer& response,
                              rpc::call_context& ctx);
    static status on_deregister(void* self, const call_envelope& envelope,
                                span<const std::byte> body, buffer& response,
                                rpc::call_context& ctx);
    static status on_set_state(void* self, const call_envelope& envelope,
                               span<const std::byte> body, buffer& response,
                               rpc::call_context& ctx);
    static status on_query(void* self, const call_envelope& envelope,
                           span<const std::byte> body, buffer& response,
                           rpc::call_context& ctx);
    static status on_next_fence(void* self, const call_envelope& envelope,
                                span<const std::byte> body, buffer& response,
                                rpc::call_context& ctx);
    // The implementation body of `on_next_fence`, wrapped so a throw is
    // diagnosable instead of silently becoming `internal`.
    [[nodiscard]] status on_next_fence_impl(registry_service& owner,
                                            span<const std::byte> body,
                                            buffer& response);
    static status on_ping(void* self, const call_envelope& envelope,
                          span<const std::byte> body, buffer& response,
                          rpc::call_context& ctx);
    // The four KV handlers (step4 PR-7).
    static status on_kv_get(void* self, const call_envelope& envelope,
                            span<const std::byte> body, buffer& response,
                            rpc::call_context& ctx);
    static status on_kv_put(void* self, const call_envelope& envelope,
                            span<const std::byte> body, buffer& response,
                            rpc::call_context& ctx);
    static status on_kv_cas(void* self, const call_envelope& envelope,
                            span<const std::byte> body, buffer& response,
                            rpc::call_context& ctx);
    static status on_kv_delete(void* self, const call_envelope& envelope,
                               span<const std::byte> body, buffer& response,
                               rpc::call_context& ctx);
    // The implementation bodies of the KV handlers, wrapped so a throw is
    // diagnosable instead of silently becoming `internal`.
    [[nodiscard]] status on_kv_get_impl(registry_service& owner,
                                        span<const std::byte> body,
                                        buffer& response);
    [[nodiscard]] status on_kv_put_impl(registry_service& owner,
                                        span<const std::byte> body,
                                        buffer& response);
    [[nodiscard]] status on_kv_cas_impl(registry_service& owner,
                                        span<const std::byte> body,
                                        buffer& response);
    [[nodiscard]] status on_kv_delete_impl(registry_service& owner,
                                           span<const std::byte> body,
                                           buffer& response);

    struct impl;
    std::unique_ptr<impl> impl_;
};

// --- epoch_store --------------------------------------------------------------//
// Persists the writer-epoch counter in its own file. VERDICT section 4.2 fixes
// four rules; each is implemented and commented where it applies:
//   1. a separate file, written with `fs::write_all_bytes_atomic`;
//   2. persist before returning (a crash skips a number, never reuses one);
//   3. `seed = max(persisted, registry.fence())` with `fence()` read at seed
//      time, not at restore time;
//   4. seeding to the global fence covers every per-key floor.
class CRPC_API epoch_store {
public:
    // The file is `<state_dir>/registry.epoch`.
    epoch_store() = default;

    // Binds the store to a state directory without changing the counter. Used
    // by tests and by the service, which seeds after restore.
    void bind(const fs::path& state_dir) { path_ = file_path(state_dir); }

    // Seeds the counter. `persisted` is the value read from the file (0 when
    // the file is absent); `registry_fence` must be the registry's *current*
    // fence, read after `restore_from_log` returned.
    void seed(u64 persisted, u64 registry_fence) noexcept;

    // Returns the next epoch, persisting it first. On a write failure the
    // number is NOT returned: the caller receives the error and no epoch is
    // consumed.
    [[nodiscard]] result<u64> next();

    [[nodiscard]] u64 value() const noexcept { return value_; }

    // Reads the persisted value without changing the counter.
    [[nodiscard]] static result<u64> read(const fs::path& state_dir);

    // Test seam: makes the next `persist()` fail without touching the file, so
    // a crash between "number issued" and "number on disk" is observable. A
    // production deployment never calls this.
    void inject_persist_failure() noexcept { fail_persist_ = true; }

    // The epoch file path for a state directory.
    [[nodiscard]] static fs::path file_path(const fs::path& state_dir);

private:
    [[nodiscard]] status persist(u64 value) const;

    u64 value_ = 0;
    fs::path path_;
    bool fail_persist_ = false;
};

} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_REGISTRY_SERVICE_HPP
