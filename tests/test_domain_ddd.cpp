// CRPC -- domain layer tests: entities, value objects, aggregate event
// accumulation, in-memory repositories and unit-of-work atomicity.
//
// The acceptance points from the design are checked here, one per case:
//   D1  entity equality is identity-only (the id, never the fields);
//   D2  a value object is equal by value and its hash agrees with equality;
//   D3  aggregate events accumulate, carry a monotonic sequence and survive a
//       clear as a version high-water mark;
//   D4  the in-memory repository implements save/find/remove/paged find_all;
//   D5  a unit of work commits every participant atomically;
//   D6  a failed commit rolls every participant back; nothing partially
//       applies;
//   D7  a second commit on the same unit of work is rejected;
//   D8  rollback and destruction both discard staged work.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#pragma comment(lib, "dbghelp.lib")
#endif
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <crpc/domain/aggregate.hpp>
#include <crpc/domain/domain_event.hpp>
#include <crpc/domain/entity.hpp>
#include <crpc/domain/repository.hpp>
#include <crpc/domain/unit_of_work.hpp>
#include <crpc/domain/value_object.hpp>

using namespace crpc;
using namespace crpc::domain;

namespace {

// --- Entities ---------------------------------------------------------------

class account final : public entity<int, account> {
public:
    account(int id, std::string owner, long balance)
        : entity<int, account>(id), owner_(std::move(owner)), balance_(balance) {}

    [[nodiscard]] const std::string& owner() const noexcept { return owner_; }
    [[nodiscard]] long balance() const noexcept { return balance_; }

private:
    std::string owner_;
    long balance_;
};

// --- Value objects ----------------------------------------------------------

class money final : public value_object<money> {
public:
    money(long amount, std::string currency)
        : amount_(amount), currency_(std::move(currency)) {}

    [[nodiscard]] bool equals(const money& other) const noexcept {
        return amount_ == other.amount_ && currency_ == other.currency_;
    }

    [[nodiscard]] std::size_t hash() const noexcept {
        return std::hash<long>{}(amount_) * std::size_t{31} +
               std::hash<std::string>{}(currency_);
    }

    [[nodiscard]] long amount() const noexcept { return amount_; }
    [[nodiscard]] const std::string& currency() const noexcept { return currency_; }

private:
    long amount_;
    std::string currency_;
};

// --- Aggregates -------------------------------------------------------------

class order final : public aggregate_root<std::uint64_t, order> {
public:
    explicit order(std::uint64_t id) : aggregate_root<std::uint64_t, order>(id) {}

    void add_line(int quantity) {
        raise(make_domain_event("line_added", std::to_string(quantity)));
    }

    void cancel() { raise("order_canceled", "{}"); }

    // Used by the rehydration test: a repository replays stored history
    // without queueing it as uncommitted work.
    void observe(const domain_event& event) { observe_persisted(event); }
};

// --- Repository payloads ----------------------------------------------------

struct user {
    int id = 0;
    std::string name;
    int age = 0;
};

struct user_id_of {
    [[nodiscard]] int operator()(const user& value) const noexcept { return value.id; }
};

// A participant double that records lifecycle calls and can fail its flush.
// It stages saves in a live map (so reads see them) and restores the snapshot
// taken at begin() from rollback().
template <typename T, typename Id, typename IdOf>
class staging_repository final : public repository<T, Id> {
public:
    explicit staging_repository(IdOf id_of = IdOf{}) : id_of_(std::move(id_of)) {}

    [[nodiscard]] result<std::optional<T>> find(const Id& id) override {
        const auto it = values_.find(id);
        if (it == values_.end()) {
            return std::optional<T>{};
        }
        return std::optional<T>(it->second);
    }

    [[nodiscard]] status save(const T& value) override {
        values_[id_of_(value)] = value;
        pending_ = true;
        return status{};
    }

    [[nodiscard]] status remove(const Id& id) override {
        values_.erase(id);
        pending_ = true;
        return status{};
    }

