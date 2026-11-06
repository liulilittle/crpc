// CRPC -- domain layer: CQRS tests (commands, queries, registry, projections).
//
// Acceptance points (`D8`-`D11`):
//   D8   an unregistered command or query is `not_found`, never a silent ok;
//   D9   a handler's error value reaches the caller unchanged;
//   D10  a projection applies named events in order and ignores unnamed ones;
//   D11  the replay guard drops duplicates and reorders per stream.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#pragma comment(lib, "dbghelp.lib")
#endif
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <crpc/domain/cqrs.hpp>
#include <crpc/domain/domain_event.hpp>

using namespace crpc;
using namespace crpc::domain;

namespace {

// --- Messages ---------------------------------------------------------------
//
// A message declares `result_type` itself instead of deriving from
// `command<T>`/`query<T>`: in C++17 a class with a base class is only an
// aggregate if the initializer also names the base, so `add_line{21}` would
// have to be spelled `add_line{{}, 21}`. The tag templates remain available for
// callers who prefer inheritance (see cqrs_command_base_template_declares_result_type);
// dispatch only requires `result_type`.

struct add_line {
    using result_type = int;
    int quantity = 0;
};

struct cancel_order {
    using result_type = void;
    std::uint64_t order_id = 0;
};

struct rename_user {
    using result_type = bool;
    int user_id = 0;
    std::string name;
};

struct failing_command {
    using result_type = int;
    rpc_code code = rpc_code::internal;
};

struct order_total {
    using result_type = long;
    std::uint64_t order_id = 0;
};

struct order_count {
    using result_type = std::size_t;
};

struct missing_query {
    using result_type = std::string;
};

struct reset_everything {
    using result_type = void;
};

// A message that DOES derive from the tag base. The base is a marker with no
// data, so an explicit constructor keeps it usable without brace gymnastics.
struct refund final : command<bool> {
    explicit refund(int amount) : amount_(amount) {}
    [[nodiscard]] int amount() const noexcept { return amount_; }

private:
    int amount_;
};

// --- Test doubles -----------------------------------------------------------

// A read model that records the events it receives, in order.
class recording_projection final : public projection {
public:
    void apply(const domain_event& event) override {
        names_.push_back(event.name);
        sequences_.push_back(event.sequence);
        payloads_.push_back(event.payload);
    }

    [[nodiscard]] const std::vector<std::string>& names() const noexcept { return names_; }
    [[nodiscard]] const std::vector<std::uint64_t>& sequences() const noexcept {
        return sequences_;
    }
    [[nodiscard]] const std::vector<std::string>& payloads() const noexcept {
        return payloads_;
    }

private:
    std::vector<std::string> names_;
    std::vector<std::uint64_t> sequences_;
    std::vector<std::string> payloads_;
};

domain_event make_event(std::string name, std::uint64_t sequence) {
    domain_event event(std::move(name), "{}");
    event.sequence = sequence;
    return event;
}

} // namespace

// --- Commands ---------------------------------------------------------------

CRPC_TEST(cqrs_command_dispatch_returns_value) {
    handler_registry registry;
    int seen = 0;
    registry.register_command<add_line>([&seen](const add_line& command) {
        seen = command.quantity;
        return command.quantity * 2;
    });

    CRPC_CHECK(registry.contains_command<add_line>());
    CRPC_CHECK_EQ(1u, registry.command_count());

    const auto response = registry.dispatch(add_line{21});
    CRPC_REQUIRE(response.ok());
    CRPC_CHECK_EQ(42, response.value());
    CRPC_CHECK_EQ(21, seen);
}

CRPC_TEST(cqrs_void_command_returns_status) {
    handler_registry registry;
    std::uint64_t canceled = 0;
    registry.register_command<cancel_order>([&canceled](const cancel_order& command) {
        canceled = command.order_id;
        return status{};
    });

    const status applied = registry.dispatch(cancel_order{77});
    CRPC_CHECK(applied.ok());
    CRPC_CHECK_EQ(std::uint64_t{77}, canceled);
}

