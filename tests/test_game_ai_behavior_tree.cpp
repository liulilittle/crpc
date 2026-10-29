// CRPC -- tests for crpc/game/ai: behaviour trees.
//
// The central property is the one a decision tree cannot express: a `running`
// result must suspend the traversal in place. The sequence tests below assert
// that a sibling which already succeeded is not ticked again and that the
// running child is resumed, not restarted. Every other node type is exercised
// on success/failure/running as well, and the cooldown boundary is checked at
// exactly the configured duration.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <crpc/game/ai/behavior_tree.hpp>

#include "test_framework.hpp"

namespace {

using crpc::game::ai::action_node;
using crpc::game::ai::behavior_node;
using crpc::game::ai::behavior_tree;
using crpc::game::ai::blackboard;
using crpc::game::ai::bt_status;
using crpc::game::ai::cooldown_node;
using crpc::game::ai::condition_node;
using crpc::game::ai::failer_node;
using crpc::game::ai::inverter_node;
using crpc::game::ai::parallel_node;
using crpc::game::ai::repeat_node;
using crpc::game::ai::retry_until_success_node;
using crpc::game::ai::selector_node;
using crpc::game::ai::sequence_node;
using crpc::game::ai::succeeder_node;

// A deterministic action script: call N returns statuses[N], and the last
// status repeats. `calls` records how many times the node was actually ticked.
struct script {
    std::vector<bt_status> statuses;
    std::size_t calls = 0;

    bt_status next() {
        if (statuses.empty()) {
            return bt_status::failure;
        }
        const std::size_t index =
            calls < statuses.size() ? calls : statuses.size() - 1;
        const bt_status result = statuses[index];
        ++calls;
        return result;
    }
};

std::unique_ptr<behavior_node> make_action(std::shared_ptr<script> state,
                                           const std::string& name = {}) {
    return std::make_unique<action_node>(
        [state](blackboard&) { return state->next(); }, name);
}

CRPC_TEST(behavior_tree_sequence_resumes_in_place) {
    // A succeeds immediately. B runs for two ticks. On the second tick A must
    // not be ticked again: the sequence is parked at child index 1.
    auto a = std::make_shared<script>();
    a->statuses = {bt_status::success};
    auto b = std::make_shared<script>();
    b->statuses = {bt_status::running, bt_status::success};

    sequence_node sequence;
    sequence.add(make_action(a, "a"));
    sequence.add(make_action(b, "b"));
    blackboard board;

    CRPC_CHECK(sequence.tick(board) == bt_status::running);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), b->calls);

    CRPC_CHECK(sequence.tick(board) == bt_status::success);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), b->calls);
}

CRPC_TEST(behavior_tree_sequence_waits_for_running_child) {
    // A runs for two ticks, then succeeds. B must not be ticked before A
    // completes, and must run in the same tick in which A succeeds.
    auto a = std::make_shared<script>();
    a->statuses = {bt_status::running, bt_status::success};
    auto b = std::make_shared<script>();
    b->statuses = {bt_status::success};

    sequence_node sequence;
    sequence.add(make_action(a));
    sequence.add(make_action(b));
    blackboard board;

    CRPC_CHECK(sequence.tick(board) == bt_status::running);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), b->calls);

    CRPC_CHECK(sequence.tick(board) == bt_status::success);
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), b->calls);
}

CRPC_TEST(behavior_tree_sequence_fails_and_restarts) {
    auto a = std::make_shared<script>();
    a->statuses = {bt_status::failure};
    auto b = std::make_shared<script>();
    b->statuses = {bt_status::success};

    sequence_node sequence;
    sequence.add(make_action(a));
    sequence.add(make_action(b));
    blackboard board;

    CRPC_CHECK(sequence.tick(board) == bt_status::failure);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), b->calls);

    // The failed activation was reset; the next tick starts from child 0.
    CRPC_CHECK(sequence.tick(board) == bt_status::failure);
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), a->calls);
}

