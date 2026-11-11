// CRPC -- G19 step4 PR-8: Consul-compatible HTTP facade tests.
//
// These cases drive `consul_http` directly: the request/response types are the
// HTTP message types, but no socket is involved. That is deliberate -- the
// facade's job is translation (Consul shape <-> registry shape), and a socket
// would add a second thing that can fail without adding coverage of the
// translation.
//
// What the cases pin, in the order the spec lists them (VERDICT 7.4):
//   * every endpoint's success path;
//   * `X-Consul-Index` on every response that carries one;
//   * base64 round-trip and the `cas` contract (0 = create-only, N = match
//     modify_index, conflict = 409 + `cas_conflict`);
//   * the refusal set: 401 (no token), 403 (denied), 404 (unknown path),
//     405 (unknown method), 400 (bad base64 / bad JSON / bad flag),
//     413 (oversized body);
//   * watch timeout is 200 with the current value, NOT 504.
//
// A note on the 401/403 split: `rpc_code` has no `unauthenticated` member, so
// the facade decides 401 itself when the token is absent, before dispatch. That
// branch is asserted directly (`missing_token_is_401`) because a mapping table
// alone cannot distinguish "no credential" from "insufficient credential".
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "test_framework.hpp"

#include <crpc/cluster/consul_http.hpp>
#include <crpc/cluster/kv_store.hpp>
#include <crpc/cluster/registry_service.hpp>
#include <crpc/crypto/base64.hpp>
#include <crpc/fs/directory.hpp>
#include <crpc/fs/file.hpp>

namespace cl = crpc::cluster;
namespace fs = crpc::fs;
using namespace std::chrono_literals;

namespace {

crpc::u64 fnv1a(const std::string& value) {
    crpc::u64 hash = 1469598103934665603ull;
    for (const char c : value) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

// A service without a listener: the facade calls `dispatch` in-process, so no
// port is bound and the case cannot collide with a parallel run.
struct fixture {
    std::unique_ptr<cl::kv_store> store;
    std::unique_ptr<cl::registry_service> service;
    std::unique_ptr<cl::consul_http> http;
    fs::path dir;

    bool start(const std::string& name) {
        dir = fs::temporary_directory().value() / name;
        (void)fs::remove_all(dir);

        store = std::make_unique<cl::kv_store>();

        cl::registry_service_config config;
        config.state_dir = dir;
        // No listener: `prepare` still restores the catalog and registers the
        // handlers, which is what dispatch needs. The default config binds
        // 127.0.0.1:0, so it must be cleared explicitly.
        config.listen = crpc::endpoint{};
        config.kv = store.get();
        config.registry.admin_token = "admin-secret";
        // A writer that may read and write anything. `node_id` is left empty:
        // an empty node_id means "any node" (registry_acl.hpp), which is what a
        // KV write needs -- the KV namespace is global, not per-node, and the
        // KV handlers pass an empty node_id to check_acl.
        cl::acl_rule writer;
        writer.token_hash = fnv1a("token-A");
        writer.principal_id = "node-A";
        writer.read_prefixes.push_back("");
        writer.write_prefixes.push_back("");
        config.acl.push_back(writer);
        // A reader with no write prefix: the 403 case needs a token that is
        // valid but not allowed to write.
        cl::acl_rule reader;
        reader.token_hash = fnv1a("token-read");
        reader.principal_id = "node-R";
        reader.read_prefixes.push_back("");
        config.acl.push_back(reader);

        service = std::make_unique<cl::registry_service>(std::move(config));
        crpc::server::builder builder;
        const crpc::status prepared = service->prepare(builder);
        if (!prepared.ok()) {
            std::printf("prepare failed: %s\n", prepared.message().c_str());
            return false;
        }
        http = std::make_unique<cl::consul_http>(*service);
        return true;
    }

    ~fixture() {
        http.reset();
        service.reset();
        store.reset();
        (void)fs::remove_all(dir);
    }
};

crpc::http::request make_request(crpc::http::method verb,
                                 const std::string& target,
                                 const std::string& token,
                                 const std::string& body = {}) {
    crpc::http::request request;
    request.method = verb;
    request.method_token = crpc::http::to_token(verb);
    request.target = target;
    const std::size_t question = target.find('?');
    if (question == std::string::npos) {
        request.path = target;
    } else {
        request.path = target.substr(0, question);
        request.query = target.substr(question + 1);
    }
    if (!token.empty()) {
        request.headers.add("X-Consul-Token", token);
    }
    request.body = body;
    return request;
}

// The base64 the facade puts in `Value`. Returns false when the field is absent
// or is not base64, which a passing case must never see.
bool value_field(const std::string& body, std::string& raw) {
    const std::string key = "\"Value\":";
    const std::size_t at = body.find(key);
    if (at == std::string::npos) {
        return false;
    }
    std::size_t pos = at + key.size();
    while (pos < body.size() && body[pos] == ' ') {
        ++pos;
    }
    if (pos >= body.size() || body[pos] != '"') {
        return false;
    }
    ++pos;
    std::string encoded;
    while (pos < body.size() && body[pos] != '"') {
        if (body[pos] == '\\') {
            return false;
        }
        encoded.push_back(body[pos]);
        ++pos;
    }
    const crpc::result<std::string> decoded =
        crpc::crypto::base64_decode(encoded);
    if (!decoded.ok()) {
        return false;
    }
    raw = decoded.value();
    return true;
}

// Reads `"<name>":<digits>` out of a JSON body. Returns false when the field is
// absent or not a plain unsigned integer.
bool json_u64(const std::string& body, const std::string& name,
              crpc::u64& out) {
    const std::string key = "\"" + name + "\":";
    const std::size_t at = body.find(key);
    if (at == std::string::npos) {
        return false;
    }
    std::size_t pos = at + key.size();
    crpc::u64 value = 0;
    bool any = false;
    while (pos < body.size() && body[pos] >= '0' && body[pos] <= '9') {
        value = value * 10 + static_cast<crpc::u64>(body[pos] - '0');
        any = true;
        ++pos;
    }
    if (!any) {
        return false;
    }
    out = value;
    return true;
}

} // namespace

// --- Success paths -----------------------------------------------------------

CRPC_TEST(kv_put_then_get_round_trips_base64) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_kv_roundtrip"));

