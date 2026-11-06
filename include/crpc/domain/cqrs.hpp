// CRPC -- domain layer: CQRS (commands, queries, handler registry, projections).
//
// Commands change state and return a result; queries read state and return a
// result. Both are plain value types: a message carries data, never behaviour,
// which is what lets the same command be dispatched, logged, queued or replayed
// without dragging the handler along.
//
//   template <typename T> struct add_line : command<int> { int quantity; };
//   registry.register_command<add_line>([](const add_line& c) -> int { ... });
//   result<int> r = registry.dispatch(add_line{2});
//
// The registry is a start-up-time table: handlers are registered once and the
// dispatch path is a hash lookup plus one indirect call. Type erasure is done
// with a per-message-type `std::function` whose address is keyed by a
// RTTI-free type token (a function-local static), so the header compiles and
// works under `/GR-` and `-fno-rtti`. Consequence worth knowing: the registry
// and its dispatch must live in the same module; two DLLs each get their own
// copy of the token.
//
// Handler shapes accepted by a command/query of result type `R`:
//   * `status`                    when `R` is `void`; the message result is the
//                                 status (ok, or the error to propagate);
//   * `result<R>`                 used as-is, so an error reaches the caller
//                                 with its code and message unchanged;
//   * `R`                         wrapped as a success;
//   * anything `result<R>` is constructible from (e.g. `rpc_code`,
//                                 `rpc_error`) for the error-oriented styles.
// A handler that returns `void` is accepted for a `void` command and counts as
// success. Handlers must not throw: an escaping exception would cross the
// dispatch boundary, which this library models as a value instead.
//
// An unregistered message type is not an internal error: `dispatch` returns
// `rpc_code::not_found` with the message type only named in the diagnostic
// text, so a caller that dispatches the wrong type sees a definite answer and
// not a silent success (`D8`).
//
// Registering the same message type again replaces the previous handler (last
// registration wins). This is the startup idiom "default handler, then the
// configured one" and keeps registration unconditional.
//
// Projections are the read side: a projection consumes domain events and
// updates a denormalised view. `apply_events` is the batch helper, and
// `replay_guard` rejects a redelivered or out-of-order sequence for a named
// stream, which is what makes at-least-once delivery safe (see D3: sequences
// are per aggregate and strictly increasing).
#ifndef CRPC_DOMAIN_CQRS_HPP
#define CRPC_DOMAIN_CQRS_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <crpc/domain/domain_event.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace domain {

// --- Messages ---------------------------------------------------------------

// A state-changing message. `T` is the value a successful dispatch returns;
// `void` is allowed and makes the dispatch return a `status`.
template <typename T>
class command {
public:
    using result_type = T;
};

// A read-only message. Same result contract as `command`.
template <typename T>
class query {
public:
    using result_type = T;
};

// The value `dispatch` returns for a message whose result type is `T`.
// `result<void>` does not exist in this library (error.hpp), so a void message
// yields a `status` instead.
template <typename T>
struct message_outcome {
    using type = result<T>;
};

template <>
struct message_outcome<void> {
    using type = status;
};

template <typename T>
using message_outcome_t = typename message_outcome<T>::type;

// The outcome of dispatching `Message`, resolved through its `result_type`.
template <typename Message>
using message_outcome_for_t = message_outcome_t<typename Message::result_type>;

namespace detail {

// RTTI-free type identity. One function-local static per type, merged by the
// linker within a module; the address is stable and unique per type.
template <typename T>
[[nodiscard]] inline const void* type_token() noexcept {
    static const char token = 0;
    return &token;
}

template <typename T, typename = void>
struct has_result_type : std::false_type {};

template <typename T>
struct has_result_type<T, std::void_t<typename T::result_type>> : std::true_type {};

// Adapts a handler's return value to the message's declared outcome, so the
// registry stores one uniform callable per message type.
template <typename Outcome, typename Handler, typename Message>
[[nodiscard]] Outcome coerce(Handler& handler, const Message& message) {
    using raw = decltype(handler(message));
    if constexpr (std::is_same<raw, Outcome>::value) {
        return handler(message);
    } else if constexpr (std::is_void<raw>::value) {
        // Only valid for a void message, where Outcome is `status`.
        handler(message);
        return Outcome{};
    } else {
        return Outcome(handler(message));
    }
}

} // namespace detail

// --- Handler registry -------------------------------------------------------

class handler_registry {
public:
    handler_registry() = default;

    handler_registry(const handler_registry&) = delete;
    handler_registry& operator=(const handler_registry&) = delete;

    // Registers the handler for `Command`. The callable is moved into
    // shared storage and copied into the per-type function once, so a
    // move-only handler (one capturing a unique_ptr) is accepted.
    template <typename Command, typename Handler>
    void register_command(Handler&& handler) {
        static_assert(detail::has_result_type<Command>::value,
                      "register_command: the command must declare using result_type = T; "
                      "derive it from crpc::domain::command<T>");
        commands_[detail::type_token<Command>()] =
            make_callable<Command>(std::forward<Handler>(handler));
    }

    template <typename Query, typename Handler>
    void register_query(Handler&& handler) {
        static_assert(detail::has_result_type<Query>::value,
                      "register_query: the query must declare using result_type = T; "
                      "derive it from crpc::domain::query<T>");
        queries_[detail::type_token<Query>()] =
            make_callable<Query>(std::forward<Handler>(handler));
    }

