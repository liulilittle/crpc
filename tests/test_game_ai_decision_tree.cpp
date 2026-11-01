// CRPC -- tests for crpc/game/ai: CART decision tree.
//
// The suite is deliberately data-driven rather than shape-driven:
//
//   * a linearly separable 2-D problem must reach 100% training accuracy;
//   * XOR must be learned only when the depth allows the second split;
//   * a stump on XOR must stay at chance, which proves the depth limit is
//     enforced by the trainer and not just stored in the config;
//   * a 10k x 20 training run is exercised for correctness; its wall time is
//     reported by bench/bench_algorithms.cpp, not asserted here (plan.md D3).
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <crpc/game/ai/decision_tree.hpp>

#include "test_framework.hpp"

namespace {

using crpc::game::ai::decision_tree;
using crpc::game::ai::decision_tree_config;
using crpc::game::ai::decision_tree_dataset;
using crpc::game::ai::decision_tree_task;

decision_tree_dataset make_dataset(std::size_t cols, std::vector<double> features,
                                   std::vector<double> targets) {
    decision_tree_dataset data;
    data.cols = cols;
    data.rows = targets.size();
    data.features = std::move(features);
    data.targets = std::move(targets);
    return data;
}

// Four corners, each repeated `repeats` times. Labels are XOR of the two
// binary coordinates, so one axis-aligned split can never do better than 50%.
decision_tree_dataset make_xor(std::size_t repeats) {
    static const double corners[4][2] = {{0.0, 0.0}, {0.0, 1.0},
                                         {1.0, 0.0}, {1.0, 1.0}};
    decision_tree_dataset data;
    data.cols = 2;
    for (std::size_t corner = 0; corner < 4; ++corner) {
        const double label = (corner == 1 || corner == 2) ? 1.0 : 0.0;
        for (std::size_t r = 0; r < repeats; ++r) {
            data.features.push_back(corners[corner][0]);
            data.features.push_back(corners[corner][1]);
            data.targets.push_back(label);
        }
    }
    data.rows = data.targets.size();
    return data;
}

// label = (x + y > 1). This needs at least two axis-aligned splits.
decision_tree_dataset make_diagonal() {
    decision_tree_dataset data;
    data.cols = 2;
    for (int ix = 0; ix <= 10; ++ix) {
        for (int iy = 0; iy <= 10; ++iy) {
            const double x = static_cast<double>(ix) / 10.0;
            const double y = static_cast<double>(iy) / 10.0;
            data.features.push_back(x);
            data.features.push_back(y);
            data.targets.push_back((x + y > 1.0) ? 1.0 : 0.0);
        }
    }
    data.rows = data.targets.size();
    return data;
}

double accuracy(const decision_tree& tree, const decision_tree_dataset& data) {
    std::size_t correct = 0;
    for (std::size_t row = 0; row < data.rows; ++row) {
        const crpc::span<const double> features(
            data.features.data() + row * data.cols, data.cols);
        const auto prediction = tree.predict(features);
        if (!prediction.ok()) {
            return -1.0;
        }
        if (prediction.value() == data.targets[row]) {
            ++correct;
        }
    }
    return static_cast<double>(correct) / static_cast<double>(data.rows);
}

double mean_squared_error(const decision_tree& tree,
                          const decision_tree_dataset& data) {
    double total = 0.0;
    for (std::size_t row = 0; row < data.rows; ++row) {
        const crpc::span<const double> features(
            data.features.data() + row * data.cols, data.cols);
        const auto prediction = tree.predict(features);
        if (!prediction.ok()) {
            return -1.0;
        }
        const double diff = prediction.value() - data.targets[row];
        total += diff * diff;
    }
    return total / static_cast<double>(data.rows);
}

// Walks the reachable tree, checks the structural invariants the visualiser
// relies on, and returns the observed depth. Every node must be reachable:
// pruning compacts the storage, so an orphan is a bug.
std::size_t tree_depth_and_check(const decision_tree& tree) {
    const auto& nodes = tree.nodes();
    CRPC_CHECK(!nodes.empty());
    std::vector<bool> seen(nodes.size(), false);
    std::vector<std::pair<std::size_t, std::size_t>> stack;
    stack.push_back({tree.root(), 0});
    std::size_t max_depth = 0;
    while (!stack.empty()) {
        const std::size_t index = stack.back().first;
        const std::size_t depth = stack.back().second;
        stack.pop_back();
        CRPC_CHECK(index < nodes.size());
        if (seen[index]) {
            continue;
        }
        seen[index] = true;
        if (depth > max_depth) {
            max_depth = depth;
        }
        const auto& node = nodes[index];
        if (!node.leaf) {
            CRPC_CHECK(node.feature < tree.feature_count());
            CRPC_CHECK(node.left < nodes.size());
            CRPC_CHECK(node.right < nodes.size());
            CRPC_CHECK(node.left != node.right);
            CRPC_CHECK(node.left != crpc::game::ai::decision_tree_no_child);
            stack.push_back({node.left, depth + 1});
            stack.push_back({node.right, depth + 1});
        } else {
            CRPC_CHECK(node.left == crpc::game::ai::decision_tree_no_child);
            CRPC_CHECK(node.right == crpc::game::ai::decision_tree_no_child);
        }
    }
    std::size_t reachable = 0;
    for (bool value : seen) {
        if (value) {
            ++reachable;
        }
    }
    CRPC_CHECK_EQ(nodes.size(), reachable);
    return max_depth;
}

CRPC_TEST(decision_tree_linearly_separable) {
    const decision_tree_dataset data = make_diagonal();

    decision_tree_config shallow_config;
    shallow_config.max_depth = 1;
    decision_tree shallow(shallow_config);
    CRPC_CHECK(shallow.train(data).ok());
    const double shallow_accuracy = accuracy(shallow, data);
    CRPC_CHECK(shallow_accuracy < 1.0);

    decision_tree_config config;
    config.max_depth = 8;
    decision_tree tree(config);
    CRPC_CHECK(tree.train(data).ok());
    const double train_accuracy = accuracy(tree, data);
    CRPC_CHECK_EQ(1.0, train_accuracy);
    CRPC_CHECK(tree_depth_and_check(tree) >= 2);
}

CRPC_TEST(decision_tree_xor_needs_two_levels) {
    const decision_tree_dataset data = make_xor(25);

    decision_tree_config shallow_config;
    shallow_config.max_depth = 1;
    decision_tree shallow(shallow_config);
    CRPC_CHECK(shallow.train(data).ok());
    const double shallow_accuracy = accuracy(shallow, data);
    CRPC_CHECK(shallow_accuracy <= 0.5000001);
    CRPC_CHECK(shallow.nodes().size() <= 3);

    decision_tree_config deep_config2;
    deep_config2.max_depth = 1;
    deep_config2.min_gain = 0.0;
    decision_tree deep2(deep_config2);
    CRPC_CHECK(deep2.train(data).ok());
    CRPC_CHECK_EQ(0.5, accuracy(deep2, data));
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), deep2.nodes().size());

    decision_tree_config deep_config;
    deep_config.max_depth = 4;
    decision_tree deep(deep_config);
    CRPC_CHECK(deep.train(data).ok());
    CRPC_CHECK_EQ(1.0, accuracy(deep, data));
    CRPC_CHECK(tree_depth_and_check(deep) >= 2);
    CRPC_CHECK(deep.nodes().size() >= 7);
}