    const std::string payload = "hello, consulate";
    crpc::http::response put_response;
    f.http->handle_kv(make_request(crpc::http::method::put, "/v1/kv/svc/key",
                                   "token-A", payload),
                      "svc/key", cl::consul_kv_op::put, put_response);
    CRPC_CHECK_EQ(200, put_response.status_code);
    CRPC_CHECK_EQ(std::string("true"), put_response.body);
    const std::string* put_index = put_response.headers.find("X-Consul-Index");
    CRPC_REQUIRE(put_index != nullptr);
    CRPC_CHECK(!put_index->empty());

    crpc::http::response get_response;
    f.http->handle_kv(make_request(crpc::http::method::get, "/v1/kv/svc/key",
                                   "token-A"),
                      "svc/key", cl::consul_kv_op::get, get_response);
    CRPC_CHECK_EQ(200, get_response.status_code);

    std::string raw;
    CRPC_REQUIRE(value_field(get_response.body, raw));
    CRPC_CHECK_EQ(payload, raw);
}

CRPC_TEST(kv_get_missing_key_is_404_with_null_body) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_kv_missing"));

    crpc::http::response response;
    f.http->handle_kv(make_request(crpc::http::method::get, "/v1/kv/absent",
                                   "token-A"),
                      "absent", cl::consul_kv_op::get, response);
    CRPC_CHECK_EQ(404, response.status_code);
    CRPC_CHECK_EQ(std::string("null"), response.body);
    // A 404 still carries the index: a client long-polling on a missing key
    // must be able to learn the cluster moved.
    CRPC_CHECK(response.headers.find("X-Consul-Index") != nullptr);
}