CRPC_TEST(cqrs_void_command_from_void_handler_is_success) {
    handler_registry registry;
    registry.register_command<reset_everything>([](const reset_everything&) {});
    CRPC_CHECK(registry.dispatch(reset_everything{}).ok());
}

CRPC_TEST(cqrs_handler_result_error_is_propagated) {
    handler_registry registry;
    registry.register_command<failing_command>([](const failing_command& command) -> result<int> {
        return rpc_error(command.code, "the domain refused the command");
    });

    const auto outcome = registry.dispatch(failing_command{rpc_code::failed_precondition});
    CRPC_REQUIRE(!outcome.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, outcome.code());
    CRPC_CHECK_EQ(std::string("the domain refused the command"), outcome.error().message);
}

CRPC_TEST(cqrs_handler_rpc_error_is_propagated) {
    handler_registry registry;
    registry.register_command<failing_command>([](const failing_command& command) {
        return rpc_error(command.code, "rejected by policy");
    });

    const auto outcome = registry.dispatch(failing_command{rpc_code::permission_denied});
    CRPC_REQUIRE(!outcome.ok());
    CRPC_CHECK_EQ(rpc_code::permission_denied, outcome.code());
    CRPC_CHECK_EQ(std::string("rejected by policy"), outcome.error().message);
}

CRPC_TEST(cqrs_void_command_handler_status_is_propagated) {
    handler_registry registry;
    registry.register_command<cancel_order>([](const cancel_order&) {
        return status(rpc_code::not_found, "order does not exist");
    });

    const status outcome = registry.dispatch(cancel_order{1});
    CRPC_REQUIRE(!outcome.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, outcome.code());
    CRPC_CHECK_EQ(std::string("order does not exist"), outcome.message());
}

CRPC_TEST(cqrs_re_registration_replaces_handler) {
    handler_registry registry;
    registry.register_command<add_line>([](const add_line&) { return 1; });
    registry.register_command<add_line>([](const add_line& command) {
        return command.quantity;
    });

    const auto response = registry.dispatch(add_line{9});
    CRPC_REQUIRE(response.ok());
    CRPC_CHECK_EQ(9, response.value());
    CRPC_CHECK_EQ(1u, registry.command_count());
}

CRPC_TEST(cqrs_move_only_handler_is_accepted) {
    handler_registry registry;
    auto counter = std::make_unique<int>(0);
    registry.register_command<add_line>(
        [moved = std::move(counter)](const add_line& command) {
            *moved += command.quantity;
            return *moved;
        });

    const auto first = registry.dispatch(add_line{4});
    CRPC_REQUIRE(first.ok());
    CRPC_CHECK_EQ(4, first.value());
    const auto second = registry.dispatch(add_line{5});
    CRPC_REQUIRE(second.ok());
    CRPC_CHECK_EQ(9, second.value());
}

// --- Tag templates ----------------------------------------------------------

CRPC_TEST(cqrs_command_base_template_declares_result_type) {
    static_assert(std::is_same<command<int>::result_type, int>::value,
                  "command<T> exposes T as result_type");
    static_assert(std::is_same<query<long>::result_type, long>::value,
                  "query<T> exposes T as result_type");
    static_assert(std::is_same<refund::result_type, bool>::value,
                  "a command deriving from command<T> inherits result_type");

    handler_registry registry;
    registry.register_command<refund>([](const refund& message) {
        return message.amount() > 0;
    });
    CRPC_CHECK(registry.contains_command<refund>());

    const auto accepted = registry.dispatch(refund{10});
    CRPC_REQUIRE(accepted.ok());
    CRPC_CHECK(accepted.value());

    const auto rejected = registry.dispatch(refund{-1});
    CRPC_REQUIRE(rejected.ok());
    CRPC_CHECK_FALSE(rejected.value());
}

// --- D8: unregistered types ------------------------------------------------

CRPC_TEST(cqrs_unregistered_command_is_not_found) {
    handler_registry registry;
    registry.register_command<add_line>([](const add_line& command) {
        return command.quantity;
    });

    // A different type, registered nowhere: definite not_found, not ok.
    const auto outcome = registry.dispatch(rename_user{1, "alice"});
    CRPC_REQUIRE(!outcome.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, outcome.code());
    CRPC_CHECK_EQ(1u, registry.command_count());
    CRPC_CHECK_FALSE(registry.contains_command<rename_user>());
}

