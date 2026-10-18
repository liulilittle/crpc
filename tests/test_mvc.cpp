// CRPC -- MVC router, macro and binding tests.
//
// The compile-time signature guarantee (a CRPC_HTTP_* macro naming a member the
// controller does not have, or a handler whose signature method_traits does not
// recognize, must fail to compile) is documented here rather than added to the
// suite, because a test that does not compile would break the build:
//
//   struct bad_controller {
//       CRPC_ROUTE("/bad")
//       void handler(int wrong_signature);
//   };
//   CRPC_HTTP_GET(bad_controller, handler, "/")   // error: no method_traits
//                                                 // specialization for
//                                                 // void (bad_controller::*)(int)
//
// The macro also takes `&bad_controller::handler`, so naming a member that does
// not exist is a compile error at the macro site.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <crpc/mvc/attributes.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// test_framework.hpp (Debug) issues a file-scope `#pragma section(".CRT$XCU",
// read)` that makes subsequent `static inline` data-member dynamic initializers
// inert under MSVC. RTTI's per-field attribute tables are exactly such members,
// so `_DEBUG` is undefined around the harness include to keep the pragma out of
// this translation unit. The STL headers above were already included (so
// _ITERATOR_DEBUG_LEVEL is fixed consistently), and the CRT is still linked
// through /MDd. This mirrors tests/test_codec.cpp.
#undef _DEBUG
#include "test_framework.hpp"

// --- Reflected DTOs and controllers -----------------------------------------

struct get_user_req {
    std::int64_t id = 0;
};
RTTI_DEFINE_TYPE(get_user_req, id)
RTTI_DEFINE_FIELD_ATTRS(get_user_req,
                        RTTI_FIELD_ATTR_ENTRY(id, CRPC_FROM_ROUTE("id")))

struct list_users_req {
    std::int64_t page = 0;
    std::string q;
    std::optional<std::int64_t> limit;
    std::string token;
};
RTTI_DEFINE_TYPE(list_users_req, page, q, limit, token)
RTTI_DEFINE_FIELD_ATTRS(
    list_users_req, RTTI_FIELD_ATTR_ENTRY(page, CRPC_FROM_QUERY("page")),
    RTTI_FIELD_ATTR_ENTRY(q, CRPC_FROM_QUERY("q")),
    RTTI_FIELD_ATTR_ENTRY(limit, CRPC_FROM_QUERY("limit")),
    RTTI_FIELD_ATTR_ENTRY(token, CRPC_FROM_HEADER("x-token")))

struct create_user_req {
    std::string name;
    std::int32_t age = 0;
};
RTTI_DEFINE_TYPE(create_user_req, name, age)

struct user_view {
    std::int64_t id = 0;
    std::string name;
};
RTTI_DEFINE_TYPE(user_view, id, name)

struct form_req {
    std::string title;
    std::int32_t count = 0;
};
RTTI_DEFINE_TYPE(form_req, title, count)
RTTI_DEFINE_FIELD_ATTRS(
    form_req, RTTI_FIELD_ATTR_ENTRY(title, CRPC_FROM_FORM("title")),
    RTTI_FIELD_ATTR_ENTRY(count, CRPC_FROM_FORM("count")))

struct upload_req {
    std::string title;
    crpc::mvc::file_part document;
};
RTTI_DEFINE_TYPE(upload_req, title, document)
RTTI_DEFINE_FIELD_ATTRS(
    upload_req, RTTI_FIELD_ATTR_ENTRY(title, CRPC_FROM_FORM("title")),
    RTTI_FIELD_ATTR_ENTRY(document, CRPC_FROM_FILE))

enum class sort_order : std::uint8_t { asc = 1, desc = 2 };
RTTI_DEFINE_ENUM(sort_order, asc, desc)

struct sorted_req {
    sort_order order = sort_order::asc;
};
RTTI_DEFINE_TYPE(sorted_req, order)
RTTI_DEFINE_FIELD_ATTRS(sorted_req,
                        RTTI_FIELD_ATTR_ENTRY(order, CRPC_FROM_QUERY("order")))