CRPC_TEST(kv_delete_removes_the_key) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_kv_delete"));

    crpc::http::response put;
    f.http->handle_kv(make_request(crpc::http::method::put, "/v1/kv/k",
                                   "token-A", "v"),
                      "k", cl::consul_kv_op::put, put);
    CRPC_CHECK_EQ(200, put.status_code);

    crpc::http::response erased;
    f.http->handle_kv(make_request(crpc::http::method::del, "/v1/kv/k",
                                   "token-A"),
                      "k", cl::consul_kv_op::del, erased);
    CRPC_CHECK_EQ(200, erased.status_code);

    crpc::http::response after;
    f.http->handle_kv(make_request(crpc::http::method::get, "/v1/kv/k",
                                   "token-A"),
                      "k", cl::consul_kv_op::get, after);
    CRPC_CHECK_EQ(404, after.status_code);
}

// --- cas contract ------------------------------------------------------------

CRPC_TEST(kv_cas_zero_creates_and_second_zero_conflicts) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_cas_zero"));

    crpc::http::response created;
    f.http->handle_kv(make_request(crpc::http::method::put,
                                   "/v1/kv/k?cas=0", "token-A", "v1"),
                      "k", cl::consul_kv_op::put, created);
    CRPC_CHECK_EQ(200, created.status_code);

    // A second cas=0 must conflict: the key exists.
    crpc::http::response conflict;
    f.http->handle_kv(make_request(crpc::http::method::put,
                                   "/v1/kv/k?cas=0", "token-A", "v2"),
                      "k", cl::consul_kv_op::put, conflict);
    CRPC_CHECK_EQ(409, conflict.status_code);
    CRPC_CHECK(conflict.body.find("cas_conflict") != std::string::npos);
}

CRPC_TEST(kv_cas_matching_index_succeeds_and_stale_conflicts) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_cas_match"));

    crpc::http::response first;
    f.http->handle_kv(make_request(crpc::http::method::put, "/v1/kv/k",
                                   "token-A", "v1"),
                      "k", cl::consul_kv_op::put, first);
    CRPC_CHECK_EQ(200, first.status_code);

    // Read the modify index the facade reported.
    crpc::http::response got;
    f.http->handle_kv(make_request(crpc::http::method::get, "/v1/kv/k",
                                   "token-A"),
                      "k", cl::consul_kv_op::get, got);
    CRPC_CHECK_EQ(200, got.status_code);
    crpc::u64 modify_index = 0;
    CRPC_REQUIRE(json_u64(got.body, "ModifyIndex", modify_index));
    CRPC_CHECK(modify_index != 0);

    const std::string matching =
        "/v1/kv/k?cas=" + std::to_string(modify_index);
    crpc::http::response updated;
    f.http->handle_kv(make_request(crpc::http::method::put, matching,
                                   "token-A", "v2"),
                      "k", cl::consul_kv_op::put, updated);
    CRPC_CHECK_EQ(200, updated.status_code);

    // The same cas again is now stale: the write above advanced modify_index.
    // This is the case that fails when the KV index is not monotonic (two
    // writes sharing one index would leave the old cas still matching).
    crpc::http::response stale;
    f.http->handle_kv(make_request(crpc::http::method::put, matching,
                                   "token-A", "v3"),
                      "k", cl::consul_kv_op::put, stale);
    CRPC_CHECK_EQ(409, stale.status_code);
    CRPC_CHECK(stale.body.find("cas_conflict") != std::string::npos);
}

// --- Refusals ----------------------------------------------------------------

CRPC_TEST(missing_token_is_401) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_no_token"));

    const crpc::http::method verbs[] = {crpc::http::method::get,
                                        crpc::http::method::put,
                                        crpc::http::method::del};
    for (const crpc::http::method verb : verbs) {
        crpc::http::response response;
        if (verb == crpc::http::method::del) {
            f.http->handle_kv(make_request(verb, "/v1/kv/k", ""), "k",
                              cl::consul_kv_op::del, response);
        } else if (verb == crpc::http::method::put) {
            f.http->handle_kv(make_request(verb, "/v1/kv/k", "", "v"), "k",
                              cl::consul_kv_op::put, response);
        } else {
            f.http->handle_kv(make_request(verb, "/v1/kv/k", ""), "k",
                              cl::consul_kv_op::get, response);
        }
        CRPC_CHECK_EQ(401, response.status_code);
    }
}

