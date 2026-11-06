// CRPC -- domain layer: aggregate roots and their uncommitted events.
//
// An aggregate is the consistency boundary: one cluster of entities and value
// objects that must change together. Outside code may only hold the root, and
// every invariant is enforced inside the aggregate's own methods.
//
// Events are the aggregate's outbox. Three rules make it safe (`D3`):
//
//   1. ACCUMULATE. `raise()` appends. Nothing ever replaces the buffer with a
//      snapshot of "the latest event", because a repository persisting only
//      the last event would silently lose all but one change. `D1`-style bugs
//      here are the reason the test suite raises several times in a row.
//   2. CLEAR EXPLICITLY. Only `clear_events()` drains the buffer, and the
//      caller does it after the events are durably published. `uncommitted_events()`
//      hands out a const reference, so a caller cannot quietly truncate it.
//   3. MONOTONIC SEQUENCE. Each raised event is stamped with `++version_`
//      starting at 1, so the stream is strictly increasing within one
//      aggregate. Replay (`observe_persisted`) advances the version without
//      queueing, so the next raised event continues after the replayed
//      history instead of reusing a sequence a consumer has already applied.
//
// Events are moved out of the outbox by `clear_events()`; a subclass that wants
// to read them first calls `uncommitted_events()` before clearing.
#ifndef CRPC_DOMAIN_AGGREGATE_HPP
#define CRPC_DOMAIN_AGGREGATE_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <crpc/domain/domain_event.hpp>
#include <crpc/domain/entity.hpp>

namespace crpc {
namespace domain {

template <typename Id, typename Derived>
class aggregate_root : public entity<Id, Derived> {
public:
    using id_type = Id;

    // The currently queued, not yet acknowledged events, in raise order.
    // Never null, never null elements; empty after `clear_events()`.
    [[nodiscard]] const std::vector<domain_event>& uncommitted_events() const noexcept {
        return events_;
    }

    [[nodiscard]] bool has_uncommitted_events() const noexcept { return !events_.empty(); }

    // The aggregate's version: the sequence of the last raised or replayed
    // event, or 0 when the aggregate has no history. It does not decrease when
    // the outbox is cleared.
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

    // Drains the outbox. The version stays as the high-water mark, so the next
    // raised event cannot reuse a sequence number (`D3`).
    void clear_events() noexcept { events_.clear(); }

protected:
    explicit aggregate_root(Id id) : entity<Id, Derived>(std::move(id)) {}

    // Queues `event` and stamps it with the next version. A pre-set sequence is
    // overwritten: sequencing belongs to the aggregate, not the caller.
    void raise(domain_event event) {
        event.sequence = ++version_;
        events_.push_back(std::move(event));
    }

    // Convenience overload: builds the event and stamps the current time.
    void raise(std::string name, std::string payload = {}) {
        raise(domain_event(std::move(name), std::move(payload)));
    }

    // Rehydration: records that `event` is already part of this aggregate's
    // history. It advances the version past the event's sequence but does NOT
    // queue it as uncommitted work -- replayed history has already been
    // persisted and must not be published a second time.
    void observe_persisted(const domain_event& event) noexcept {
        if (event.sequence > version_) {
            version_ = event.sequence;
        }
    }

    aggregate_root(const aggregate_root&) = default;
    aggregate_root& operator=(const aggregate_root&) = default;
    aggregate_root(aggregate_root&&) = default;
    aggregate_root& operator=(aggregate_root&&) = default;
    ~aggregate_root() = default;

private:
    std::vector<domain_event> events_;
    std::uint64_t version_ = 0;
};

} // namespace domain
} // namespace crpc

#endif // CRPC_DOMAIN_AGGREGATE_HPP