    [[nodiscard]] result<std::vector<T>> find_all(std::size_t limit = 100,
                                                  std::size_t offset = 0) override {
        std::vector<T> page;
        std::size_t skipped = 0;
        for (const auto& entry : values_) {
            if (skipped < offset) {
                ++skipped;
                continue;
            }
            if (page.size() >= limit) {
                break;
            }
            page.push_back(entry.second);
        }
        return page;
    }

    void begin() override {
        ++begin_calls;
        snapshot_ = values_;
        pending_ = false;
    }

    [[nodiscard]] status flush() override {
        ++flush_calls;
        if (fail_flush) {
            return status(rpc_code::unavailable, "archive backend is down");
        }
        pending_ = false;
        return status{};
    }

    void commit() noexcept override {
        ++commit_calls;
        pending_ = false;
    }

    void rollback() noexcept override {
        ++rollback_calls;
        values_ = snapshot_;
        pending_ = false;
    }

    [[nodiscard]] bool has_pending() const noexcept override { return pending_; }

    bool fail_flush = false;
    int begin_calls = 0;
    int flush_calls = 0;
    int commit_calls = 0;
    int rollback_calls = 0;

private:
    IdOf id_of_;
    std::map<Id, T> values_;
    std::map<Id, T> snapshot_;
    bool pending_ = false;
};

} // namespace

// --- D1: identity-only entity equality --------------------------------------

CRPC_TEST(entity_equality_uses_identity_only) {
    const account first(7, "alice", 100);
    const account second(7, "bob", -5);
    const account other(8, "alice", 100);

    // Same id, completely different fields: still the same entity.
    CRPC_CHECK(first == second);
    CRPC_CHECK_FALSE(first != second);
    // Same fields, different id: a different entity.
    CRPC_CHECK(first != other);
    CRPC_CHECK_FALSE(first == other);
    // Reflexive, and the id accessor is the constructor argument.
    CRPC_CHECK(first == first);
    CRPC_CHECK_EQ(7, first.id());
    CRPC_CHECK_EQ(std::string("alice"), first.owner());
}

// --- D2: value equality and hash agreement ----------------------------------

CRPC_TEST(value_object_equality_and_hash_agree) {
    const money ten_usd(1000, "USD");
    const money same(1000, "USD");
    const money ten_eur(1000, "EUR");
    const money eleven_usd(1100, "USD");

    CRPC_CHECK(ten_usd == same);
    CRPC_CHECK_FALSE(ten_usd != same);
    CRPC_CHECK(ten_usd != ten_eur);
    CRPC_CHECK(ten_usd != eleven_usd);
    CRPC_CHECK_EQ(ten_usd.hash(), same.hash());
    CRPC_CHECK_EQ(ten_usd.hash_value(), same.hash_value());
    CRPC_CHECK_EQ(std::string("USD"), ten_usd.currency());
    CRPC_CHECK_EQ(1100, eleven_usd.amount());
}

CRPC_TEST(value_object_hash_deduplicates_in_unordered_set) {
    std::unordered_set<money, value_object_hash<money>> wallet;
    wallet.insert(money(1000, "USD"));
    wallet.insert(money(1000, "USD"));
    wallet.insert(money(1000, "EUR"));
    wallet.insert(money(2000, "USD"));

    CRPC_CHECK_EQ(3u, wallet.size());
    CRPC_CHECK_EQ(1u, wallet.count(money(1000, "USD")));
    CRPC_CHECK_EQ(0u, wallet.count(money(500, "USD")));
}

// --- D3: aggregate event accumulation and sequence monotonicity -------------

CRPC_TEST(aggregate_events_accumulate_in_raise_order) {
    order target(42);
    target.add_line(1);
    target.add_line(2);
    target.cancel();

    const std::vector<domain_event>& events = target.uncommitted_events();
    CRPC_REQUIRE(events.size() == 3);
    CRPC_CHECK_EQ(std::string("line_added"), events[0].name);
    CRPC_CHECK_EQ(std::string("1"), events[0].payload);
    CRPC_CHECK_EQ(std::string("line_added"), events[1].name);
    CRPC_CHECK_EQ(std::string("2"), events[1].payload);
    CRPC_CHECK_EQ(std::string("order_canceled"), events[2].name);
    CRPC_CHECK_EQ(std::uint64_t{3}, target.version());
    CRPC_CHECK(target.has_uncommitted_events());
}

