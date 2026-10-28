// CRPC -- tests for crpc/game/ai: binary discrete AdaBoost over stumps.
//
// Two properties are asserted empirically rather than assumed:
//
//   1. On a problem no single stump can solve (3-bit majority), the boosted
//      ensemble is substantially more accurate than one stump.
//   2. The unweighted training error of the ensemble is monotone
//      non-increasing round by round. That is a consequence of the correct
//      exponential weight update; a regression in the update shows up here.
//
// XOR is included explicitly, but with depth-2 weak learners. A depth-1 stump
// has zero mutual information with each axis of a balanced XOR (both sides of
// every axis split are 50/50), so no stump can beat chance and boosting a stump
// cannot start: that is asserted separately. The depth-2 tree can represent
// the XOR boundary, and the test compares the boosted model with a single
// stump to keep the required stump-vs-boosted contrast.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <crpc/game/ai/adaboost.hpp>
#include <crpc/game/ai/decision_tree.hpp>

#include "test_framework.hpp"

namespace {

using crpc::game::ai::adaboost_classifier;
using crpc::game::ai::adaboost_config;
using crpc::game::ai::decision_tree;
using crpc::game::ai::decision_tree_config;
using crpc::game::ai::decision_tree_dataset;

double accuracy(const adaboost_classifier& model,
                const decision_tree_dataset& data) {
    std::size_t correct = 0;
    for (std::size_t row = 0; row < data.rows; ++row) {
        const crpc::span<const double> features(
            data.features.data() + row * data.cols, data.cols);
        const auto prediction = model.predict(features);
        if (!prediction.ok()) {
            return -1.0;
        }
        if (prediction.value() == data.targets[row]) {
            ++correct;
        }
    }
    return static_cast<double>(correct) / static_cast<double>(data.rows);
}

double stump_accuracy(const decision_tree_dataset& data) {
    decision_tree_config config;
    config.max_depth = 1;
    config.min_gain = 0.0;
    decision_tree stump(config);
    if (!stump.train(data).ok()) {
        return -1.0;
    }
    std::size_t correct = 0;
    for (std::size_t row = 0; row < data.rows; ++row) {
        const crpc::span<const double> features(
            data.features.data() + row * data.cols, data.cols);
        const auto prediction = stump.predict(features);
        if (!prediction.ok()) {
            return -1.0;
        }
        if (prediction.value() == data.targets[row]) {
            ++correct;
        }
    }
    return static_cast<double>(correct) / static_cast<double>(data.rows);
}

// Majority of three bits. One stump reaches exactly 75%; the combined
// ensemble should reach 100%.
decision_tree_dataset make_majority3(std::size_t repeats) {
    decision_tree_dataset data;
    data.cols = 3;
    for (int mask = 0; mask < 8; ++mask) {
        const double x0 = static_cast<double>((mask >> 2) & 1);
        const double x1 = static_cast<double>((mask >> 1) & 1);
        const double x2 = static_cast<double>(mask & 1);
        const double label = (x0 + x1 + x2 >= 2.0) ? 1.0 : 0.0;
        for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
            data.features.push_back(x0);
            data.features.push_back(x1);
            data.features.push_back(x2);
            data.targets.push_back(label);
        }
    }
    data.rows = data.targets.size();
    return data;
}

decision_tree_dataset make_balanced_xor(std::size_t repeats) {
    static const double corners[4][2] = {{0.0, 0.0}, {0.0, 1.0},
                                         {1.0, 0.0}, {1.0, 1.0}};
    decision_tree_dataset data;
    data.cols = 2;
    for (std::size_t corner = 0; corner < 4; ++corner) {
        const double label = (corner == 1 || corner == 2) ? 1.0 : 0.0;
        for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
            data.features.push_back(corners[corner][0]);
            data.features.push_back(corners[corner][1]);
            data.targets.push_back(label);
        }
    }
    data.rows = data.targets.size();
    return data;
}

