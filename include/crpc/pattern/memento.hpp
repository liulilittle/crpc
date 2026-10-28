// CRPC -- memento pattern: deep snapshots and RAII transactional scope.
//
// `snapshot<T>` owns a private deep copy of a value; copying the snapshot
// source via `memento_traits<T>::copy` is what guarantees the snapshot never
// aliases the target's storage. For a value type (int, std::string,
// std::vector, ...) the copy constructor already deep-copies, so the default
// trait is correct. For types whose copy constructor shares state
// (std::shared_ptr members, intrusive structures), specialise `memento_traits`
// for that type and return an independent clone; the scope relies on that same
// trait, so a shallow-copying type would make rollback a no-op, which the
// pattern must not silently do.
//
// `scoped_memento<T>` guards one object for a block:
//   * normal exit commits the changes -- the guard's destructor does nothing;
//   * an exception that unwinds the block restores the pre-scope state;
//   * `rollback()` restores immediately and latches (a later unwind preserves
//     the restored state);
//   * `commit()` latches the changes so the block may be exited by exception
//     without restoring.
//
// The library itself never throws; the exception path exists for application
// code that does. `restore()` must not throw: T's copy assignment must be
// noexcept for the restored types (true for the standard value types).
#ifndef CRPC_PATTERN_MEMENTO_HPP
#define CRPC_PATTERN_MEMENTO_HPP

#include <exception>
#include <utility>

namespace crpc {
namespace pattern {

// Customisation point for deep copying. The default copy-constructs, which is
// the correct deep copy for value types. Specialise for types whose copy
// constructor shares ownership.
template <typename T>
struct memento_traits {
    static T copy(const T& value) { return T(value); }
};

// An owned, deeply-copied snapshot of a value. Independent of the source: it is
// only ever written by the copy performed at construction.
template <typename T>
class snapshot {
public:
    snapshot() = default;

    explicit snapshot(const T& value) : value_(memento_traits<T>::copy(value)) {}

    const T& value() const noexcept { return value_; }
    T& value() noexcept { return value_; }

    // Moves the contained value out; leaves the snapshot consuming `value()`.
    T release() { return std::move(value_); }

    snapshot(const snapshot&) = default;
    snapshot& operator=(const snapshot&) = default;

private:
    T value_;
};

template <typename T>
snapshot<T> make_snapshot(const T& value) {
    return snapshot<T>(value);
}

// RAII transactional scope over one object. See the file comment for the
// commit / rollback / unwinding contract.
template <typename T>
class scoped_memento {
public:
    explicit scoped_memento(T& target)
        : target_(target),
          before_(memento_traits<T>::copy(target)),
          uncaught_(std::uncaught_exceptions()) {}

    scoped_memento(const scoped_memento&) = delete;
    scoped_memento& operator=(const scoped_memento&) = delete;

    ~scoped_memento() noexcept {
        if (!done_ && std::uncaught_exceptions() > uncaught_) {
            restore();
        }
    }

    // Persist the scope's changes; the destructor will not restore them even
    // if the block is later unwound by an exception.
    void commit() noexcept { done_ = true; }

    // Restore immediately. Idempotent and latched: a later commit()/unwinding
    // is a no-op.
    void rollback() noexcept {
        if (!done_) {
            restore();
            done_ = true;
        }
    }

    // The pre-scope state; nowhere aliased with `target()`.
    const T& before() const noexcept { return before_; }

    bool done() const noexcept { return done_; }

private:
    void restore() noexcept { target_ = before_; }

    T& target_;
    T before_;
    int uncaught_;
    bool done_ = false;
};

} // namespace pattern
} // namespace crpc

#endif // CRPC_PATTERN_MEMENTO_HPP