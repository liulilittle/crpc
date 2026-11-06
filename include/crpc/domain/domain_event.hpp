// CRPC -- domain layer: the domain event.
//
// A `domain_event` is the immutable record of something that happened in the
// domain, already serialised. The domain layer deliberately does not know the
// payload format: `payload` is the encoded event body produced by whichever
// codec the application uses, so the aggregate, the repository and the
// projection stay free of serialisation dependencies.
//
// `sequence` is the aggregate's own version stamp, not a wall-clock ordering:
// it is assigned by `aggregate_root::raise` (see aggregate.hpp), it is strictly
// increasing within one aggregate instance, and it never repeats for that
// aggregate. That is what makes an event stream replay-safe: a consumer that
// has already applied sequence N can reject a redelivered N instead of
// applying it twice.
//
// `occurred_at` is a wall-clock timestamp for observation and diagnostics. It
// is never used for ordering (two hosts do not share a clock).
#ifndef CRPC_DOMAIN_DOMAIN_EVENT_HPP
#define CRPC_DOMAIN_DOMAIN_EVENT_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace crpc {
namespace domain {

// One immutable domain fact.
struct domain_event {
    // The event's name, e.g. "order_canceled". Used for routing and filtering.
    std::string name;
    // The serialised payload. Empty is legal (an event with no data).
    std::string payload;
    // Monotonic version within the raising aggregate; 0 means "not stamped".
    std::uint64_t sequence = 0;
    // When the fact happened, for observation only. Never an ordering key.
    std::chrono::system_clock::time_point occurred_at{};

    domain_event() = default;

    // Builds an event and stamps it with the current time. The sequence is
    // left at 0: only the aggregate that raises the event owns sequencing.
    domain_event(std::string event_name, std::string event_payload = {})
        : name(std::move(event_name)),
          payload(std::move(event_payload)),
          occurred_at(std::chrono::system_clock::now()) {}

    // An event without a name carries no fact and must not be dispatched.
    [[nodiscard]] bool empty() const noexcept { return name.empty(); }
    explicit operator bool() const noexcept { return !name.empty(); }

    // Full structural comparison; `occurred_at` included, so two events are
    // equal only when they are the same concrete record.
    [[nodiscard]] bool operator==(const domain_event& other) const {
        return name == other.name && payload == other.payload &&
               sequence == other.sequence && occurred_at == other.occurred_at;
    }

    [[nodiscard]] bool operator!=(const domain_event& other) const { return !(*this == other); }
};

// Shorthand for the two-field construction used by aggregate methods.
[[nodiscard]] inline domain_event make_domain_event(std::string name, std::string payload = {}) {
    return domain_event(std::move(name), std::move(payload));
}

} // namespace domain
} // namespace crpc

#endif // CRPC_DOMAIN_DOMAIN_EVENT_HPP