CRPC_TEST(aggregate_event_sequences_are_monotonic) {
    order target(1);
    for (int i = 0; i < 5; ++i) {
        target.add_line(i);
    }

    const std::vector<domain_event>& events = target.uncommitted_events();
    CRPC_REQUIRE(events.size() == 5);
    for (std::size_t i = 0; i < events.size(); ++i) {
        // The aggregate owns sequencing: every emitted event is stamped with
        // the next version, starting at 1.
        CRPC_CHECK_EQ(static_cast<std::uint64_t>(i + 1), events[i].sequence);
        if (i > 0) {
            CRPC_CHECK(events[i - 1].sequence < events[i].sequence);
        }
    }
    CRPC_CHECK_EQ(std::uint64_t{5}, target.version());
}

CRPC_TEST(aggregate_clear_discards_buffer_but_not_version) {
    order target(9);
    target.add_line(1);
    target.add_line(2);
    CRPC_CHECK_EQ(2u, target.uncommitted_events().size());

    target.clear_events();
    CRPC_CHECK(target.uncommitted_events().empty());
    CRPC_CHECK_FALSE(target.has_uncommitted_events());
    // A clear only drains the outbox; the version high-water mark stays, so
    // the next event cannot reuse a sequence another consumer already saw.
    CRPC_CHECK_EQ(std::uint64_t{2}, target.version());

    target.add_line(3);
    CRPC_REQUIRE(target.uncommitted_events().size() == 1);
    CRPC_CHECK_EQ(std::uint64_t{3}, target.uncommitted_events()[0].sequence);
    CRPC_CHECK_EQ(std::uint64_t{3}, target.version());
}

CRPC_TEST(aggregate_stamps_events_and_preserves_identity) {
    order target(123);
    const auto before = std::chrono::system_clock::now();
    target.add_line(1);
    const auto after = std::chrono::system_clock::now();

    CRPC_REQUIRE(target.uncommitted_events().size() == 1);
    const domain_event& event = target.uncommitted_events()[0];
    CRPC_CHECK(event.occurred_at != std::chrono::system_clock::time_point{});
    CRPC_CHECK(event.occurred_at >= before);
    CRPC_CHECK(event.occurred_at <= after);
    CRPC_CHECK_EQ(std::uint64_t{123}, target.id());
}

CRPC_TEST(aggregate_replay_advances_version_without_queueing) {
    order target(5);
    domain_event cached;
    cached.name = "history";
    cached.sequence = 41;
    target.observe(cached);

    CRPC_CHECK(target.uncommitted_events().empty());
    CRPC_CHECK_EQ(std::uint64_t{41}, target.version());

    target.add_line(1);
    CRPC_REQUIRE(target.uncommitted_events().size() == 1);
    CRPC_CHECK_EQ(std::uint64_t{42}, target.uncommitted_events()[0].sequence);
}

// --- D4: in-memory repository CRUD and pagination ---------------------------

CRPC_TEST(repository_save_find_remove) {
    in_memory_repository<user, int, user_id_of> repo;

    CRPC_CHECK(repo.save(user{1, "alice", 30}).ok());
    CRPC_CHECK(repo.save(user{2, "bob", 41}).ok());

    const auto found = repo.find(1);
    CRPC_REQUIRE(found.ok());
    CRPC_REQUIRE(found.value().has_value());
    CRPC_CHECK_EQ(std::string("alice"), found.value()->name);
    CRPC_CHECK_EQ(2u, repo.size());

    const auto missing = repo.find(99);
    CRPC_CHECK(missing.ok());
    CRPC_CHECK_FALSE(missing.value().has_value());

    CRPC_CHECK(repo.remove(1).ok());
    CRPC_CHECK(repo.find(1).value().has_value() == false);
    // Removing an absent row is an idempotent no-op, not an error.
    CRPC_CHECK(repo.remove(1).ok());
    CRPC_CHECK_EQ(1u, repo.size());
}

