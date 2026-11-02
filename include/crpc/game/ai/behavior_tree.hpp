// CRPC -- game AI: behaviour trees with cross-tick `running` state.
//
// A behaviour tree differs from a decision tree in one decisive way: a tick is
// not a pure query. A node may answer `running`, and the next tick must resume
// exactly where the previous one stopped. That is why every composite stores
// its current child index instead of re-evaluating the subtree from scratch:
//
//   * Sequence returns `running` and keeps `current_` when a child is running.
//     On the next tick it ticks that same child again, not the subtree.
//   * Selector behaves symmetrically: it remembers which child is running and
//     does not re-tick children that already failed.
//   * Parallel tracks which children have finished for the current activation
//     and only ticks the unfinished ones.
//   * Repeat and RetryUntilSuccess keep their progress across ticks.
//
// Leaf nodes are user callbacks (`action_node`, `condition_node`); composites
// and decorators are provided by the library. All node types share one
// `blackboard`, a key/value store used for monster state ("hp", "target",
// "alert_level", ...). Values are a small closed variant rather than void*:
// the blackboard is copied/serialised by tooling much more often than it is
// extended with a new runtime type.
//
// Status algebra:
//   Sequence:  empty -> success; first failure -> failure; all success ->
//              success; any running -> running (resume there).
//   Selector:  empty -> failure; first success -> success; all failure ->
//              failure; any running -> running (resume there).
//   Parallel:  empty -> success; any failure -> failure; all success ->
//              success; otherwise running. Finished children are not re-ticked
//              in the same activation.
//   Inverter:  success <-> failure; running stays running.
//   Succeeder: running stays running, any terminal child result -> success.
//   Failer:    running stays running, any terminal child result -> failure.
//   Repeat:    runs the child once per tick; `times == 0` repeats forever.
//              A child failure fails the node. Successful repetitions return
//              running until the count is reached, then success.
//   RetryUntilSuccess: a child failure schedules another attempt on the next
//              tick and returns running; `max_attempts == 0` is unlimited.
//   Cooldown:  while `now - last_success < duration` the child is not ticked
//              and the node returns failure. The boundary is inclusive:
//              exactly `duration` later the child is allowed again. A running
//              child does not start the timer; success does; failure does not.
//
// `reset()` clears transient traversal state recursively. Cooldown also clears
// its last-success time, so a designer can force a ready state. The blackboard
// is not cleared by `reset()`; callers own that lifecycle.
//
// Thread safety: a behavior tree is single-threaded. Tick it from one game
// loop; do not share one instance across threads without external locking.
#ifndef CRPC_GAME_AI_BEHAVIOR_TREE_HPP
#define CRPC_GAME_AI_BEHAVIOR_TREE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <crpc/config.hpp>

namespace crpc {
namespace game {
namespace ai {

enum class bt_status : std::uint8_t {
    success = 0,
    failure = 1,
    running = 2,
};

// Shared key/value state. Keys are ordered so iteration and serialisation are
// deterministic; values are exactly one of the four types in `value`.
class CRPC_API blackboard {
public:
    using value = std::variant<double, std::int64_t, bool, std::string>;
    using container_type = std::map<std::string, value>;

    blackboard() = default;

    void set(std::string key, value item);
    [[nodiscard]] const value* find(const std::string& key) const noexcept;
    [[nodiscard]] bool has(const std::string& key) const noexcept;
    bool erase(const std::string& key);
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] container_type::const_iterator begin() const noexcept {
        return values_.begin();
    }
    [[nodiscard]] container_type::const_iterator end() const noexcept {
        return values_.end();
    }

private:
    container_type values_;
};

class CRPC_API behavior_node {
public:
    virtual ~behavior_node() = default;

    // Advances the node by one tick. `board` is shared by the whole tree.
    [[nodiscard]] virtual bt_status tick(blackboard& board) = 0;