void check_monotone_training_error(const adaboost_classifier& model) {
    const auto& rounds = model.rounds();
    CRPC_CHECK(!rounds.empty());
    for (std::size_t index = 1; index < rounds.size(); ++index) {
        CRPC_CHECK(rounds[index].training_error <=
                   rounds[index - 1].training_error + 1e-12);
    }
    for (std::size_t index = 0; index < rounds.size(); ++index) {
        CRPC_CHECK_EQ(index, rounds[index].index);
        CRPC_CHECK(rounds[index].alpha > 0.0);
        CRPC_CHECK(rounds[index].weighted_error > 0.0);
        CRPC_CHECK(rounds[index].weighted_error < 0.5);
        CRPC_CHECK(rounds[index].training_accuracy >= 0.0);
        CRPC_CHECK(rounds[index].training_accuracy <= 1.0);
    }
}

CRPC_TEST(adaboost_majority3_beats_single_stump) {
    const decision_tree_dataset data = make_majority3(50);

    const double single = stump_accuracy(data);
    CRPC_CHECK(single >= 0.70);
    CRPC_CHECK(single <= 0.80);

    adaboost_config config;
    config.max_rounds = 30;
    config.target_accuracy = 1.0;
    adaboost_classifier model(config);
    const crpc::status trained = model.train(data);
    CRPC_CHECK(trained.ok());
    CRPC_CHECK(model.trained());
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), model.classes().size());
    CRPC_CHECK_EQ(model.rounds().size(), model.weak_learners().size());
    CRPC_CHECK(model.rounds().size() >= 2);

    const double boosted = accuracy(model, data);
    std::printf("adaboost majority3: stump=%.4f boosted=%.4f rounds=%zu\n",
                single, boosted, model.rounds().size());
    for (const auto& round : model.rounds()) {
        std::printf("  round %zu alpha=%.6f weighted_error=%.6f "
                    "training_error=%.6f\n",
                    round.index, round.alpha, round.weighted_error,
                    round.training_error);
    }
    std::fflush(stdout);
    CRPC_CHECK(boosted >= 0.99);
    CRPC_CHECK(boosted - single >= 0.20);
    check_monotone_training_error(model);
}

CRPC_TEST(adaboost_xor_beats_single_stump_with_depth_two_weak_learners) {
    const decision_tree_dataset data = make_balanced_xor(25);

    const double single = stump_accuracy(data);
    CRPC_CHECK_EQ(0.5, single);

    adaboost_config config;
    config.max_rounds = 10;
    config.target_accuracy = 1.0;
    config.weak_max_depth = 2;
    adaboost_classifier model(config);
    CRPC_CHECK(model.train(data).ok());
    CRPC_CHECK(model.rounds().size() >= 1);

    const double boosted = accuracy(model, data);
    std::printf("adaboost xor: stump=%.4f boosted=%.4f rounds=%zu\n", single,
                boosted, model.rounds().size());
    std::fflush(stdout);
    CRPC_CHECK_EQ(1.0, boosted);
    CRPC_CHECK(boosted - single >= 0.40);
    CRPC_CHECK_EQ(0.0, model.rounds().back().training_error);
}

CRPC_TEST(adaboost_balanced_xor_stops_without_weak_learning) {
    // Every stump has exactly 0.5 weighted error here, so alpha would be zero
    // and the weight distribution would never change. Stopping is the correct
    // behaviour; looping would burn rounds without changing the model.
    const decision_tree_dataset data = make_balanced_xor(25);
    adaboost_config config;
    config.max_rounds = 10;
    adaboost_classifier model(config);
    CRPC_CHECK(model.train(data).ok());
    CRPC_CHECK(model.rounds().empty());
    CRPC_CHECK(model.weak_learners().empty());
    CRPC_CHECK_EQ(0.5, accuracy(model, data));
}

CRPC_TEST(adaboost_early_stop_on_target_accuracy) {
    const decision_tree_dataset data = make_majority3(50);
    adaboost_config config;
    config.max_rounds = 30;
    config.target_accuracy = 0.75;
    adaboost_classifier model(config);
    CRPC_CHECK(model.train(data).ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), model.rounds().size());
    CRPC_CHECK(accuracy(model, data) >= 0.75);
}