CRPC_TEST(read_only_token_write_is_403) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_acl_denied"));

    crpc::http::response response;
    f.http->handle_kv(make_request(crpc::http::method::put, "/v1/kv/secret",
                                   "token-read", "v"),
                      "secret", cl::consul_kv_op::put, response);
    CRPC_CHECK_EQ(403, response.status_code);
}

CRPC_TEST(bad_cas_value_is_400) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_bad_cas"));

    const char* const bad[] = {"/v1/kv/k?cas=abc", "/v1/kv/k?cas=-1",
                               "/v1/kv/k?cas="};
    for (const char* target : bad) {
        crpc::http::response response;
        f.http->handle_kv(make_request(crpc::http::method::put, target,
                                       "token-A", "v"),
                          "k", cl::consul_kv_op::put, response);
        CRPC_CHECK_EQ(400, response.status_code);
    }
}

CRPC_TEST(oversized_value_is_413) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_too_large"));

    // The fixture's default cap is 512 KiB; 513 KiB must be refused before the
    // value reaches the store.
    const std::string huge(513u * 1024u, 'x');
    crpc::http::response response;
    f.http->handle_kv(make_request(crpc::http::method::put, "/v1/kv/k",
                                   "token-A", huge),
                      "k", cl::consul_kv_op::put, response);
    CRPC_CHECK_EQ(413, response.status_code);
}

CRPC_TEST(bad_json_register_body_is_400) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_bad_json"));

    const char* const bad[] = {
        "",
        "{",
        "{\"Name\":}",
        "{\"Name\":\"x\",\"Port\":\"80\"}",  // Port must be a number.
        "{\"Name\":\"x\",\"Port\":80.5}",    // Fractional port.
        "{\"Name\":\"x\",\"Port\":80,}",     // Trailing comma.
        "not json",
    };
    for (const char* body : bad) {
        crpc::http::response response;
        f.http->handle_register(
            make_request(crpc::http::method::put,
                         "/v1/agent/service/register", "token-A", body),
            response);
        CRPC_CHECK_EQ(400, response.status_code);
    }
}

CRPC_TEST(register_requires_name_and_port) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_register_required"));

    // A body with no Port: the registry would otherwise create an instance on
    // port 0 that can never be dialled.
    crpc::http::response no_port;
    f.http->handle_register(
        make_request(crpc::http::method::put, "/v1/agent/service/register",
                     "token-A", "{\"Name\":\"svc\"}"),
        no_port);
    CRPC_CHECK_EQ(400, no_port.status_code);

    crpc::http::response no_name;
    f.http->handle_register(
        make_request(crpc::http::method::put, "/v1/agent/service/register",
                     "token-A", "{\"Port\":80}"),
        no_name);
    CRPC_CHECK_EQ(400, no_name.status_code);
}

CRPC_TEST(health_passing_flag_must_be_boolean) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_passing_flag"));

    const char* const bad[] = {"yes", "1x", "", "TRUE "};
    for (const char* value : bad) {
        const std::string target =
            std::string("/v1/health/service/svc?passing=") + value;
        crpc::http::response response;
        f.http->handle_health_service(
            make_request(crpc::http::method::get, target, "token-A"), "svc",
            response);
        CRPC_CHECK_EQ(400, response.status_code);
    }
}

CRPC_TEST(watch_rejects_bad_index_and_wait) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_watch_bad"));

    const char* const bad[] = {"/v1/watch/kv/k?index=abc",
                               "/v1/watch/kv/k?wait=soon",
                               "/v1/watch/kv/k?wait=5x"};
    for (const char* target : bad) {
        crpc::http::response response;
        f.http->handle_watch(
            make_request(crpc::http::method::get, target, "token-A"), "k",
            response);
        CRPC_CHECK_EQ(400, response.status_code);
    }
}

// --- Watch ------------------------------------------------------------------