    // Clears transient state (current child, repeat counters, timers). The
    // default is a no-op for stateless leaves.
    virtual void reset() {}

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

protected:
    behavior_node() = default;
    explicit behavior_node(std::string name) : name_(std::move(name)) {}

private:
    std::string name_;
};

// --- Composites --------------------------------------------------------------

class CRPC_API sequence_node final : public behavior_node {
    // Owns its children; a copy would have to clone the subtree, which this type
    // does not do. Stated explicitly so an exported build does not instantiate the
    // implicitly-deleted copy operations -- the static build never looked at them.
    sequence_node(const sequence_node&) = delete;
    sequence_node& operator=(const sequence_node&) = delete;
public:
    sequence_node() = default;
    explicit sequence_node(std::vector<std::unique_ptr<behavior_node>> children,
                           std::string name = {});
    [[nodiscard]] bt_status tick(blackboard& board) override;
    void reset() override;
    void add(std::unique_ptr<behavior_node> child);
    [[nodiscard]] std::size_t child_count() const noexcept {
        return children_.size();
    }

private:
    std::vector<std::unique_ptr<behavior_node>> children_;
    std::size_t current_ = 0;
};

class CRPC_API selector_node final : public behavior_node {
    // Owns its children; a copy would have to clone the subtree, which this type
    // does not do. Stated explicitly so an exported build does not instantiate the
    // implicitly-deleted copy operations -- the static build never looked at them.
    selector_node(const selector_node&) = delete;
    selector_node& operator=(const selector_node&) = delete;
public:
    selector_node() = default;
    explicit selector_node(std::vector<std::unique_ptr<behavior_node>> children,
                           std::string name = {});
    [[nodiscard]] bt_status tick(blackboard& board) override;
    void reset() override;
    void add(std::unique_ptr<behavior_node> child);
    [[nodiscard]] std::size_t child_count() const noexcept {
        return children_.size();
    }

private:
    std::vector<std::unique_ptr<behavior_node>> children_;
    std::size_t current_ = 0;
};

class CRPC_API parallel_node final : public behavior_node {
    // Owns its children; a copy would have to clone the subtree, which this type
    // does not do. Stated explicitly so an exported build does not instantiate the
    // implicitly-deleted copy operations -- the static build never looked at them.
    parallel_node(const parallel_node&) = delete;
    parallel_node& operator=(const parallel_node&) = delete;
public:
    parallel_node() = default;
    explicit parallel_node(std::vector<std::unique_ptr<behavior_node>> children,
                           std::string name = {});
    [[nodiscard]] bt_status tick(blackboard& board) override;
    void reset() override;
    void add(std::unique_ptr<behavior_node> child);
    [[nodiscard]] std::size_t child_count() const noexcept {
        return children_.size();
    }

private:
    std::vector<std::unique_ptr<behavior_node>> children_;
    std::vector<bool> finished_;
};

// --- Decorators --------------------------------------------------------------

class CRPC_API inverter_node final : public behavior_node {
public:
    explicit inverter_node(std::unique_ptr<behavior_node> child,
                           std::string name = {});
    [[nodiscard]] bt_status tick(blackboard& board) override;
    void reset() override;

private:
    std::unique_ptr<behavior_node> child_;
};

class CRPC_API succeeder_node final : public behavior_node {
public:
    explicit succeeder_node(std::unique_ptr<behavior_node> child,
                            std::string name = {});
    [[nodiscard]] bt_status tick(blackboard& board) override;
    void reset() override;

private:
    std::unique_ptr<behavior_node> child_;
};

class CRPC_API failer_node final : public behavior_node {
public:
    explicit failer_node(std::unique_ptr<behavior_node> child,
                         std::string name = {});
    [[nodiscard]] bt_status tick(blackboard& board) override;
    void reset() override;

private:
    std::unique_ptr<behavior_node> child_;
};

class CRPC_API repeat_node final : public behavior_node {
public:
    // `times == 0` means repeat forever.
    explicit repeat_node(std::unique_ptr<behavior_node> child, std::size_t times,
                         std::string name = {});
    [[nodiscard]] bt_status tick(blackboard& board) override;
    void reset() override;

private:
    std::unique_ptr<behavior_node> child_;
    std::size_t times_ = 1;
    std::size_t completed_ = 0;
};

class CRPC_API retry_until_success_node final : public behavior_node {
public:
    // `max_attempts == 0` means unlimited.
    explicit retry_until_success_node(std::unique_ptr<behavior_node> child,
                                      std::size_t max_attempts = 0,
                                      std::string name = {});
    [[nodiscard]] bt_status tick(blackboard& board) override;
    void reset() override;

private:
    std::unique_ptr<behavior_node> child_;
    std::size_t max_attempts_ = 0;
    std::size_t attempts_ = 0;
};

using bt_clock = std::function<std::chrono::steady_clock::time_point()>;

class CRPC_API cooldown_node final : public behavior_node {
public:
    // `clock` defaults to `std::chrono::steady_clock::now`; tests and
    // deterministic simulations inject their own.
    explicit cooldown_node(std::unique_ptr<behavior_node> child,
                           std::chrono::milliseconds cooldown,
                           bt_clock clock = {}, std::string name = {});
    [[nodiscard]] bt_status tick(blackboard& board) override;
    void reset() override;

private:
    std::unique_ptr<behavior_node> child_;
    std::chrono::milliseconds cooldown_;
    bt_clock clock_;
    bool has_last_ = false;
    std::chrono::steady_clock::time_point last_{};
};

// --- Leaves ------------------------------------------------------------------

using action_fn = std::function<bt_status(blackboard&)>;
using condition_fn = std::function<bool(blackboard&)>;

class CRPC_API action_node final : public behavior_node {
public:
    // An empty callback is a failure, never a crash.
    explicit action_node(action_fn fn, std::string name = {});
    [[nodiscard]] bt_status tick(blackboard& board) override;

private:
    action_fn fn_;
};

class CRPC_API condition_node final : public behavior_node {
public:
    // A true predicate is success, false is failure. Never returns running.
    explicit condition_node(condition_fn fn, std::string name = {});
    [[nodiscard]] bt_status tick(blackboard& board) override;

private:
    condition_fn fn_;
};

// --- Tree root ---------------------------------------------------------------

class CRPC_API behavior_tree {
public:
    behavior_tree() = default;
    explicit behavior_tree(std::unique_ptr<behavior_node> root);
    behavior_tree(behavior_tree&&) noexcept = default;
    behavior_tree& operator=(behavior_tree&&) noexcept = default;
    behavior_tree(const behavior_tree&) = delete;
    behavior_tree& operator=(const behavior_tree&) = delete;
    ~behavior_tree() = default;

    void set_root(std::unique_ptr<behavior_node> root);
    [[nodiscard]] bt_status tick();
    [[nodiscard]] bt_status tick(blackboard& board);
    void reset();
    [[nodiscard]] blackboard& board() noexcept { return board_; }
    [[nodiscard]] const blackboard& board() const noexcept { return board_; }
    [[nodiscard]] behavior_node* root() const noexcept { return root_.get(); }

private:
    std::unique_ptr<behavior_node> root_;
    blackboard board_;
};

} // namespace ai
} // namespace game
} // namespace crpc

#endif // CRPC_GAME_AI_BEHAVIOR_TREE_HPP