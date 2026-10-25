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
#include <type_traits>
#include <utility>

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

CRPC_TEST_MAIN()
