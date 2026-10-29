// CRPC -- server-side idempotency dedup table implementation.
//
// See dedup.hpp for the semantics contract. The implementation notes that are
// not visible in the header:
//
//   * One mutex guards the whole table. The handler always runs outside it; a
//     suspended duplicate waits on its entry's condition variable, which
//     releases the mutex while blocked. Contention is therefore bounded by the
//     claim/complete critical sections, not by handler duration.
//   * Entries live in an std::list whose nodes never move, so a leader can hold
//     a raw `entry*` across its whole handler execution: an in-flight entry is
//     pinned and cannot be evicted until it completes.
//   * Only successful responses within `max_cached_bytes` are retained. An
//     uncached completion keeps the payload alive just long enough for the
//     duplicates suspended at that moment, then drops it (the last taker
//     releases), so a flood of oversized responses cannot accumulate.
#include <crpc/rpc/dedup.hpp>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace crpc::rpc {

// --- entry --------------------------------------------------------------------

// The record itself is defined in the header (std::list stores it by value, so
// the type must be complete where the list member is declared).

// --- lease --------------------------------------------------------------------

// --- lease --------------------------------------------------------------------

dedup_table::lease::~lease() {
    // RAII safety net: a leader that is destroyed without publishing (a dispatch
    // that exited through an unexpected path) must still wake the duplicates
    // suspended against it. `complete_failed` never allocates, so this is safe
    // in a destructor.
    if (table_ != nullptr && entry_ != nullptr) {
        table_->complete_failed_impl(*entry_);
    }
    table_ = nullptr;
    entry_ = nullptr;
    state_ = action::none;
}

dedup_table::lease::lease(lease&& other) noexcept
    : table_(other.table_),
      entry_(other.entry_),
      state_(other.state_),
      payload_(std::move(other.payload_)),
      code_(other.code_) {
    other.table_ = nullptr;
    other.entry_ = nullptr;
    other.state_ = action::none;
    other.code_ = rpc_code::ok;
}

dedup_table::lease& dedup_table::lease::operator=(lease&& other) noexcept {
    if (this != &other) {
        if (table_ != nullptr && entry_ != nullptr) {
            table_->complete_failed_impl(*entry_);
        }
        table_ = other.table_;
        entry_ = other.entry_;
        state_ = other.state_;
        payload_ = std::move(other.payload_);
        code_ = other.code_;
        other.table_ = nullptr;
        other.entry_ = nullptr;
        other.state_ = action::none;
        other.code_ = rpc_code::ok;
    }
    return *this;
}

void dedup_table::lease::complete(span<const std::byte> payload, bool ok, rpc_code code) {
    if (table_ == nullptr || entry_ == nullptr) {
        return;
    }
    table_->complete_impl(*entry_, payload, ok, code);
    table_ = nullptr;
    entry_ = nullptr;
    state_ = action::none;
}

void dedup_table::lease::complete_failed() noexcept {
    if (table_ == nullptr || entry_ == nullptr) {
        return;
    }
    table_->complete_failed_impl(*entry_);
    table_ = nullptr;
    entry_ = nullptr;
    state_ = action::none;
}

// --- construction / destruction ------------------------------------------------

dedup_table::~dedup_table() {
    // Every shard must be joined before the table dies (the server destroys its
    // dispatcher only after `stop()`), so no duplicate can be suspended here.
    // The debug check documents that invariant.
    std::lock_guard<std::mutex> lk(mutex_);
#if defined(_DEBUG) || !defined(NDEBUG)
    for (const entry& e : entries_) {
        if (!e.completed || !e.waiters.empty()) {
            std::fputs("crpc: dedup_table destroyed with live entries\n", stderr);
            break;
        }
    }
#endif
}

// --- claim ---------------------------------------------------------------------

