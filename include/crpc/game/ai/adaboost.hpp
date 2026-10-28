// CRPC -- game AI: binary AdaBoost over decision stumps.
//
// Variant: discrete AdaBoost (Freund & Schapire, 1997), not SAMME.
//
// Discrete AdaBoost is the right choice here because the problem is binary:
// the weak learner is a decision stump whose output is mapped to -1/+1, the
// classifier weight is alpha = 1/2 * ln((1-e)/e), and the vote is a signed
// weighted sum. SAMME adds a log(K-1) term and a multiclass vote that buys
// nothing for two classes; it would only make the alpha and the diagnostics
// harder to reason about. The game AI layer uses boosting as a compact,
// inspectable "several simple combat rules vote" model, so the two-class
// formulation is the natural fit.
//
// The weak learner is literally the decision tree from this package, trained
// with `max_depth = 1` (a stump) and the current sample weights. Every round
// is exposed in `rounds()` with its alpha and weighted error, which is what a
// designer needs to see when a monster's policy stops improving. A stump has
// zero mutual information with any single axis of a balanced XOR, so boosting
// a stump cannot start on that function. `weak_max_depth` therefore remains
// configurable; setting it to 2 lets the same decision-tree weak learner
// represent XOR, while the default stays the cheapest inspectable rule.
//
// Weight update: w_i *= exp(-alpha * y_i * h_i), then renormalise. Correctness
// has a testable consequence: the unweighted training error of the ensemble is
// monotone non-increasing round by round. The test suite asserts that on data
// sets that are not separable by a single stump.
//
// Stopping rules:
//   * zero weighted error (a perfect weak learner) -- accepted and then stop;
//   * weighted error >= 1/2 -- no weak learning, stop without adding a
//     zero-weight voter (otherwise the distribution never changes);
//   * target unweighted accuracy reached;
//   * `max_rounds` exhausted.
//
// No exceptions. Bad input is `rpc_code::invalid_argument`; prediction before
// training is `failed_precondition`.
#ifndef CRPC_GAME_AI_ADABOOST_HPP
#define CRPC_GAME_AI_ADABOOST_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/game/ai/decision_tree.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace game {
namespace ai {

struct adaboost_config {
    // Number of boosting rounds requested. Must be at least 1.
    std::size_t max_rounds = 50;
    // Stop as soon as the unweighted training accuracy reaches this value.
    // Must be in (0, 1].
    double target_accuracy = 1.0;
    // Depth of the weak decision tree. One means a decision stump.
    std::size_t weak_max_depth = 1;
    std::size_t weak_min_samples_leaf = 1;
    double weak_min_gain = 0.0;
    // Categorical features forwarded to every weak learner.
    std::vector<std::size_t> categorical_features;
    // Numerical floor used when a perfect weak learner would otherwise make
    // alpha infinite.
    double epsilon = 1e-12;
};

// One accepted boosting round. Rounds are zero-indexed in `rounds()`.
struct adaboost_round {
    std::size_t index = 0;
    // 1/2 * ln((1 - weighted_error) / weighted_error).
    double alpha = 0.0;
    // Weighted error of this weak learner under the distribution it saw.
    double weighted_error = 0.0;
    // Unweighted error of the whole ensemble after this learner was added.
    double training_error = 0.0;
    double training_accuracy = 0.0;
};

class CRPC_API adaboost_classifier {
public:
    adaboost_classifier() = default;
    explicit adaboost_classifier(adaboost_config config);

    // Fits the ensemble. The data set must contain exactly two distinct target
    // values; labels are sorted and mapped to -1/+1. If `dataset.weights` is
    // non-empty it is used as the initial distribution, otherwise the samples
    // start uniform.
    [[nodiscard]] status train(const decision_tree_dataset& dataset);

    // Weighted vote label. Fails with `failed_precondition` when untrained and
    // `invalid_argument` on a feature-count mismatch.
    [[nodiscard]] result<double> predict(crpc::span<const double> features) const;

    // Raw signed vote sum; useful for confidence and for threshold tuning.
    [[nodiscard]] result<double> decision_function(
        crpc::span<const double> features) const;

    const adaboost_config& config() const noexcept { return config_; }
    const std::vector<adaboost_round>& rounds() const noexcept { return rounds_; }
    const std::vector<decision_tree>& weak_learners() const noexcept {
        return weak_learners_;
    }
    const std::vector<double>& classes() const noexcept { return classes_; }
    std::size_t round_count() const noexcept { return rounds_.size(); }
    std::size_t feature_count() const noexcept { return feature_count_; }
    bool trained() const noexcept { return trained_; }

private:
    void reset_model();

    adaboost_config config_;
    std::vector<adaboost_round> rounds_;
    std::vector<decision_tree> weak_learners_;
    std::vector<double> classes_;
    std::size_t feature_count_ = 0;
    bool trained_ = false;
};

} // namespace ai
} // namespace game
} // namespace crpc

#endif // CRPC_GAME_AI_ADABOOST_HPP