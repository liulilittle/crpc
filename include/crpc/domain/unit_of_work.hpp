// CRPC -- domain layer: the unit of work.
//
// A unit of work is the transaction boundary of the domain model. Application
// code enlists the repositories it is going to use, mutates aggregates, and
// calls `commit()` once. The outcome is all-or-nothing (`D4`-`D8`):
//
//   * every participant is flushed in enlistment order; if all succeed, every
//     participant is committed;
//   * the first failing flush aborts the sequence, and EVERY participant --
//     including the ones not reached yet -- is rolled back in reverse
//     enlistment order. Rolling back a participant that was never flushed is
//     deliberate: a repository's `begin()` may already have staged work (a
//     local cache, an open cursor), and leaving it staged would leak state
//     into whatever happens next on that repository;
//   * the commit is one-shot. After a success or a failure the unit of work is
//     inactive, and a second `commit()` reports `failed_precondition` instead
//     of running the transaction again. This is what stops a retry loop from
//     double-applying a transaction it cannot tell apart from a crash;
//   * the destructor rolls back an active unit of work, so an exception or an
//     early return cannot leave staged writes behind. `rollback()` itself is
//     idempotent and safe to call after any outcome.
//
// The unit of work does not own the repositories. They must outlive it, which
// is why `enlist` takes a reference and the class is non-copyable and
// non-movable.
//
// Thread contract: a unit of work belongs to one logical task. It is not
// internally synchronised; concurrent `commit()` and `enlist()` are a data
// race.
#ifndef CRPC_DOMAIN_UNIT_OF_WORK_HPP
#define CRPC_DOMAIN_UNIT_OF_WORK_HPP

#include <cstddef>
#include <vector>

#include <crpc/domain/repository.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace domain {

class unit_of_work {
public:
    unit_of_work() = default;

    unit_of_work(const unit_of_work&) = delete;
    unit_of_work& operator=(const unit_of_work&) = delete;
    unit_of_work(unit_of_work&&) = delete;
    unit_of_work& operator=(unit_of_work&&) = delete;

    // Rolls back whatever is still staged. Never throws.
    ~unit_of_work() { rollback(); }

    // Registers a participant and opens its staging window immediately, so
    // writes made after `enlist` are covered by the eventual rollback.
    //
    // Enlisting the same repository twice is a no-op: a transaction must not
    // flush one participant twice. Enlisting after the unit of work became
    // inactive is ignored, because there is no transaction left to join.
    void enlist(repository_base& repo) {
        if (!active_) {
            return;
        }
        for (const repository_base* existing : participants_) {
            if (existing == &repo) {
                return;
            }
        }
        participants_.push_back(&repo);
        repo.begin();
    }

    // Flushes every participant, then commits them. On the first flush failure
    // every participant is rolled back in reverse order and that error is
    // returned unchanged, so the caller sees the real cause and not a generic
    // "transaction failed".
    [[nodiscard]] status commit() {
        if (!active_) {
            return status(rpc_code::failed_precondition,
                          "unit of work is no longer active; commit may run only once");
        }

        for (repository_base* repo : participants_) {
            const status flushed = repo->flush();
            if (!flushed.ok()) {
                // The failing participant may already have partially written,
                // and the later ones were never reached; rolling back all of
                // them in reverse order is what makes the failure atomic.
                rollback_participants();
                discard();
                return flushed;
            }
        }

        for (repository_base* repo : participants_) {
            repo->commit();
        }
        discard();
        return status{};
    }

    // Discards staged work. Idempotent; a no-op when the unit of work is
    // already inactive.
    void rollback() noexcept {
        if (!active_) {
            return;
        }
        rollback_participants();
        discard();
    }

    // True between construction/enlist and the terminal commit/rollback.
    [[nodiscard]] bool is_active() const noexcept { return active_; }

    // How many distinct repositories are enlisted. Diagnostics only.
    [[nodiscard]] std::size_t participant_count() const noexcept {
        return participants_.size();
    }

private:
    // Reverse order: a participant enlisted later may depend on an earlier
    // one (a child table on its parent row), so it must be undone first.
    void rollback_participants() noexcept {
        for (auto it = participants_.rbegin(); it != participants_.rend(); ++it) {
            (*it)->rollback();
        }
    }

    void discard() noexcept {
        participants_.clear();
        active_ = false;
    }

    std::vector<repository_base*> participants_;
    bool active_ = true;
};

} // namespace domain
} // namespace crpc

#endif // CRPC_DOMAIN_UNIT_OF_WORK_HPP
