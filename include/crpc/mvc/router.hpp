// CRPC -- MVC route table, matching and dispatch.
//
// Lifecycle
// ---------
// Routes are registered by the CRPC_HTTP_* macros at static-initialization time
// into a process-wide pending list (a function-local static, so there is no
// static-initialization-order hazard). `default_router()` copies that list,
// compiles the patterns and freezes the table on first use. A caller that wants
// a private table builds a `router` by hand, calls `add` for each entry and then
// `freeze`.
//
// Freeze contract
// ---------------
//   * `add` and `set_options` are only legal before `freeze`; afterwards they
//     fail with `failed_precondition`.
//   * `freeze` compiles every pattern, rejects malformed patterns and detects
//     conflicts (same method + same normalized pattern) as a startup error.
//     It is idempotent.
//   * After `freeze`, `match`/`handle` only read immutable tables: no locks, no
//     allocation on the lookup itself (the captured parameter vector is the one
//     allocation). Concurrent lookups from any number of threads are safe.
//
// Matching
// --------
// A path is split into segments. A pattern segment is static, a parameter
// (`:name`) or a catch-all (`*name`, which must be last). Priority is evaluated
// segment by segment: static > parameter > catch-all. An exact (all-static)
// pattern is looked up in a hash table first; the remaining patterns are scanned
// in priority order. A path that exists under other methods yields 405 with an
// `Allow` header; an unknown path yields 404. Trailing slashes are normalized
// away. Path matching is case-sensitive unless `case_insensitive_routes` is set;
// the method token is always matched case-insensitively.
#ifndef CRPC_MVC_ROUTER_HPP
#define CRPC_MVC_ROUTER_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include <crpc/buffer.hpp>
#include <crpc/codec.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/mvc/binding.hpp>

namespace crpc::mvc {

// --- Controller prefix -------------------------------------------------------

// The class-level route prefix declared by CRPC_ROUTE.
struct controller_meta {
    const char* prefix;
    constexpr explicit controller_meta(const char* value) noexcept
        : prefix(value) {}
};

// --- Method traits -----------------------------------------------------------

// Recognized handler shapes. `request_type` is void for a no-argument handler.
template <typename Signature>
struct method_traits;

template <typename C, typename R, typename Q>
struct method_traits<R (C::*)(const Q&)> {
    using controller_type = C;
    using request_type = Q;
    using response_type = R;
};

template <typename C, typename R, typename Q>
struct method_traits<R (C::*)(const Q&) const> {
    using controller_type = C;
    using request_type = Q;
    using response_type = R;
};

template <typename C, typename R>
struct method_traits<R (C::*)()> {
    using controller_type = C;
    using request_type = void;
    using response_type = R;
};

template <typename C, typename R>
struct method_traits<R (C::*)() const> {
    using controller_type = C;
    using request_type = void;
    using response_type = R;
};

// --- Explicit return wrappers ------------------------------------------------

// Wraps a value with an explicit status code, overriding the default mapping.
template <typename T>
struct status_body {
    int status = 200;
    T value{};
};

template <typename T>
[[nodiscard]] status_body<std::decay_t<T>> with_status(int status_code,
                                                       T&& value) {
    return status_body<std::decay_t<T>>{status_code, std::forward<T>(value)};
}

// A 204 No Content marker.
struct no_content_t {};
inline constexpr no_content_t no_content{};

// --- Route table types -------------------------------------------------------

struct compiled_segment {
    enum class kind : std::uint8_t { static_, parameter, catch_all };
    kind type = kind::static_;
    std::string text; // literal text, parameter name or catch-all name
};

using invoke_fn = status (*)(void* controller, const http::request& request,
                             const route_params& params,
                             const codec& response_codec,
                             const codec_registry& registry,
                             http::response& response);

struct route_entry {
    http::method method = http::method::get;
    std::string path; // full pattern, normalized at freeze
    std::string name; // action (member) name
    void* controller = nullptr;
    const std::type_info* controller_type = nullptr;
    // Type metadata derived from the method pointer. `request_type` is null for
    // a no-argument handler; it names the DTO whose reflected fields drive
    // parameter binding. `response_type` is null for a void handler.
    const std::type_info* request_type = nullptr;
    const std::type_info* response_type = nullptr;
    // Declared content types. Empty means "negotiate the response from Accept"
    // and "infer the request codec from Content-Type".
    std::string produces;
    std::string consumes;
    // Declared status code; 0 means the default return-value mapping.
    int status_code = 0;
    invoke_fn invoke = nullptr;
    std::uint32_t ordinal = 0;
    std::vector<compiled_segment> segments;
};

enum class match_kind : std::uint8_t {
    matched,
    not_found,
    method_not_allowed,
    bad_request
};

struct route_match {
    const route_entry* route = nullptr;
    route_params params;
};

struct match_result {
    match_kind kind = match_kind::not_found;
    route_match match;
    std::string allow;
};

// --- Router ------------------------------------------------------------------

class router {
public:
    struct options {
        bool case_insensitive_routes = false;
        binding_limits limits{};
    };