CRPC_TEST(watch_timeout_is_200_with_current_value) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_watch_timeout"));

    crpc::http::response put;
    f.http->handle_kv(make_request(crpc::http::method::put, "/v1/kv/k",
                                   "token-A", "v"),
                      "k", cl::consul_kv_op::put, put);
    CRPC_CHECK_EQ(200, put.status_code);

    // Ask for an index far ahead: the watch can never reach it, so it must
    // time out. A short `wait` keeps the case fast.
    crpc::http::response response;
    f.http->handle_watch(
        make_request(crpc::http::method::get,
                     "/v1/watch/kv/k?index=999999&wait=30ms", "token-A"),
        "k", response);
    // A timeout is 200 with the current value -- NOT 504.
    CRPC_CHECK_EQ(200, response.status_code);
    std::string raw;
    CRPC_REQUIRE(value_field(response.body, raw));
    CRPC_CHECK_EQ(std::string("v"), raw);
}

CRPC_TEST(watch_returns_immediately_when_index_is_stale) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_watch_fresh"));

    crpc::http::response put;
    f.http->handle_kv(make_request(crpc::http::method::put, "/v1/kv/k",
                                   "token-A", "v"),
                      "k", cl::consul_kv_op::put, put);
    CRPC_CHECK_EQ(200, put.status_code);

    // index=0 is behind every real index, so the watch must not park at all.
    // This is the case that fails when `anchor().index` ignores KV writes: the
    // catalog index stays 0, `0 > 0` is false, and the watch sleeps the full
    // 10 s.
    const auto started = std::chrono::steady_clock::now();
    crpc::http::response response;
    f.http->handle_watch(
        make_request(crpc::http::method::get,
                     "/v1/watch/kv/k?index=0&wait=10s", "token-A"),
        "k", response);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_EQ(200, response.status_code);
    // Generous bound: a parking bug would sleep the full 10 s.
    CRPC_CHECK(elapsed < std::chrono::seconds(2));
}

// --- Catalog and health -----------------------------------------------------

CRPC_TEST(catalog_services_lists_every_registered_service) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_catalog_services"));

    crpc::http::response registered;
    f.http->handle_register(
        make_request(crpc::http::method::put, "/v1/agent/service/register",
                     "token-A",
                     "{\"Name\":\"svc-a\",\"ID\":\"node-A\",\"Address\":"
                     "\"127.0.0.1\",\"Port\":7001,\"Tags\":[\"v1\"]}"),
        registered);
    CRPC_CHECK_EQ(200, registered.status_code);

    crpc::http::response listed;
    f.http->handle_catalog_services(
        make_request(crpc::http::method::get, "/v1/catalog/services",
                     "token-A"),
        listed);
    CRPC_CHECK_EQ(200, listed.status_code);
    CRPC_CHECK(listed.body.find("svc-a") != std::string::npos);
    CRPC_CHECK(listed.headers.find("X-Consul-Index") != nullptr);
}

CRPC_TEST(catalog_service_reports_the_instance) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_catalog_service"));

    crpc::http::response registered;
    f.http->handle_register(
        make_request(crpc::http::method::put, "/v1/agent/service/register",
                     "token-A",
                     "{\"Name\":\"svc-b\",\"ID\":\"node-A\",\"Address\":"
                     "\"10.0.0.7\",\"Port\":7002}"),
        registered);
    CRPC_CHECK_EQ(200, registered.status_code);

    crpc::http::response listed;
    f.http->handle_catalog_service(
        make_request(crpc::http::method::get, "/v1/catalog/service/svc-b",
                     "token-A"),
        "svc-b", listed);
    CRPC_CHECK_EQ(200, listed.status_code);
    CRPC_CHECK(listed.body.find("\"ServicePort\":7002") != std::string::npos);
    CRPC_CHECK(listed.body.find("10.0.0.7") != std::string::npos);
}

