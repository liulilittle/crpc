// CRPC -- domain layer: repositories.
//
// A repository is the only way the application reaches persistence for one
// aggregate type. It speaks the domain's language (`find`, `save`, `remove`)
// and never exposes SQL, cursors or rows; the mapping between an aggregate and
// a table belongs to the implementation.
//
// Two levels exist here:
//
//   `repository_base` is the non-template half a unit of work can hold. A unit
//   of work must be able to tell every participant to begin, flush, commit or
//   roll back without knowing `T` or `Id`, so those four operations live in a
//   type-erased base. The defaults make a repository that has no staging
//   concept work unchanged: `begin`/`commit` are no-ops, `flush` succeeds, and
//   `rollback` does nothing.
//
//   `repository<T, Id>` adds the typed CRUD surface. It deliberately returns
//   `status` rather than `result<void>`: this library has no `result<void>`
//   (see error.hpp), and a `status` is the same value with fewer surprises.
//
// Failure convention: an absent row is NOT an error. `find` returns
// `result<std::optional<T>>` whose success value is empty when the row does not
// exist, so "not there" and "the store is down" cannot be confused (an error
// result). `remove` of an absent row is an idempotent no-op.
//
// Thread contract: no repository in this header is thread-safe. A unit of work
// and its repositories are meant to be used from one logical task at a time;
// an implementation that supports concurrent readers documents its own
// contract.
#ifndef CRPC_DOMAIN_REPOSITORY_HPP
#define CRPC_DOMAIN_REPOSITORY_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <crpc/error.hpp>

namespace crpc {
namespace domain {

// The type-erased participant a unit of work drives.
//
// A repository written against a real database implements `begin`/`flush`/
// `commit`/`rollback` in terms of the backend's transaction; an in-memory
// repository keeps a snapshot. The defaults below describe a repository whose
// writes are immediately durable, which is a valid and common shape.
class repository_base {
public:
    virtual ~repository_base() = default;

    repository_base(const repository_base&) = delete;
    repository_base& operator=(const repository_base&) = delete;

    // Opens the staging window. Called once by `unit_of_work::enlist`.
    virtual void begin() {}

    // Writes staged changes to the store. Must be safe to call with nothing
    // staged. Called in enlistment order by `unit_of_work::commit`.
    [[nodiscard]] virtual status flush() { return status{}; }

    // Finalises the transaction: staged state becomes durable and any
    // rollback journal may be dropped. Called only after every participant
    // flushed successfully.
    virtual void commit() noexcept {}

    // Discards staged changes and restores the state observed at `begin()`.
    // Called after a failed commit and on unit-of-work destruction. Must be
    // idempotent.
    virtual void rollback() noexcept {}

    // True when the repository holds staged work that `flush` would write.
    // The unit of work does not use this to skip a flush (a repository may
    // need to flush state it tracks internally); it is for diagnostics.
    [[nodiscard]] virtual bool has_pending() const noexcept { return false; }

protected:
    repository_base() = default;
};

// Typed CRUD surface for one aggregate type.
//
// `T` may be an aggregate root or a plain record; the repository does not care
// about the distinction. `Id` is the key type, normally `T::id_type`.
template <typename T, typename Id>
class repository : public repository_base {
public:
    using value_type = T;
    using id_type = Id;

    // The row, or an empty optional when it does not exist.
    [[nodiscard]] virtual result<std::optional<T>> find(const Id& id) = 0;

    // Insert or update, keyed by the identifier the repository extracts.
    [[nodiscard]] virtual status save(const T& value) = 0;

    // Delete by key. Removing an absent row succeeds.
    [[nodiscard]] virtual status remove(const Id& id) = 0;