dedup_table::lease dedup_table::claim_impl(const call_id& key, u64 service_id,
                                           u64 method_id,
                                           std::chrono::steady_clock::time_point now,
                                           std::chrono::steady_clock::time_point wait_deadline,
                                           const void* wait_key, bool wait_for_response) {
    const table_key lookup{key, service_id, method_id};
    std::unique_lock<std::mutex> lk(mutex_);

    const auto found = index_.find(lookup);
    if (found != index_.end()) {
        entry& e = *found->second;
        if (!e.completed) {
            // In flight. A one-way duplicate has no response to wait for: drop
            // it silently (the side effect executes exactly once).
            if (!wait_for_response) {
                return lease{};
            }
            if (wait_deadline <= now) {
                // Suspension disabled (or the wire deadline is already gone):
                // refuse immediately instead of blocking a shard thread.
                waiters_expired_.fetch_add(1, std::memory_order_relaxed);
                lease refused;
                refused.state_ = lease::action::expired;
                return refused;
            }
            const u64 token = ++waiter_seq_;
            e.waiters.push_back(entry::waiter{token, wait_key, false});
            claims_waiter_.fetch_add(1, std::memory_order_relaxed);
            const bool woke = e.cv.wait_until(lk, wait_deadline, [&e, token] {
                return e.completed ||
                       std::any_of(e.waiters.begin(), e.waiters.end(),
                                   [token](const entry::waiter& w) {
                                       return w.id == token && w.abandoned;
                                   });
            });
            // The lock is held again. Find the own record, read the reason and
            // remove it; the last taker of an uncached payload releases it.
            bool abandoned = false;
            for (std::size_t i = 0; i < e.waiters.size(); ++i) {
                if (e.waiters[i].id == token) {
                    abandoned = e.waiters[i].abandoned;
                    e.waiters[i] = e.waiters.back();
                    e.waiters.pop_back();
                    break;
                }
            }
            if (abandoned) {
                waiters_abandoned_.fetch_add(1, std::memory_order_relaxed);
                lease outcome;
                outcome.state_ = lease::action::abandoned;
                return outcome;
            }
            if (!woke || !e.completed) {
                // The wait bound elapsed before the leader published. The
                // dispatcher picks the response code (wire deadline vs wait
                // cap); the entry stays as it is.
                waiters_expired_.fetch_add(1, std::memory_order_relaxed);
                lease outcome;
                outcome.state_ = lease::action::expired;
                return outcome;
            }
            waiters_woken_.fetch_add(1, std::memory_order_relaxed);
            lease outcome;
            outcome.state_ = lease::action::served;
            outcome.payload_ = e.payload;
            outcome.code_ = e.completed_code;
            if (!e.retained && e.waiters.empty()) {
                e.payload.reset();
            }
            return outcome;
        }

        // Completed. Replay only a retained, unexpired response; otherwise the
        // entry is re-claimed (the handler runs again).
        if (e.retained && e.payload != nullptr &&
            (options_.entry_ttl.count() <= 0 || now < e.expires)) {
            // LRU touch: move the node to the front. Splice keeps every
            // iterator and pointer valid, so the index needs no update.
            entries_.splice(entries_.begin(), entries_, found->second);
            claims_replay_.fetch_add(1, std::memory_order_relaxed);
            lease outcome;
            outcome.state_ = lease::action::replay;
            outcome.payload_ = e.payload;
            outcome.code_ = e.completed_code;
            return outcome;
        }
        erase_entry(e);
    }

    // Fresh key (or an entry that just lost its replay rights). Make room.
    if (index_.size() >= options_.capacity) {
        // Drop expired entries at the LRU tail first: they are the least useful
        // and their window is gone.
        while (index_.size() >= options_.capacity && !entries_.empty()) {
            entry& tail = entries_.back();
            if (!tail.completed || options_.entry_ttl.count() <= 0 ||
                now < tail.expires) {
                break;
            }
            erase_entry(tail);
        }
        if (index_.size() >= options_.capacity && !evict_one_lru()) {
            // Full of in-flight entries. Bypass: execute without dedup (a
            // bounded degradation to at-least-once, never a hang).
            claims_bypass_.fetch_add(1, std::memory_order_relaxed);
            return lease{};
        }
    }

    entries_.emplace_front();
    entry& fresh = entries_.front();
    fresh.key = lookup;
    fresh.completed = false;
    fresh.retained = false;
    fresh.payload.reset();
    fresh.completed_code = rpc_code::ok;
    if (options_.entry_ttl.count() > 0) {
        fresh.expires = now + options_.entry_ttl;
    } else {
        fresh.expires = std::chrono::steady_clock::time_point::max();
    }
    try {
        index_.emplace(lookup, entries_.begin());
    } catch (...) {
        // The index could not grow; the list must not keep an unindexed entry.
        entries_.pop_front();
        throw;
    }
    claims_leader_.fetch_add(1, std::memory_order_relaxed);
    lease outcome;
    outcome.table_ = this;
    outcome.entry_ = &fresh;
    outcome.state_ = lease::action::leader;
    return outcome;
}

