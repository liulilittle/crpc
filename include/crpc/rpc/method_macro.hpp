// CRPC -- declarative RPC method declaration (R8).
//
// `CRPC_RPC_METHOD(Ctl, Method, name)` is the RPC counterpart of the MVC action
// macros: it declares a method on a controller and, as its only effect, emits the
// metadata the declarative registration path reads. The service id is derived
// from `Ctl` with `method_id_from`, so no id is ever written by hand and the
// server and every client agree on the same number.
//
//   struct user_controller {
//       CRPC_RPC_METHOD(user_controller, get_user, "user.get")
//   };
//
// The macro expands to:
//
//   * `crpc_rpc_meta_##Method`, an `inline static constexpr` member carrying the
//     service id and the method name. `inline` gives the member a single address
//     across translation units.
//
//   * `crpc_rpc_reg_##Method`, a static registrar whose construction appends a
//     `method_descriptor` to `service_registrar<Ctl>::methods()`. C++17 cannot
//     enumerate a class's static members, so this explicit list is what lets
//     `add_controller<Ctl>` find every declared method.
//
//   * a `friend` declaration for `service_registrar<Ctl>`, so the registrar can
//     read the metadata members even when they are private.
//
// `Ctl` is deliberately incomplete when the macro expands (inside the
// controller body), and `method_id_from` is a constant expression that hashes
// `__FUNCSIG__`/`__PRETTY_FUNCTION__`, so completeness is not required. It only
// has to be complete by the time `add_controller<Ctl>` runs. `Method` must be
// declared before the macro: the registrar names `&Ctl::Method`.
//
// `CRPC_RPC_SERVICE(Ctl, name)` is optional and declares the service name the
// controller registers under. `add_controller<Ctl>` registers the service itself
// (id `method_id_from<Ctl>()`, name `Ctl::crpc_service_name`), so the caller does
// not call `add_service`. Without `CRPC_RPC_SERVICE` the name falls back to
// `typeid(Ctl).name()`, or to an empty string when the translation unit cannot
// write `typeid` (the registry does not use the name for dispatch, only for
// diagnostics).
#ifndef CRPC_RPC_METHOD_MACRO_HPP
#define CRPC_RPC_METHOD_MACRO_HPP

#include <string>
#include <type_traits>
#include <typeinfo>
#include <vector>

#include <crpc/rpc/dispatcher.hpp>
#include <crpc/rpc/method_meta.hpp>

// Mirrors `CRPC_CAN_USE_TYPEID` in crpc/codec.hpp: whether this translation unit
// can write the static `typeid(T)` form. Kept local so this header does not pull
// in the codec registry just for the service-name fallback. The two must agree
// for one binary; a build that mixes RTTI flags across translation units is not
// supported (see the comment in crpc/codec.hpp).
#if !defined(CRPC_RPC_TYPEID_AVAILABLE)
#if defined(_MSC_VER) && !defined(__clang__)
#define CRPC_RPC_TYPEID_AVAILABLE 1
#elif defined(__cpp_rtti)
#define CRPC_RPC_TYPEID_AVAILABLE 1
#else
#define CRPC_RPC_TYPEID_AVAILABLE 0
#endif
#endif

namespace crpc::rpc {

// Deterministic method id for a declared method: the FNV-1a hash of the wire
// name, folded with the controller's service id. The fold keeps the id unique
// across services as well as within one, so two controllers that reuse a method
// name do not collide. Scoped per service, which is where the registry looks
// methods up.
template <typename Ctl>
[[nodiscard]] constexpr u64 method_id_from_name(const char* name) noexcept {
    u64 hash = method_meta_detail::fnv1a(name);
    hash ^= method_id_from<Ctl>();
    hash *= method_meta_detail::fnv1a_prime;
    return hash;
}

// The per-controller list of declared methods. `CRPC_RPC_METHOD` appends one
// descriptor per method during static initialization; `add_controller` walks the
// list. C++17 cannot enumerate a class's static members, so this explicit list
// is what makes the declarative registration possible.
template <typename Ctl>
class service_registrar {
public:
    struct method_descriptor {
        u64 service_id;
        u64 method_id;
        const char* name;
        status (*register_fn)(service_registry&, void*);
    };