    // One page, ordered by the implementation (id order for the in-memory
    // one). `limit == 0` returns an empty page; `offset` past the end is an
    // empty page too, not an error.
    [[nodiscard]] virtual result<std::vector<T>> find_all(std::size_t limit = 100,
                                                          std::size_t offset = 0) = 0;

protected:
    repository() = default;
};

// A repository that keeps one ordered map in memory. It is the reference
// implementation used by the tests and by prototypes; it is not a cache and
// does not expire anything.
//
// Transaction model:
//   * `begin()` snapshots the visible state.
//   * `save`/`remove` mutate the visible state, so reads inside the same unit
//     of work see the staged changes (read-your-writes).
//   * `flush()` keeps the snapshot: a later participant can still fail, and
//     the rollback must be able to undo a flush that already "happened". Only
//     `commit()` drops the snapshot. This is the in-memory stand-in for a
//     database transaction, where a rolled-back write is undone by the store.
//   * `rollback()` restores the snapshot and is idempotent.
//
// `IdOf` extracts the identifier from a value. It defaults to `T::id()`; pass
// a functor for a type that has no such member.
template <typename T, typename Id, typename IdOf = void>
class in_memory_repository final : public repository<T, Id> {
private:
    // `IdOf = void` resolves to "use T::id()". The trait is spelled out rather
    // than specialised on void so the member is always a real object.
    struct id_from_member {
        [[nodiscard]] Id operator()(const T& value) const { return value.id(); }
    };
    using id_of_type = std::conditional_t<std::is_void<IdOf>::value, id_from_member, IdOf>;

public:
    using value_type = T;
    using id_type = Id;

    in_memory_repository() = default;

    explicit in_memory_repository(id_of_type id_of) : id_of_(std::move(id_of)) {}

    [[nodiscard]] result<std::optional<T>> find(const Id& id) override {
        const auto it = values_.find(id);
        if (it == values_.end()) {
            return std::optional<T>{};
        }
        return std::optional<T>(it->second);
    }

    [[nodiscard]] status save(const T& value) override {
        values_[id_of_(value)] = value;
        dirty_ = true;
        return status{};
    }

    [[nodiscard]] status remove(const Id& id) override {
        const auto it = values_.find(id);
        if (it == values_.end()) {
            return status{};
        }
        values_.erase(it);
        dirty_ = true;
        return status{};
    }

    [[nodiscard]] result<std::vector<T>> find_all(std::size_t limit = 100,
                                                  std::size_t offset = 0) override {
        std::vector<T> page;
        if (limit == 0) {
            return page;
        }
        std::size_t index = 0;
        for (const auto& entry : values_) {
            if (index++ < offset) {
                continue;
            }
            page.push_back(entry.second);
            if (page.size() == limit) {
                break;
            }
        }
        return page;
    }

    void begin() override {
        snapshot_ = values_;
        snapshot_valid_ = true;
        dirty_ = false;
    }

    [[nodiscard]] status flush() override {
        // The snapshot is kept on purpose: see the class comment.
        dirty_ = false;
        return status{};
    }

    void commit() noexcept override {
        snapshot_ = {};
        snapshot_valid_ = false;
        dirty_ = false;
    }

    void rollback() noexcept override {
        if (snapshot_valid_) {
            values_ = std::move(snapshot_);
            snapshot_valid_ = false;
        }
        dirty_ = false;
    }

    [[nodiscard]] bool has_pending() const noexcept override { return dirty_; }

    // --- Diagnostics (not part of the repository contract) ------------------

    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }
    [[nodiscard]] bool in_transaction() const noexcept { return snapshot_valid_; }

    // Drops every row and any transaction state. For tests and teardown only.
    void clear() noexcept {
        values_.clear();
        snapshot_.clear();
        snapshot_valid_ = false;
        dirty_ = false;
    }

private:
    id_of_type id_of_{};
    std::map<Id, T> values_;
    std::map<Id, T> snapshot_;
    bool snapshot_valid_ = false;
    bool dirty_ = false;
};

} // namespace domain
} // namespace crpc

#endif // CRPC_DOMAIN_REPOSITORY_HPP