// --- Controllers -------------------------------------------------------------

struct user_controller {
    CRPC_ROUTE("/users")

    crpc::mvc::status_body<user_view> get_user(const get_user_req& req) {
        return crpc::mvc::with_status(200, user_view{req.id, "alice"});
    }
    crpc::mvc::status_body<user_view> list_users(const list_users_req& req) {
        return crpc::mvc::with_status(
            200, user_view{req.page, req.q + "|" + req.token});
    }
    crpc::mvc::status_body<user_view> create_user(const create_user_req& req) {
        return crpc::mvc::with_status(201, user_view{1, req.name});
    }
    crpc::mvc::status_body<user_view> replace_user(const get_user_req& req) {
        return crpc::mvc::with_status(200, user_view{req.id, "replaced"});
    }
    crpc::mvc::status_body<user_view> delete_user(const get_user_req& req) {
        return crpc::mvc::with_status(200, user_view{req.id, "deleted"});
    }
    crpc::mvc::status_body<user_view> patch_user(const get_user_req& req) {
        return crpc::mvc::with_status(200, user_view{req.id, "patched"});
    }
    user_view plain_value(const get_user_req& req) {
        return user_view{req.id, "plain"};
    }
    crpc::mvc::no_content_t touch(const get_user_req&) {
        return crpc::mvc::no_content;
    }
    crpc::status ping(const get_user_req&) { return crpc::status{}; }
    crpc::result<user_view> maybe(const get_user_req& req) {
        if (req.id < 0) {
            return crpc::rpc_error(crpc::rpc_code::not_found, "no such user");
        }
        return user_view{req.id, "ok"};
    }
};

CRPC_HTTP_GET(user_controller, get_user, "/:id")
CRPC_HTTP_GET(user_controller, list_users, "/")
CRPC_HTTP_POST(user_controller, create_user, "/")
CRPC_HTTP_PUT(user_controller, replace_user, "/:id")
CRPC_HTTP_DELETE(user_controller, delete_user, "/:id")
CRPC_HTTP_PATCH(user_controller, patch_user, "/:id")
CRPC_HTTP_GET(user_controller, plain_value, "/:id/plain")
CRPC_HTTP_GET(user_controller, touch, "/:id/touch")
CRPC_HTTP_GET(user_controller, ping, "/:id/ping")
CRPC_HTTP_GET(user_controller, maybe, "/:id/maybe")

struct search_controller {
    CRPC_ROUTE("/search")
    crpc::mvc::status_body<user_view> find(const sorted_req& req) {
        return crpc::mvc::with_status(
            200, user_view{static_cast<std::int64_t>(req.order), "s"});
    }
};
CRPC_HTTP_GET(search_controller, find, "/")

struct upload_controller {
    CRPC_ROUTE("/upload")
    crpc::mvc::status_body<user_view> put(const upload_req& req) {
        return crpc::mvc::with_status(
            201, user_view{static_cast<std::int64_t>(req.document.size()),
                           req.title + ":" + req.document.filename});
    }
};
CRPC_HTTP_POST(upload_controller, put, "/")

// --- Test helpers ------------------------------------------------------------

namespace mvc_test {

using crpc::http::method;
using crpc::mvc::match_kind;
using crpc::mvc::route_entry;
using crpc::mvc::route_params;
using crpc::mvc::router;

crpc::status dummy_invoke(void*, const crpc::http::request&,
                          const route_params&, const crpc::codec&,
                          const crpc::codec_registry&,
                          crpc::http::response&) {
    return crpc::status{};
}

route_entry make_entry(method verb, const char* path, const char* name) {
    route_entry entry;
    entry.method = verb;
    entry.path = path;
    entry.name = name;
    entry.invoke = &dummy_invoke;
    return entry;
}

const crpc::codec_registry& registry() {
    return crpc::default_codec_registry();
}

} // namespace mvc_test