CRPC_TEST(adaboost_perfect_stump_stops_at_zero_error) {
    decision_tree_dataset data;
    data.cols = 1;
    for (int i = 0; i < 20; ++i) {
        data.features.push_back(static_cast<double>(i % 2));
        data.targets.push_back(static_cast<double>(i % 2));
    }
    data.rows = 20;

    adaboost_config config;
    config.max_rounds = 10;
    adaboost_classifier model(config);
    CRPC_CHECK(model.train(data).ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), model.rounds().size());
    CRPC_CHECK_EQ(0.0, model.rounds()[0].weighted_error);
    CRPC_CHECK_EQ(0.0, model.rounds()[0].training_error);
    CRPC_CHECK_EQ(1.0, accuracy(model, data));
}

CRPC_TEST(adaboost_decision_function_matches_prediction) {
    const decision_tree_dataset data = make_majority3(10);
    adaboost_classifier model;
    CRPC_CHECK(model.train(data).ok());
    for (std::size_t row = 0; row < data.rows; ++row) {
        const crpc::span<const double> features(
            data.features.data() + row * data.cols, data.cols);
        const auto prediction = model.predict(features);
        const auto score = model.decision_function(features);
        CRPC_CHECK(prediction.ok());
        CRPC_CHECK(score.ok());
        if (!prediction.ok() || !score.ok()) {
            continue;
        }
        const double expected = score.value() > 0.0 ? model.classes()[1]
                                                    : model.classes()[0];
        CRPC_CHECK_EQ(expected, prediction.value());
    }
}

CRPC_TEST(adaboost_rejects_invalid_datasets) {
    adaboost_classifier model;

    decision_tree_dataset empty;
    empty.cols = 2;
    CRPC_CHECK(model.train(empty).code() == crpc::rpc_code::invalid_argument);

    decision_tree_dataset one_class;
    one_class.cols = 1;
    one_class.rows = 4;
    one_class.features = {0.0, 1.0, 2.0, 3.0};
    one_class.targets = {1.0, 1.0, 1.0, 1.0};
    CRPC_CHECK(model.train(one_class).code() ==
               crpc::rpc_code::invalid_argument);

    decision_tree_dataset three_classes;
    three_classes.cols = 1;
    three_classes.rows = 3;
    three_classes.features = {0.0, 1.0, 2.0};
    three_classes.targets = {0.0, 1.0, 2.0};
    CRPC_CHECK(model.train(three_classes).code() ==
               crpc::rpc_code::invalid_argument);
}

CRPC_TEST(adaboost_rejects_invalid_config) {
    const decision_tree_dataset data = make_majority3(2);

    adaboost_config zero_rounds;
    zero_rounds.max_rounds = 0;
    adaboost_classifier no_rounds(zero_rounds);
    CRPC_CHECK(no_rounds.train(data).code() ==
               crpc::rpc_code::invalid_argument);

    adaboost_config bad_target;
    bad_target.target_accuracy = 1.5;
    adaboost_classifier bad_target_model(bad_target);
    CRPC_CHECK(bad_target_model.train(data).code() ==
               crpc::rpc_code::invalid_argument);
}

CRPC_TEST(adaboost_predict_before_training) {
    adaboost_classifier model;
    const double row[3] = {0.0, 0.0, 0.0};
    const auto prediction = model.predict(crpc::span<const double>(row, 3));
    CRPC_CHECK_FALSE(prediction.ok());
    CRPC_CHECK(prediction.code() == crpc::rpc_code::failed_precondition);
    const auto score = model.decision_function(crpc::span<const double>(row, 3));
    CRPC_CHECK_FALSE(score.ok());
    CRPC_CHECK(score.code() == crpc::rpc_code::failed_precondition);
}

CRPC_TEST(adaboost_wrong_feature_count) {
    const decision_tree_dataset data = make_majority3(5);
    adaboost_classifier model;
    CRPC_CHECK(model.train(data).ok());
    const double row[2] = {0.0, 0.0};
    const auto prediction = model.predict(crpc::span<const double>(row, 2));
    CRPC_CHECK_FALSE(prediction.ok());
    CRPC_CHECK(prediction.code() == crpc::rpc_code::invalid_argument);
}

} // namespace

CRPC_TEST_MAIN()