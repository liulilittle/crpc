// CRPC -- domain layer: value objects.
//
// A value object has no identity: it is defined entirely by its values, so two
// value objects are interchangeable when every attribute is equal (`D2`). The
// classic examples are money (amount + currency), a date range, an address or
// a colour -- replacing one with an equal instance must be invisible to the
// model.
//
// The base class cannot know the attributes, so it delegates both operations to
// the derived type, which must provide:
//
//   bool equals(const Derived&) const;    // full attribute equality
//   std::size_t hash() const;             // consistent with equals
//
// The hash/equality agreement is a hard requirement, not a suggestion: an
// unordered container with a hash that disagrees with equality loses entries.
// `value_object_hash<Derived>` is supplied so such containers need no hand
// written functor, and calling `hash_value()` keeps the two in sync because
// both go through the same `hash()`.
//
// Value objects are expected to be immutable: there is no setter contract here,
// and a mutation would change the hash of an object already stored in a
// container.
#ifndef CRPC_DOMAIN_VALUE_OBJECT_HPP
#define CRPC_DOMAIN_VALUE_OBJECT_HPP

#include <cstddef>

namespace crpc {
namespace domain {

// CRTP base. `Derived` provides `equals()` and `hash()`.
template <typename Derived>
class value_object {
public:
    // Value equality. The comparison is a member of the derived type, so it
    // sees every attribute.
    [[nodiscard]] bool operator==(const Derived& other) const {
        return self().equals(other);
    }

    [[nodiscard]] bool operator!=(const Derived& other) const {
        return !self().equals(other);
    }

    // The hash used by `value_object_hash<Derived>` and by callers that need
    // the value to key a container.
    [[nodiscard]] std::size_t hash_value() const { return self().hash(); }

protected:
    value_object() = default;
    value_object(const value_object&) = default;
    value_object& operator=(const value_object&) = default;
    value_object(value_object&&) = default;
    value_object& operator=(value_object&&) = default;
    ~value_object() = default;

private:
    // Safe under the CRTP contract; a static_assert in the constructor path
    // would still need a complete `Derived`, which the base cannot have here.
    [[nodiscard]] const Derived& self() const noexcept {
        return static_cast<const Derived&>(*this);
    }
};

// Adapter for std::unordered_set / std::unordered_map keyed by a value object.
// It reuses `hash_value()` so the container can never hash differently from
// what `operator==` compares.
template <typename Derived>
struct value_object_hash {
    [[nodiscard]] std::size_t operator()(const Derived& value) const {
        return value.hash_value();
    }
};

} // namespace domain
} // namespace crpc

#endif // CRPC_DOMAIN_VALUE_OBJECT_HPP
