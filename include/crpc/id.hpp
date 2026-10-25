// CRPC -- strong service and method identifiers.
//
// A service id and a method id are both a `u64` on the wire, so a caller that
// swapped the two arguments compiled and sent a request to the wrong method.
// These newtypes wrap the same `u64` but are distinct types: a `service_id`
// converts implicitly from a `u64` literal (so existing call sites keep
// working) but never converts to or from a `method_id`, and the conversion back
// to `u64` is explicit. Passing the two in the wrong order is therefore a
// compile error.
//
// The wire value is unchanged; this is a source-level type distinction only.
#ifndef CRPC_ID_HPP
#define CRPC_ID_HPP

#include <cstdint>

#include <crpc/config.hpp>

namespace crpc {

// A registered service. `value == 0` is the invalid id.
struct service_id {
    u64 value = 0;

    constexpr service_id() noexcept = default;
    // Implicit from a u64 literal so `start_call(1, 2, ...)` keeps resolving to
    // the u64 overload (exact match) while a typed call spells the id.
    constexpr service_id(u64 id) noexcept : value(id) {}

    [[nodiscard]] constexpr explicit operator u64() const noexcept {
        return value;
    }
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
};

// A method within a service. `value == 0` is the invalid id.
struct method_id {
    u64 value = 0;

    constexpr method_id() noexcept = default;
    constexpr method_id(u64 id) noexcept : value(id) {}

    [[nodiscard]] constexpr explicit operator u64() const noexcept {
        return value;
    }
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
};

[[nodiscard]] constexpr bool operator==(service_id a, service_id b) noexcept {
    return a.value == b.value;
}
[[nodiscard]] constexpr bool operator!=(service_id a, service_id b) noexcept {
    return a.value != b.value;
}
[[nodiscard]] constexpr bool operator==(method_id a, method_id b) noexcept {
    return a.value == b.value;
}
[[nodiscard]] constexpr bool operator!=(method_id a, method_id b) noexcept {
    return a.value != b.value;
}

} // namespace crpc

#endif // CRPC_ID_HPP