CRPC_TEST(decision_tree_depth_zero_is_a_leaf) {
    const decision_tree_dataset data = make_xor(10);
    decision_tree_config config;
    config.max_depth = 0;
    decision_tree tree(config);
    CRPC_CHECK(tree.train(data).ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), tree.nodes().size());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), tree_depth_and_check(tree));
    CRPC_CHECK(tree.nodes()[tree.root()].leaf);
}

CRPC_TEST(decision_tree_min_gain_stops_splitting) {
    const decision_tree_dataset data = make_xor(10);
    decision_tree_config config;
    config.max_depth = 4;
    config.min_gain = 10.0;
    decision_tree tree(config);
    CRPC_CHECK(tree.train(data).ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), tree.nodes().size());
}

CRPC_TEST(decision_tree_min_samples_leaf_prevents_small_split) {
    const decision_tree_dataset data = make_xor(25); // 100 rows
    decision_tree_config config;
    config.max_depth = 4;
    config.min_samples_leaf = 60;
    decision_tree tree(config);
    CRPC_CHECK(tree.train(data).ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), tree.nodes().size());
}

CRPC_TEST(decision_tree_single_class_is_a_leaf) {
    decision_tree_dataset data;
    data.cols = 2;
    for (int i = 0; i < 20; ++i) {
        data.features.push_back(static_cast<double>(i % 2));
        data.features.push_back(static_cast<double>((i / 2) % 2));
        data.targets.push_back(1.0);
    }
    data.rows = 20;

    decision_tree tree;
    CRPC_CHECK(tree.train(data).ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), tree.nodes().size());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), tree.classes().size());
    CRPC_CHECK_EQ(1.0, accuracy(tree, data));
}