    [[nodiscard]] static std::vector<method_descriptor>& methods() {
        static std::vector<method_descriptor> list;
        return list;
    }
};

namespace service_name_detail {

// True when `Ctl` declared `crpc_service_name` with `CRPC_RPC_SERVICE`.
template <typename Ctl, typename = void>
struct has_service_name : std::false_type {};

template <typename Ctl>
struct has_service_name<Ctl, std::void_t<decltype(Ctl::crpc_service_name)>>
    : std::true_type {};

} // namespace service_name_detail

// The service name a declarative controller registers under: the name declared
// with `CRPC_RPC_SERVICE`, else the compiler's type-name spelling, else an empty
// string when `typeid` is unavailable.
template <typename Ctl>
[[nodiscard]] std::string service_name_of() {
    if constexpr (service_name_detail::has_service_name<Ctl>::value) {
        const char* declared = Ctl::crpc_service_name;
        return declared != nullptr ? std::string(declared) : std::string();
    } else {
#if CRPC_RPC_TYPEID_AVAILABLE
        const char* spelling = typeid(Ctl).name();
        return spelling != nullptr ? std::string(spelling) : std::string();
#else
        return std::string();
#endif
    }
}

} // namespace crpc::rpc

// Declares `Method` as an RPC method of `Ctl` under the wire name
// `name_literal`. `Ctl` is the complete controller type; `Method` is the method
// name used to form the metadata member (`crpc_rpc_meta_##Method`) and must be
// declared before the macro.
//
// `crpc_rpc_meta_##Method` is exposed for introspection, but the registrar below
// does not need it (it recomputes the id and the name), so a controller that
// never reads it would otherwise trip clang's -Wunused-const-variable in a
// -Werror build. `[[maybe_unused]]` states that the member is deliberately
// optional rather than forgotten.
#define CRPC_RPC_METHOD(Ctl, Method, name_literal)                              \
    [[maybe_unused]] inline static constexpr ::crpc::rpc::method_meta            \
        crpc_rpc_meta_##Method{                                                  \
            ::crpc::rpc::method_id_from<Ctl>(), name_literal};                   \
    friend class ::crpc::rpc::service_registrar<Ctl>;                            \
    inline static const bool crpc_rpc_reg_##Method = [] {                        \
        ::crpc::rpc::service_registrar<Ctl>::methods().push_back(                \
            ::crpc::rpc::service_registrar<Ctl>::method_descriptor{             \
                ::crpc::rpc::method_id_from<Ctl>(),                              \
                ::crpc::rpc::method_id_from_name<Ctl>(name_literal),             \
                name_literal,                                                    \
                [](::crpc::rpc::service_registry& registry, void* self)         \
                    -> ::crpc::status {                                          \
                    return ::crpc::rpc::register_typed_method<                   \
                        Ctl, &Ctl::Method>(                                      \
                            registry, ::crpc::rpc::method_id_from<Ctl>(),        \
                            ::crpc::rpc::method_id_from_name<Ctl>(name_literal), \
                            name_literal, *static_cast<Ctl*>(self));             \
                }});                                                             \
        return true;                                                             \
    }();

// Declares the service name `Ctl` registers under. Place it inside the
// controller body. `add_controller<Ctl>` uses it; without it the name falls back
// to `typeid(Ctl).name()` (or empty when `typeid` is unavailable).
#define CRPC_RPC_SERVICE(Ctl, name_literal)                                     \
    inline static constexpr const char* crpc_service_name = name_literal;

#endif // CRPC_RPC_METHOD_MACRO_HPP
