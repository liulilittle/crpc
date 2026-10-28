// CRPC -- constraint / reduction pattern: composable rule evaluation.
//
// A `constraint<T>` is one rule over a value. A satisfied rule returns an ok
// `status`; a violated rule returns `status(rpc_code::invalid_argument, reason)`
// where the message is the human-readable reason.
//
// A `reduction<T>` applies an ordered list of constraints with a fixed,
// documented evaluation order:
//   * `reduction_op::all_of`: rules run in insertion order and short-circuit on
//     the first violation, whose reason is returned. An empty list accepts
//     every value (vacuous truth).
//   * `reduction_op::any_of`: rules run in insertion order and short-circuit on
//     the first success. If every rule fails, the FIRST rule's violation reason
//     is returned (not the last), so the outcome is deterministic. An empty
//     list rejects every value.
// Reductions compose: a nested reduction is added as a single rule via
// `add(name, sub)` or converted with `as_constraint()`, and the same ordering
// rules apply at every level.
#ifndef CRPC_PATTERN_CONSTRAINT_HPP
#define CRPC_PATTERN_CONSTRAINT_HPP

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace pattern {

enum class reduction_op : u8 {
    all_of = 0,
    any_of = 1,
};

// A single rule: given a value, either satisfied (ok) or violated (reason).
template <typename T>
class constraint {
public:
    using rule = std::function<status(const T&)>;

    constraint() = default;

    constraint(std::string name, rule fn)
        : name_(std::move(name)), fn_(std::move(fn)) {}

    [[nodiscard]] status check(const T& value) const {
        if (!fn_) {
            return status(rpc_code::internal, "constraint has no rule");
        }
        return fn_(value);
    }

    const std::string& name() const noexcept { return name_; }

    bool valid() const noexcept { return static_cast<bool>(fn_); }

private:
    std::string name_;
    rule fn_;
};

// A deterministic combination of constraints.
template <typename T>
class reduction {
public:
    explicit reduction(reduction_op op = reduction_op::all_of) noexcept : op_(op) {}

    reduction(const reduction&) = default;
    reduction& operator=(const reduction&) = default;

    // Appends a rule. Rules are evaluated in insertion order.
    reduction& add(constraint<T> rule) {
        rules_.push_back(std::move(rule));
        return *this;
    }

    // Appends a nested reduction as a single named rule.
    reduction& add(std::string name, const reduction<T>& sub) {
        return add(constraint<T>(std::move(name),
                                 [sub](const T& value) { return sub.check(value); }));
    }

    reduction& add(std::string name, reduction<T>&& sub) {
        constraint<T> rule(std::move(name),
                           [sub = std::move(sub)](const T& value) mutable {
                               return sub.check(value);
                           });
        return add(std::move(rule));
    }

    // Exposes this reduction as a single rule for reuse inside another
    // reduction (composition).
    [[nodiscard]] constraint<T> as_constraint(std::string name) const {
        return constraint<T>(std::move(name),
                             [*this](const T& value) { return check(value); });
    }

    [[nodiscard]] status check(const T& value) const {
        if (op_ == reduction_op::all_of) {
            for (const constraint<T>& rule : rules_) {
                status result = rule.check(value);
                if (!result.ok()) {
                    return result;
                }
            }
            return status();
        }

        if (rules_.empty()) {
            return status(rpc_code::invalid_argument,
                          "any_of reduction with zero rules never succeeds");
        }
        status first_violation(rpc_code::invalid_argument,
                               "any_of reduction: no rule passed");
        bool have_first = false;
        for (const constraint<T>& rule : rules_) {
            status result = rule.check(value);
            if (result.ok()) {
                return status();
            }
            if (!have_first) {
                first_violation = result;
                have_first = true;
            }
        }
        return first_violation;
    }

    std::size_t size() const noexcept { return rules_.size(); }

    bool empty() const noexcept { return rules_.empty(); }

    reduction_op op() const noexcept { return op_; }

private:
    reduction_op op_;
    std::vector<constraint<T>> rules_;
};

} // namespace pattern
} // namespace crpc

#endif // CRPC_PATTERN_CONSTRAINT_HPP