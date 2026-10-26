// CRPC MVC example.
//
// RTTI-attributed request DTOs drive parameter binding, the CRPC_HTTP_* macros
// declare a RESTful route table, and the HTTP facade serves it. The example is
// one process: it dispatches a handful of in-memory requests through the frozen
// router (GET/POST, a path parameter, a query parameter and an error return) and
// then starts a real `crpc::http_server` on an in-process endpoint to prove the
// same router is served by the facade.
//
// Exit code 0 means every dispatched request produced the expected status and
// body, and the HTTP server started and stopped cleanly.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include <crpc/http/message.hpp>
#include <crpc/http/server.hpp>
#include <crpc/io.hpp>
#include <crpc/mvc/attributes.hpp>
#include <crpc/mvc/router.hpp>

// --- Reflected DTOs ----------------------------------------------------------

// A path parameter: the router captures `:id` and the binder converts it to the
// field's type. A missing or non-numeric value is a 400.
struct get_user_req {
    std::int64_t id = 0;
};
RTTI_DEFINE_TYPE(get_user_req, id)
RTTI_DEFINE_FIELD_ATTRS(get_user_req,
                        RTTI_FIELD_ATTR_ENTRY(id, CRPC_FROM_ROUTE("id")))

// A query parameter plus an optional one. `limit` is optional, so its absence is
// not an error.
struct list_users_req {
    std::int64_t page = 0;
    std::optional<std::int64_t> limit;
};
RTTI_DEFINE_TYPE(list_users_req, page, limit)
RTTI_DEFINE_FIELD_ATTRS(
    list_users_req, RTTI_FIELD_ATTR_ENTRY(page, CRPC_FROM_QUERY("page")),
    RTTI_FIELD_ATTR_ENTRY(limit, CRPC_FROM_QUERY("limit")))

// A JSON body document: no `crpc.from` attribute, so the whole body is decoded
// into the DTO.
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

// --- Controller --------------------------------------------------------------

struct user_controller {
    CRPC_ROUTE("/users")

    crpc::mvc::status_body<user_view> get_user(const get_user_req& req) {
        return crpc::mvc::with_status(200, user_view{req.id, "alice"});
    }

    crpc::mvc::status_body<user_view> list_users(const list_users_req& req) {
        const std::int64_t limit = req.limit.value_or(10);
        return crpc::mvc::with_status(
            200, user_view{req.page * limit, "page"});
    }

    crpc::mvc::status_body<user_view> create_user(const create_user_req& req) {
        return crpc::mvc::with_status(201, user_view{1, req.name});
    }

    // A `result<T>` return maps an error to its status code (404 here) and a
    // value to 200.
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
CRPC_HTTP_GET(user_controller, maybe, "/:id/maybe")

namespace {

// --- Dispatch helpers --------------------------------------------------------

int failures = 0;

void expect_status(const char* what, int expected,
                   const crpc::http::response& response) {
    if (response.status_code != expected) {
        std::cerr << "mvc: " << what << ": expected status " << expected
                  << ", got " << response.status_code << "\n";
        ++failures;
    }
}

void expect_body_contains(const char* what, const crpc::http::response& response,
                          const std::string& needle) {
    if (response.body.find(needle) == std::string::npos) {
        std::cerr << "mvc: " << what << ": body does not contain '" << needle
                  << "': " << response.body << "\n";
        ++failures;
    }
}

crpc::http::request make_request(crpc::http::method verb, const std::string& path) {
    crpc::http::request request;
    request.method = verb;
    request.path = path;
    return request;
}

} // namespace

int main() {
    const crpc::mvc::router& routes = crpc::mvc::default_router();

    // GET /users/42 -> 200 with the reflected view encoded as JSON.
    {
        const crpc::http::request request =
            make_request(crpc::http::method::get, "/users/42");
        crpc::http::response response;
        if (const crpc::status dispatched =
                crpc::mvc::dispatch(routes, request, response);
            !dispatched.ok()) {
            std::cerr << "mvc: dispatch failed: " << dispatched.message() << "\n";
            return 1;
        }
        expect_status("GET /users/42", 200, response);
        expect_body_contains("GET /users/42", response, "alice");
        std::cout << "mvc: GET /users/42 -> " << response.status_code << " "
                  << response.body << "\n";
    }

    // GET /users?page=2&limit=5 -> 200; the query parameters are bound.
    {
        crpc::http::request request =
            make_request(crpc::http::method::get, "/users");
        request.query = "page=2&limit=5";
        crpc::http::response response;
        (void)crpc::mvc::dispatch(routes, request, response);
        expect_status("GET /users?page=2&limit=5", 200, response);
        expect_body_contains("GET /users?page=2&limit=5", response, "page");
        std::cout << "mvc: GET /users?page=2&limit=5 -> " << response.status_code
                  << " " << response.body << "\n";
    }

    // POST /users with a JSON body -> 201 and the created name.
    {
        crpc::http::request request =
            make_request(crpc::http::method::post, "/users");
        request.headers.add("content-type", "application/json");
        request.body = "{\"name\":\"carol\",\"age\":31}";
        crpc::http::response response;
        (void)crpc::mvc::dispatch(routes, request, response);
        expect_status("POST /users", 201, response);
        expect_body_contains("POST /users", response, "carol");
        std::cout << "mvc: POST /users -> " << response.status_code << " "
                  << response.body << "\n";
    }

    // GET /users/-1/maybe -> 404 from the `result<T>` error path.
    {
        const crpc::http::request request =
            make_request(crpc::http::method::get, "/users/-1/maybe");
        crpc::http::response response;
        (void)crpc::mvc::dispatch(routes, request, response);
        expect_status("GET /users/-1/maybe", 404, response);
        std::cout << "mvc: GET /users/-1/maybe -> " << response.status_code << "\n";
    }

    // GET /users/abc -> 400: the path parameter cannot convert to int64.
    {
        const crpc::http::request request =
            make_request(crpc::http::method::get, "/users/abc");
        crpc::http::response response;
        (void)crpc::mvc::dispatch(routes, request, response);
        expect_status("GET /users/abc", 400, response);
        std::cout << "mvc: GET /users/abc -> " << response.status_code << "\n";
    }

    // GET /nope -> 404: no route matches.
    {
        const crpc::http::request request =
            make_request(crpc::http::method::get, "/nope");
        crpc::http::response response;
        (void)crpc::mvc::dispatch(routes, request, response);
        expect_status("GET /nope", 404, response);
        std::cout << "mvc: GET /nope -> " << response.status_code << "\n";
    }

    // The same router is served by the HTTP facade. Start it on an in-process
    // endpoint, confirm it is listening, then stop it.
    crpc::inproc_backend backend;
    crpc::http::http_server::builder server_builder;
    server_builder.add_listener(crpc::endpoint::inproc("mvc-example"), backend);
    std::unique_ptr<crpc::http::http_server> server = server_builder.build();
    if (!server) {
        std::cerr << "mvc: build http_server failed\n";
        return 1;
    }
    if (const crpc::status started = server->start(); !started.ok()) {
        std::cerr << "mvc: start http_server failed: " << started.message()
                  << "\n";
        return 1;
    }
    std::cout << "mvc: http_server listening on inproc:mvc-example\n";
    server->stop();
    if (server->running()) {
        std::cerr << "mvc: http_server still running after stop()\n";
        return 1;
    }

    if (failures != 0) {
        std::cerr << "mvc: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "mvc OK\n";
    return 0;
}