CRPC_TEST(health_service_filters_by_passing) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_health"));

    crpc::http::response registered;
    f.http->handle_register(
        make_request(crpc::http::method::put, "/v1/agent/service/register",
                     "token-A",
                     "{\"Name\":\"svc-c\",\"ID\":\"node-A\",\"Address\":"
                     "\"127.0.0.1\",\"Port\":7003}"),
        registered);
    CRPC_CHECK_EQ(200, registered.status_code);

    crpc::http::response passing;
    f.http->handle_health_service(
        make_request(crpc::http::method::get,
                     "/v1/health/service/svc-c?passing=true", "token-A"),
        "svc-c", passing);
    CRPC_CHECK_EQ(200, passing.status_code);
    CRPC_CHECK(passing.body.find("\"Status\":\"passing\"") !=
               std::string::npos);
}

CRPC_TEST(deregister_removes_the_instance) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_deregister"));

    crpc::http::response registered;
    f.http->handle_register(
        make_request(crpc::http::method::put, "/v1/agent/service/register",
                     "token-A",
                     "{\"Name\":\"svc-d\",\"ID\":\"node-A\",\"Address\":"
                     "\"127.0.0.1\",\"Port\":7004}"),
        registered);
    CRPC_CHECK_EQ(200, registered.status_code);

    // The id is `node-A` while the service is `svc-d`: the facade must look the
    // row up rather than assume id == service.
    crpc::http::response gone;
    f.http->handle_deregister(
        make_request(crpc::http::method::put,
                     "/v1/agent/service/deregister/node-A", "token-A"),
        "node-A", gone);
    CRPC_CHECK_EQ(200, gone.status_code);

    crpc::http::response listed;
    f.http->handle_catalog_service(
        make_request(crpc::http::method::get, "/v1/catalog/service/svc-d",
                     "token-A"),
        "svc-d", listed);
    CRPC_CHECK_EQ(200, listed.status_code);
    CRPC_CHECK_EQ(std::string("[]"), listed.body);
}

// --- Routing ----------------------------------------------------------------

CRPC_TEST(route_table_has_every_endpoint_and_none_collide) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_routes"));

    crpc::mvc::router routes;
    const crpc::status added = f.http->add_routes(routes);
    CRPC_CHECK(added.ok());
    const crpc::status frozen = routes.freeze();
    CRPC_CHECK(frozen.ok());

    // Every path the spec lists must match its verb. A missing route is a 404
    // in production, so the check is: match, do not merely register.
    struct probe {
        crpc::http::method verb;
        const char* path;
    };
    const probe probes[] = {
        {crpc::http::method::get, "/v1/catalog/services"},
        {crpc::http::method::get, "/v1/catalog/service/svc"},
        {crpc::http::method::get, "/v1/health/service/svc"},
        {crpc::http::method::put, "/v1/agent/service/register"},
        {crpc::http::method::put, "/v1/agent/service/deregister/node"},
        {crpc::http::method::get, "/v1/kv/a/b/c"},
        {crpc::http::method::put, "/v1/kv/a/b/c"},
        {crpc::http::method::del, "/v1/kv/a/b/c"},
        {crpc::http::method::get, "/v1/watch/kv/a/b/c"},
    };
    for (const probe& entry : probes) {
        const crpc::mvc::match_result matched =
            routes.match(entry.verb, entry.path);
        CRPC_CHECK_EQ(crpc::mvc::match_kind::matched, matched.kind);
    }

    // A verb the path does not support is method_not_allowed, not not_found:
    // the client must be told the method is wrong, not that the endpoint is
    // absent.
    const crpc::mvc::match_result wrong_verb =
        routes.match(crpc::http::method::post, "/v1/kv/k");
    CRPC_CHECK_EQ(crpc::mvc::match_kind::method_not_allowed, wrong_verb.kind);

    // An unknown path is not_found.
    const crpc::mvc::match_result unknown =
        routes.match(crpc::http::method::get, "/v1/nope");
    CRPC_CHECK_EQ(crpc::mvc::match_kind::not_found, unknown.kind);
}

