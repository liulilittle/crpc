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
// The macro expands to one `inline static constexpr` member per method:
//
//   * `crpc_rpc_meta_##Method` carries the service id and the method name.
//     `inline` gives the member a single address across translation units, which
//     is what lets the registration path find it without a hand-written table.
//
//   * `Ctl` is deliberately incomplete when the macro expands (inside the
//     controller body), and `method_id_from` is a constant expression that hashes
//     `__FUNCSIG__`/`__PRETTY_FUNCTION__`, so completeness is not required. It
//     only has to be complete by the time `add_controller<Ctl>` runs.
//
// The `friend` declaration lets the class that walks the controller's declared
// methods (`service_registrar<Ctl>`) read the members even when they are private;
// the registrar itself arrives with `add_controller`.
#ifndef CRPC_RPC_METHOD_MACRO_HPP
#define CRPC_RPC_METHOD_MACRO_HPP

#include <crpc/rpc/method_meta.hpp>

namespace crpc::rpc {

// Forward declaration only. The friend declaration in the macro names
// `service_registrar<Ctl>` so the walker can be a friend without a definition
// existing at this point; defining it is out of scope here.
template <typename Ctl>
class service_registrar;

} // namespace crpc::rpc

// Declares `Method` as an RPC method of `Ctl` under the wire name
// `name_literal`. `Ctl` is the complete controller type; `Method` is the method
// name used to form the metadata member (`crpc_rpc_meta_##Method`).
#define CRPC_RPC_METHOD(Ctl, Method, name_literal)                              \
    inline static constexpr ::crpc::rpc::method_meta crpc_rpc_meta_##Method{     \
        ::crpc::rpc::method_id_from<Ctl>(), name_literal};                       \
    friend class ::crpc::rpc::service_registrar<Ctl>;

#endif // CRPC_RPC_METHOD_MACRO_HPP