    router() = default;

    // Pre-freeze registration. Returns `failed_precondition` when frozen or when
    // the entry is incomplete (no invoke).
    [[nodiscard]] status add(route_entry entry);

    // Pre-freeze option update.
    [[nodiscard]] status set_options(options value);

    // Compiles and freezes the table; detects conflicts and malformed patterns.
    [[nodiscard]] status freeze();

    [[nodiscard]] bool frozen() const noexcept { return frozen_; }
    [[nodiscard]] const options& settings() const noexcept { return options_; }
    [[nodiscard]] const codec_registry& registry() const noexcept {
        return *registry_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // Matches `path` for `verb`. When the path exists only under other methods
    // the result is `method_not_allowed` and `allow` (if given) receives a
    // comma-separated list. Not available before freeze (returns not_found).
    [[nodiscard]] match_result match(http::method verb, std::string_view path,
                                     std::string* allow = nullptr) const;

    // Full pipeline: match, negotiate Accept, bind, invoke and write `response`.
    // Never throws; internal failures become 500.
    [[nodiscard]] status handle(const http::request& request,
                                http::response& response) const noexcept;

private:
    [[nodiscard]] const route_entry* lookup_exact(http::method verb,
                                                  std::string_view path) const;

    std::vector<route_entry> entries_;
    std::unordered_map<std::string, const route_entry*> exact_;
    std::vector<const route_entry*> parameters_;
    options options_{};
    bool frozen_ = false;
    const codec_registry* registry_ = &default_codec_registry();
};

// Convenience wrapper around `router::handle`.
[[nodiscard]] status dispatch(const router& routes,
                              const http::request& request,
                              http::response& response);

// --- Startup registry --------------------------------------------------------

// The process-wide pending list the CRPC_HTTP_* macros append to before main.
[[nodiscard]] std::vector<route_entry>& pending_routes();

// The shared controller instance for a controller type. Stateless controllers
// are the norm; a caller that needs per-request state should register a router
// by hand with `add`.
template <typename Controller>
Controller& controller_instance() {
    static Controller instance{};
    return instance;
}

// The prefix declared by CRPC_ROUTE, or "" when the controller has none.
template <typename C, typename = void>
struct route_prefix_of {
    [[nodiscard]] static constexpr const char* value() noexcept { return ""; }
};
template <typename C>
struct route_prefix_of<C, std::void_t<decltype(C::crpc_route_meta)>> {
    [[nodiscard]] static constexpr const char* value() noexcept {
        return C::crpc_route_meta.prefix;
    }
};

// The shared, frozen router built from `pending_routes()` on first use.
[[nodiscard]] router& default_router();

// Joins a controller prefix and an action path into a single normalized
// pattern ("/users" + "/:id" -> "/users/:id"; "/users" + "/" -> "/users").
[[nodiscard]] std::string join_route_path(std::string_view prefix,
                                          std::string_view path);

// --- Non-template helpers (implemented in src/mvc/router.cpp) ----------------

[[nodiscard]] const char* reason_phrase(int status_code) noexcept;
void write_error(http::response& response, const rpc_error& error,
                 std::string_view instance);
[[nodiscard]] std::string normalize_path(std::string_view path, bool lower);
[[nodiscard]] http::method effective_method(const http::request& request);

// --- Invocation --------------------------------------------------------------

namespace detail {

template <typename T>
struct is_result : std::false_type {};
template <typename T>
struct is_result<crpc::result<T>> : std::true_type {};

template <typename T>
struct is_status_body : std::false_type {};
template <typename T>
struct is_status_body<status_body<T>> : std::true_type {};

template <typename T>
[[nodiscard]] status write_body(const codec& response_codec, const T& value,
                                http::response& response, int status_code) {
    buffer encoded;
    const status encoded_status = encode_into(response_codec, value, encoded);
    if (!encoded_status.ok()) {
        return encoded_status;
    }
    response.status_code = status_code;
    response.reason = reason_phrase(status_code);
    response.headers.add("Content-Type", response_codec.content_type());
    response.body.assign(
        reinterpret_cast<const char*>(encoded.readable().data()),
        encoded.readable().size());
    return status{};
}

inline void set_no_content(http::response& response) {
    response.status_code = 204;
    response.reason = "No Content";
    response.body.clear();
}

template <typename Response, typename Callable>
status finish_call(const codec& response_codec, Callable&& call,
                   http::response& response, std::string_view path) {
    (void)response_codec;
    (void)path;
    if constexpr (std::is_void_v<Response>) {
        call();
        set_no_content(response);
        return status{};
    } else if constexpr (std::is_same_v<Response, status>) {
        const status result = call();
        if (!result.ok()) {
            write_error(response, result.error(), path);
            return status{};
        }
        set_no_content(response);
        return status{};
    } else if constexpr (is_result<Response>::value) {
        Response result = call();
        if (!result.ok()) {
            write_error(response, result.error(), path);
            return status{};
        }
        return write_body(response_codec, result.value(), response, 200);
    } else if constexpr (is_status_body<Response>::value) {
        Response result = call();
        return write_body(response_codec, result.value, response,
                          result.status);
    } else if constexpr (std::is_same_v<Response, no_content_t>) {
        call();
        set_no_content(response);
        return status{};
    } else {
        Response value = call();
        return write_body(response_codec, value, response, 200);
    }
}

} // namespace detail

// Type-checked handler adapter. `Method` is `&Controller::method`, so the
// compiler rejects a macro that names a method the controller does not have and
// every signature mismatch is caught at compile time.
//
// In an exception-enabled translation unit a handler that throws is caught here
// and reported as `rpc_code::internal`. In a `-fno-exceptions` / `/EHs-c-` build
// there is no try/catch, so a throwing handler is undefined behaviour and in
// practice terminates the process (`std::terminate`).
template <typename Controller, auto Method>
status invoke_http(void* self, const http::request& request,
                   const route_params& params, const codec& response_codec,
                   const codec_registry& registry, http::response& response) {
    using traits = method_traits<decltype(Method)>;
    using request_type = typename traits::request_type;
    using response_type = typename traits::response_type;
    Controller* controller = static_cast<Controller*>(self);
    // The handler body is shared by both builds; only the ability to observe a
    // throw differs.
    auto invoke = [&]() -> status {
        if constexpr (std::is_void_v<request_type>) {
            return detail::finish_call<response_type>(
                response_codec,
                [&]() -> decltype(auto) { return (controller->*Method)(); },
                response, request.path);
        } else {
            request_type dto{};
            const status bound = bind_request<request_type>(
                request, params, registry, default_binding_limits(), dto);
            if (!bound.ok()) {
                write_error(response, bound.error(), request.path);
                return status{};
            }
            return detail::finish_call<response_type>(
                response_codec,
                [&]() -> decltype(auto) {
                    return (controller->*Method)(dto);
                },
                response, request.path);
        }
    };
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        return invoke();
    } catch (...) {
        write_error(response, rpc_error(rpc_code::internal, "internal error"),
                    request.path);
        return status{};
    }
#else
    // Built without exception support (-fno-exceptions, /EHs-c-). MSVC without
    // any /EH option also lands here: neither `__cpp_exceptions` nor
    // `_CPPUNWIND` is defined then. A throwing handler is undefined behaviour
    // and in practice terminates the process (`std::terminate`); the `internal`
    // error path above is unreachable in this build.
    return invoke();
#endif
}

// --- Registration ------------------------------------------------------------

// One registration object per CRPC_HTTP_* macro. Its constructor appends a
// descriptor to `pending_routes()` during static initialization.
template <typename Controller, auto Method>
class route_registrar {
public:
    route_registrar(http::method verb, const char* path, const char* name,
                    std::uint32_t ordinal) {
        using traits = method_traits<decltype(Method)>;
        route_entry entry;
        entry.method = verb;
        entry.path = join_route_path(route_prefix_of<Controller>::value(), path);
        entry.name = name != nullptr ? name : "";
        entry.controller = &controller_instance<Controller>();
        entry.controller_type = &typeid(Controller);
        if constexpr (!std::is_void_v<typename traits::request_type>) {
            entry.request_type = &typeid(typename traits::request_type);
        }
        if constexpr (!std::is_void_v<typename traits::response_type>) {
            entry.response_type = &typeid(typename traits::response_type);
        }
        entry.invoke = &invoke_http<Controller, Method>;
        entry.ordinal = ordinal;
        pending_routes().push_back(std::move(entry));
    }
};

} // namespace crpc::mvc

#endif // CRPC_MVC_ROUTER_HPP
