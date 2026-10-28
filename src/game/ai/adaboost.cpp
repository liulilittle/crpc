// CRPC -- binary discrete AdaBoost over decision stumps.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

#include <crpc/game/ai/adaboost.hpp>

namespace crpc {
namespace game {
namespace ai {

namespace {

bool all_finite(const std::vector<double>& values) {
    for (double value : values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

// Weighted vote: a positive score is the second sorted class (+1), a negative
// score is the first (-1), and an exact tie favours the first class. The same
// rule is used for training-error accounting, so the diagnostic curve and the
// public prediction cannot disagree.
double vote_label(double score, const std::vector<double>& classes) {
    return score > 0.0 ? classes[1] : classes[0];
}

} // namespace

adaboost_classifier::adaboost_classifier(adaboost_config config)
    : config_(std::move(config)) {}

void adaboost_classifier::reset_model() {
    trained_ = false;
    rounds_.clear();
    weak_learners_.clear();
    classes_.clear();
    feature_count_ = 0;
}

status adaboost_classifier::train(const decision_tree_dataset& dataset) {
    reset_model();

    if (config_.max_rounds == 0) {
        return status(rpc_code::invalid_argument,
                      "adaboost: max_rounds must be >= 1");
    }
    if (!(config_.target_accuracy > 0.0) ||
        !(config_.target_accuracy <= 1.0)) {
        return status(rpc_code::invalid_argument,
                      "adaboost: target_accuracy must be in (0, 1]");
    }
    if (!std::isfinite(config_.epsilon) || !(config_.epsilon > 0.0) ||
        !(config_.epsilon < 0.5)) {
        return status(rpc_code::invalid_argument,
                      "adaboost: epsilon must be in (0, 0.5)");
    }
    if (config_.weak_min_samples_leaf == 0) {
        return status(rpc_code::invalid_argument,
                      "adaboost: weak_min_samples_leaf must be >= 1");
    }
    if (!std::isfinite(config_.weak_min_gain) ||
        config_.weak_min_gain < 0.0) {
        return status(rpc_code::invalid_argument,
                      "adaboost: weak_min_gain must be finite and >= 0");
    }
    if (dataset.rows == 0) {
        return status(rpc_code::invalid_argument,
                      "adaboost: dataset is empty");
    }
    if (dataset.cols == 0) {
        return status(rpc_code::invalid_argument,
                      "adaboost: dataset has no features");
    }
    if (dataset.features.size() % dataset.cols != 0 ||
        dataset.features.size() / dataset.cols != dataset.rows) {
        return status(rpc_code::invalid_argument,
                      "adaboost: feature matrix shape does not match rows");
    }
    if (dataset.targets.size() != dataset.rows) {
        return status(rpc_code::invalid_argument,
                      "adaboost: target count does not match rows");
    }
    if (!dataset.weights.empty() && dataset.weights.size() != dataset.rows) {
        return status(rpc_code::invalid_argument,
                      "adaboost: weight count does not match rows");
    }
    if (!all_finite(dataset.features) || !all_finite(dataset.targets)) {
        return status(rpc_code::invalid_argument,
                      "adaboost: features and targets must be finite");
    }

    std::vector<double> sorted_targets = dataset.targets;
    std::sort(sorted_targets.begin(), sorted_targets.end());
    sorted_targets.erase(
        std::unique(sorted_targets.begin(), sorted_targets.end()),
        sorted_targets.end());
    if (sorted_targets.size() != 2) {
        return status(rpc_code::invalid_argument,
                      "adaboost: exactly two target classes are required");
    }
    classes_ = sorted_targets;

    const std::size_t sample_count = dataset.rows;
    std::vector<double> labels(sample_count, -1.0);
    for (std::size_t row = 0; row < sample_count; ++row) {
        labels[row] =
            dataset.targets[row] == classes_[1] ? 1.0 : -1.0;
    }

    std::vector<double> weights;
    if (dataset.weights.empty()) {
        weights.assign(sample_count, 1.0 / static_cast<double>(sample_count));
    } else {
        weights = dataset.weights;
        double total = 0.0;
        for (double weight : weights) {
            if (!std::isfinite(weight) || weight < 0.0) {
                return status(rpc_code::invalid_argument,
                              "adaboost: sample weights must be finite and >= 0");
            }
            total += weight;
        }
        if (!(total > 0.0)) {
            return status(rpc_code::invalid_argument,
                          "adaboost: sample weights sum to zero");
        }
        for (double& weight : weights) {
            weight /= total;
        }
    }

    decision_tree_dataset base = dataset;
    base.weights.clear();
    std::vector<double> scores(sample_count, 0.0);

    for (std::size_t round = 0; round < config_.max_rounds; ++round) {
        decision_tree_dataset weak_samples = base;
        weak_samples.weights = weights;

        decision_tree_config weak_config;
        weak_config.task = decision_tree_task::classification;
        weak_config.max_depth = config_.weak_max_depth;
        weak_config.min_samples_leaf = config_.weak_min_samples_leaf;
        weak_config.min_gain = config_.weak_min_gain;
        weak_config.max_features = 0;
        weak_config.categorical_features = config_.categorical_features;
        weak_config.ccp_alpha = 0.0;

        decision_tree weak(weak_config);
        const status weak_status = weak.train(weak_samples);
        if (!weak_status.ok()) {
            return weak_status;
        }

        std::vector<double> hypotheses(sample_count, -1.0);
        double weighted_error = 0.0;
        for (std::size_t row = 0; row < sample_count; ++row) {
            const crpc::span<const double> features(
                dataset.features.data() + row * dataset.cols, dataset.cols);
            const auto prediction = weak.predict(features);
            if (!prediction.ok()) {
                return prediction.error();
            }
            const double hypothesis =
                prediction.value() == classes_[1] ? 1.0 : -1.0;
            hypotheses[row] = hypothesis;
            if (hypothesis != labels[row]) {
                weighted_error += weights[row];
            }
        }

        // err >= 1/2 means the weak learner is no better than chance under
        // this distribution. Adding it with alpha <= 0 would leave the
        // distribution unchanged (or move it backwards) and loop forever on
        // the same stump, so the honest action is to stop.
        if (!(weighted_error < 0.5)) {
            break;
        }

        double alpha = 0.0;
        if (weighted_error <= config_.epsilon) {
            alpha = 0.5 * std::log((1.0 - config_.epsilon) /
                                   config_.epsilon);
        } else {
            alpha = 0.5 * std::log((1.0 - weighted_error) / weighted_error);
        }
        if (!(alpha > 0.0)) {
            break;
        }

        weak_learners_.push_back(std::move(weak));
        for (std::size_t row = 0; row < sample_count; ++row) {
            scores[row] += alpha * hypotheses[row];
        }

        adaboost_round record;
        record.index = rounds_.size();
        record.alpha = alpha;
        record.weighted_error = weighted_error;

        std::size_t correct = 0;
        for (std::size_t row = 0; row < sample_count; ++row) {
            const double predicted = vote_label(scores[row], classes_);
            if (predicted == dataset.targets[row]) {
                ++correct;
            }
        }
        record.training_accuracy =
            static_cast<double>(correct) / static_cast<double>(sample_count);
        record.training_error = 1.0 - record.training_accuracy;
        rounds_.push_back(record);

        if (weighted_error <= config_.epsilon) {
            break;
        }
        if (record.training_accuracy >= config_.target_accuracy) {
            break;
        }

        double total = 0.0;
        for (std::size_t row = 0; row < sample_count; ++row) {
            const double exponent =
                -alpha * labels[row] * hypotheses[row];
            weights[row] *= std::exp(exponent);
            total += weights[row];
        }
        if (!(total > 0.0) || !std::isfinite(total)) {
            return status(rpc_code::internal,
                          "adaboost: weight normalisation failed");
        }
        for (double& weight : weights) {
            weight /= total;
        }
    }

    feature_count_ = dataset.cols;
    trained_ = true;
    return status{};
}

result<double> adaboost_classifier::decision_function(
    crpc::span<const double> features) const {
    if (!trained_) {
        return rpc_error(rpc_code::failed_precondition,
                         "adaboost: model is not trained");
    }
    if (features.size() != feature_count_) {
        return rpc_error(rpc_code::invalid_argument,
                         "adaboost: feature count does not match the model");
    }
    double score = 0.0;
    for (std::size_t index = 0; index < weak_learners_.size(); ++index) {
        const auto prediction = weak_learners_[index].predict(features);
        if (!prediction.ok()) {
            return prediction.error();
        }
        const double hypothesis =
            prediction.value() == classes_[1] ? 1.0 : -1.0;
        score += rounds_[index].alpha * hypothesis;
    }
    return score;
}

result<double> adaboost_classifier::predict(
    crpc::span<const double> features) const {
    if (!trained_) {
        return rpc_error(rpc_code::failed_precondition,
                         "adaboost: model is not trained");
    }
    const auto score = decision_function(features);
    if (!score.ok()) {
        return score.error();
    }
    return vote_label(score.value(), classes_);
}

} // namespace ai
} // namespace game
} // namespace crpc