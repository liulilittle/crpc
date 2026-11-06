// CRPC -- domain layer: entities.
//
// An entity is a thing with a lifecycle and a stable identity. Two entities are
// the same entity exactly when their identifiers are equal; every other field
// may differ and the entity is still the same one (`D1`). This is why equality
// lives in the base class and never looks at `Derived`:
//
//   * a repository that reloads a row returns a *different object* holding the
//     same identity, and `==` must report "same entity" or every in-memory
//     lookup, set membership test and change tracker breaks;
//   * comparing by fields would make a rename look like a delete plus an
//     insert, which is precisely the bug identity exists to prevent.
//
// The CRTP parameter `Derived` exists so the base can be told apart in
// diagnostics and so derived types can add typed helpers; equality itself is
// non-virtual and compares only the id.
//
// Contract for `Id`:
//   * copy-constructible and copy-assignable;
//   * `operator==` decides identity and must not throw in practice (a throwing
//     comparison would terminate inside the noexcept-correct base helpers of
//     callers that rely on them).
//
// An entity is *not* a polymorphic base: it has no virtual functions and no
// virtual destructor, so it must never be deleted through an `entity*`.
#ifndef CRPC_DOMAIN_ENTITY_HPP
#define CRPC_DOMAIN_ENTITY_HPP

#include <utility>

namespace crpc {
namespace domain {

// `Derived` is the concrete entity type; it is not used by equality directly,
// but it keeps distinct entity hierarchies distinct in the type system.
template <typename Id, typename Derived>
class entity {
public:
    using id_type = Id;

    // The identity. Never empty by construction: an entity is created with
    // its id.
    [[nodiscard]] const Id& id() const noexcept { return id_; }

    // Identity equality. Fields are deliberately not consulted (`D1`).
    [[nodiscard]] bool operator==(const entity& other) const
        noexcept(noexcept(std::declval<const Id&>() == std::declval<const Id&>())) {
        return id_ == other.id_;
    }

    [[nodiscard]] bool operator!=(const entity& other) const
        noexcept(noexcept(std::declval<const Id&>() == std::declval<const Id&>())) {
        return !(*this == other);
    }

protected:
    explicit entity(Id id) : id_(std::move(id)) {}

    entity(const entity&) = default;
    entity& operator=(const entity&) = default;
    entity(entity&&) = default;
    entity& operator=(entity&&) = default;
    ~entity() = default;

private:
    Id id_;
};

} // namespace domain
} // namespace crpc

#endif // CRPC_DOMAIN_ENTITY_HPP
