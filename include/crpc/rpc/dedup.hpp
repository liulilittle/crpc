// CRPC -- server-side idempotency dedup table.
//
// This closes the "key carried, no dedup table" gap: `call_id` (the R6 triple
// `(client_id, client_epoch, seq)`) travels in the call envelope and is reused
// unchanged by every retry, so the server can recognize a replayed request.
// Without a table, a lost response plus a client retry executes the handler
// twice -- a correctness incident for every non-idempotent operation (money
// transfer, stock decrement, order creation).
//
// Semantics
// ---------
// One entry per accepted call. On a keyed request the dispatcher asks the
// table for a claim:
//
//   * fresh key            -> `leader`: this dispatch runs the handler and must
//                             publish the outcome with `lease::complete`.
//   * key still in flight  -> `served`: the caller is SUSPENDED inside `claim`
//                             until the leader publishes, then resumes with the
//                             leader's exact encoded response. A duplicate is
//                             never simply dropped: the retrying client waits
//                             for a response.
//   * key completed and
//     cached               -> `replay`: the cached encoded response is resent
//                             without running the handler.
//   * key completed but
//     uncached (see below) -> treated as fresh: the handler runs again.
//
// Only SUCCESSFUL responses are cached and replayed. A failed completion
// completes the entry as an uncached tombstone: concurrent duplicates of the
// same attempt still receive the leader's error (they were suspended against
// that one execution), but a LATER replay re-executes, because the first
// attempt produced no result and a retry must be able to succeed. This is what
// keeps the client-side retry policy meaningful (`retry_idempotent_only`
// depends on a failed first attempt being retried for real).
//
// Bounds (all defaults overridable through `dedup_options`)
// --------------------------------------------------------
//   * capacity: the table holds at most `capacity` entries. Eviction is LRU --
//     the least recently used completed entry is dropped when a new key needs
//     room; in-flight entries are pinned until their leader completes. Why LRU
//     and not the per-client FIFO ring of the design sketch: a per-client ring
//     is unbounded in the number of clients, which is exactly the memory
//     growth this table must not have; LRU bounds memory for any client
//     population and keeps the keys whose retries are still plausible.
//   * entry_ttl: the dedup window. An entry older than the TTL (measured with
//     the dispatcher's monotonic clock from the dispatch instant -- never a
//     wall clock) no longer deduplicates: a replay re-executes. TTL <= 0
//     disables time-based expiry; capacity remains the only bound.
//   * max_cached_bytes: a response larger than this is NOT cached. Its
//     concurrent duplicates still receive it (handed over at completion), but a
//     later replay re-executes. Zero disables caching entirely (dedup window =
//     the in-flight window only).
//   * max_wait: how long a suspended duplicate may block its shard thread.
//     The dispatcher additionally bounds the wait by the request's own wire
//     deadline. `max_wait <= 0` disables suspension: an in-flight duplicate is
//     refused immediately with an explicit retryable error.
//
// When the table cannot admit a new key (full of in-flight entries), the call
// is executed WITHOUT dedup (`claims_bypass`) -- a degradation to at-least-once,
// never a hang and never unbounded memory.
//
// Known, documented limits
// ------------------------
//   * After eviction or TTL expiry a replay re-executes: the exactly-once
//     guarantee holds only inside the window `min(TTL, LRU lifetime)`. This is
//     the固有限制 of any bounded table and is why the client's retry budget
//     (requirement 16) and the server window must be sized together.
//   * `client_epoch` changes (client restart) create fresh keys; old-epoch
//     entries can never shadow a new-epoch call (the epoch is part of the key)
//     and age out through the same TTL and LRU bounds -- nothing is pinned
//     forever.
//   * The table is per process. Dispatches from every shard of one server
//     process share it (all shards call the same `dispatcher`), so the guarantee
//     is process-wide. Multiple server processes/nodes each dedup independently;
//     routing a given `call_id` to the same node is a load-balancer concern and
//     is NOT solved here.
//   * A suspended duplicate blocks one shard thread. With N shards, N
//     concurrent duplicates can hold all N threads for at most the suspension
//     bound; the wait is always bounded (wire deadline, then `max_wait`).
//
// ABI note
// --------
// `dedup_options` and `dedup_statistics` are consumed only by INLINE
// constructors and inline member functions, so their layout never crosses the
// shared-library boundary (docs/abi.md section 4 excludes exactly this shape);
// if the inline constructors are ever moved out of line, the abi docs test
// forces the record to be guarded or listed. `dedup_table` and its nested
// `lease` are `CRPC_API` classes with private members, not plain records.
//
// Thread safety: every method is safe for concurrent use. The handler always
// runs outside the table lock; only claim/complete/abandon take it.
#ifndef CRPC_RPC_DEDUP_HPP
#define CRPC_RPC_DEDUP_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/span.hpp>

