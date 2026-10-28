// CRPC -- game AI: CART decision tree for classification and regression.
//
// The model is a real, inspectable tree rather than a prediction closure: game
// AI needs to show a designer why a monster chose an action, and a flat vector
// of nodes with explicit child indices is the shape a debugger, a DOT exporter
// and a live editor can all consume. `nodes()` is stored in pre-order with the
// root at index 0, so the tree is trivially traversable and serialisable.
//
// Split selection is CART greedy:
//   * classification -- weighted Gini impurity;
//   * regression     -- weighted variance (squared-error reduction).
// A numeric feature splits on `value <= threshold`; the threshold stored is the
// largest feature value on the left, so the partition is exact even when two
// neighbouring floating-point values round to the same midpoint.
//
// A categorical feature is declared through `config.categorical_features` and
// splits into a *subset* of its values (not merely one value). Categories are
// ordered by their weighted mean class index (classification) or mean target
// (regression) and the best prefix is chosen; this is the standard exact
// ordering for the binary case, and a deterministic heuristic for more classes.
// A value never seen during training goes to the right child.
//
// Overfitting control (both mechanisms are deliberate):
//   * structural pre-pruning, always active: `max_depth` bounds the model,
//     `min_samples_leaf` refuses splits with too little support, and
//     `min_gain` refuses splits that do not reduce impurity enough. This is the
//     primary defence because game training sets are small and a deep tree can
//     memorise a handful of designer-authored states.
//   * optional cost-complexity pruning (`ccp_alpha`): after the tree is grown,
//     a bottom-up weakest-link pass collapses a subtree when
//         leaf_error <= subtree_error + ccp_alpha * (subtree_leaves - 1).
//     Unlike reduced-error pruning it needs no held-out split, which keeps the
//     training call deterministic and usable with a single small data set.
//
// The library never throws. Invalid input is reported as a `status` with
// `rpc_code::invalid_argument`; using a model before training, or with the
// wrong feature count, is reported as a `result` error.
#ifndef CRPC_GAME_AI_DECISION_TREE_HPP
#define CRPC_GAME_AI_DECISION_TREE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace game {
namespace ai {

// Sentinel for a leaf's child slots.
inline constexpr std::size_t decision_tree_no_child =
    static_cast<std::size_t>(-1);

enum class decision_tree_task : std::uint8_t {
    classification = 0,
    regression = 1,
};

struct decision_tree_config {
    decision_tree_task task = decision_tree_task::classification;
    // Maximum number of splits from the root to a leaf. Zero means the root is
    // a leaf (a majority/mean predictor).
    std::size_t max_depth = 8;
    // Both children of a split must contain at least this many samples.
    std::size_t min_samples_leaf = 1;
    // Minimum impurity reduction a split must achieve to be accepted.
    double min_gain = 1e-9;
    // Number of features considered at each node; zero means all. A
    // deterministic per-node sample is used, not a global RNG, so training is
    // reproducible.
    std::size_t max_features = 0;
    // Feature indices whose values are discrete categories. Index must be less
    // than the dataset's column count.
    std::vector<std::size_t> categorical_features;
    // Cost-complexity pruning strength. Zero disables post-pruning.
    double ccp_alpha = 0.0;
};

// Row-major feature matrix with one target per row. `weights` is optional and
// may be empty (every sample then weighs 1); it is used by AdaBoost to train
// each weak learner on the current sample distribution.
struct decision_tree_dataset {
    std::vector<double> features;
    std::vector<double> targets;
    std::vector<double> weights;
    std::size_t rows = 0;
    std::size_t cols = 0;
};

// One node of the trained tree. Internal nodes have `leaf == false` and valid
// `left`/`right` indices; leaves have both set to `decision_tree_no_child`.
struct decision_tree_node {
    bool leaf = true;
    bool categorical = false;
    std::size_t feature = 0;
    // Numeric split: go left when feature <= threshold.
    double threshold = 0.0;
    // Categorical split: go left when the feature value is in this set.
    std::vector<double> categories;
    std::size_t left = decision_tree_no_child;
    std::size_t right = decision_tree_no_child;
    // Number of training samples that reached the node.
    std::size_t samples = 0;
    // Sum of the sample weights that reached the node.
    double weight = 0.0;
    // Weighted Gini impurity (classification) or weighted variance (regression).
    double impurity = 0.0;
    // Impurity reduction of the split; zero for a leaf.
    double gain = 0.0;
    // Regression leaf value, or the predicted class label for a classifier.
    double value = 0.0;
    // Classification leaf distribution, normalised over the node's weight.
    std::vector<double> class_probabilities;
    // Index into `classes()` for a classification leaf.
    std::size_t predicted_class = 0;
    // Weighted misclassification mass (classification) or weighted SSE
    // (regression) if the node were a leaf. Used by cost-complexity pruning
    // and useful in inspection output.
    double leaf_error = 0.0;
};

class CRPC_API decision_tree {
public:
    decision_tree() = default;
    explicit decision_tree(decision_tree_config config);

    // Fits the tree. On failure the previous model (if any) is discarded and
    // `trained()` becomes false. Validates shapes, finiteness, weights,
    // categorical indices and hyperparameters.
    [[nodiscard]] status train(const decision_tree_dataset& dataset);

    // Returns the predicted class label (classification) or value
    // (regression). Fails with `failed_precondition` when untrained and
    // `invalid_argument` when the row has the wrong number of features.
    [[nodiscard]] result<double> predict(crpc::span<const double> features) const;

    // Class probability distribution at the reached leaf. Classification only;
    // a regression model reports `failed_precondition`.
    [[nodiscard]] result<std::vector<double>> predict_proba(
        crpc::span<const double> features) const;

    const decision_tree_config& config() const noexcept { return config_; }
    const std::vector<decision_tree_node>& nodes() const noexcept {
        return nodes_;
    }
    std::size_t root() const noexcept { return 0; }
    const std::vector<double>& classes() const noexcept { return classes_; }
    std::size_t feature_count() const noexcept { return feature_count_; }
    std::size_t depth() const;
    bool trained() const noexcept { return trained_; }

private:
    decision_tree_config config_;
    std::vector<decision_tree_node> nodes_;
    std::vector<double> classes_;
    std::size_t feature_count_ = 0;
    bool trained_ = false;
};

} // namespace ai
} // namespace game
} // namespace crpc

#endif // CRPC_GAME_AI_DECISION_TREE_HPP