CRPC_TEST(kv_catch_all_keeps_the_whole_key) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_catch_all"));

    crpc::mvc::router routes;
    CRPC_REQUIRE(f.http->add_routes(routes).ok());
    CRPC_REQUIRE(routes.freeze().ok());

    const crpc::mvc::match_result matched =
        routes.match(crpc::http::method::get, "/v1/kv/servers/eu-01/http");
    CRPC_REQUIRE(matched.kind == crpc::mvc::match_kind::matched);
    const std::string* key = matched.match.params.find("key");
    CRPC_REQUIRE(key != nullptr);
    CRPC_CHECK_EQ(std::string("servers/eu-01/http"), *key);
}

// --- Error mapping ----------------------------------------------------------

CRPC_TEST(cas_conflict_maps_to_409_and_names_the_reason) {
    crpc::rpc_error error = crpc::cluster::registry_failure(
        crpc::rpc_code::failed_precondition, "registry: cas_conflict",
        crpc::cluster::registry_reason_cas_conflict);
    const cl::consul_error mapped = cl::to_consul_error(error);
    CRPC_CHECK_EQ(409, mapped.http_status);
    CRPC_CHECK(mapped.cas_conflict);
    CRPC_CHECK_EQ(std::string("cas_conflict"), mapped.reason);
    CRPC_CHECK_EQ(std::string("cas_conflict"), mapped.detail);
}

CRPC_TEST(stale_anchor_maps_to_409_but_is_not_a_cas_conflict) {
    // A stale anchor is also a `failed_precondition`, so a naive mapping would
    // report it as a cas conflict. The two must stay distinguishable.
    crpc::rpc_error error = crpc::cluster::registry_failure(
        crpc::rpc_code::failed_precondition,
        "registry: client anchor is behind",
        crpc::cluster::registry_reason_stale_anchor);
    const cl::consul_error mapped = cl::to_consul_error(error);
    CRPC_CHECK_EQ(409, mapped.http_status);
    CRPC_CHECK(!mapped.cas_conflict);
    CRPC_CHECK_EQ(std::string("stale_anchor"), mapped.reason);
}

CRPC_TEST(acl_denied_maps_to_403_and_invalid_key_to_400) {
    const crpc::rpc_error denied = crpc::cluster::registry_failure(
        crpc::rpc_code::permission_denied, "registry: acl_denied",
        crpc::cluster::registry_reason_acl_denied);
    CRPC_CHECK_EQ(403, cl::to_consul_error(denied).http_status);

    const crpc::rpc_error bad_key = crpc::cluster::registry_failure(
        crpc::rpc_code::invalid_argument, "registry: kv invalid key", "bad");
    CRPC_CHECK_EQ(400, cl::to_consul_error(bad_key).http_status);

    const crpc::rpc_error too_big = crpc::cluster::registry_failure(
        crpc::rpc_code::resource_exhausted, "too large", "big");
    CRPC_CHECK_EQ(413, cl::to_consul_error(too_big).http_status);
}

CRPC_TEST(route_count_matches_the_spec) {
    // The spec lists seven endpoints; the KV path needs three verbs, so the
    // route table has nine entries. Pinning both numbers stops a later edit
    // from silently dropping one verb (which would answer 405 in production).
    CRPC_CHECK_EQ(static_cast<std::size_t>(7), cl::consul_http::route_count());

    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_route_count"));
    crpc::mvc::router routes;
    CRPC_REQUIRE(f.http->add_routes(routes).ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(9), routes.size());
}

CRPC_TEST(unknown_method_id_is_refused_by_dispatch) {
    // `dispatch` is the seam the facade uses; an unknown id must be an error,
    // never a silent success. This is the fail-closed property at its narrowest.
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_dispatch_unknown"));

    crpc::buffer response;
    const crpc::status dispatched = f.service->dispatch(9999, {}, response);
    CRPC_CHECK(!dispatched.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::no_such_method, dispatched.code());
}

CRPC_TEST(empty_key_is_refused) {
    fixture f;
    CRPC_REQUIRE(f.start("crpc_g19p4_empty_key"));

    crpc::http::response response;
    f.http->handle_kv(make_request(crpc::http::method::get, "/v1/kv/",
                                   "token-A"),
                      "", cl::consul_kv_op::get, response);
    CRPC_CHECK_EQ(400, response.status_code);
}

CRPC_TEST_MAIN()
