// CRPC -- behaviour tree implementation.
//
// The only non-obvious rule in this file is the difference between completing
// an activation and an explicit reset. A composite clears its own traversal
// cursor when it finishes (sequence/selector) or when its current activation
// resolves (parallel); it does not recursively wipe its children, because a
// Cooldown's last-success timestamp is world state that must survive a parent
// re-entry. `reset()` is the explicit, recursive wipe used by tooling and by a
// designer who wants the whole tree to start over.

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <crpc/game/ai/behavior_tree.hpp>

namespace crpc {
namespace game {
namespace ai {

// --- Blackboard --------------------------------------------------------------

void blackboard::set(std::string key, value item) {
    values_[std::move(key)] = std::move(item);
}

const blackboard::value* blackboard::find(const std::string& key) const noexcept {
    const auto found = values_.find(key);
    return found == values_.end() ? nullptr : &found->second;
}

bool blackboard::has(const std::string& key) const noexcept {
    return values_.find(key) != values_.end();
}

bool blackboard::erase(const std::string& key) {
    return values_.erase(key) != 0;
}

void blackboard::clear() noexcept { values_.clear(); }

std::size_t blackboard::size() const noexcept { return values_.size(); }

// --- Composites --------------------------------------------------------------

sequence_node::sequence_node(
    std::vector<std::unique_ptr<behavior_node>> children, std::string name)
    : behavior_node(std::move(name)), children_(std::move(children)) {}

void sequence_node::add(std::unique_ptr<behavior_node> child) {
    children_.push_back(std::move(child));
}

bt_status sequence_node::tick(blackboard& board) {
    if (children_.empty()) {
        return bt_status::success;
    }
    while (current_ < children_.size()) {
        behavior_node* child = children_[current_].get();
        if (child == nullptr) {
            current_ = 0;
            return bt_status::failure;
        }
        const bt_status result = child->tick(board);
        if (result == bt_status::running) {
            return bt_status::running;
        }
        if (result == bt_status::failure) {
            current_ = 0;
            return bt_status::failure;
        }
        ++current_;
    }
    current_ = 0;
    return bt_status::success;
}

void sequence_node::reset() {
    current_ = 0;
    for (auto& child : children_) {
        if (child) {
            child->reset();
        }
    }
}

selector_node::selector_node(
    std::vector<std::unique_ptr<behavior_node>> children, std::string name)
    : behavior_node(std::move(name)), children_(std::move(children)) {}

void selector_node::add(std::unique_ptr<behavior_node> child) {
    children_.push_back(std::move(child));
}

bt_status selector_node::tick(blackboard& board) {
    if (children_.empty()) {
        return bt_status::failure;
    }
    while (current_ < children_.size()) {
        behavior_node* child = children_[current_].get();
        if (child == nullptr) {
            ++current_;
            continue;
        }
        const bt_status result = child->tick(board);
        if (result == bt_status::running) {
            return bt_status::running;
        }
        if (result == bt_status::success) {
            current_ = 0;
            return bt_status::success;
        }
        ++current_;
    }
    current_ = 0;
    return bt_status::failure;
}

void selector_node::reset() {
    current_ = 0;
    for (auto& child : children_) {
        if (child) {
            child->reset();
        }
    }
}

parallel_node::parallel_node(
    std::vector<std::unique_ptr<behavior_node>> children, std::string name)
    : behavior_node(std::move(name)),
      children_(std::move(children)),
      finished_(children_.size(), false) {}

void parallel_node::add(std::unique_ptr<behavior_node> child) {
    children_.push_back(std::move(child));
    finished_.push_back(false);
}

bt_status parallel_node::tick(blackboard& board) {
    if (children_.empty()) {
        return bt_status::success;
    }
    if (finished_.size() != children_.size()) {
        finished_.assign(children_.size(), false);
    }

    bool any_failed = false;
    for (std::size_t index = 0; index < children_.size(); ++index) {
        if (finished_[index]) {
            continue;
        }
        behavior_node* child = children_[index].get();
        if (child == nullptr) {
            finished_[index] = true;
            any_failed = true;
            continue;
        }
        const bt_status result = child->tick(board);
        if (result == bt_status::running) {
            continue;
        }
        finished_[index] = true;
        if (result == bt_status::failure) {
            any_failed = true;
        }
    }

    if (any_failed) {
        finished_.assign(children_.size(), false);
        return bt_status::failure;
    }
    bool all_finished = true;
    for (bool done : finished_) {
        if (!done) {
            all_finished = false;
            break;
        }
    }
    if (all_finished) {
        finished_.assign(children_.size(), false);
        return bt_status::success;
    }
    return bt_status::running;
}

void parallel_node::reset() {
    finished_.assign(children_.size(), false);
    for (auto& child : children_) {
        if (child) {
            child->reset();
        }
    }
}

// --- Decorators --------------------------------------------------------------

inverter_node::inverter_node(std::unique_ptr<behavior_node> child,
                             std::string name)
    : behavior_node(std::move(name)), child_(std::move(child)) {}

bt_status inverter_node::tick(blackboard& board) {
    if (!child_) {
        return bt_status::failure;
    }
    const bt_status result = child_->tick(board);
    if (result == bt_status::running) {
        return bt_status::running;
    }
    return result == bt_status::success ? bt_status::failure
                                        : bt_status::success;
}

void inverter_node::reset() {
    if (child_) {
        child_->reset();
    }
}

succeeder_node::succeeder_node(std::unique_ptr<behavior_node> child,
                               std::string name)
    : behavior_node(std::move(name)), child_(std::move(child)) {}

bt_status succeeder_node::tick(blackboard& board) {
    if (!child_) {
        return bt_status::failure;
    }
    const bt_status result = child_->tick(board);
    if (result == bt_status::running) {
        return bt_status::running;
    }
    return bt_status::success;
}

void succeeder_node::reset() {
    if (child_) {
        child_->reset();
    }
}

failer_node::failer_node(std::unique_ptr<behavior_node> child, std::string name)
    : behavior_node(std::move(name)), child_(std::move(child)) {}

bt_status failer_node::tick(blackboard& board) {
    if (!child_) {
        return bt_status::failure;
    }
    const bt_status result = child_->tick(board);
    if (result == bt_status::running) {
        return bt_status::running;
    }
    return bt_status::failure;
}

void failer_node::reset() {
    if (child_) {
        child_->reset();
    }
}

repeat_node::repeat_node(std::unique_ptr<behavior_node> child,
                         std::size_t times, std::string name)
    : behavior_node(std::move(name)), child_(std::move(child)), times_(times) {}

bt_status repeat_node::tick(blackboard& board) {
    if (!child_) {
        return bt_status::failure;
    }
    const bt_status result = child_->tick(board);
    if (result == bt_status::running) {
        return bt_status::running;
    }
    if (result == bt_status::failure) {
        completed_ = 0;
        return bt_status::failure;
    }
    if (times_ != 0) {
        ++completed_;
        if (completed_ >= times_) {
            completed_ = 0;
            return bt_status::success;
        }
    }
    return bt_status::running;
}

void repeat_node::reset() {
    completed_ = 0;
    if (child_) {
        child_->reset();
    }
}

retry_until_success_node::retry_until_success_node(
    std::unique_ptr<behavior_node> child, std::size_t max_attempts,
    std::string name)
    : behavior_node(std::move(name)),
      child_(std::move(child)),
      max_attempts_(max_attempts) {}

bt_status retry_until_success_node::tick(blackboard& board) {
    if (!child_) {
        return bt_status::failure;
    }
    const bt_status result = child_->tick(board);
    if (result == bt_status::success) {
        attempts_ = 0;
        return bt_status::success;
    }
    if (result == bt_status::running) {
        return bt_status::running;
    }
    if (max_attempts_ != 0 && attempts_ + 1 >= max_attempts_) {
        attempts_ = 0;
        return bt_status::failure;
    }
    ++attempts_;
    return bt_status::running;
}

void retry_until_success_node::reset() {
    attempts_ = 0;
    if (child_) {
        child_->reset();
    }
}

cooldown_node::cooldown_node(std::unique_ptr<behavior_node> child,
                             std::chrono::milliseconds cooldown,
                             bt_clock clock, std::string name)
    : behavior_node(std::move(name)),
      child_(std::move(child)),
      cooldown_(cooldown),
      clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = []() { return std::chrono::steady_clock::now(); };
    }
}

bt_status cooldown_node::tick(blackboard& board) {
    if (!child_) {
        return bt_status::failure;
    }
    const std::chrono::steady_clock::time_point now = clock_();
    if (has_last_ && now - last_ < cooldown_) {
        return bt_status::failure;
    }
    const bt_status result = child_->tick(board);
    if (result == bt_status::success) {
        has_last_ = true;
        last_ = clock_();
    }
    return result;
}

void cooldown_node::reset() {
    has_last_ = false;
    if (child_) {
        child_->reset();
    }
}

// --- Leaves ------------------------------------------------------------------

action_node::action_node(action_fn fn, std::string name)
    : behavior_node(std::move(name)), fn_(std::move(fn)) {}

bt_status action_node::tick(blackboard& board) {
    if (!fn_) {
        return bt_status::failure;
    }
    return fn_(board);
}

condition_node::condition_node(condition_fn fn, std::string name)
    : behavior_node(std::move(name)), fn_(std::move(fn)) {}

bt_status condition_node::tick(blackboard& board) {
    if (!fn_) {
        return bt_status::failure;
    }
    return fn_(board) ? bt_status::success : bt_status::failure;
}

// --- Tree root ---------------------------------------------------------------

behavior_tree::behavior_tree(std::unique_ptr<behavior_node> root)
    : root_(std::move(root)) {}

void behavior_tree::set_root(std::unique_ptr<behavior_node> root) {
    root_ = std::move(root);
}

bt_status behavior_tree::tick() { return tick(board_); }

bt_status behavior_tree::tick(blackboard& board) {
    if (!root_) {
        return bt_status::failure;
    }
    return root_->tick(board);
}

void behavior_tree::reset() {
    if (root_) {
        root_->reset();
    }
}

} // namespace ai
} // namespace game
} // namespace crpc