CRPC_TEST(decision_tree_single_sample_is_a_leaf) {
    const decision_tree_dataset data =
        make_dataset(2, std::vector<double>{0.25, 0.75}, std::vector<double>{7.0});
    decision_tree tree;
    CRPC_CHECK(tree.train(data).ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), tree.nodes().size());
    const double row[2] = {0.25, 0.75};
    const auto prediction = tree.predict(crpc::span<const double>(row, 2));
    CRPC_CHECK(prediction.ok());
    if (prediction.ok()) {
        CRPC_CHECK_EQ(7.0, prediction.value());
    }
}

CRPC_TEST(decision_tree_empty_dataset_is_rejected) {
    decision_tree_dataset data;
    data.cols = 2;
    decision_tree tree;
    const crpc::status result = tree.train(data);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK(result.code() == crpc::rpc_code::invalid_argument);
    CRPC_CHECK_FALSE(tree.trained());
}

CRPC_TEST(decision_tree_inconsistent_shapes_are_rejected) {
    decision_tree_dataset data;
    data.cols = 2;
    data.rows = 2;
    data.features = {0.0, 0.0, 1.0}; // one short
    data.targets = {0.0, 1.0};
    decision_tree tree;
    const crpc::status result = tree.train(data);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK(result.code() == crpc::rpc_code::invalid_argument);
}

CRPC_TEST(decision_tree_constant_features_cannot_split) {
    decision_tree_dataset data;
    data.cols = 2;
    for (int i = 0; i < 100; ++i) {
        data.features.push_back(0.5);
        data.features.push_back(0.5);
        data.targets.push_back(static_cast<double>(i % 2));
    }
    data.rows = 100;

    decision_tree_config config;
    config.max_depth = 8;
    decision_tree tree(config);
    CRPC_CHECK(tree.train(data).ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), tree.nodes().size());
    CRPC_CHECK_EQ(0.5, accuracy(tree, data));
}

CRPC_TEST(decision_tree_categorical_subset_split) {
    decision_tree_dataset data;
    data.cols = 2;
    for (int category = 0; category < 3; ++category) {
        for (int repeat = 0; repeat < 10; ++repeat) {
            data.features.push_back(static_cast<double>(category));
            data.features.push_back(0.5);
            data.targets.push_back(category == 0 ? 0.0 : 1.0);
        }
    }
    data.rows = 30;

    decision_tree_config config;
    config.max_depth = 1;
    config.categorical_features = {0};
    decision_tree tree(config);
    CRPC_CHECK(tree.train(data).ok());
    CRPC_CHECK_EQ(1.0, accuracy(tree, data));
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), tree.nodes().size());

    const auto& root = tree.nodes()[tree.root()];
    CRPC_CHECK_FALSE(root.leaf);
    CRPC_CHECK(root.categorical);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), root.categories.size());
    if (!root.categories.empty()) {
        CRPC_CHECK_EQ(0.0, root.categories[0]);
    }

    const double unseen[2] = {99.0, 0.5};
    const auto prediction = tree.predict(crpc::span<const double>(unseen, 2));
    CRPC_CHECK(prediction.ok());
    if (prediction.ok()) {
        CRPC_CHECK_EQ(1.0, prediction.value());
    }
}

CRPC_TEST(decision_tree_regression) {
    decision_tree_dataset data;
    data.cols = 2;
    for (int a = 0; a < 2; ++a) {
        for (int b = 0; b < 2; ++b) {
            for (int repeat = 0; repeat < 25; ++repeat) {
                data.features.push_back(static_cast<double>(a));
                data.features.push_back(static_cast<double>(b));
                const double target = a == 0 ? (b == 0 ? -10.0 : -5.0)
                                             : (b == 0 ? 5.0 : 10.0);
                data.targets.push_back(target);
            }
        }
    }
    data.rows = 100;

    decision_tree_config config;
    config.task = decision_tree_task::regression;
    config.max_depth = 2;
    decision_tree tree(config);
    CRPC_CHECK(tree.train(data).ok());
    CRPC_CHECK(mean_squared_error(tree, data) < 1e-12);
    CRPC_CHECK_FALSE(tree.predict_proba(crpc::span<const double>(
                                         data.features.data(), 2))
                         .ok());
}

