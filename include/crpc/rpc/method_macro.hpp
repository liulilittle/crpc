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
#ifndef CRPC_RPC_METHOD_MACRO_HPP
#define CRPC_RPC_METHOD_MACRO_HPP

#include <vector>

#include <crpc/rpc/dispatcher.hpp>
#include <crpc/rpc/method_meta.hpp>

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

} // namespace crpc::rpc

// Declares `Method` as an RPC method of `Ctl` under the wire name
// `name_literal`. `Ctl` is the complete controller type; `Method` is the method
// name used to form the metadata member (`crpc_rpc_meta_##Method`) and must be
// declared before the macro.
#define CRPC_RPC_METHOD(Ctl, Method, name_literal)                              \
    inline static constexpr ::crpc::rpc::method_meta crpc_rpc_meta_##Method{     \
        ::crpc::rpc::method_id_from<Ctl>(), name_literal};                       \
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

#endif // CRPC_RPC_METHOD_MACRO_HPP