using namespace mvc_test;

// --- Route matching ----------------------------------------------------------

CRPC_TEST(mvc_route_priority) {
    router routes;
    CRPC_CHECK(routes.add(make_entry(method::get, "/users", "users")).ok());
    CRPC_CHECK(routes.add(make_entry(method::get, "/users/me", "me")).ok());
    CRPC_CHECK(routes.add(make_entry(method::get, "/users/:id", "byid")).ok());
    CRPC_CHECK(
        routes.add(make_entry(method::get, "/users/:id/posts", "posts")).ok());
    CRPC_CHECK(routes.add(make_entry(method::get, "/files/*path", "files")).ok());
    CRPC_CHECK(routes.freeze().ok());
    CRPC_CHECK(routes.frozen());

    // Static beats parameter at the same segment.
    const auto users = routes.match(method::get, "/users", nullptr);
    CRPC_CHECK(users.kind == match_kind::matched);
    CRPC_CHECK_EQ(std::string("users"), users.match.route->name);

    const auto me = routes.match(method::get, "/users/me", nullptr);
    CRPC_CHECK(me.kind == match_kind::matched);
    CRPC_CHECK_EQ(std::string("me"), me.match.route->name);

    const auto by_id = routes.match(method::get, "/users/42", nullptr);
    CRPC_CHECK(by_id.kind == match_kind::matched);
    CRPC_CHECK_EQ(std::string("byid"), by_id.match.route->name);
    const std::string* id = by_id.match.params.find("id");
    CRPC_CHECK(id != nullptr);
    if (id != nullptr) {
        CRPC_CHECK_EQ(std::string("42"), *id);
    }

    const auto posts = routes.match(method::get, "/users/42/posts", nullptr);
    CRPC_CHECK(posts.kind == match_kind::matched);
    CRPC_CHECK_EQ(std::string("posts"), posts.match.route->name);

    const auto files = routes.match(method::get, "/files/a/b/c", nullptr);
    CRPC_CHECK(files.kind == match_kind::matched);
    CRPC_CHECK_EQ(std::string("files"), files.match.route->name);
    const std::string* path = files.match.params.find("path");
    CRPC_CHECK(path != nullptr);
    if (path != nullptr) {
        CRPC_CHECK_EQ(std::string("a/b/c"), *path);
    }

    CRPC_CHECK(routes.match(method::get, "/nope", nullptr).kind ==
               match_kind::not_found);
    CRPC_CHECK(routes.match(method::get, "/users/42/extra", nullptr).kind ==
               match_kind::not_found);
}

CRPC_TEST(mvc_route_metadata) {
    const crpc::mvc::router& routes = crpc::mvc::default_router();
    const auto matched = routes.match(method::get, "/users/5", nullptr);
    CRPC_CHECK(matched.kind == match_kind::matched);
    if (matched.kind == match_kind::matched) {
        CRPC_CHECK(matched.match.route->controller_type != nullptr);
        CRPC_CHECK(matched.match.route->request_type != nullptr);
        CRPC_CHECK(matched.match.route->response_type != nullptr);
        CRPC_CHECK_EQ(std::string("get_user"), matched.match.route->name);
    }
}

CRPC_TEST(mvc_route_404_and_405_allow) {
    router routes;
    CRPC_CHECK(routes.add(make_entry(method::get, "/a", "get_a")).ok());
    CRPC_CHECK(routes.add(make_entry(method::post, "/a", "post_a")).ok());
    CRPC_CHECK(routes.freeze().ok());

    CRPC_CHECK(routes.match(method::get, "/a", nullptr).kind ==
               match_kind::matched);
    CRPC_CHECK(routes.match(method::post, "/a", nullptr).kind ==
               match_kind::matched);

    std::string allow;
    const auto wrong = routes.match(method::put, "/a", &allow);
    CRPC_CHECK(wrong.kind == match_kind::method_not_allowed);
    CRPC_CHECK(allow.find("GET") != std::string::npos);
    CRPC_CHECK(allow.find("POST") != std::string::npos);

    CRPC_CHECK(routes.match(method::get, "/missing", nullptr).kind ==
               match_kind::not_found);
}