CRPC_TEST(behavior_tree_selector_resumes_in_place) {
    auto a = std::make_shared<script>();
    a->statuses = {bt_status::failure};
    auto b = std::make_shared<script>();
    b->statuses = {bt_status::running, bt_status::success};

    selector_node selector;
    selector.add(make_action(a));
    selector.add(make_action(b));
    blackboard board;

    CRPC_CHECK(selector.tick(board) == bt_status::running);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), b->calls);

    CRPC_CHECK(selector.tick(board) == bt_status::success);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), b->calls);
}

CRPC_TEST(behavior_tree_selector_all_children_fail) {
    auto a = std::make_shared<script>();
    a->statuses = {bt_status::failure};
    auto b = std::make_shared<script>();
    b->statuses = {bt_status::failure};

    selector_node selector;
    selector.add(make_action(a));
    selector.add(make_action(b));
    blackboard board;

    CRPC_CHECK(selector.tick(board) == bt_status::failure);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), b->calls);
}

CRPC_TEST(behavior_tree_parallel_success_tracks_finished_children) {
    auto a = std::make_shared<script>();
    a->statuses = {bt_status::running, bt_status::success};
    auto b = std::make_shared<script>();
    b->statuses = {bt_status::success};

    parallel_node parallel;
    parallel.add(make_action(a));
    parallel.add(make_action(b));
    blackboard board;

    CRPC_CHECK(parallel.tick(board) == bt_status::running);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), b->calls);

    // A finished child must not be ticked again.
    CRPC_CHECK(parallel.tick(board) == bt_status::success);
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), b->calls);
}

CRPC_TEST(behavior_tree_parallel_failure_short_circuits) {
    auto a = std::make_shared<script>();
    a->statuses = {bt_status::failure};
    auto b = std::make_shared<script>();
    b->statuses = {bt_status::running};

    parallel_node parallel;
    parallel.add(make_action(a));
    parallel.add(make_action(b));
    blackboard board;

    CRPC_CHECK(parallel.tick(board) == bt_status::failure);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), b->calls);
}

CRPC_TEST(behavior_tree_empty_composites) {
    sequence_node empty_sequence;
    selector_node empty_selector;
    parallel_node empty_parallel;
    blackboard board;

    CRPC_CHECK(empty_sequence.tick(board) == bt_status::success);
    CRPC_CHECK(empty_selector.tick(board) == bt_status::failure);
    CRPC_CHECK(empty_parallel.tick(board) == bt_status::success);
}

CRPC_TEST(behavior_tree_inverter) {
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::success};
        inverter_node node(make_action(child));
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::failure);
    }
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::failure};
        inverter_node node(make_action(child));
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::success);
    }
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::running, bt_status::success};
        inverter_node node(make_action(child));
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::running);
        CRPC_CHECK(node.tick(board) == bt_status::failure);
        CRPC_CHECK_EQ(static_cast<std::size_t>(2), child->calls);
    }
}

CRPC_TEST(behavior_tree_succeeder_and_failer) {
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::failure};
        succeeder_node node(make_action(child));
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::success);
    }
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::running, bt_status::success};
        succeeder_node node(make_action(child));
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::running);
        CRPC_CHECK(node.tick(board) == bt_status::success);
        CRPC_CHECK_EQ(static_cast<std::size_t>(2), child->calls);
    }
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::success};
        failer_node node(make_action(child));
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::failure);
    }
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::running, bt_status::failure};
        failer_node node(make_action(child));
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::running);
        CRPC_CHECK(node.tick(board) == bt_status::failure);
        CRPC_CHECK_EQ(static_cast<std::size_t>(2), child->calls);
    }
}

CRPC_TEST(behavior_tree_repeat) {
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::success};
        repeat_node node(make_action(child), 3);
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::running);
        CRPC_CHECK(node.tick(board) == bt_status::running);
        CRPC_CHECK(node.tick(board) == bt_status::success);
        CRPC_CHECK_EQ(static_cast<std::size_t>(3), child->calls);
    }
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::success};
        repeat_node node(make_action(child), 1);
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::success);
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), child->calls);
    }
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::success};
        repeat_node node(make_action(child), 0); // infinite
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::running);
        CRPC_CHECK(node.tick(board) == bt_status::running);
        CRPC_CHECK_EQ(static_cast<std::size_t>(2), child->calls);
    }
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::failure};
        repeat_node node(make_action(child), 3);
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::failure);
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), child->calls);
    }
}