CRPC_TEST(repository_save_updates_existing_value) {
    in_memory_repository<user, int, user_id_of> repo;
    CRPC_CHECK(repo.save(user{3, "carol", 20}).ok());
    CRPC_CHECK(repo.save(user{3, "carol2", 21}).ok());

    const auto found = repo.find(3);
    CRPC_REQUIRE(found.ok());
    CRPC_REQUIRE(found.value().has_value());
    CRPC_CHECK_EQ(std::string("carol2"), found.value()->name);
    CRPC_CHECK_EQ(21, found.value()->age);
    CRPC_CHECK_EQ(1u, repo.size());
}

CRPC_TEST(repository_find_all_paginates_in_id_order) {
    in_memory_repository<user, int, user_id_of> repo;
    for (int id = 1; id <= 5; ++id) {
        CRPC_CHECK(repo.save(user{id, "user" + std::to_string(id), id * 10}).ok());
    }

    const auto first_page = repo.find_all(2, 0);
    CRPC_REQUIRE(first_page.ok());
    CRPC_REQUIRE(first_page.value().size() == 2);
    CRPC_CHECK_EQ(1, first_page.value()[0].id);
    CRPC_CHECK_EQ(2, first_page.value()[1].id);

    const auto second_page = repo.find_all(2, 2);
    CRPC_REQUIRE(second_page.ok());
    CRPC_REQUIRE(second_page.value().size() == 2);
    CRPC_CHECK_EQ(3, second_page.value()[0].id);
    CRPC_CHECK_EQ(4, second_page.value()[1].id);

    const auto tail = repo.find_all(2, 4);
    CRPC_REQUIRE(tail.ok());
    CRPC_REQUIRE(tail.value().size() == 1);
    CRPC_CHECK_EQ(5, tail.value()[0].id);

    CRPC_CHECK(repo.find_all(10, 99).value().empty());
    CRPC_CHECK(repo.find_all(0, 0).value().empty());
}

// --- D5: unit of work success ----------------------------------------------

CRPC_TEST(unit_of_work_commit_applies_every_participant) {
    in_memory_repository<user, int, user_id_of> users;
    in_memory_repository<user, int, user_id_of> archive;

    unit_of_work uow;
    CRPC_CHECK(uow.is_active());
    uow.enlist(users);
    uow.enlist(archive);

    CRPC_CHECK(users.save(user{1, "alice", 30}).ok());
    CRPC_CHECK(archive.save(user{2, "bob", 40}).ok());

    const status committed = uow.commit();
    CRPC_CHECK(committed.ok());
    CRPC_CHECK_FALSE(uow.is_active());

    const auto alice = users.find(1);
    CRPC_REQUIRE(alice.ok());
    CRPC_REQUIRE(alice.value().has_value());
    CRPC_CHECK_EQ(std::string("alice"), alice.value()->name);

    const auto bob = archive.find(2);
    CRPC_REQUIRE(bob.ok());
    CRPC_REQUIRE(bob.value().has_value());
    CRPC_CHECK_EQ(std::string("bob"), bob.value()->name);
}

CRPC_TEST(unit_of_work_commit_finalizes_rollback_journal) {
    in_memory_repository<user, int, user_id_of> users;

    unit_of_work first;
    first.enlist(users);
    CRPC_CHECK(users.save(user{1, "alice", 30}).ok());
    CRPC_CHECK(first.commit().ok());

    // A later transaction that rolls back must not undo the committed write.
    unit_of_work second;
    second.enlist(users);
    second.rollback();

    const auto found = users.find(1);
    CRPC_REQUIRE(found.ok());
    CRPC_REQUIRE(found.value().has_value());
    CRPC_CHECK_EQ(std::string("alice"), found.value()->name);
}

// --- D6: unit of work failure is atomic ------------------------------------