CRPC_TEST(mvc_route_conflict_detected_at_freeze) {
    router routes;
    CRPC_CHECK(routes.add(make_entry(method::get, "/dup", "one")).ok());
    CRPC_CHECK(routes.add(make_entry(method::get, "/dup", "two")).ok());
    const crpc::status frozen = routes.freeze();
    CRPC_CHECK(!frozen.ok());
    CRPC_CHECK(frozen.code() == crpc::rpc_code::failed_precondition);
    CRPC_CHECK(!routes.frozen());
}

CRPC_TEST(mvc_route_malformed_pattern) {
    router routes;
    CRPC_CHECK(routes.add(make_entry(method::get, "/a/*rest/b", "bad")).ok());
    const crpc::status frozen = routes.freeze();
    CRPC_CHECK(!frozen.ok());
}

CRPC_TEST(mvc_route_trailing_slash_and_case) {
    router sensitive;
    CRPC_CHECK(sensitive.add(make_entry(method::get, "/Users", "u")).ok());
    CRPC_CHECK(sensitive.freeze().ok());
    CRPC_CHECK(sensitive.match(method::get, "/Users", nullptr).kind ==
               match_kind::matched);
    CRPC_CHECK(sensitive.match(method::get, "/Users/", nullptr).kind ==
               match_kind::matched);
    CRPC_CHECK(sensitive.match(method::get, "/users", nullptr).kind ==
               match_kind::not_found);

    router insensitive;
    router::options options;
    options.case_insensitive_routes = true;
    CRPC_CHECK(insensitive.set_options(options).ok());
    CRPC_CHECK(insensitive.add(make_entry(method::get, "/Users", "u")).ok());
    CRPC_CHECK(insensitive.freeze().ok());
    CRPC_CHECK(insensitive.match(method::get, "/users", nullptr).kind ==
               match_kind::matched);
}

CRPC_TEST(mvc_route_percent_decoding) {
    router routes;
    CRPC_CHECK(routes.add(make_entry(method::get, "/users/:id", "byid")).ok());
    CRPC_CHECK(routes.freeze().ok());

    const auto decoded = routes.match(method::get, "/users/a%20b", nullptr);
    CRPC_CHECK(decoded.kind == match_kind::matched);
    const std::string* id = decoded.match.params.find("id");
    CRPC_CHECK(id != nullptr);
    if (id != nullptr) {
        CRPC_CHECK_EQ(std::string("a b"), *id);
    }

    CRPC_CHECK(routes.match(method::get, "/users/a%2Fb", nullptr).kind ==
               match_kind::bad_request);
    CRPC_CHECK(routes.match(method::get, "/users/%zz", nullptr).kind ==
               match_kind::bad_request);
}

// --- Binding -----------------------------------------------------------------

CRPC_TEST(mvc_binding_route_query_header) {
    route_params params;
    params.entries.emplace_back("id", "42");

    crpc::http::request request;
    request.query = "page=3&q=hello+world&limit=10";
    request.headers.add("x-token", "secret");

    list_users_req out;
    const crpc::status bound = crpc::mvc::bind_request<list_users_req>(
        request, params, registry(), crpc::mvc::default_binding_limits(), out);
    CRPC_CHECK(bound.ok());
    CRPC_CHECK_EQ(std::int64_t{3}, out.page);
    CRPC_CHECK_EQ(std::string("hello world"), out.q);
    CRPC_CHECK(out.limit.has_value());
    if (out.limit.has_value()) {
        CRPC_CHECK_EQ(std::int64_t{10}, *out.limit);
    }
    CRPC_CHECK_EQ(std::string("secret"), out.token);
}