CRPC_TEST(cqrs_unregistered_void_command_is_not_found) {
    handler_registry registry;
    const status outcome = registry.dispatch(cancel_order{5});
    CRPC_REQUIRE(!outcome.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, outcome.code());
    CRPC_CHECK_FALSE(outcome.message().empty());
}

CRPC_TEST(cqrs_unregistered_query_is_not_found) {
    handler_registry registry;
    registry.register_query<order_total>([](const order_total&) { return 100L; });

    const auto unknown = registry.ask(missing_query{});
    CRPC_REQUIRE(!unknown.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, unknown.code());

    const auto known = registry.ask(order_total{});
    CRPC_REQUIRE(known.ok());
    CRPC_CHECK_EQ(100L, known.value());
}

// --- Queries ----------------------------------------------------------------

CRPC_TEST(cqrs_query_dispatch_returns_value) {
    handler_registry registry;
    registry.register_query<order_total>([](const order_total& query) {
        return static_cast<long>(query.order_id * 10);
    });
    registry.register_query<order_count>([](const order_count&) {
        return std::size_t{3};
    });

    CRPC_CHECK(registry.contains_query<order_total>());
    CRPC_CHECK_EQ(2u, registry.query_count());

    const auto total = registry.ask(order_total{4});
    CRPC_REQUIRE(total.ok());
    CRPC_CHECK_EQ(40L, total.value());

    const auto count = registry.ask(order_count{});
    CRPC_REQUIRE(count.ok());
    CRPC_CHECK_EQ(3u, count.value());
}

CRPC_TEST(cqrs_query_error_is_propagated) {
    handler_registry registry;
    registry.register_query<order_total>([](const order_total&) -> result<long> {
        return rpc_error(rpc_code::not_found, "no such order");
    });

    const auto outcome = registry.ask(order_total{404});
    CRPC_REQUIRE(!outcome.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, outcome.code());
    CRPC_CHECK_EQ(std::string("no such order"), outcome.error().message);
}

CRPC_TEST(cqrs_dispatch_any_routes_both_kinds) {
    handler_registry registry;
    registry.register_command<add_line>([](const add_line& command) {
        return command.quantity;
    });
    registry.register_query<order_total>([](const order_total&) { return 7L; });

    const auto written = registry.dispatch_any(add_line{6});
    CRPC_REQUIRE(written.ok());
    CRPC_CHECK_EQ(6, written.value());

    const auto read = registry.dispatch_any(order_total{1});
    CRPC_REQUIRE(read.ok());
    CRPC_CHECK_EQ(7L, read.value());

    const auto neither = registry.dispatch_any(missing_query{});
    CRPC_CHECK_FALSE(neither.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, neither.code());
}

CRPC_TEST(cqrs_clear_removes_every_handler) {
    handler_registry registry;
    registry.register_command<add_line>([](const add_line&) { return 1; });
    registry.register_query<order_total>([](const order_total&) { return 1L; });
    CRPC_CHECK_EQ(1u, registry.command_count());
    CRPC_CHECK_EQ(1u, registry.query_count());

    registry.clear();
    CRPC_CHECK_EQ(0u, registry.command_count());
    CRPC_CHECK_EQ(0u, registry.query_count());
    CRPC_CHECK_EQ(rpc_code::not_found, registry.dispatch(add_line{1}).code());
    CRPC_CHECK_EQ(rpc_code::not_found, registry.ask(order_total{1}).code());
}

// --- D10: projections ------------------------------------------------------