    // Runs the registered command handler, or reports `not_found`.
    template <typename Command>
    [[nodiscard]] message_outcome_for_t<Command> dispatch(const Command& message) const {
        using outcome = message_outcome_for_t<Command>;
        const auto it = commands_.find(detail::type_token<Command>());
        if (it == commands_.end()) {
            return missing<outcome>("command", "no command handler is registered for this type");
        }
        auto* callable = static_cast<callable_for<Command>*>(it->second.get());
        return (*callable)(message);
    }

    // Runs the registered query handler, or reports `not_found`.
    template <typename Query>
    [[nodiscard]] message_outcome_for_t<Query> ask(const Query& message) const {
        using outcome = message_outcome_for_t<Query>;
        const auto it = queries_.find(detail::type_token<Query>());
        if (it == queries_.end()) {
            return missing<outcome>("query", "no query handler is registered for this type");
        }
        auto* callable = static_cast<callable_for<Query>*>(it->second.get());
        return (*callable)(message);
    }

    // Ergonomic alias used by callers that treat every message uniformly: it
    // checks the command table first, then the query table, so a front door can
    // accept both without knowing which kind a message is. A type registered as
    // both is dispatched as a command (the primary table).
    template <typename Message>
    [[nodiscard]] message_outcome_for_t<Message> dispatch_any(const Message& message) const {
        if (contains_command<Message>()) {
            return dispatch(message);
        }
        return ask(message);
    }

    template <typename Command>
    [[nodiscard]] bool contains_command() const noexcept {
        return commands_.find(detail::type_token<Command>()) != commands_.end();
    }

    template <typename Query>
    [[nodiscard]] bool contains_query() const noexcept {
        return queries_.find(detail::type_token<Query>()) != queries_.end();
    }

    [[nodiscard]] std::size_t command_count() const noexcept { return commands_.size(); }
    [[nodiscard]] std::size_t query_count() const noexcept { return queries_.size(); }

    void clear() noexcept {
        commands_.clear();
        queries_.clear();
    }

private:
    template <typename Message>
    using callable_for = std::function<message_outcome_for_t<Message>(const Message&)>;

    template <typename Message, typename Handler>
    [[nodiscard]] std::shared_ptr<void> make_callable(Handler&& handler) {
        using handler_type = std::decay_t<Handler>;
        auto owned = std::make_shared<handler_type>(std::forward<Handler>(handler));
        auto callable = std::make_shared<callable_for<Message>>(
            [owned](const Message& message) -> message_outcome_for_t<Message> {
                return detail::coerce<message_outcome_for_t<Message>>(*owned, message);
            });
        return callable;
    }

    // `not_found` for both shapes: `status` and `result<T>` are constructible
    // from an `rpc_error`, so one spelling serves both outcomes.
    template <typename Outcome>
    [[nodiscard]] static Outcome missing(const char* kind, const char* what) {
        return Outcome(rpc_error(rpc_code::not_found, std::string(kind) + ": " + what));
    }

    std::unordered_map<const void*, std::shared_ptr<void>> commands_;
    std::unordered_map<const void*, std::shared_ptr<void>> queries_;
};

// --- Projections (read models) ---------------------------------------------

// The read side of CQRS: one denormalised view updated from the event stream.
// A projection is a stateful consumer; `apply` must be idempotent only when the
// caller has no `replay_guard` (with a guard, duplicates never arrive).
class projection {
public:
    virtual ~projection() = default;

    projection(const projection&) = delete;
    projection& operator=(const projection&) = delete;

    virtual void apply(const domain_event& event) = 0;

protected:
    projection() = default;
};

// Applies every named event, in order, and returns how many were applied.
// An unnamed event carries no fact and is skipped rather than handed to the
// projection.
inline std::size_t apply_events(projection& target, const std::vector<domain_event>& events) {
    std::size_t applied = 0;
    for (const domain_event& event : events) {
        if (!event) {
            continue;
        }
        target.apply(event);
        ++applied;
    }
    return applied;
}

// Drops a redelivered or out-of-order event for one named stream. Streams are
// independent (each aggregate has its own sequence): the caller passes an
// aggregate key, so two aggregates that both start at sequence 1 do not
// suppress each other. An event with sequence 0 is unstamped and always
// accepted -- it cannot be ordered, and pretending otherwise would drop data.
class replay_guard {
public:
    // True when `event` may be applied to `stream`; false when its sequence is
    // not greater than the last accepted one (a duplicate or a reorder).
    [[nodiscard]] bool accept(const std::string& stream, const domain_event& event) {
        if (event.sequence == 0) {
            return true;
        }
        const auto it = last_sequence_.find(stream);
        if (it != last_sequence_.end() && event.sequence <= it->second) {
            return false;
        }
        last_sequence_[stream] = event.sequence;
        return true;
    }

    [[nodiscard]] std::uint64_t last_sequence(const std::string& stream) const noexcept {
        const auto it = last_sequence_.find(stream);
        return it == last_sequence_.end() ? 0 : it->second;
    }

    [[nodiscard]] std::size_t stream_count() const noexcept { return last_sequence_.size(); }

    void reset() noexcept { last_sequence_.clear(); }

private:
    std::unordered_map<std::string, std::uint64_t> last_sequence_;
};

} // namespace domain
} // namespace crpc

#endif // CRPC_DOMAIN_CQRS_HPP
