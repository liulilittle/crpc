// CRPC -- constraint / reduction pattern tests.
//
// Reduction evaluates a fixed rule list in insertion order. all_of short-
// circuits on the first violation and returns its reason; any_of short-circuits
// on the first success and, when everything fails, returns the FIRST rule's
// violation reason. Nested reductions keep that order deterministically.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#pragma comment(lib, "dbghelp.lib")
#endif
#include "test_framework.hpp"

#include <string>

#include <crpc/pattern/constraint.hpp>

using namespace crpc;
using namespace crpc::pattern;

namespace {

struct document {
    std::string title;
    int size = 0;
};

int code_id(rpc_code code) {
    return static_cast<int>(code);
}

status violation(std::string reason) {
    return status(rpc_code::invalid_argument, std::move(reason));
}

} // namespace

CRPC_TEST(constraint_single_rule_pass_and_fail) {
    constraint<document> has_title("has title", [](const document& doc) {
        return doc.title.empty() ? violation("title must not be empty")
                                 : status();
    });

    CRPC_CHECK(has_title.check(document{"hello", 1}).ok());

    const auto bad = has_title.check(document{"", 1});
    CRPC_CHECK_FALSE(bad.ok());
    CRPC_CHECK_EQ(code_id(rpc_code::invalid_argument), code_id(bad.code()));
    CRPC_CHECK_EQ(std::string("title must not be empty"), bad.message());
    CRPC_CHECK_EQ(std::string("has title"), has_title.name());
}

CRPC_TEST(reduction_empty_sets) {
    reduction<document> all;
    CRPC_CHECK(all.empty());
    CRPC_CHECK_EQ(0u, all.size());
    // Vacuous truth: an empty all_of accepts every value.
    CRPC_CHECK(all.check(document{"x", 0}).ok());

    reduction<document> any(reduction_op::any_of);
    CRPC_CHECK(any.empty());
    // An empty any_of can never be satisfied.
    const auto r = any.check(document{"x", 0});
    CRPC_CHECK_FALSE(r.ok());
    CRPC_CHECK_FALSE(r.message().empty());
}

CRPC_TEST(reduction_all_of_first_violation_and_short_circuit) {
    int evaluated = 0;
    reduction<document> rules;
    rules.add(constraint<document>("min", [&](const document& doc) {
        ++evaluated;
        return doc.size >= 1 ? status() : violation("too small");
    }));
    rules.add(constraint<document>("max", [&](const document& doc) {
        ++evaluated;
        return doc.size <= 2 ? status() : violation("too big");
    }));

    evaluated = 0;
    CRPC_CHECK(rules.check(document{"x", 1}).ok());
    CRPC_CHECK_EQ(2, evaluated);

    // First rule passes, second fails: reason is the second rule's, and the
    // (nonexistent) third rule is never touched.
    evaluated = 0;
    const auto big = rules.check(document{"x", 5});
    CRPC_CHECK_FALSE(big.ok());
    CRPC_CHECK_EQ(std::string("too big"), big.message());
    CRPC_CHECK_EQ(2, evaluated);

    // First rule fails: short-circuit, second rule never evaluated.
    evaluated = 0;
    const auto small = rules.check(document{"x", -1});
    CRPC_CHECK_FALSE(small.ok());
    CRPC_CHECK_EQ(std::string("too small"), small.message());
    CRPC_CHECK_EQ(1, evaluated);
}