namespace crpc::rpc {

// Knobs of the dedup table. Defaults are documented on the class.
struct dedup_options {
    // Maximum number of entries. Zero disables the table entirely (every call
    // behaves as if no key were present).
    std::size_t capacity = 4096;
    // Per-entry cap on the cached encoded response. Larger responses are not
    // cached (their concurrent duplicates still receive them).
    std::size_t max_cached_bytes = 16384;
    // Dedup window measured from the dispatch instant. Non-positive disables
    // time-based expiry (capacity remains the bound).
    std::chrono::milliseconds entry_ttl{300000};
    // Upper bound on suspending a duplicate. Non-positive disables suspension.
    std::chrono::milliseconds max_wait{30000};
};

// Snapshot of the monotonic counters. Returned by the inline `stats()`, so the
// record itself never crosses the shared-library boundary.
struct dedup_statistics {
    u64 claims_leader = 0;     // fresh key: caller executes the handler
    u64 claims_replay = 0;     // completed and cached: response replayed
    u64 claims_waiter = 0;     // in flight: caller suspended until completion
    u64 claims_bypass = 0;     // table saturated: executed without dedup
    u64 waiters_woken = 0;     // suspended duplicates handed the leader outcome
    u64 waiters_abandoned = 0; // suspended duplicates ended by connection close
    u64 waiters_expired = 0;   // suspended duplicates that hit the wait bound
    u64 evictions = 0;         // entries evicted (LRU room, expiry, re-execute)
    u64 entries = 0;           // live entries at snapshot time
};

class CRPC_API dedup_table {
private:
    // Forward declaration in the SAME access section as the definition below
    // (redeclaring a nested class with different access is a non-standard
    // extension that MSVC rejects under /W4).
    struct entry;

public:
    // The claim a dispatch holds. Move-only. For a leader it is RAII: if it is
    // destroyed without `complete` having published (a dispatch that exited
    // through an unexpected path), the destructor completes the entry with
    // `canceled` and wakes the suspended duplicates -- a waiter can never be
    // stranded by a leader that died mid-flight.
    class CRPC_API lease {
    public:
        enum class action : u8 {
            none = 0,     // no dedup (empty key, table disabled or saturated)
            leader,       // run the handler, then publish with `complete`
            replay,       // cached response: resend `replay_payload`
            served,       // leader finished while suspended: resend its outcome
            abandoned,    // connection closed while suspended
            expired,      // the wait bound elapsed before the leader finished
            duplicate     // one-way duplicate: drop silently
        };

        lease() noexcept = default;
        ~lease();
        lease(lease&& other) noexcept;
        lease& operator=(lease&& other) noexcept;
        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;

        [[nodiscard]] action state() const noexcept { return state_; }
        [[nodiscard]] bool is_leader() const noexcept {
            return state_ == action::leader && table_ != nullptr && entry_ != nullptr;
        }
        [[nodiscard]] bool inactive() const noexcept { return state_ == action::none; }