CRPC_TEST(unit_of_work_commit_failure_rolls_back_every_participant) {
    in_memory_repository<user, int, user_id_of> users;
    staging_repository<user, int, user_id_of> archive;
    staging_repository<user, int, user_id_of> cache;

    archive.fail_flush = true;

    unit_of_work uow;
    uow.enlist(users);
    uow.enlist(archive);
    uow.enlist(cache);

    CRPC_CHECK(users.save(user{1, "alice", 30}).ok());
    CRPC_CHECK(archive.save(user{2, "bob", 40}).ok());
    CRPC_CHECK(cache.save(user{3, "carol", 50}).ok());

    const status failed = uow.commit();
    CRPC_CHECK_FALSE(failed.ok());
    CRPC_CHECK_EQ(rpc_code::unavailable, failed.code());
    CRPC_CHECK_FALSE(uow.is_active());

    // The first participant had already flushed when the second one failed;
    // the rollback must have restored it, otherwise the write is a partial
    // application of a failed transaction.
    const auto alice = users.find(1);
    CRPC_REQUIRE(alice.ok());
    CRPC_CHECK_FALSE(alice.value().has_value());
    CRPC_CHECK_EQ(1, archive.flush_calls);
    CRPC_CHECK_EQ(1, archive.rollback_calls);
    // The third participant was never reached by the flush phase.
    CRPC_CHECK_EQ(0, cache.flush_calls);
    CRPC_CHECK_EQ(1, cache.rollback_calls);
    CRPC_CHECK_EQ(0, cache.commit_calls);
}

// --- D7: one-shot commit ----------------------------------------------------

CRPC_TEST(unit_of_work_repeated_commit_is_rejected) {
    unit_of_work uow;
    const status first = uow.commit();
    CRPC_CHECK(first.ok());
    CRPC_CHECK_FALSE(uow.is_active());

    const status second = uow.commit();
    CRPC_CHECK_FALSE(second.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, second.code());
}

CRPC_TEST(unit_of_work_commit_after_failed_commit_is_rejected) {
    staging_repository<user, int, user_id_of> repo;
    repo.fail_flush = true;

    unit_of_work uow;
    uow.enlist(repo);
    CRPC_CHECK(repo.save(user{1, "alice", 30}).ok());
    CRPC_CHECK_FALSE(uow.commit().ok());

    const status again = uow.commit();
    CRPC_CHECK_FALSE(again.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, again.code());
}

// --- D8: rollback and destruction discard staged work -----------------------

CRPC_TEST(unit_of_work_rollback_discards_staged_changes) {
    in_memory_repository<user, int, user_id_of> users;

    unit_of_work uow;
    uow.enlist(users);
    CRPC_CHECK(users.save(user{1, "alice", 30}).ok());
    // Staged changes are visible to reads inside the transaction.
    CRPC_CHECK(users.find(1).value().has_value());

    uow.rollback();
    CRPC_CHECK_FALSE(uow.is_active());
    CRPC_CHECK_FALSE(users.find(1).value().has_value());

    // Rollback is idempotent and does not resurrect anything.
    uow.rollback();
    CRPC_CHECK_FALSE(users.find(1).value().has_value());
}

CRPC_TEST(unit_of_work_destructor_rolls_back_uncommitted_work) {
    in_memory_repository<user, int, user_id_of> users;
    {
        unit_of_work uow;
        uow.enlist(users);
        CRPC_CHECK(users.save(user{1, "alice", 30}).ok());
        CRPC_CHECK(users.find(1).value().has_value());
    }
    CRPC_CHECK_FALSE(users.find(1).value().has_value());
}

CRPC_TEST(unit_of_work_deduplicates_enlisted_repositories) {
    staging_repository<user, int, user_id_of> repo;

    unit_of_work uow;
    uow.enlist(repo);
    uow.enlist(repo);
    uow.enlist(repo);
    CRPC_CHECK(repo.save(user{1, "alice", 30}).ok());

    const status committed = uow.commit();
    CRPC_CHECK(committed.ok());
    CRPC_CHECK_EQ(1, repo.begin_calls);
    CRPC_CHECK_EQ(1, repo.flush_calls);
    CRPC_CHECK_EQ(1, repo.commit_calls);
    CRPC_CHECK_EQ(0, repo.rollback_calls);
}

CRPC_TEST_MAIN()