CRPC_TEST(decision_tree_cost_complexity_pruning) {
    const decision_tree_dataset data = make_xor(25);

    decision_tree_config kept_config;
    kept_config.max_depth = 4;
    kept_config.ccp_alpha = 0.0;
    decision_tree kept(kept_config);
    CRPC_CHECK(kept.train(data).ok());
    CRPC_CHECK(kept.nodes().size() >= 7);

    decision_tree_config pruned_config;
    pruned_config.max_depth = 4;
    pruned_config.ccp_alpha = 100.0;
    decision_tree pruned(pruned_config);
    CRPC_CHECK(pruned.train(data).ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), pruned.nodes().size());
    CRPC_CHECK_EQ(0.5, accuracy(pruned, data));
    CRPC_CHECK(tree_depth_and_check(pruned) == 0);
}

CRPC_TEST(decision_tree_training_is_fast_enough) {
    // 10,000 samples x 20 features. Half of the features are pure noise, so
    // the trainer has to rank all of them at every node.
    constexpr std::size_t k_rows = 10000;
    constexpr std::size_t k_cols = 20;
    decision_tree_dataset data;
    data.cols = k_cols;
    data.features.reserve(k_rows * k_cols);
    data.targets.reserve(k_rows);

    std::uint64_t state = 88172645463325252ull;
    auto next_unit = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<double>(state >> 11) *
               (1.0 / 9007199254740992.0);
    };
    for (std::size_t row = 0; row < k_rows; ++row) {
        double sum = 0.0;
        for (std::size_t col = 0; col < k_cols; ++col) {
            const double value = next_unit();
            data.features.push_back(value);
            if (col < 3) {
                sum += value;
            }
        }
        data.targets.push_back(sum > 1.5 ? 1.0 : 0.0);
    }
    data.rows = k_rows;

    decision_tree_config config;
    config.max_depth = 8;
    config.min_samples_leaf = 5;
    config.min_gain = 1e-7;
    decision_tree tree(config);

    const auto started = std::chrono::steady_clock::now();
    const crpc::status trained = tree.train(data);
    const auto finished = std::chrono::steady_clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(finished - started)
            .count();

    const double train_accuracy = accuracy(tree, data);
    std::printf("decision_tree perf: 10000x20 training=%lld ms accuracy=%.4f "
                "nodes=%zu depth=%zu\n",
                static_cast<long long>(elapsed_ms), train_accuracy,
                tree.nodes().size(), tree_depth_and_check(tree));
    std::fflush(stdout);

    CRPC_CHECK(trained.ok());
    CRPC_CHECK(train_accuracy > 0.90);
    // The 20 s wall-clock bound was a degeneration gate with no reachable
    // V_bad; it lives in bench/bench_algorithms.cpp now (plan.md D3).
}

CRPC_TEST(decision_tree_predict_before_training) {
    decision_tree tree;
    const double row[2] = {0.0, 0.0};
    const auto prediction = tree.predict(crpc::span<const double>(row, 2));
    CRPC_CHECK_FALSE(prediction.ok());
    CRPC_CHECK(prediction.code() == crpc::rpc_code::failed_precondition);
}

CRPC_TEST(decision_tree_wrong_feature_count) {
    const decision_tree_dataset data = make_xor(5);
    decision_tree tree;
    CRPC_CHECK(tree.train(data).ok());
    const double row[3] = {0.0, 0.0, 0.0};
    const auto prediction = tree.predict(crpc::span<const double>(row, 3));
    CRPC_CHECK_FALSE(prediction.ok());
    CRPC_CHECK(prediction.code() == crpc::rpc_code::invalid_argument);
}

CRPC_TEST(decision_tree_non_finite_input_is_rejected) {
    decision_tree_dataset data;
    data.cols = 2;
    data.rows = 1;
    data.features = {0.0, std::numeric_limits<double>::quiet_NaN()};
    data.targets = {0.0};
    decision_tree tree;
    const crpc::status result = tree.train(data);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK(result.code() == crpc::rpc_code::invalid_argument);
}

} // namespace

CRPC_TEST_MAIN()