// --- completion ----------------------------------------------------------------

void dedup_table::complete_impl(entry& e, span<const std::byte> payload, bool ok,
                                rpc_code code) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (e.completed) {
        // Double publish (explicit complete racing the RAII net): the first
        // outcome wins, the second is ignored.
        return;
    }
    e.completed = true;
    e.completed_code = ok ? rpc_code::ok : code;
    const bool cacheable = ok && payload.size() <= options_.max_cached_bytes;
    if (cacheable) {
        // Retained for later replays until the entry is evicted.
        e.payload = std::make_shared<const std::vector<std::byte>>(
            payload.begin(), payload.end());
        e.retained = true;
    } else if (!e.waiters.empty() && !payload.empty()) {
        // Handed to the duplicates suspended at this moment, then released by
        // the last taker. Never retained for replays.
        e.payload = std::make_shared<const std::vector<std::byte>>(
            payload.begin(), payload.end());
        e.retained = false;
    } else {
        e.payload.reset();
        e.retained = false;
    }
    e.cv.notify_all();
}

void dedup_table::complete_failed_impl(entry& e) noexcept {
    try {
        std::lock_guard<std::mutex> lk(mutex_);
        if (e.completed) {
            return;
        }
        e.completed = true;
        e.retained = false;
        e.payload.reset();
        e.completed_code = rpc_code::canceled;
        e.cv.notify_all();
    } catch (...) {
        // A mutex acquisition failure in a destructor fallback must not
        // terminate the process; the suspended duplicates still wake through
        // their own wait bound.
    }
}

std::size_t dedup_table::abandon_impl(const void* wait_key) noexcept {
    if (wait_key == nullptr) {
        return 0;
    }
    std::size_t ended = 0;
    try {
        std::lock_guard<std::mutex> lk(mutex_);
        for (entry& e : entries_) {
            bool touched = false;
            for (entry::waiter& w : e.waiters) {
                if (w.wait_key == wait_key && !w.abandoned) {
                    w.abandoned = true;
                    touched = true;
                    ++ended;
                }
            }
            if (touched) {
                e.cv.notify_all();
            }
        }
    } catch (...) {
        // As above: never terminate from a teardown path. The waiters keep
        // their own wait bound.
        return ended;
    }
    return ended;
}

// --- bookkeeping ---------------------------------------------------------------

void dedup_table::erase_entry(entry& e) noexcept {
    const auto found = index_.find(e.key);
    if (found != index_.end()) {
        // The index holds the node's list iterator: erase through it instead of
        // walking the list (std::list nodes are stable, so the iterator is the
        // identity of the entry).
        entries_.erase(found->second);
        index_.erase(found);
    }
    evictions_.fetch_add(1, std::memory_order_relaxed);
}

bool dedup_table::evict_one_lru() {
    // Walk from the LRU tail (least recently used) toward the front. In-flight
    // entries are pinned. The scan is bounded: under a flood of in-flight keyed
    // calls the caller degrades to bypass instead of scanning the whole table
    // under the lock.
    auto it = entries_.end();
    for (std::size_t scanned = 0; scanned < 128 && it != entries_.begin(); ++scanned) {
        --it;
        entry& e = *it;
        if (!e.completed) {
            continue;
        }
        erase_entry(e);
        return true;
    }
    return false;
}

dedup_statistics dedup_table::stats() const noexcept {
    dedup_statistics snapshot;
    snapshot.claims_leader = claims_leader_.load(std::memory_order_relaxed);
    snapshot.claims_replay = claims_replay_.load(std::memory_order_relaxed);
    snapshot.claims_waiter = claims_waiter_.load(std::memory_order_relaxed);
    snapshot.claims_bypass = claims_bypass_.load(std::memory_order_relaxed);
    snapshot.waiters_woken = waiters_woken_.load(std::memory_order_relaxed);
    snapshot.waiters_abandoned = waiters_abandoned_.load(std::memory_order_relaxed);
    snapshot.waiters_expired = waiters_expired_.load(std::memory_order_relaxed);
    snapshot.evictions = evictions_.load(std::memory_order_relaxed);
    try {
        std::lock_guard<std::mutex> lk(mutex_);
        snapshot.entries = entries_.size();
    } catch (...) {
        // A snapshot must never throw; the entry count stays at the last read.
    }
    return snapshot;
}

} // namespace crpc::rpc