CRPC_TEST(behavior_tree_repeat_reset) {
    auto child = std::make_shared<script>();
    child->statuses = {bt_status::success};
    repeat_node node(make_action(child), 3);
    blackboard board;
    CRPC_CHECK(node.tick(board) == bt_status::running);
    CRPC_CHECK(node.tick(board) == bt_status::running);
    node.reset();
    CRPC_CHECK(node.tick(board) == bt_status::running);
    CRPC_CHECK(node.tick(board) == bt_status::running);
    CRPC_CHECK(node.tick(board) == bt_status::success);
}

CRPC_TEST(behavior_tree_retry_until_success) {
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::failure, bt_status::failure,
                           bt_status::success};
        retry_until_success_node node(make_action(child));
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::running);
        CRPC_CHECK(node.tick(board) == bt_status::running);
        CRPC_CHECK(node.tick(board) == bt_status::success);
        CRPC_CHECK_EQ(static_cast<std::size_t>(3), child->calls);
    }
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::failure};
        retry_until_success_node node(make_action(child), 2);
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::running);
        CRPC_CHECK(node.tick(board) == bt_status::failure);
        CRPC_CHECK_EQ(static_cast<std::size_t>(2), child->calls);
    }
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::failure};
        retry_until_success_node node(make_action(child), 1);
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::failure);
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), child->calls);
    }
    {
        auto child = std::make_shared<script>();
        child->statuses = {bt_status::running, bt_status::success};
        retry_until_success_node node(make_action(child));
        blackboard board;
        CRPC_CHECK(node.tick(board) == bt_status::running);
        CRPC_CHECK(node.tick(board) == bt_status::success);
        CRPC_CHECK_EQ(static_cast<std::size_t>(2), child->calls);
    }
}

CRPC_TEST(behavior_tree_cooldown_boundary) {
    auto now = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::time_point{});
    const auto clock = [now]() { return *now; };

    auto child = std::make_shared<script>();
    child->statuses = {bt_status::success};
    cooldown_node node(make_action(child), std::chrono::milliseconds(100), clock);
    blackboard board;

    CRPC_CHECK(node.tick(board) == bt_status::success);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), child->calls);

    *now += std::chrono::milliseconds(99);
    CRPC_CHECK(node.tick(board) == bt_status::failure);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), child->calls);

    // Exactly the configured duration is allowed (>=, not >).
    *now += std::chrono::milliseconds(1);
    CRPC_CHECK(node.tick(board) == bt_status::success);
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), child->calls);
}

CRPC_TEST(behavior_tree_cooldown_starts_after_success_not_running) {
    auto now = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::time_point{});
    const auto clock = [now]() { return *now; };

    auto child = std::make_shared<script>();
    child->statuses = {bt_status::running, bt_status::running,
                       bt_status::success, bt_status::success};
    cooldown_node node(make_action(child), std::chrono::milliseconds(50), clock);
    blackboard board;

    CRPC_CHECK(node.tick(board) == bt_status::running);
    *now += std::chrono::milliseconds(10);
    CRPC_CHECK(node.tick(board) == bt_status::running);
    *now += std::chrono::milliseconds(10);
    CRPC_CHECK(node.tick(board) == bt_status::success);
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), child->calls);

    *now += std::chrono::milliseconds(49);
    CRPC_CHECK(node.tick(board) == bt_status::failure);
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), child->calls);

    *now += std::chrono::milliseconds(1);
    CRPC_CHECK(node.tick(board) == bt_status::success);
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), child->calls);
}

CRPC_TEST(behavior_tree_cooldown_failure_does_not_start_it) {
    auto now = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::time_point{});
    const auto clock = [now]() { return *now; };

    auto child = std::make_shared<script>();
    child->statuses = {bt_status::failure, bt_status::success};
    cooldown_node node(make_action(child), std::chrono::milliseconds(100), clock);
    blackboard board;

    CRPC_CHECK(node.tick(board) == bt_status::failure);
    *now += std::chrono::milliseconds(1);
    CRPC_CHECK(node.tick(board) == bt_status::success);
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), child->calls);
}

