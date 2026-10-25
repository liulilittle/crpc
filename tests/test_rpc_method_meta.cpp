// CRPC -- deterministic method/service id and method metadata.
//
// The declarative service API derives a service id from the controller type and
// a method name from a string literal. Both the server and every client must
// compute the same number from the same type without a hand-maintained table,
// so `method_id_from<Ctl>()` has to be a compile-time constant, deterministic,
// and distinct for distinct types.
//
// The checks below cover four properties:
//
//   * the FNV-1a primitive matches the published 64-bit test vectors (the hash
//     itself is correct, not merely self-consistent);
//   * `method_id_from` is a constant expression and is stable across the point
//     of use -- including inside a class body, where the controller type is
//     still incomplete, which is exactly where the macro evaluates it;
//   * distinct types get distinct ids, over a spread of instantiations;
//   * `method_meta` carries the service id and the (non-owning) method name.
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <crpc/rpc/method_macro.hpp>
#include <crpc/rpc/method_meta.hpp>

#include "test_framework.hpp"

namespace {

using namespace crpc;
using namespace crpc::rpc;

// A minimal controller stand-in; `method_id_from` never needs the type complete.
struct controller_alpha {};
struct controller_beta {};
struct controller_gamma {};

// Forward-declared and never defined: the controller is incomplete inside its
// own class body, so the id must not depend on completeness.
struct controller_incomplete;

// --- Compile-time properties -------------------------------------------------

// Deterministic: the same type always hashes to the same id.
static_assert(method_id_from<controller_alpha>() == method_id_from<controller_alpha>(),
              "method_id_from must be deterministic");

// Distinct types hash to distinct ids.
static_assert(method_id_from<controller_alpha>() != method_id_from<controller_beta>(),
              "distinct controllers must have distinct ids");
static_assert(method_id_from<controller_alpha>() != method_id_from<controller_gamma>(),
              "distinct controllers must have distinct ids");

// The documented contract: constexpr and noexcept.
static_assert(noexcept(method_id_from<controller_alpha>()),
              "method_id_from must be noexcept");
static_assert(method_id_from<controller_alpha>() != 0,
              "a derived id is never the invalid id");

// An incomplete controller type is usable.
static_assert(method_id_from<controller_incomplete>() != 0,
              "an incomplete controller type must still hash");

// The FNV-1a primitive matches the published 64-bit vectors, so the ids are
// computed by a correct hash rather than an arbitrary self-consistent one.
static_assert(method_meta_detail::fnv1a("") == 0xcbf29ce484222325ull, "FNV-1a(\"\")");
static_assert(method_meta_detail::fnv1a("a") == 0xaf63dc4c8601ec8cull, "FNV-1a(\"a\")");
static_assert(method_meta_detail::fnv1a("foobar") == 0x85944171f73967e8ull,
              "FNV-1a(\"foobar\")");

// The hash distinguishes an appended byte, i.e. it actually reads the whole
// string rather than a prefix.
static_assert(method_meta_detail::fnv1a("crpc::A") != method_meta_detail::fnv1a("crpc::B"),
              "FNV-1a must depend on every byte");

// --- Type identity for the collision sweep -----------------------------------

template <std::size_t N>
struct ctl_tag {};

template <std::size_t... I>
bool all_ids_distinct(std::index_sequence<I...>) {
    const u64 ids[] = {method_id_from<ctl_tag<I>>()...};
    constexpr std::size_t count = sizeof...(I);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            if (ids[i] == ids[j]) {
                return false;
            }
        }
    }
    return true;
}

// A controller that uses the id in exactly the position the macro will: as the
// initializer of an `inline static constexpr` member, with the type incomplete.
struct declaring_controller {
    inline static constexpr method_meta meta{method_id_from<declaring_controller>(),
                                             "declaring.ping"};
    static constexpr u64 id_inside_class() { return method_id_from<declaring_controller>(); }
};

// --- CRPC_RPC_METHOD ---------------------------------------------------------

// A controller that declares its methods with the macro. The controller type is
// incomplete inside its own body, so the macro must be able to derive the service
// id from an incomplete type exactly where it is expanded.
struct macro_controller {
    CRPC_RPC_METHOD(macro_controller, ping, "macro.ping")
};

// Two methods in one controller: the macro must generate a distinct static member
// per method, all sharing the controller's service id.
struct macro_controller_two {
    CRPC_RPC_METHOD(macro_controller_two, ping, "macro.ping")
    CRPC_RPC_METHOD(macro_controller_two, pong, "macro.pong")
};

// The generated member exists, is a compile-time constant, and carries the
// controller's service id and the declared method name.
static_assert(std::is_same_v<decltype(macro_controller::crpc_rpc_meta_ping),
                             const method_meta>,
              "the macro must generate a constexpr method_meta member");
static_assert(macro_controller::crpc_rpc_meta_ping.service_id ==
                  method_id_from<macro_controller>(),
              "the macro must carry the controller's service id");
static_assert(std::string_view(macro_controller::crpc_rpc_meta_ping.name) ==
                  std::string_view("macro.ping"),
              "the macro must carry the declared method name");
static_assert(macro_controller_two::crpc_rpc_meta_ping.service_id ==
                  macro_controller_two::crpc_rpc_meta_pong.service_id,
              "methods of one controller share its service id");