        // Leader only: publishes the encoded RESPONSE payload (success or
        // error) and wakes every suspended duplicate. `ok` selects the caching
        // policy: only successes within `max_cached_bytes` are retained for
        // later replays; everything else completes as an uncached tombstone.
        // `code` is the fallback answer for a payload-less completion.
        void complete(span<const std::byte> payload, bool ok, rpc_code code);

        // Leader only, allocation-free fallback used on the unwinding path.
        void complete_failed() noexcept;

        // For `replay`/`served`: the exact encoded RESPONSE payload the leader
        // produced, or null when the outcome carried no payload (then
        // `replay_code` names the error to answer with).
        [[nodiscard]] std::shared_ptr<const std::vector<std::byte>> replay_payload()
            const noexcept {
            return payload_;
        }
        [[nodiscard]] rpc_code replay_code() const noexcept { return code_; }

    private:
        friend class dedup_table;
        dedup_table* table_ = nullptr;
        entry* entry_ = nullptr;
        action state_ = action::none;
        std::shared_ptr<const std::vector<std::byte>> payload_;
        rpc_code code_ = rpc_code::ok;
    };

    dedup_table() noexcept : options_{} {}
    // INLINE on purpose: the options are consumed here, in the consumer's
    // translation unit, so their layout never crosses the shared-library
    // boundary (docs/abi.md section 4).
    explicit dedup_table(const dedup_options& options) noexcept
        : options_(options) {}
    ~dedup_table();

    dedup_table(const dedup_table&) = delete;
    dedup_table& operator=(const dedup_table&) = delete;
    dedup_table(dedup_table&&) = delete;
    dedup_table& operator=(dedup_table&&) = delete;

    // Asks for the right to execute one keyed call.
    //
    // `now` is the dispatch instant (the TTL anchor); `wait_deadline` is the
    // absolute instant a suspended duplicate gives up at; `wait_key` is the
    // opaque connection identity passed to `abandon` (null = not abandonable);
    // `wait_for_response` is false for ONE_WAY frames, whose duplicates are
    // dropped instead of suspended. Blocks only while suspended as a duplicate.
    [[nodiscard]] lease claim(const call_id& key, u64 service_id, u64 method_id,
                              std::chrono::steady_clock::time_point now,
                              std::chrono::steady_clock::time_point wait_deadline,
                              const void* wait_key, bool wait_for_response) {
        if (options_.capacity == 0) {
            return lease{};
        }
        return claim_impl(key, service_id, method_id, now, wait_deadline, wait_key,
                          wait_for_response);
    }

    // Ends every suspended duplicate whose `wait_key` matches with an explicit
    // `canceled` error. Called when a connection closes; returns the number of
    // waiters ended. A null key never matches anything.
    std::size_t abandon(const void* wait_key) noexcept {
        if (wait_key == nullptr) {
            return 0;
        }
        return abandon_impl(wait_key);
    }

    [[nodiscard]] const dedup_options& options() const noexcept { return options_; }

    [[nodiscard]] dedup_statistics stats() const noexcept;

private:
    lease claim_impl(const call_id& key, u64 service_id, u64 method_id,
                     std::chrono::steady_clock::time_point now,
                     std::chrono::steady_clock::time_point wait_deadline,
                     const void* wait_key, bool wait_for_response);
    void complete_impl(entry& e, span<const std::byte> payload, bool ok,
                       rpc_code code);
    void complete_failed_impl(entry& e) noexcept;
    std::size_t abandon_impl(const void* wait_key) noexcept;
    // Drops the least recently used completed entry; returns false when the
    // scan found none (the table stays full and the caller must bypass).
    [[nodiscard]] bool evict_one_lru();
    void erase_entry(entry& e) noexcept;