CRPC_TEST(mvc_binding_missing_optional) {
    crpc::http::request request;
    request.query = "page=1&q=x";
    request.headers.add("x-token", "t");
    route_params params;
    list_users_req out;
    const crpc::status bound = crpc::mvc::bind_request<list_users_req>(
        request, params, registry(), crpc::mvc::default_binding_limits(), out);
    CRPC_CHECK(bound.ok());
    CRPC_CHECK(!out.limit.has_value());
}

CRPC_TEST(mvc_binding_conversion_failure) {
    crpc::http::request request;
    request.query = "page=abc";
    route_params params;
    list_users_req out;
    const crpc::status bound = crpc::mvc::bind_request<list_users_req>(
        request, params, registry(), crpc::mvc::default_binding_limits(), out);
    CRPC_CHECK(!bound.ok());
    CRPC_CHECK(bound.code() == crpc::rpc_code::invalid_argument);
    CRPC_CHECK(bound.message().find("page") != std::string::npos);
}

CRPC_TEST(mvc_binding_missing_required) {
    crpc::http::request request;
    route_params params; // no "id"
    get_user_req out;
    const crpc::status bound = crpc::mvc::bind_request<get_user_req>(
        request, params, registry(), crpc::mvc::default_binding_limits(), out);
    CRPC_CHECK(!bound.ok());
    CRPC_CHECK(bound.code() == crpc::rpc_code::invalid_argument);
    CRPC_CHECK(bound.message().find("id") != std::string::npos);
}

CRPC_TEST(mvc_binding_enum) {
    route_params params;
    crpc::http::request request;
    request.query = "order=desc";
    sorted_req out;
    CRPC_CHECK(crpc::mvc::bind_request<sorted_req>(
                   request, params, registry(),
                   crpc::mvc::default_binding_limits(), out)
                   .ok());
    CRPC_CHECK(out.order == sort_order::desc);

    request.query = "order=1";
    CRPC_CHECK(crpc::mvc::bind_request<sorted_req>(
                   request, params, registry(),
                   crpc::mvc::default_binding_limits(), out)
                   .ok());
    CRPC_CHECK(out.order == sort_order::asc);

    request.query = "order=bogus";
    const crpc::status bad = crpc::mvc::bind_request<sorted_req>(
        request, params, registry(), crpc::mvc::default_binding_limits(), out);
    CRPC_CHECK(!bad.ok());
    CRPC_CHECK(bad.code() == crpc::rpc_code::invalid_argument);
}

CRPC_TEST(mvc_binding_form) {
    route_params params;
    crpc::http::request request;
    request.headers.add("content-type", "application/x-www-form-urlencoded");
    request.body = "title=My+Doc&count=5";
    form_req out;
    CRPC_CHECK(crpc::mvc::bind_request<form_req>(
                   request, params, registry(),
                   crpc::mvc::default_binding_limits(), out)
                   .ok());
    CRPC_CHECK_EQ(std::string("My Doc"), out.title);
    CRPC_CHECK_EQ(std::int32_t{5}, out.count);
}

CRPC_TEST(mvc_binding_body_json) {
    route_params params;
    crpc::http::request request;
    request.headers.add("content-type", "application/json");
    request.body = "{\"name\":\"bob\",\"age\":30,\"extra\":true}";
    create_user_req out;
    CRPC_CHECK(crpc::mvc::bind_request<create_user_req>(
                   request, params, registry(),
                   crpc::mvc::default_binding_limits(), out)
                   .ok());
    CRPC_CHECK_EQ(std::string("bob"), out.name);
    CRPC_CHECK_EQ(std::int32_t{30}, out.age);
}

CRPC_TEST(mvc_binding_body_decode_failure) {
    route_params params;
    crpc::http::request request;
    request.headers.add("content-type", "application/json");
    request.body = "{\"name\": ";
    create_user_req out;
    const crpc::status bound = crpc::mvc::bind_request<create_user_req>(
        request, params, registry(), crpc::mvc::default_binding_limits(), out);
    CRPC_CHECK(!bound.ok());
    CRPC_CHECK(bound.code() == crpc::rpc_code::decode_error);
}

