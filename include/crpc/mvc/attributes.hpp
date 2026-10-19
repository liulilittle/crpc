// CRPC -- MVC declaration macros (R8).
//
// C++17 cannot infer a member pointer from a string, so every action macro
// carries the controller type and the member name explicitly. The macro then
// takes `&controller_t::method`, which makes the compiler check that the method
// exists and that its signature is one of the shapes method_traits accepts.
//
//   struct user_controller {
//       CRPC_ROUTE("/users")
//       status_body<user> get_user(const get_user_req& request);
//   };
//   CRPC_HTTP_GET(user_controller, get_user, "/:id")
//
// Field-level binding is declared on the request DTO with RTTI field
// attributes. The attribute name is "crpc.from"; its first argument is the
// source kind and its optional second argument is the source name:
//
//   struct get_user_req { std::int64_t id; };
//   RTTI_DEFINE_TYPE(get_user_req, id)
//   RTTI_DEFINE_FIELD_ATTRS(get_user_req,
//       RTTI_FIELD_ATTR_ENTRY(id, CRPC_FROM_ROUTE("id")))
//
// RTTI is consumed read-only; this header never modifies it.
#ifndef CRPC_MVC_ATTRIBUTES_HPP
#define CRPC_MVC_ATTRIBUTES_HPP

#include <cstdint>

#include <crpc/config.hpp>
#include <crpc/http/message.hpp>
#include <crpc/mvc/router.hpp>

// `crpc/codec.hpp` (included through mvc/router.hpp) defines CRPC_HAVE_RTTI to
// 1 or 0 and includes the RTTI header when it is 1. The test must be numeric:
// `defined(CRPC_HAVE_RTTI)` is also true for the value 0, which used to define
// CRPC_MVC_HAVE_RTTI_ATTRS without the macros behind it existing.
#if !defined(CRPC_MVC_HAVE_RTTI_ATTRS)
#if CRPC_HAVE_RTTI
#define CRPC_MVC_HAVE_RTTI_ATTRS 1
#endif
#endif

// Class-level route prefix. Expands to a static member read by the action
// macros through route_prefix_of.
#define CRPC_ROUTE(path_literal)                                              \
    inline static constexpr ::crpc::mvc::controller_meta crpc_route_meta{      \
        path_literal};

// Internal: one action registration object. `tag` keeps the object name unique
// per verb; the object has internal-to-the-header linkage via `inline`. The
// member parameter is named `member` rather than `method` so it never collides
// with the `::crpc::http::method` token in the verb argument.
#define CRPC_MVC_ACTION(tag, controller_t, member, verb_enum, path_literal)     \
    inline const ::crpc::mvc::route_registrar<controller_t,                    \
                                              &controller_t::member>           \
        crpc_route_##tag##_##controller_t##_##member{                          \
            verb_enum, path_literal, #member,                                  \
            static_cast<std::uint32_t>(__LINE__)};

#define CRPC_HTTP_GET(controller_t, member, path_literal)                     \
    CRPC_MVC_ACTION(get, controller_t, member, ::crpc::http::method::get,      \
                    path_literal)
#define CRPC_HTTP_POST(controller_t, member, path_literal)                    \
    CRPC_MVC_ACTION(post, controller_t, member, ::crpc::http::method::post,    \
                    path_literal)
#define CRPC_HTTP_PUT(controller_t, member, path_literal)                     \
    CRPC_MVC_ACTION(put, controller_t, member, ::crpc::http::method::put,      \
                    path_literal)
#define CRPC_HTTP_DELETE(controller_t, member, path_literal)                  \
    CRPC_MVC_ACTION(del, controller_t, member, ::crpc::http::method::del,      \
                    path_literal)
#define CRPC_HTTP_PATCH(controller_t, member, path_literal)                   \
    CRPC_MVC_ACTION(patch, controller_t, member, ::crpc::http::method::patch,  \
                    path_literal)

// Field-source attributes. Each expands to an RTTI attribute named "crpc.from"
// whose first argument is the source kind and whose second (when present) is
// the source name. They are only meaningful inside RTTI_DEFINE_FIELD_ATTRS.
#if defined(CRPC_MVC_HAVE_RTTI_ATTRS)
#define CRPC_FROM_ROUTE(name) RTTI_ATTR("crpc.from", "route", name)
#define CRPC_FROM_QUERY(name) RTTI_ATTR("crpc.from", "query", name)
#define CRPC_FROM_BODY RTTI_ATTR("crpc.from", "body")
#define CRPC_FROM_HEADER(name) RTTI_ATTR("crpc.from", "header", name)
#define CRPC_FROM_FORM(name) RTTI_ATTR("crpc.from", "form", name)
#define CRPC_FROM_FILE RTTI_ATTR("crpc.from", "file")
#endif

#endif // CRPC_MVC_ATTRIBUTES_HPP

