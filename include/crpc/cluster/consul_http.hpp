// CRPC -- G19 step4 PR-8: Consul-compatible HTTP facade over the registry.
//
// This is the HTTP surface described in docs/design.md 15.5 and VERDICT 7.4.
// It translates a Consul-shaped request into an in-process call on the
// registry service's catalog, kv_store and ACL, and translates the outcome
// into Consul's wire conventions:
//
//   * `X-Consul-Index` carries the global index. It is set on every successful
//     response, including 404 (a nil result still has an index) -- a client
//     that long-polls on the index would otherwise never learn the cluster
//     moved.
//   * KV values are base64 on the wire. `?raw` is NOT supported: a caller that
//     wants bytes gets base64 and decodes it.
//   * `cas=N` succeeds only when the entry's `modify_index == N`; `cas=0`
//     means create-only. A conflict is 409, and the body names the reason.
//   * Every unknown path, method, query flag, enum value, JSON shape, base64
//     payload or absent token is refused. There is no permissive default: a
//     request that this layer cannot classify is 400/404/405/501, never a
//     silently-ignored field.
//
// Trust boundary (same as the binary channel, see registry_acl.hpp)
// ----------------------------------------------------------------
// The token is a bearer credential. `X-Consul-Token` carries it; the same
// `registry_service::check_acl` call the CRPC handlers use decides the outcome,
// so the two channels cannot drift. There is no transport security here; a
// deployment that needs authenticated peers terminates TLS in front.
//
// Why a separate class instead of extending registry_service
// ---------------------------------------------------------
// `registry_service` owns the write path, the log and the epoch store; it must
// not grow an HTTP concern (and its `prepare()` must keep working for callers
// that want only the binary channel). `consul_http` borrows a
// `registry_service&` and does the translation. The routes themselves are
// registered through `add_routes`, so the caller decides which `mvc::router`
// they land in.
#ifndef CRPC_CLUSTER_CONSUL_HTTP_HPP
#define CRPC_CLUSTER_CONSUL_HTTP_HPP

#include <chrono>
#include <cstddef>
#include <string>

#include <crpc/cluster/kv_store.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/mvc/router.hpp>

namespace crpc {
namespace cluster {

class registry_service;

// What the facade answers when the caller asks for a KV write.
enum class consul_kv_op : std::uint8_t { get, put, del };

// One denied request, carried out of the translation layer so the caller (and
// the tests) can see WHY a status code was chosen instead of only the code.
struct CRPC_API consul_error {
    rpc_code code = rpc_code::ok;
    int http_status = 200;
    // Consul returns a plain string body on failure. `detail` is that string.
    std::string detail;
    // The registry reason token, when the registry supplied one.
    std::string reason;
    // True for a `cas` conflict, which is the only 409 the facade produces for
    // a KV write. A stale anchor is also 409 but is NOT a cas conflict.
    bool cas_conflict = false;
};

// Turns an rpc_error into the Consul error shape. Exposed so the tests can
// assert the mapping directly instead of driving a full router.
[[nodiscard]] CRPC_API consul_error to_consul_error(const rpc_error& error);

// The HTTP facade. One instance borrows one service; it holds no state of its
// own, so a caller may build one per router.
class CRPC_API consul_http {
public:
    struct config {
        // The largest KV value the facade accepts, in bytes. A larger body is
        // 413, not a partial write.
        std::size_t max_value_bytes = 512u * 1024u;
        // The largest request body the facade reads at all. A body over this
        // is 413 regardless of whether it would parse.
        std::size_t max_body_bytes = 8u * 1024u * 1024u;
        // The longest `wait` a watch accepts. Consul caps at 10 minutes; a
        // longer request is clamped rather than rejected (matching Consul).
        std::chrono::milliseconds max_watch_wait{600000};
        // The default watch wait when the request omits `wait`.
        std::chrono::milliseconds default_watch_wait{300000};
        // How often a parked watch re-checks the index. The facade does not
        // own a scheduler; it polls this often until the index moves, the wait
        // expires, or the caller goes away.
        std::chrono::milliseconds watch_poll_interval{20};
    };

    explicit consul_http(registry_service& service, config options = config{});

    // Registers every endpoint on `routes`. The router must not be frozen yet.
    // Each route's `controller` is `this`, so the facade must outlive the
    // router (and therefore the server built from it).
    [[nodiscard]] status add_routes(mvc::router& routes);

    // The endpoint count `add_routes` installs (seven endpoints; the KV path
    // needs three verbs, so the table has nine entries). Exposed for tests.
    [[nodiscard]] static std::size_t route_count() noexcept { return 7; }

    // --- Translation layer --------------------------------------------------
    //
    // These take a decoded request and write a decoded response. They are
    // public so a test can drive one endpoint without building an HTTP
    // connection, and so a caller embedding the facade can reuse a single
    // path.

    // GET /v1/catalog/services
    void handle_catalog_services(const http::request& request,
                                 http::response& response);
    // GET /v1/catalog/service/:name
    void handle_catalog_service(const http::request& request,
                                const std::string& service,
                                http::response& response);
    // GET /v1/health/service/:name
    void handle_health_service(const http::request& request,
                               const std::string& service,
                               http::response& response);
    // PUT /v1/agent/service/register
    void handle_register(const http::request& request,
                         http::response& response);
    // PUT /v1/agent/service/deregister/:id
    void handle_deregister(const http::request& request,
                           const std::string& instance_id,
                           http::response& response);
    // GET / PUT / DELETE /v1/kv/:key
    void handle_kv(const http::request& request, const std::string& key,
                   consul_kv_op op, http::response& response);
    // GET /v1/watch/kv/:key
    void handle_watch(const http::request& request, const std::string& key,
                      http::response& response);

    const config& options() const noexcept { return config_; }

private:
    registry_service& service_;
    config config_;
};

} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_CONSUL_HTTP_HPP
