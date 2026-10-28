// CRPC -- CART decision tree implementation.
//
// The split search is the hot path. Each node scans every candidate feature
// once: numeric features are sorted by value and swept left-to-right while
// accumulating weighted class counts (classification) or weighted target sums
// (regression); categorical features are grouped by value, ordered by their
// target mean, and swept as prefix subsets. The per-node sort dominates and is
// O(d * n log n), which is the expected cost of exact CART.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <utility>
#include <vector>

#include <crpc/game/ai/decision_tree.hpp>

namespace crpc {
namespace game {
namespace ai {

namespace {

struct category_accumulator {
    std::size_t count = 0;
    double weight = 0.0;
    double sum_y = 0.0;
    double sum_sq = 0.0;
    std::vector<double> class_weights;
};

struct split_candidate {
    bool valid = false;
    std::size_t feature = 0;
    bool categorical = false;
    double threshold = 0.0;
    std::vector<double> categories;
    double gain = -std::numeric_limits<double>::infinity();
};

struct build_context {
    const std::vector<double>& features;
    const std::vector<double>& targets;
    const std::vector<double>& weights;
    const std::vector<std::size_t>& class_of;
    const std::vector<char>& categorical;
    std::size_t cols = 0;
    std::size_t class_count = 0;
    bool classification = true;
    std::size_t min_samples_leaf = 1;
    std::size_t max_features = 0;
};

struct ordered_category {
    double value = 0.0;
    double key = 0.0;
};

struct subset_statistics {
    double weight = 0.0;
    std::vector<double> class_counts;
    double sum = 0.0;
    double sum_sq = 0.0;
    double impurity = 0.0;
};

std::uint64_t xorshift64(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

double weighted_gini(const std::vector<double>& counts, double weight) {
    if (!(weight > 0.0)) {
        return 0.0;
    }
    double squared = 0.0;
    for (double count : counts) {
        const double proportion = count / weight;
        squared += proportion * proportion;
    }
    const double result = 1.0 - squared;
    return result > 0.0 ? result : 0.0;
}

double weighted_variance(double sum, double sum_sq, double weight) {
    if (!(weight > 0.0)) {
        return 0.0;
    }
    const double mean = sum / weight;
    const double result = sum_sq / weight - mean * mean;
    return result > 0.0 ? result : 0.0;
}

bool contains_value(const std::vector<double>& values, double value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

subset_statistics compute_subset_statistics(
    const std::vector<std::size_t>& indices, const build_context& context) {
    subset_statistics stats;
    stats.class_counts.assign(context.class_count, 0.0);
    for (std::size_t row : indices) {
        const double weight = context.weights[row];
        stats.weight += weight;
        const double target = context.targets[row];
        stats.sum += weight * target;
        stats.sum_sq += weight * target * target;
        if (context.classification) {
            stats.class_counts[context.class_of[row]] += weight;
        }
    }
    stats.impurity =
        context.classification
            ? weighted_gini(stats.class_counts, stats.weight)
            : weighted_variance(stats.sum, stats.sum_sq, stats.weight);
    return stats;
}

split_candidate find_best_split(const std::vector<std::size_t>& indices,
                                const build_context& context,
                                std::size_t node_index, double node_weight,
                                double parent_impurity,
                                const std::vector<double>& total_counts,
                                double total_sum, double total_sum_sq) {
    split_candidate best;

    std::vector<std::size_t> features(context.cols);
    std::iota(features.begin(), features.end(), static_cast<std::size_t>(0));
    if (context.max_features != 0 && context.max_features < context.cols) {
        // Deterministic per-node partial Fisher-Yates: the same training data
        // always produces the same forest of candidate sets, with no global
        // RNG and no thread-local state.
        std::uint64_t state =
            static_cast<std::uint64_t>(node_index) * 0x9E3779B97F4A7C15ull +
            0xD1B54A32D192ED03ull;
        for (std::size_t i = 0; i < context.max_features; ++i) {
            const std::size_t remaining = context.cols - i;
            const std::size_t offset =
                static_cast<std::size_t>(xorshift64(state) % remaining);
            std::swap(features[i], features[i + offset]);
        }
        features.resize(context.max_features);
    }

    const std::size_t min_leaf = context.min_samples_leaf;
    std::vector<double> left_counts(context.class_count, 0.0);
    std::vector<double> right_counts(context.class_count, 0.0);

    for (std::size_t feature : features) {
        const bool is_categorical = context.categorical[feature] != 0;
        if (!is_categorical) {
            std::vector<std::size_t> order(indices);
            std::sort(order.begin(), order.end(),
                      [&context, feature](std::size_t lhs, std::size_t rhs) {
                          const double a =
                              context.features[lhs * context.cols + feature];
                          const double b =
                              context.features[rhs * context.cols + feature];
                          if (a < b) {
                              return true;
                          }
                          if (b < a) {
                              return false;
                          }
                          return lhs < rhs;
                      });

            std::fill(left_counts.begin(), left_counts.end(), 0.0);
            double left_weight = 0.0;
            double left_sum = 0.0;
            double left_sum_sq = 0.0;
            for (std::size_t position = 0; position + 1 < order.size();
                 ++position) {
                const std::size_t row = order[position];
                const double weight = context.weights[row];
                const double value =
                    context.features[row * context.cols + feature];
                left_weight += weight;
                if (context.classification) {
                    left_counts[context.class_of[row]] += weight;
                }
                const double target = context.targets[row];
                left_sum += weight * target;
                left_sum_sq += weight * target * target;

                const std::size_t next_row = order[position + 1];
                const double next_value =
                    context.features[next_row * context.cols + feature];
                if (!(value < next_value)) {
                    continue; // tied values: no threshold can separate them
                }
                const std::size_t left_samples = position + 1;
                const std::size_t right_samples = order.size() - left_samples;
                if (left_samples < min_leaf || right_samples < min_leaf) {
                    continue;
                }
                if (!(left_weight > 0.0)) {
                    continue;
                }
                const double right_weight = node_weight - left_weight;
                if (!(right_weight > 0.0)) {
                    continue;
                }

                double impurity_left = 0.0;
                double impurity_right = 0.0;
                if (context.classification) {
                    for (std::size_t c = 0; c < context.class_count; ++c) {
                        right_counts[c] = total_counts[c] - left_counts[c];
                    }
                    impurity_left = weighted_gini(left_counts, left_weight);
                    impurity_right = weighted_gini(right_counts, right_weight);
                } else {
                    impurity_left =
                        weighted_variance(left_sum, left_sum_sq, left_weight);
                    impurity_right = weighted_variance(
                        total_sum - left_sum, total_sum_sq - left_sum_sq,
                        right_weight);
                }

                const double gain =
                    parent_impurity -
                    (left_weight / node_weight) * impurity_left -
                    (right_weight / node_weight) * impurity_right;
                if (gain > best.gain) {
                    best.valid = true;
                    best.feature = feature;
                    best.categorical = false;
                    best.threshold = value;
                    best.categories.clear();
                    best.gain = gain;
                }
            }
            continue;
        }

        // Categorical feature: group rows by exact value, order categories by
        // their weighted target mean, then consider every prefix as the left
        // subset. This is the exact subset search for a binary target and a
        // deterministic heuristic for more classes.
        std::map<double, category_accumulator> groups;
        for (std::size_t row : indices) {
            const double value =
                context.features[row * context.cols + feature];
            auto found = groups.find(value);
            if (found == groups.end()) {
                category_accumulator fresh;
                if (context.classification) {
                    fresh.class_weights.assign(context.class_count, 0.0);
                }
                found = groups.emplace(value, std::move(fresh)).first;
            }
            category_accumulator& group = found->second;
            ++group.count;
            const double weight = context.weights[row];
            group.weight += weight;
            const double target = context.targets[row];
            group.sum_y += weight * target;
            group.sum_sq += weight * target * target;
            if (context.classification) {
                group.class_weights[context.class_of[row]] += weight;
            }
        }
        if (groups.size() < 2) {
            continue;
        }

        std::vector<ordered_category> ordered;
        ordered.reserve(groups.size());
        for (const auto& entry : groups) {
            const category_accumulator& group = entry.second;
            double key = 0.0;
            if (group.weight > 0.0) {
                if (context.classification) {
                    double weighted_class = 0.0;
                    for (std::size_t c = 0; c < context.class_count; ++c) {
                        weighted_class += static_cast<double>(c) *
                                          group.class_weights[c];
                    }
                    key = weighted_class / group.weight;
                } else {
                    key = group.sum_y / group.weight;
                }
            }
            ordered.push_back(ordered_category{entry.first, key});
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const ordered_category& lhs, const ordered_category& rhs) {
                      if (lhs.key < rhs.key) {
                          return true;
                      }
                      if (rhs.key < lhs.key) {
                          return false;
                      }
                      return lhs.value < rhs.value;
                  });

        std::fill(left_counts.begin(), left_counts.end(), 0.0);
        double left_weight = 0.0;
        double left_sum = 0.0;
        double left_sum_sq = 0.0;
        std::size_t left_samples = 0;
        for (std::size_t prefix = 1; prefix < ordered.size(); ++prefix) {
            const auto group_it = groups.find(ordered[prefix - 1].value);
            if (group_it == groups.end()) {
                continue;
            }
            const category_accumulator& group = group_it->second;
            left_samples += group.count;
            left_weight += group.weight;
            left_sum += group.sum_y;
            left_sum_sq += group.sum_sq;
            if (context.classification) {
                for (std::size_t c = 0; c < context.class_count; ++c) {
                    left_counts[c] += group.class_weights[c];
                }
            }

            const std::size_t right_samples = indices.size() - left_samples;
            if (left_samples < min_leaf || right_samples < min_leaf) {
                continue;
            }
            if (!(left_weight > 0.0)) {
                continue;
            }
            const double right_weight = node_weight - left_weight;
            if (!(right_weight > 0.0)) {
                continue;
            }

            double impurity_left = 0.0;
            double impurity_right = 0.0;
            if (context.classification) {
                for (std::size_t c = 0; c < context.class_count; ++c) {
                    right_counts[c] = total_counts[c] - left_counts[c];
                }
                impurity_left = weighted_gini(left_counts, left_weight);
                impurity_right = weighted_gini(right_counts, right_weight);
            } else {
                impurity_left =
                    weighted_variance(left_sum, left_sum_sq, left_weight);
                impurity_right = weighted_variance(
                    total_sum - left_sum, total_sum_sq - left_sum_sq,
                    right_weight);
            }

            const double gain =
                parent_impurity -
                (left_weight / node_weight) * impurity_left -
                (right_weight / node_weight) * impurity_right;
            if (gain > best.gain) {
                best.valid = true;
                best.feature = feature;
                best.categorical = true;
                best.threshold = 0.0;
                best.categories.clear();
                best.categories.reserve(prefix);
                for (std::size_t index = 0; index < prefix; ++index) {
                    best.categories.push_back(ordered[index].value);
                }
                best.gain = gain;
            }
        }
    }

    return best;
}

// A greedy root split can have exactly zero impurity gain while making a
// deeper split possible. Balanced XOR is the canonical example: every single
// axis split leaves two 50/50 children, but a second split on the other axis
// makes them pure. When the best split has no immediate gain, accept it only
// if at least one of its children has a positive-gain split and the depth
// budget still allows that child to split. This keeps zero-gain splits from
// being emitted on noise while letting the tree learn XOR and other
// parity-shaped boundaries.
bool subset_offers_gain(const std::vector<std::size_t>& indices,
                        const build_context& context,
                        const decision_tree_config& config) {
    if (config.min_samples_leaf > indices.size() / 2) {
        return false;
    }
    const subset_statistics stats =
        compute_subset_statistics(indices, context);
    if (!(stats.weight > 0.0) || stats.impurity <= 0.0) {
        return false;
    }
    const split_candidate candidate =
        find_best_split(indices, context, 0, stats.weight, stats.impurity,
                        stats.class_counts, stats.sum, stats.sum_sq);
    return candidate.valid && candidate.gain > config.min_gain;
}

void build_node(std::vector<decision_tree_node>& nodes,
                std::size_t node_index, std::vector<std::size_t> indices,
                std::size_t depth, const build_context& context,
                const decision_tree_config& config,
                const std::vector<double>& classes) {
    double node_weight = 0.0;
    double target_sum = 0.0;
    double target_sum_sq = 0.0;
    std::vector<double> total_counts(context.class_count, 0.0);
    for (std::size_t row : indices) {
        const double weight = context.weights[row];
        node_weight += weight;
        const double target = context.targets[row];
        target_sum += weight * target;
        target_sum_sq += weight * target * target;
        if (context.classification) {
            total_counts[context.class_of[row]] += weight;
        }
    }

    nodes[node_index].samples = indices.size();
    nodes[node_index].weight = node_weight;

    if (context.classification) {
        std::vector<double>& probabilities =
            nodes[node_index].class_probabilities;
        probabilities.assign(context.class_count, 0.0);
        double best_count = -1.0;
        std::size_t best_class = 0;
        for (std::size_t c = 0; c < context.class_count; ++c) {
            const double count = total_counts[c];
            if (count > best_count) {
                best_count = count;
                best_class = c;
            }
            probabilities[c] =
                node_weight > 0.0 ? count / node_weight : 0.0;
        }
        nodes[node_index].predicted_class = best_class;
        nodes[node_index].value = classes[best_class];
        nodes[node_index].impurity =
            weighted_gini(total_counts, node_weight);
        nodes[node_index].leaf_error =
            node_weight > 0.0 ? node_weight - best_count : 0.0;
    } else {
        const double mean = node_weight > 0.0 ? target_sum / node_weight : 0.0;
        nodes[node_index].value = mean;
        nodes[node_index].impurity =
            weighted_variance(target_sum, target_sum_sq, node_weight);
        nodes[node_index].leaf_error =
            node_weight > 0.0
                ? std::max(0.0, target_sum_sq -
                                    target_sum * target_sum / node_weight)
                : 0.0;
    }

    if (node_weight <= 0.0) {
        return;
    }
    if (depth >= config.max_depth) {
        return;
    }
    if (config.min_samples_leaf > indices.size() / 2) {
        return;
    }
    if (nodes[node_index].impurity <= 0.0) {
        return;
    }

    const split_candidate best = find_best_split(
        indices, context, node_index, node_weight,
        nodes[node_index].impurity, total_counts, target_sum, target_sum_sq);
    if (!best.valid) {
        return;
    }

    std::vector<std::size_t> left_indices;
    std::vector<std::size_t> right_indices;
    left_indices.reserve(indices.size() / 2);
    right_indices.reserve(indices.size() / 2);
    for (std::size_t row : indices) {
        const double value =
            context.features[row * context.cols + best.feature];
        const bool goes_left =
            best.categorical ? contains_value(best.categories, value)
                             : value <= best.threshold;
        if (goes_left) {
            left_indices.push_back(row);
        } else {
            right_indices.push_back(row);
        }
    }
    if (left_indices.empty() || right_indices.empty()) {
        return;
    }
    if (best.gain <= config.min_gain) {
        if (depth + 2 > config.max_depth) {
            return;
        }
        const bool left_gain =
            subset_offers_gain(left_indices, context, config);
        const bool right_gain =
            subset_offers_gain(right_indices, context, config);
        if (!left_gain && !right_gain) {
            return;
        }
    }

    nodes[node_index].leaf = false;
    nodes[node_index].categorical = best.categorical;
    nodes[node_index].feature = best.feature;
    nodes[node_index].threshold = best.threshold;
    nodes[node_index].categories = best.categories;
    nodes[node_index].gain = best.gain;

    const std::size_t left_index = nodes.size();
    nodes.push_back(decision_tree_node{});
    nodes[node_index].left = left_index;
    build_node(nodes, left_index, std::move(left_indices), depth + 1, context,
               config, classes);

    const std::size_t right_index = nodes.size();
    nodes.push_back(decision_tree_node{});
    nodes[node_index].right = right_index;
    build_node(nodes, right_index, std::move(right_indices), depth + 1, context,
               config, classes);
}

void compact_tree(std::vector<decision_tree_node>& nodes) {
    if (nodes.empty()) {
        return;
    }
    std::vector<decision_tree_node> compacted;
    compacted.reserve(nodes.size());
    const auto copy_node = [&nodes, &compacted](const auto& self,
                                                std::size_t old_index)
        -> std::size_t {
        const decision_tree_node original = nodes[old_index];
        const std::size_t new_index = compacted.size();
        compacted.push_back(original);
        if (!original.leaf) {
            const std::size_t left = self(self, original.left);
            const std::size_t right = self(self, original.right);
            compacted[new_index].left = left;
            compacted[new_index].right = right;
        } else {
            compacted[new_index].left = decision_tree_no_child;
            compacted[new_index].right = decision_tree_no_child;
            compacted[new_index].categories.clear();
        }
        return new_index;
    };
    copy_node(copy_node, 0);
    nodes = std::move(compacted);
}

void prune_tree(std::vector<decision_tree_node>& nodes, double ccp_alpha) {
    if (nodes.empty()) {
        return;
    }
    std::vector<std::size_t> leaf_counts(nodes.size(), 1);
    std::vector<double> subtree_errors(nodes.size(), 0.0);
    // Parent indices always precede their descendants, so a reverse sweep is
    // a bottom-up traversal.
    for (std::size_t index = nodes.size(); index-- > 0;) {
        if (nodes[index].leaf) {
            leaf_counts[index] = 1;
            subtree_errors[index] = nodes[index].leaf_error;
            continue;
        }
        const std::size_t left = nodes[index].left;
        const std::size_t right = nodes[index].right;
        const std::size_t leaves = leaf_counts[left] + leaf_counts[right];
        const double subtree_error =
            subtree_errors[left] + subtree_errors[right];
        const double penalty =
            ccp_alpha * static_cast<double>(leaves > 0 ? leaves - 1 : 0);
        if (nodes[index].leaf_error <= subtree_error + penalty) {
            decision_tree_node& node = nodes[index];
            node.leaf = true;
            node.categorical = false;
            node.left = decision_tree_no_child;
            node.right = decision_tree_no_child;
            node.categories.clear();
            node.gain = 0.0;
            leaf_counts[index] = 1;
            subtree_errors[index] = node.leaf_error;
        } else {
            leaf_counts[index] = leaves;
            subtree_errors[index] = subtree_error;
        }
    }
    compact_tree(nodes);
}

const decision_tree_node* find_leaf(
    const std::vector<decision_tree_node>& nodes,
    crpc::span<const double> features) {
    std::size_t index = 0;
    for (std::size_t guard = 0; guard <= nodes.size(); ++guard) {
        if (index >= nodes.size()) {
            return nullptr;
        }
        const decision_tree_node& node = nodes[index];
        if (node.leaf) {
            return &node;
        }
        const bool goes_left =
            node.categorical
                ? contains_value(node.categories, features[node.feature])
                : features[node.feature] <= node.threshold;
        index = goes_left ? node.left : node.right;
    }
    return nullptr;
}

bool all_finite(const std::vector<double>& values) {
    for (double value : values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

} // namespace

decision_tree::decision_tree(decision_tree_config config)
    : config_(std::move(config)) {}

status decision_tree::train(const decision_tree_dataset& dataset) {
    trained_ = false;
    nodes_.clear();
    classes_.clear();
    feature_count_ = 0;

    if (!std::isfinite(config_.min_gain) || config_.min_gain < 0.0) {
        return status(rpc_code::invalid_argument,
                      "decision_tree: min_gain must be finite and >= 0");
    }
    if (!std::isfinite(config_.ccp_alpha) || config_.ccp_alpha < 0.0) {
        return status(rpc_code::invalid_argument,
                      "decision_tree: ccp_alpha must be finite and >= 0");
    }
    if (config_.min_samples_leaf == 0) {
        return status(rpc_code::invalid_argument,
                      "decision_tree: min_samples_leaf must be >= 1");
    }
    if (dataset.rows == 0) {
        return status(rpc_code::invalid_argument,
                      "decision_tree: dataset is empty");
    }
    if (dataset.cols == 0) {
        return status(rpc_code::invalid_argument,
                      "decision_tree: dataset has no features");
    }
    if (dataset.features.size() % dataset.cols != 0 ||
        dataset.features.size() / dataset.cols != dataset.rows) {
        return status(rpc_code::invalid_argument,
                      "decision_tree: feature matrix shape does not match rows");
    }
    if (dataset.targets.size() != dataset.rows) {
        return status(rpc_code::invalid_argument,
                      "decision_tree: target count does not match rows");
    }
    if (!dataset.weights.empty() && dataset.weights.size() != dataset.rows) {
        return status(rpc_code::invalid_argument,
                      "decision_tree: weight count does not match rows");
    }
    if (!all_finite(dataset.features) || !all_finite(dataset.targets)) {
        return status(rpc_code::invalid_argument,
                      "decision_tree: features and targets must be finite");
    }

    std::vector<char> categorical(dataset.cols, 0);
    for (std::size_t feature : config_.categorical_features) {
        if (feature >= dataset.cols) {
            return status(rpc_code::invalid_argument,
                          "decision_tree: categorical feature index out of range");
        }
        categorical[feature] = 1;
    }

    std::vector<double> weights;
    if (dataset.weights.empty()) {
        weights.assign(dataset.rows, 1.0);
    } else {
        weights = dataset.weights;
    }
    double total_weight = 0.0;
    for (double weight : weights) {
        if (!std::isfinite(weight) || weight < 0.0) {
            return status(rpc_code::invalid_argument,
                          "decision_tree: sample weights must be finite and >= 0");
        }
        total_weight += weight;
    }
    if (!(total_weight > 0.0)) {
        return status(rpc_code::invalid_argument,
                      "decision_tree: sample weights sum to zero");
    }

    std::vector<std::size_t> class_of(dataset.rows, 0);
    std::size_t class_count = 0;
    const bool classification = config_.task == decision_tree_task::classification;
    if (classification) {
        std::vector<double> sorted_targets = dataset.targets;
        std::sort(sorted_targets.begin(), sorted_targets.end());
        sorted_targets.erase(
            std::unique(sorted_targets.begin(), sorted_targets.end()),
            sorted_targets.end());
        classes_ = sorted_targets;
        class_count = classes_.size();
        for (std::size_t row = 0; row < dataset.rows; ++row) {
            const auto position = std::lower_bound(classes_.begin(),
                                                   classes_.end(),
                                                   dataset.targets[row]);
            class_of[row] =
                static_cast<std::size_t>(position - classes_.begin());
        }
    }

    const build_context context{dataset.features,
                                dataset.targets,
                                weights,
                                class_of,
                                categorical,
                                dataset.cols,
                                class_count,
                                classification,
                                config_.min_samples_leaf,
                                config_.max_features};

    std::vector<std::size_t> indices(dataset.rows);
    std::iota(indices.begin(), indices.end(), static_cast<std::size_t>(0));

    nodes_.clear();
    nodes_.reserve(dataset.rows + 1);
    nodes_.push_back(decision_tree_node{});
    build_node(nodes_, 0, std::move(indices), 0, context, config_, classes_);

    if (config_.ccp_alpha > 0.0) {
        prune_tree(nodes_, config_.ccp_alpha);
    }

    feature_count_ = dataset.cols;
    trained_ = true;
    return status{};
}

result<double> decision_tree::predict(
    crpc::span<const double> features) const {
    if (!trained_) {
        return rpc_error(rpc_code::failed_precondition,
                         "decision_tree: model is not trained");
    }
    if (features.size() != feature_count_) {
        return rpc_error(rpc_code::invalid_argument,
                         "decision_tree: feature count does not match the model");
    }
    const decision_tree_node* leaf = find_leaf(nodes_, features);
    if (leaf == nullptr) {
        return rpc_error(rpc_code::internal,
                         "decision_tree: corrupted tree traversal");
    }
    return leaf->value;
}

result<std::vector<double>> decision_tree::predict_proba(
    crpc::span<const double> features) const {
    if (!trained_ ||
        config_.task != decision_tree_task::classification) {
        return rpc_error(rpc_code::failed_precondition,
                         "decision_tree: predict_proba requires a trained "
                         "classification model");
    }
    if (features.size() != feature_count_) {
        return rpc_error(rpc_code::invalid_argument,
                         "decision_tree: feature count does not match the model");
    }
    const decision_tree_node* leaf = find_leaf(nodes_, features);
    if (leaf == nullptr) {
        return rpc_error(rpc_code::internal,
                         "decision_tree: corrupted tree traversal");
    }
    return leaf->class_probabilities;
}

std::size_t decision_tree::depth() const {
    if (nodes_.empty()) {
        return 0;
    }
    std::vector<std::pair<std::size_t, std::size_t>> stack;
    stack.push_back({0, 0});
    std::size_t maximum = 0;
    while (!stack.empty()) {
        const std::size_t index = stack.back().first;
        const std::size_t depth = stack.back().second;
        stack.pop_back();
        if (index >= nodes_.size()) {
            continue;
        }
        if (depth > maximum) {
            maximum = depth;
        }
        const decision_tree_node& node = nodes_[index];
        if (!node.leaf) {
            stack.push_back({node.left, depth + 1});
            stack.push_back({node.right, depth + 1});
        }
    }
    return maximum;
}

} // namespace ai
} // namespace game
} // namespace crpc
