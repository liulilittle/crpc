// CRPC -- memento pattern tests (deep snapshots, RAII transactional scope).
//
// The deep-copy case is the acceptance gate: the snapshot must own its own
// buffer (distinct from the target's), so mutating the target never touches the
// snapshot and vice versa.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#pragma comment(lib, "dbghelp.lib")
#endif
#include "test_framework.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include <crpc/pattern/memento.hpp>

using namespace crpc;
using namespace crpc::pattern;

namespace {

struct ledger {
    std::vector<int> entries;
    std::string name;

    bool operator==(const ledger& other) const {
        return entries == other.entries && name == other.name;
    }
};

ledger make_ledger() {
    ledger value;
    value.entries = {1, 2, 3};
    value.name = "base";
    return value;
}

} // namespace

CRPC_TEST(memento_rollback_restores_original) {
    ledger target = make_ledger();

    {
        scoped_memento<ledger> scope(target);
        target.entries.push_back(4);
        target.name = "mutated";
        CRPC_CHECK_EQ(4u, target.entries.size());

        scope.rollback();

        CRPC_CHECK_EQ(3u, target.entries.size());
        CRPC_CHECK_EQ(std::string("base"), target.name);
    }
    // Still restored after the scope destroyed.
    CRPC_CHECK_EQ(3u, target.entries.size());
    CRPC_CHECK_EQ(std::string("base"), target.name);
}

CRPC_TEST(memento_normal_exit_commits) {
    ledger target = make_ledger();

    {
        scoped_memento<ledger> scope(target);
        target.entries.push_back(5);
        target.name = "forwarded";
        // No commit()/rollback() and no exception: normal exit commits.
    }

    CRPC_CHECK_EQ(4u, target.entries.size());
    CRPC_CHECK_EQ(std::string("forwarded"), target.name);
}

CRPC_TEST(memento_explicit_commit_survives_unwind) {
    ledger target = make_ledger();

    try {
        scoped_memento<ledger> scope(target);
        target.entries.push_back(6);
        scope.commit();
        throw std::runtime_error("after commit");
    } catch (const std::runtime_error&) {
        // Expected; the scope was committed so unwinding must not restore.
    }

    CRPC_CHECK_EQ(4u, target.entries.size());
}

CRPC_TEST(memento_restores_when_unwound_by_exception) {
    ledger target = make_ledger();

    try {
        scoped_memento<ledger> scope(target);
        target.entries.push_back(7);
        target.name = "doomed";
        throw std::runtime_error("boom");
    } catch (const std::runtime_error&) {
        // Expected; the scope was not committed.
    }

    CRPC_CHECK_EQ(3u, target.entries.size());
    CRPC_CHECK_EQ(std::string("base"), target.name);
}

CRPC_TEST(memento_snapshot_is_deep_copy) {
    ledger target = make_ledger();
    const snapshot<ledger> snap(target);

    // The snapshot owns its own buffer: distinct allocation from the target.
    CRPC_CHECK(snap.value().entries.data() != target.entries.data());
    CRPC_CHECK_EQ(3u, snap.value().entries.size());

    // Mutating the target must not reach the snapshot.
    target.entries.push_back(99);
    target.name = "changed";
    CRPC_CHECK_EQ(3u, snap.value().entries.size());
    CRPC_CHECK_EQ(std::string("base"), snap.value().name);

    // Mutating the snapshot must not reach the target.
    snapshot<ledger> mutable_snap(target);
    const int* original_buffer = target.entries.data();
    mutable_snap.value().entries.clear();
    mutable_snap.value().name = "snapshot only";
    CRPC_CHECK_EQ(4u, target.entries.size());
    CRPC_CHECK_EQ(std::string("changed"), target.name);
    CRPC_CHECK(target.entries.data() == original_buffer);
}

CRPC_TEST(memento_snapshot_can_restore_target) {
    ledger target = make_ledger();
    const snapshot<ledger> snap(target);

    target.entries.push_back(123);

    target = snap.value();
    CRPC_CHECK(target == make_ledger());
}

CRPC_TEST(memento_nested_scopes) {
    std::string value = "a";

    {
        scoped_memento<std::string> outer(value);
        value = "ab";
        {
            scoped_memento<std::string> inner(value);
            value = "abc";
            inner.rollback();
        }
        CRPC_CHECK_EQ(std::string("ab"), value);
    }
    CRPC_CHECK_EQ(std::string("ab"), value);
}

CRPC_TEST_MAIN()