CRPC_TEST(cqrs_projection_applies_events_in_order) {
    recording_projection view;
    const std::vector<domain_event> events = {
        make_event("order_created", 1),
        make_event("line_added", 2),
        make_event("order_canceled", 3),
    };

    CRPC_CHECK_EQ(3u, apply_events(view, events));
    CRPC_REQUIRE(view.names().size() == 3);
    CRPC_CHECK_EQ(std::string("order_created"), view.names()[0]);
    CRPC_CHECK_EQ(std::string("line_added"), view.names()[1]);
    CRPC_CHECK_EQ(std::string("order_canceled"), view.names()[2]);
    CRPC_CHECK_EQ(std::uint64_t{3}, view.sequences()[2]);
}

CRPC_TEST(cqrs_projection_skips_unnamed_events) {
    recording_projection view;
    std::vector<domain_event> events;
    events.push_back(make_event("", 1));
    events.push_back(make_event("order_created", 2));

    CRPC_CHECK_EQ(1u, apply_events(view, events));
    CRPC_REQUIRE(view.names().size() == 1);
    CRPC_CHECK_EQ(std::string("order_created"), view.names()[0]);
}

CRPC_TEST(cqrs_projection_empty_batch_applies_nothing) {
    recording_projection view;
    CRPC_CHECK_EQ(0u, apply_events(view, {}));
    CRPC_CHECK(view.names().empty());
}

// --- D11: replay guard -----------------------------------------------------

CRPC_TEST(cqrs_replay_guard_rejects_duplicates_and_reorders) {
    replay_guard guard;
    CRPC_CHECK(guard.accept("order-1", make_event("a", 1)));
    CRPC_CHECK(guard.accept("order-1", make_event("b", 2)));
    // Redelivery of an already applied sequence.
    CRPC_CHECK_FALSE(guard.accept("order-1", make_event("b", 2)));
    CRPC_CHECK_FALSE(guard.accept("order-1", make_event("a", 1)));
    // An older sequence arriving late.
    CRPC_CHECK_FALSE(guard.accept("order-1", make_event("c", 2) /*same*/));
    CRPC_CHECK(guard.accept("order-1", make_event("c", 3)));
    CRPC_CHECK_EQ(std::uint64_t{3}, guard.last_sequence("order-1"));
}

CRPC_TEST(cqrs_replay_guard_streams_are_independent) {
    replay_guard guard;
    CRPC_CHECK(guard.accept("order-1", make_event("created", 1)));
    CRPC_CHECK(guard.accept("order-2", make_event("created", 1)));
    CRPC_CHECK(guard.accept("order-1", make_event("paid", 2)));
    CRPC_CHECK(guard.accept("order-2", make_event("paid", 2)));
    CRPC_CHECK_EQ(2u, guard.stream_count());
    CRPC_CHECK_EQ(std::uint64_t{2}, guard.last_sequence("order-1"));

    guard.reset();
    CRPC_CHECK_EQ(0u, guard.stream_count());
    CRPC_CHECK(guard.accept("order-1", make_event("created", 1)));
}

CRPC_TEST(cqrs_replay_guard_accepts_unstamped_events) {
    replay_guard guard;
    domain_event unstamped("noted", "{}");
    CRPC_CHECK_EQ(std::uint64_t{0}, unstamped.sequence);
    // No sequence means no ordering claim; dropping it would lose data.
    CRPC_CHECK(guard.accept("order-1", unstamped));
    CRPC_CHECK(guard.accept("order-1", unstamped));
    CRPC_CHECK_EQ(std::uint64_t{0}, guard.last_sequence("order-1"));
}

CRPC_TEST(cqrs_replay_guard_with_projection_end_to_end) {
    replay_guard guard;
    recording_projection view;
    const std::vector<domain_event> delivered = {
        make_event("order_created", 1),
        make_event("order_created", 1), // duplicate delivery
        make_event("line_added", 2),
        make_event("line_added", 2), // duplicate delivery
        make_event("order_canceled", 3),
    };

    std::size_t applied = 0;
    for (const domain_event& event : delivered) {
        if (guard.accept("order-1", event)) {
            view.apply(event);
            ++applied;
        }
    }

    CRPC_CHECK_EQ(3u, applied);
    CRPC_REQUIRE(view.names().size() == 3);
    CRPC_CHECK_EQ(std::string("order_canceled"), view.names()[2]);
}

CRPC_TEST_MAIN()