CRPC_TEST(behavior_tree_blackboard_is_shared) {
    auto setter = std::make_unique<action_node>([](blackboard& board) {
        board.set("hp", 5.0);
        board.set("mode", std::string("attack"));
        return bt_status::success;
    });
    auto guard = std::make_unique<condition_node>([](blackboard& board) {
        const auto* value = board.find("hp");
        return value != nullptr && std::get<double>(*value) > 0.0;
    });
    auto reader = std::make_unique<action_node>([](blackboard& board) {
        const auto* value = board.find("hp");
        if (value == nullptr) {
            return bt_status::failure;
        }
        board.set("seen", std::get<double>(*value));
        return bt_status::success;
    });

    sequence_node sequence;
    sequence.add(std::move(setter));
    sequence.add(std::move(guard));
    sequence.add(std::move(reader));

    blackboard board;
    CRPC_CHECK(sequence.tick(board) == bt_status::success);
    CRPC_CHECK(board.has("hp"));
    CRPC_CHECK(board.has("mode"));
    CRPC_CHECK(board.has("seen"));
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), board.size());
    const auto* seen = board.find("seen");
    CRPC_CHECK(seen != nullptr);
    if (seen != nullptr) {
        CRPC_CHECK_EQ(5.0, std::get<double>(*seen));
    }
    const auto* mode = board.find("mode");
    CRPC_CHECK(mode != nullptr);
    if (mode != nullptr) {
        CRPC_CHECK(std::get<std::string>(*mode) == "attack");
    }

    CRPC_CHECK(board.erase("mode"));
    CRPC_CHECK_FALSE(board.has("mode"));
    board.clear();
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), board.size());
}

CRPC_TEST(behavior_tree_blackboard_variant_types) {
    blackboard board;
    board.set("number", 1.5);
    board.set("integer", std::int64_t{42});
    board.set("flag", true);
    board.set("text", std::string("patrol"));

    const auto* number = board.find("number");
    const auto* integer = board.find("integer");
    const auto* flag = board.find("flag");
    const auto* text = board.find("text");
    CRPC_CHECK(number != nullptr && std::get<double>(*number) == 1.5);
    CRPC_CHECK(integer != nullptr && std::get<std::int64_t>(*integer) == 42);
    CRPC_CHECK(flag != nullptr && std::get<bool>(*flag));
    CRPC_CHECK(text != nullptr && std::get<std::string>(*text) == "patrol");
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), board.size());
}

CRPC_TEST(behavior_tree_own_board_and_reset) {
    auto a = std::make_shared<script>();
    a->statuses = {bt_status::running, bt_status::success,
                   bt_status::success};
    auto b = std::make_shared<script>();
    b->statuses = {bt_status::success};

    auto sequence = std::make_unique<sequence_node>();
    sequence->add(make_action(a));
    sequence->add(make_action(b));
    behavior_tree tree(std::move(sequence));

    CRPC_CHECK(tree.tick() == bt_status::running);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), b->calls);

    // Reset returns the traversal to child 0; A is ticked again.
    tree.reset();
    CRPC_CHECK(tree.tick() == bt_status::success);
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), a->calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), b->calls);
}

CRPC_TEST(behavior_tree_condition_leaf) {
    bool flag = false;
    condition_node node([&flag](blackboard&) { return flag; });
    blackboard board;
    CRPC_CHECK(node.tick(board) == bt_status::failure);
    flag = true;
    CRPC_CHECK(node.tick(board) == bt_status::success);
}

CRPC_TEST(behavior_tree_empty_root_fails) {
    behavior_tree tree;
    CRPC_CHECK(tree.tick() == bt_status::failure);
    CRPC_CHECK(tree.root() == nullptr);
}

CRPC_TEST(behavior_tree_scripted_action_name) {
    auto state = std::make_shared<script>();
    state->statuses = {bt_status::success};
    action_node node([state](blackboard&) { return state->next(); }, "patrol");
    CRPC_CHECK(node.name() == "patrol");
    blackboard board;
    CRPC_CHECK(node.tick(board) == bt_status::success);
}

} // namespace

CRPC_TEST_MAIN()