CRPC_TEST(reduction_any_of_first_success_and_all_fail_reason) {
    int evaluated = 0;
    reduction<document> rules(reduction_op::any_of);
    rules.add(constraint<document>("even", [&](const document& doc) {
        ++evaluated;
        return doc.size % 2 == 0 ? status() : violation("not even");
    }));
    rules.add(constraint<document>("large", [&](const document& doc) {
        ++evaluated;
        return doc.size > 100 ? status() : violation("not large");
    }));

    // First rule passes: short-circuit, second never evaluated.
    evaluated = 0;
    CRPC_CHECK(rules.check(document{"x", 2}).ok());
    CRPC_CHECK_EQ(1, evaluated);

    // First fails, second passes.
    evaluated = 0;
    CRPC_CHECK(rules.check(document{"x", 101}).ok());
    CRPC_CHECK_EQ(2, evaluated);

    // Both fail: deterministic FIRST reason ("not even"), not the last.
    evaluated = 0;
    const auto r = rules.check(document{"x", 3});
    CRPC_CHECK_FALSE(r.ok());
    CRPC_CHECK_EQ(std::string("not even"), r.message());
    CRPC_CHECK_EQ(2, evaluated);
}

CRPC_TEST(reduction_nested_composition_is_deterministic) {
    reduction<document> positive;
    positive.add(constraint<document>("positive", [](const document& doc) {
        return doc.size > 0 ? status() : violation("not positive");
    }));

    reduction<document> anywhere(reduction_op::any_of);
    anywhere.add(constraint<document>("small", [](const document& doc) {
        return doc.size <= 10 ? status() : violation("not small");
    }));
    anywhere.add(constraint<document>("huge", [](const document& doc) {
        return doc.size > 1000 ? status() : violation("not huge");
    }));

    reduction<document> size_ok;
    size_ok.add("positive", positive);
    size_ok.add("anywhere", anywhere);

    int evaluated = 0;
    reduction<document> composite;
    composite.add(constraint<document>("has title", [&](const document& doc) {
        ++evaluated;
        return doc.title.empty() ? violation("no title") : status();
    }));
    composite.add("size_ok", size_ok);

    CRPC_CHECK_EQ(2u, composite.size());

    // All branches pass.
    evaluated = 0;
    CRPC_CHECK(composite.check(document{"ok", 5}).ok());
    CRPC_CHECK_EQ(1, evaluated);

    // Outer rule fails first -> short-circuit, nested never runs.
    evaluated = 0;
    const auto no_title = composite.check(document{"", 5});
    CRPC_CHECK_FALSE(no_title.ok());
    CRPC_CHECK_EQ(std::string("no title"), no_title.message());
    CRPC_CHECK_EQ(1, evaluated);

    // Nested positive branch fails -> its reason surfaces, and counter inside
    // the nested scope is unchanged because short-circuiting stops there.
    const auto not_positive = composite.check(document{"ok", -1});
    CRPC_CHECK_FALSE(not_positive.ok());
    CRPC_CHECK_EQ(std::string("not positive"), not_positive.message());

    // Nested any_of branch: neither small nor huge apply -> first ("not small").
    const auto neither = composite.check(document{"ok", 500});
    CRPC_CHECK_FALSE(neither.ok());
    CRPC_CHECK_EQ(std::string("not small"), neither.message());

    // Identical input repeated -> identical reason (deterministic order).
    CRPC_CHECK_EQ(neither.message(), composite.check(document{"ok", 500}).message());
}

CRPC_TEST(reduction_as_constraint_reuses_nested_reduction) {
    reduction<document> positive;
    positive.add(constraint<document>("positive", [](const document& doc) {
        return doc.size > 0 ? status() : violation("not positive");
    }));

    const constraint<document> as_rule = positive.as_constraint("positive wrapper");

    reduction<document> com;
    com.add(as_rule);
    com.add(constraint<document>("even", [](const document& doc) {
        return doc.size % 2 == 0 ? status() : violation("not even");
    }));

    CRPC_CHECK(com.check(document{"x", 4}).ok());
    const auto bad = com.check(document{"x", -2});
    CRPC_CHECK_FALSE(bad.ok());
    CRPC_CHECK_EQ(std::string("not positive"), bad.message());
    CRPC_CHECK_EQ(std::string("positive wrapper"), as_rule.name());
}

CRPC_TEST_MAIN()