    dedup_options options_;
    // Monotonic counters; `stats()` reads them with relaxed loads.
    std::atomic<u64> claims_leader_{0};
    std::atomic<u64> claims_replay_{0};
    std::atomic<u64> claims_waiter_{0};
    std::atomic<u64> claims_bypass_{0};
    std::atomic<u64> waiters_woken_{0};
    std::atomic<u64> waiters_abandoned_{0};
    std::atomic<u64> waiters_expired_{0};
    std::atomic<u64> evictions_{0};

    // The table key. The R6 triple alone is NOT unique across methods: the
    // design allocates `seq` per `(client_id, target_service)`, so the same
    // triple can recur under a different method. The envelope ids scope the
    // entry and rule out cross-method false replays.
    struct table_key {
        call_id id;
        u64 service_id = 0;
        u64 method_id = 0;
    };

    // One table record. Complete here because the LRU list stores it by value.
    struct entry {
        table_key key{};

        // Valid once `completed`: the TTL anchor (the dispatch instant) plus
        // the configured window.
        std::chrono::steady_clock::time_point expires{};

        // False while the leader is executing the handler; true once the
        // outcome is published. An in-flight entry is pinned against eviction.
        bool completed = false;
        // True only when `payload` is kept for later replays (success within
        // the cache bound).
        bool retained = false;
        // The encoded RESPONSE payload. For a retained entry it lives until
        // the entry is evicted; for an uncached one it is released as soon as
        // every duplicate suspended at completion has taken its copy.
        std::shared_ptr<const std::vector<std::byte>> payload;
        // Fallback answer when the completion carried no payload.
        rpc_code completed_code = rpc_code::ok;

        // One suspended duplicate. `id` identifies the record to its owner
        // across vector reallocation; `wait_key` is the connection identity
        // `abandon` matches.
        struct waiter {
            u64 id = 0;
            const void* wait_key = nullptr;
            bool abandoned = false;
        };

        std::vector<waiter> waiters;
        // Per-entry so a completion wakes only the duplicates of this call.
        // The entry is non-movable (std::list constructs in place) and the
        // table mutex is the associated mutex for every wait.
        std::condition_variable cv;
    };

    struct key_hash {
        // FNV-1a over the canonical key form plus the envelope ids.
        [[nodiscard]] std::size_t operator()(const table_key& key) const noexcept {
            u64 hash = 1469598103934665603ull;
            const auto mix = [&hash](const std::byte* bytes, std::size_t count) {
                for (std::size_t i = 0; i < count; ++i) {
                    hash ^= static_cast<u64>(bytes[i]);
                    hash *= 1099511628211ull;
                }
            };
            mix(key.id.client_id.data(), key.id.client_id.size());
            mix(reinterpret_cast<const std::byte*>(&key.id.client_epoch),
                sizeof(key.id.client_epoch));
            mix(reinterpret_cast<const std::byte*>(&key.id.seq), sizeof(key.id.seq));
            mix(reinterpret_cast<const std::byte*>(&key.service_id),
                sizeof(key.service_id));
            mix(reinterpret_cast<const std::byte*>(&key.method_id),
                sizeof(key.method_id));
            return static_cast<std::size_t>(hash);
        }
    };

    struct key_equal {
        [[nodiscard]] bool operator()(const table_key& a, const table_key& b) const noexcept {
            return a.id.client_epoch == b.id.client_epoch && a.id.seq == b.id.seq &&
                   a.id.client_id == b.id.client_id && a.service_id == b.service_id &&
                   a.method_id == b.method_id;
        }
    };

    // Everything below is guarded by `mutex_`. The handler runs outside it.
    mutable std::mutex mutex_;
    // LRU order: front = most recently used. Nodes are stable (std::list), so
    // entry pointers and iterators survive inserts, touches and erases of
    // other entries.
    std::list<entry> entries_;
    std::unordered_map<table_key, std::list<entry>::iterator, key_hash, key_equal> index_;
    u64 waiter_seq_ = 0;
};

} // namespace crpc::rpc

#endif // CRPC_RPC_DEDUP_HPP