static_assert(std::string_view(macro_controller_two::crpc_rpc_meta_pong.name) ==
                  std::string_view("macro.pong"),
              "the macro must carry the declared method name");
static_assert(macro_controller::crpc_rpc_meta_ping.service_id !=
                  macro_controller_two::crpc_rpc_meta_ping.service_id,
              "distinct controllers must have distinct service ids");

} // namespace

// --- Runtime checks ----------------------------------------------------------

CRPC_TEST(method_id_is_deterministic_at_runtime) {
    CRPC_CHECK_EQ(method_id_from<controller_alpha>(), method_id_from<controller_alpha>());
    CRPC_CHECK_EQ(method_id_from<controller_beta>(), method_id_from<controller_beta>());
}

CRPC_TEST(method_id_differs_for_different_types) {
    CRPC_CHECK(method_id_from<controller_alpha>() != method_id_from<controller_beta>());
    CRPC_CHECK(method_id_from<controller_beta>() != method_id_from<controller_gamma>());
    CRPC_CHECK(method_id_from<controller_alpha>() != method_id_from<controller_gamma>());
}

CRPC_TEST(method_id_is_the_same_constant_expression_and_runtime_value) {
    constexpr u64 at_compile_time = method_id_from<controller_alpha>();
    const u64 at_runtime = method_id_from<controller_alpha>();
    CRPC_CHECK_EQ(at_compile_time, at_runtime);
    CRPC_CHECK_EQ(at_compile_time, static_cast<u64>(method_id_from<controller_alpha>()));
}

CRPC_TEST(method_id_is_stable_across_point_of_use) {
    // Same function template, so the hashed signature is identical whether it is
    // spelled from a client or from inside the (incomplete) controller body.
    CRPC_CHECK_EQ(method_id_from<declaring_controller>(),
                  declaring_controller::id_inside_class());
    CRPC_CHECK_EQ(method_id_from<declaring_controller>(), declaring_controller::meta.service_id);
}

CRPC_TEST(fnv1a_matches_published_vectors) {
    CRPC_CHECK_EQ(method_meta_detail::fnv1a(""), 0xcbf29ce484222325ull);
    CRPC_CHECK_EQ(method_meta_detail::fnv1a("a"), 0xaf63dc4c8601ec8cull);
    CRPC_CHECK_EQ(method_meta_detail::fnv1a("foobar"), 0x85944171f73967e8ull);
}

CRPC_TEST(fnv1a_is_deterministic_and_order_sensitive) {
    CRPC_CHECK_EQ(method_meta_detail::fnv1a("crpc::A"), method_meta_detail::fnv1a("crpc::A"));
    CRPC_CHECK(method_meta_detail::fnv1a("crpc::A") != method_meta_detail::fnv1a("crpc::B"));
    CRPC_CHECK(method_meta_detail::fnv1a("ab") != method_meta_detail::fnv1a("ba"));
}

CRPC_TEST(method_id_no_collision_over_a_spread_of_types) {
    CRPC_CHECK(all_ids_distinct(std::make_index_sequence<64>{}));
}

CRPC_TEST(method_meta_carries_service_id_and_name) {
    constexpr method_meta meta{method_id_from<controller_alpha>(), "alpha.ping"};
    CRPC_CHECK_EQ(meta.service_id, method_id_from<controller_alpha>());
    CRPC_CHECK(meta.name != nullptr);
    CRPC_CHECK_EQ(std::string(meta.name), std::string("alpha.ping"));
}

CRPC_TEST(method_meta_is_a_standard_layout_aggregate) {
    static_assert(std::is_standard_layout_v<method_meta>, "method_meta must be standard layout");
    static_assert(std::is_trivially_copyable_v<method_meta>, "method_meta must be trivially copyable");
    CRPC_CHECK(true);
}

CRPC_TEST(macro_generates_a_method_meta_with_service_id_and_name) {
    const method_meta& ping = macro_controller::crpc_rpc_meta_ping;
    CRPC_CHECK_EQ(ping.service_id, method_id_from<macro_controller>());
    CRPC_CHECK(ping.name != nullptr);
    CRPC_CHECK_EQ(std::string(ping.name), std::string("macro.ping"));
}

CRPC_TEST(macro_supports_multiple_methods_in_one_controller) {
    const method_meta& ping = macro_controller_two::crpc_rpc_meta_ping;
    const method_meta& pong = macro_controller_two::crpc_rpc_meta_pong;
    CRPC_CHECK_EQ(ping.service_id, method_id_from<macro_controller_two>());
    CRPC_CHECK_EQ(pong.service_id, method_id_from<macro_controller_two>());
    CRPC_CHECK_EQ(std::string(ping.name), std::string("macro.ping"));
    CRPC_CHECK_EQ(std::string(pong.name), std::string("macro.pong"));
    CRPC_CHECK(ping.service_id != method_id_from<macro_controller>());
}

CRPC_TEST(macro_meta_is_usable_as_a_constant_expression) {
    constexpr u64 id = macro_controller::crpc_rpc_meta_ping.service_id;
    CRPC_CHECK_EQ(id, method_id_from<macro_controller>());
}

CRPC_TEST_MAIN()