CRPC_TEST(mvc_binding_oversize_body) {
    route_params params;
    crpc::http::request request;
    request.headers.add("content-type", "application/json");
    request.body = "{\"name\":\"a very long name\"}";
    crpc::mvc::binding_limits limits;
    limits.max_body_bytes = 4;
    create_user_req out;
    const crpc::status bound = crpc::mvc::bind_request<create_user_req>(
        request, params, registry(), limits, out);
    CRPC_CHECK(!bound.ok());
    CRPC_CHECK(bound.code() == crpc::rpc_code::resource_exhausted);
}

CRPC_TEST(mvc_binding_multipart_file) {
    const std::string body =
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"title\"\r\n"
        "\r\n"
        "My Doc\r\n"
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"document\"; "
        "filename=\"note.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "hello file\r\n"
        "--BOUND--\r\n";

    route_params params;
    crpc::http::request request;
    request.headers.add("content-type",
                        "multipart/form-data; boundary=BOUND");
    request.body = body;
    upload_req out;
    const crpc::status bound = crpc::mvc::bind_request<upload_req>(
        request, params, registry(), crpc::mvc::default_binding_limits(), out);
    CRPC_CHECK(bound.ok());
    CRPC_CHECK_EQ(std::string("My Doc"), out.title);
    CRPC_CHECK_EQ(std::string("note.txt"), out.document.filename);
    CRPC_CHECK_EQ(std::string("hello file"), out.document.data);
}

// --- End-to-end through the macro-built router ------------------------------

CRPC_TEST(mvc_dispatch_return_mapping) {
    const crpc::mvc::router& routes = crpc::mvc::default_router();
    CRPC_CHECK(routes.frozen());

    // value -> 200
    {
        crpc::http::request request;
        request.method = method::get;
        request.path = "/users/7";
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(200, response.status_code);
        CRPC_CHECK(response.body.find("alice") != std::string::npos);
    }
    // POST -> explicit 201 via status_body
    {
        crpc::http::request request;
        request.method = method::post;
        request.path = "/users";
        request.headers.add("content-type", "application/json");
        request.body = "{\"name\":\"carol\",\"age\":9}";
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(201, response.status_code);
        CRPC_CHECK(response.body.find("carol") != std::string::npos);
    }
    // no_content -> 204 empty
    {
        crpc::http::request request;
        request.method = method::get;
        request.path = "/users/7/touch";
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(204, response.status_code);
        CRPC_CHECK(response.body.empty());
    }
    // status -> 204
    {
        crpc::http::request request;
        request.method = method::get;
        request.path = "/users/7/ping";
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(204, response.status_code);
    }
    // result<T> ok -> 200
    {
        crpc::http::request request;
        request.method = method::get;
        request.path = "/users/7/maybe";
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(200, response.status_code);
    }
    // result<T> error -> 404
    {
        crpc::http::request request;
        request.method = method::get;
        request.path = "/users/-1/maybe";
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(404, response.status_code);
    }
    // plain value -> 200
    {
        crpc::http::request request;
        request.method = method::get;
        request.path = "/users/7/plain";
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(200, response.status_code);
        CRPC_CHECK(response.body.find("plain") != std::string::npos);
    }
}

CRPC_TEST(mvc_dispatch_query_and_header) {
    const crpc::mvc::router& routes = crpc::mvc::default_router();
    crpc::http::request request;
    request.method = method::get;
    request.path = "/users";
    request.query = "page=2&q=needle";
    request.headers.add("x-token", "t0k");
    crpc::http::response response;
    CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
    CRPC_CHECK_EQ(200, response.status_code);
    CRPC_CHECK(response.body.find("needle|t0k") != std::string::npos);
}

CRPC_TEST(mvc_dispatch_404_405_406) {
    const crpc::mvc::router& routes = crpc::mvc::default_router();

    {
        crpc::http::request request;
        request.method = method::get;
        request.path = "/does-not-exist";
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(404, response.status_code);
    }
    {
        crpc::http::request request;
        request.method = method::post;
        request.path = "/users/7";
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(405, response.status_code);
        const std::string* allow = response.headers.find("Allow");
        CRPC_CHECK(allow != nullptr);
        if (allow != nullptr) {
            CRPC_CHECK(allow->find("GET") != std::string::npos);
            CRPC_CHECK(allow->find("PUT") != std::string::npos);
        }
    }
    {
        crpc::http::request request;
        request.method = method::get;
        request.path = "/users/7";
        request.headers.add("accept", "application/x-unknown");
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(406, response.status_code);
    }
}

CRPC_TEST(mvc_dispatch_bad_route_parameter) {
    const crpc::mvc::router& routes = crpc::mvc::default_router();
    crpc::http::request request;
    request.method = method::get;
    request.path = "/users/a%2Fb";
    crpc::http::response response;
    CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
    CRPC_CHECK_EQ(400, response.status_code);
}

CRPC_TEST(mvc_dispatch_upload_multipart) {
    const crpc::mvc::router& routes = crpc::mvc::default_router();
    crpc::http::request request;
    request.method = method::post;
    request.path = "/upload";
    request.headers.add("content-type",
                        "multipart/form-data; boundary=BOUND");
    request.body =
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"title\"\r\n"
        "\r\n"
        "Doc\r\n"
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"document\"; "
        "filename=\"a.txt\"\r\n"
        "\r\n"
        "abc\r\n"
        "--BOUND--\r\n";
    crpc::http::response response;
    CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
    CRPC_CHECK_EQ(201, response.status_code);
    CRPC_CHECK(response.body.find("Doc:a.txt") != std::string::npos);
}

CRPC_TEST(mvc_dispatch_enum_query) {
    const crpc::mvc::router& routes = crpc::mvc::default_router();
    crpc::http::request request;
    request.method = method::get;
    request.path = "/search";
    request.query = "order=desc";
    crpc::http::response response;
    CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
    CRPC_CHECK_EQ(200, response.status_code);
    CRPC_CHECK(response.body.find("s") != std::string::npos);
}

// --- Raw HTTP end-to-end -----------------------------------------------------

CRPC_TEST(mvc_end_to_end_raw_http) {
    const std::string raw =
        "GET /users/9 HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Accept: application/json\r\n"
        "\r\n";

    crpc::http::http_parser parser(crpc::http::http_parser::kind::request);
    const crpc::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(raw.data()), raw.size());
    const crpc::http::parse_status parsed = parser.feed(bytes);
    CRPC_CHECK(parsed == crpc::http::parse_status::message_complete);
    CRPC_CHECK(parser.complete());

    crpc::http::response response;
    CRPC_CHECK(crpc::mvc::dispatch(crpc::mvc::default_router(),
                                   parser.get_request(), response)
                   .ok());
    CRPC_CHECK_EQ(200, response.status_code);
    CRPC_CHECK(response.body.find("alice") != std::string::npos);

    std::string wire;
    CRPC_CHECK(crpc::http::serialize(response, wire).ok());
    CRPC_CHECK(wire.rfind("HTTP/1.1 200", 0) == 0);
    CRPC_CHECK(wire.find("alice") != std::string::npos);
}

CRPC_TEST(mvc_join_route_path) {
    CRPC_CHECK_EQ(std::string("/users/:id"),
                  crpc::mvc::join_route_path("/users", "/:id"));
    CRPC_CHECK_EQ(std::string("/users"),
                  crpc::mvc::join_route_path("/users", "/"));
    CRPC_CHECK_EQ(std::string("/users"),
                  crpc::mvc::join_route_path("/users/", "/"));
    CRPC_CHECK_EQ(std::string("/a/b"),
                  crpc::mvc::join_route_path("", "a/b"));
    CRPC_CHECK_EQ(std::string("/"), crpc::mvc::join_route_path("", "/"));
}

CRPC_TEST_MAIN()
