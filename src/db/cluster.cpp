// CRPC -- primary/replica cluster: routing, health, transactions, consistency.
#include <crpc/db/cluster.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <crpc/db/types.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace db {
namespace {

using steady_clock = std::chrono::steady_clock;

const char* k_no_replica = "no replica configured";

[[nodiscard]] bool is_blank(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

[[nodiscard]] bool is_word(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '$' || c == '@';
}

[[nodiscard]] bool ascii_iequal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') {
            x = static_cast<char>(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = static_cast<char>(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}

// Advances past whitespace and the three comment shapes, returning the index of
// the next token (or `s.size()`).
[[nodiscard]] std::size_t skip_space_comments(std::string_view s, std::size_t i) noexcept {
    for (;;) {
        while (i < s.size() && is_blank(s[i])) {
            ++i;
        }
        if (i >= s.size()) {
            break;
        }
        if (s[i] == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            while (i < s.size() && s[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (s[i] == '#') {
            while (i < s.size() && s[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            bool closed = false;
            while (i + 1 < s.size()) {
                if (s[i] == '*' && s[i + 1] == '/') {
                    i += 2;
                    closed = true;
                    break;
                }
                ++i;
            }
            if (!closed) {
                i = s.size();
            }
            continue;
        }
        break;
    }
    return i;
}

// `i` is at an opening quote. Skips the quoted literal, honouring a doubled
// quote (SQL standard / TDS) and a backslash escape (MySQL).
[[nodiscard]] std::size_t skip_quoted(std::string_view s, std::size_t i) noexcept {
    const char quote = s[i];
    ++i;
    while (i < s.size()) {
        if (s[i] == quote) {
            if (i + 1 < s.size() && s[i + 1] == quote) {
                i += 2;
                continue;
            }
            ++i;
            break;
        }
        if (s[i] == '\\' && i + 1 < s.size()) {
            i += 2;
            continue;
        }
        ++i;
    }
    return i;
}

// `i` is at a `[` (TDS bracket identifier). Skips to the matching `]`.
[[nodiscard]] std::size_t skip_bracket(std::string_view s, std::size_t i) noexcept {
    ++i;
    while (i < s.size()) {
        if (s[i] == ']') {
            if (i + 1 < s.size() && s[i + 1] == ']') {
                i += 2;
                continue;
            }
            ++i;
            break;
        }
        ++i;
    }
    return i;
}

// A transport failure is one that says the endpoint is unreachable. An
// application error -- any reply with an SQL-level error, however severe --
// proves the endpoint is answering, so it must never be counted against a
// replica's health.
[[nodiscard]] bool is_transport_failure(rpc_code code) noexcept {
    switch (code) {
    case rpc_code::unavailable:
    case rpc_code::connection_refused:
    case rpc_code::connection_reset:
    case rpc_code::broken_pipe:
    case rpc_code::eof:
    case rpc_code::deadline_exceeded:
    case rpc_code::not_acknowledged:
    case rpc_code::shutting_down:
        return true;
    default:
        return false;
    }
}

// The last-write consistency token, shared between the cluster and every
// outstanding transaction. `capture_or_clear` samples the primary after a write
// and stores the token; a failed sample *clears* the token rather than keeping a
// stale one, because waiting on a stale token can return before a newer write
// has replicated -- which is exactly the stale read the guard exists to forbid.
struct write_state {
    std::mutex mutex;
    std::string token;
};

void capture_or_clear(const std::shared_ptr<write_state>& state,
                      replica_consistency& consistency, connection& conn) {
    std::string token;
    const status sampled = consistency.capture(conn, token);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->token = sampled.ok() ? token : std::string();
}

[[nodiscard]] std::string current_token(const std::shared_ptr<write_state>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->token;
}

} // namespace

const char* to_string(read_route route) noexcept {
    switch (route) {
    case read_route::primary:
        return "primary";
    case read_route::replica:
        return "replica";
    case read_route::prefer_replica:
        return "prefer_replica";
    case read_route::round_robin:
        return "round_robin";
    }
    return "primary";
}

const char* to_string(replica_depleted_policy policy) noexcept {
    switch (policy) {
    case replica_depleted_policy::fallback_to_primary:
        return "fallback_to_primary";
    case replica_depleted_policy::error:
        return "error";
    }
    return "fallback_to_primary";
}

statement_kind classify_statement(std::string_view sql) noexcept {
    static constexpr std::string_view write_verbs[] = {
        "insert",   "update",  "delete",  "replace", "truncate", "create", "drop",
        "alter",    "rename",  "begin",   "start",   "commit",   "rollback", "savepoint",
        "release",  "set",     "use",     "call",    "load",     "grant",  "revoke",
        "lock",     "unlock",  "flush",   "reset",   "kill",     "xa",     "analyze",
        "optimize", "check",   "checksum", "repair", "change",   "purge",  "install",
        "uninstall", "shutdown", "binlog", "cache",  "handler",  "merge",  "backup",
        "restore",  "bulk"};
    static constexpr std::string_view read_verbs[] = {
        "select", "show", "describe", "desc", "explain", "with", "values", "table"};

    std::vector<std::string_view> tokens;
    tokens.reserve(8);
    std::size_t i = 0;
    while (i < sql.size()) {
        i = skip_space_comments(sql, i);
        if (i >= sql.size()) {
            break;
        }
        const char c = sql[i];
        if (c == '\'' || c == '"' || c == '`') {
            i = skip_quoted(sql, i);
            continue;
        }
        if (c == '[') {
            i = skip_bracket(sql, i);
            continue;
        }
        if (!is_word(c)) {
            ++i;
            continue;
        }
        // TDS unicode/hex literal prefix (N'...' / X'...').
        if ((c == 'N' || c == 'n' || c == 'X' || c == 'x') && i + 1 < sql.size() &&
            (sql[i + 1] == '\'' || sql[i + 1] == '"')) {
            i = skip_quoted(sql, i + 1);
            continue;
        }
        const std::size_t start = i;
        while (i < sql.size() && is_word(sql[i])) {
            ++i;
        }
        tokens.push_back(sql.substr(start, i - start));
    }

    // Any write verb anywhere in the token stream makes the statement a write,
    // so a batch starting with SELECT cannot smuggle an INSERT to a replica.
    for (const std::string_view token : tokens) {
        for (const std::string_view verb : write_verbs) {
            if (ascii_iequal(token, verb)) {
                return statement_kind::write;
            }
        }
    }
    if (!tokens.empty()) {
        for (const std::string_view verb : read_verbs) {
            if (ascii_iequal(tokens.front(), verb)) {
                return statement_kind::read;
            }
        }
    }
    // A statement this classifier cannot prove to be a read is routed as a
    // write. Misrouting a read costs a primary round trip; misrouting a write
    // would corrupt a replica, so the classifier errs toward the primary.
    return statement_kind::write;
}

replica_consistency::~replica_consistency() = default;

struct cluster::impl {
    struct replica_node {
        std::unique_ptr<pool> connections;
        std::string name;
        bool alive = true;
        bool probing = false;
        std::uint32_t consecutive_failures = 0;
        rpc_code last_error = rpc_code::ok;
        std::string last_error_message;
        steady_clock::time_point dead_since{};
    };

    std::unique_ptr<connection_factory> primary_factory;
    std::unique_ptr<pool> primary_pool;
    std::vector<std::unique_ptr<connection_factory>> replica_factories;
    std::vector<replica_node> replicas;
    std::shared_ptr<replica_consistency> consistency;
    std::shared_ptr<write_state> writes_state;
    cluster_options options;
    mutable std::mutex health_mutex;
    std::uint64_t rotation = 0;
    bool closed = false;

    // --- health -------------------------------------------------------------

    void mark_reachable_locked(std::size_t index) {
        replica_node& node = replicas[index];
        node.consecutive_failures = 0;
        node.last_error = rpc_code::ok;
        node.last_error_message.clear();
        if (!node.alive) {
            node.alive = true;
            node.dead_since = steady_clock::time_point{};
        }
    }

    void mark_failure_locked(std::size_t index, const rpc_error& error) {
        if (!is_transport_failure(error.code)) {
            mark_reachable_locked(index);
            return;
        }
        replica_node& node = replicas[index];
        node.last_error = error.code;
        node.last_error_message = error.message;
        if (node.consecutive_failures != std::numeric_limits<std::uint32_t>::max()) {
            ++node.consecutive_failures;
        }
        if (node.alive && options.replica_failure_threshold != 0 &&
            node.consecutive_failures >= options.replica_failure_threshold) {
            node.alive = false;
            node.dead_since = steady_clock::now();
        } else if (!node.alive) {
            node.dead_since = steady_clock::now();
        }
    }

    void note_reachable(std::size_t index) {
        std::lock_guard<std::mutex> lock(health_mutex);
        mark_reachable_locked(index);
    }

    void note_failure(std::size_t index, const rpc_error& error) {
        std::lock_guard<std::mutex> lock(health_mutex);
        mark_failure_locked(index, error);
    }

    [[nodiscard]] bool ping_replica(std::size_t index) {
        result<pool::lease> lease = replicas[index].connections->acquire();
        if (!lease) {
            return false;
        }
        const status pinged = lease.value()->ping();
        (void)lease.value().release();
        return pinged.ok();
    }

    // Probes every dead replica whose cooldown has elapsed. Called at the start
    // of a read that may use replicas; probing happens outside the health lock.
    // A failed probe restarts the cooldown, so a permanently down replica is
    // not retried on every call.
    void maybe_probe() {
        if (replicas.empty()) {
            return;
        }
        std::vector<std::size_t> due;
        {
            std::lock_guard<std::mutex> lock(health_mutex);
            const steady_clock::time_point now = steady_clock::now();
            for (std::size_t index = 0; index < replicas.size(); ++index) {
                replica_node& node = replicas[index];
                if (node.alive || node.probing) {
                    continue;
                }
                if (options.replica_retry_cooldown.count() > 0 &&
                    now - node.dead_since < options.replica_retry_cooldown) {
                    continue;
                }
                node.probing = true;
                due.push_back(index);
            }
        }
        for (const std::size_t index : due) {
            const bool ok = ping_replica(index);
            std::lock_guard<std::mutex> lock(health_mutex);
            replicas[index].probing = false;
            if (ok) {
                mark_reachable_locked(index);
            } else {
                replicas[index].dead_since = steady_clock::now();
            }
        }
    }

    // --- primary ------------------------------------------------------------

    result<result_set*> run_on_primary(std::string_view sql, span<const parameter> params,
                                       bool capture) {
        result<pool::lease> lease = primary_pool->acquire();
        if (!lease) {
            return lease.error();
        }
        result_set* rows = nullptr;
        const status ran = lease.value()->query(sql, params, &rows);
        if (!ran) {
            (void)lease.value().release();
            return ran.error();
        }
        if (capture && consistency) {
            capture_or_clear(writes_state, *consistency, *lease.value().get());
        }
        (void)lease.value().release();
        return rows;
    }

    // --- reads --------------------------------------------------------------

    result<result_set*> run_read(std::string_view sql, span<const parameter> params,
                                 const route_hint& hint) {
        maybe_probe();

        rpc_error immediate_error;
        bool base_primary = false;
        bool fallback_primary = false;
        std::vector<std::size_t> order;
        {
            std::lock_guard<std::mutex> lock(health_mutex);

            const bool hinted_primary = hint.reads == route_hint::target::primary;
            const bool hinted_replica = hint.reads == route_hint::target::replica;
            const bool natural_primary =
                !hinted_replica && options.reads == read_route::primary;
            if (hinted_primary || natural_primary) {
                base_primary = true;
            } else {
                bool fallback_allowed;
                if (options.reads == read_route::prefer_replica && !hinted_replica) {
                    fallback_allowed = true;
                } else {
                    fallback_allowed =
                        options.depleted == replica_depleted_policy::fallback_to_primary;
                }

                std::vector<std::size_t> live;
                for (std::size_t index = 0; index < replicas.size(); ++index) {
                    if (replicas[index].alive) {
                        live.push_back(index);
                    }
                }
                if (live.empty()) {
                    if (replicas.empty()) {
                        // prefer_replica means "a replica is a convenience";
                        // with none configured the primary serves. Any stricter
                        // mode with zero replicas is a configuration mistake and
                        // says so.
                        if (options.reads == read_route::prefer_replica && !hinted_replica) {
                            base_primary = true;
                        } else {
                            immediate_error =
                                rpc_error{rpc_code::failed_precondition, k_no_replica};
                        }
                    } else if (fallback_allowed) {
                        base_primary = true;
                    } else {
                        immediate_error =
                            rpc_error{rpc_code::unavailable, "no replica alive"};
                    }
                } else if (options.reads == read_route::round_robin && !hinted_replica) {
                    // A natural round-robin read rotates the primary into the
                    // sequence: position 0 of the slot ring is the primary.
                    const std::size_t slots = 1 + live.size();
                    const std::size_t position = rotation % slots;
                    ++rotation;
                    if (position == 0) {
                        base_primary = true;
                    } else {
                        order.reserve(live.size());
                        for (std::size_t k = 0; k < live.size(); ++k) {
                            order.push_back(live[(position - 1 + k) % live.size()]);
                        }
                        fallback_primary = fallback_allowed;
                    }
                } else {
                    const std::size_t start = rotation % live.size();
                    ++rotation;
                    order.reserve(live.size());
                    for (std::size_t k = 0; k < live.size(); ++k) {
                        order.push_back(live[(start + k) % live.size()]);
                    }
                    fallback_primary = fallback_allowed;
                }
            }
        }

        if (!immediate_error.ok()) {
            return immediate_error;
        }
        if (base_primary) {
            return run_on_primary(sql, params, false);
        }

        rpc_error last_error{rpc_code::unavailable, "no replica available"};
        bool have_error = false;
        for (const std::size_t index : order) {
            result<pool::lease> lease = replicas[index].connections->acquire();
            if (!lease) {
                note_failure(index, lease.error());
                last_error = lease.error();
                have_error = true;
                continue;
            }
            // Consistent reads wait for the last captured write before the
            // SELECT. A replica that answers but is behind is not a transport
            // failure: it stays alive for other reads, and the wait's own error
            // is what the caller sees if no replica catches up.
            if (hint.consistent && consistency) {
                const std::string token = current_token(writes_state);
                if (!token.empty()) {
                    const status waited = consistency->wait(*lease.value().get(), token);
                    if (!waited) {
                        note_reachable(index);
                        last_error = waited.error();
                        have_error = true;
                        (void)lease.value().release();
                        continue;
                    }
                }
            }
            result_set* rows = nullptr;
            const status ran = lease.value()->query(sql, params, &rows);
            (void)lease.value().release();
            if (ran.ok()) {
                note_reachable(index);
                return rows;
            }
            if (is_transport_failure(ran.error().code)) {
                note_failure(index, ran.error());
                last_error = ran.error();
                have_error = true;
                continue;
            }
            // An application error proves the replica answered; it is forwarded
            // unchanged, and the replica is not penalised.
            note_reachable(index);
            return ran.error();
        }

        if (fallback_primary) {
            return run_on_primary(sql, params, false);
        }
        return have_error ? last_error
                          : rpc_error{rpc_code::unavailable, "no replica available"};
    }
};

// --- cluster ----------------------------------------------------------------

cluster::cluster(std::unique_ptr<connection_factory> primary_factory,
                 std::vector<std::unique_ptr<connection_factory>> replica_factories,
                 std::shared_ptr<replica_consistency> consistency, cluster_options options)
    : impl_(new impl) {
    impl_->options = options;
    impl_->primary_factory = std::move(primary_factory);
    impl_->primary_pool = std::make_unique<pool>(options.pool, *impl_->primary_factory);
    impl_->replica_factories = std::move(replica_factories);
    impl_->consistency = std::move(consistency);
    impl_->writes_state = std::make_shared<write_state>();
    impl_->replicas.reserve(impl_->replica_factories.size());
    for (std::size_t index = 0; index < impl_->replica_factories.size(); ++index) {
        impl::replica_node node;
        node.connections = std::make_unique<pool>(options.pool, *impl_->replica_factories[index]);
        node.name = index < options.replica_names.size() && !options.replica_names[index].empty()
                        ? options.replica_names[index]
                        : std::string(impl_->replica_factories[index]->name());
        impl_->replicas.push_back(std::move(node));
    }
}

cluster::~cluster() = default;

result<result_set*> cluster::query(std::string_view sql, span<const parameter> params,
                                   route_hint hint) {
    if (impl_->closed) {
        return rpc_error{rpc_code::shutting_down, "database cluster is closed"};
    }
    const statement_kind kind = classify_statement(sql);
    if (kind == statement_kind::write) {
        if (hint.reads == route_hint::target::replica) {
            return rpc_error{rpc_code::invalid_argument,
                             "a write statement cannot be routed to a replica"};
        }
        return impl_->run_on_primary(sql, params, impl_->consistency != nullptr);
    }
    return impl_->run_read(sql, params, hint);
}

result<result_set*> cluster::call(std::string_view sql, span<parameter> params) {
    if (impl_->closed) {
        return rpc_error{rpc_code::shutting_down, "database cluster is closed"};
    }
    // A stored procedure may write, so it always runs on the primary and is
    // included in the consistency sampling like any other write.
    return impl_->run_on_primary(sql, span<const parameter>(params.data(), params.size()),
                                 impl_->consistency != nullptr);
}

status cluster::execute(std::string_view sql, span<const parameter> params, route_hint hint) {
    result<result_set*> rows = query(sql, params, hint);
    if (!rows) {
        return rows.error();
    }
    delete rows.value();
    return status{};
}

result<cluster_txn> cluster::begin_transaction() {
    if (impl_->closed) {
        return rpc_error{rpc_code::shutting_down, "database cluster is closed"};
    }
    result<pool::lease> lease = impl_->primary_pool->acquire();
    if (!lease) {
        return lease.error();
    }
    const status began = lease.value()->begin();
    if (!began) {
        (void)lease.value().release();
        return began.error();
    }
    return cluster_txn(std::move(lease.value()), impl_->consistency, impl_->writes_state);
}

std::vector<::crpc::db::replica_health> cluster::replica_health() const {
    std::vector<::crpc::db::replica_health> out;
    out.reserve(impl_->replicas.size());
    std::lock_guard<std::mutex> lock(impl_->health_mutex);
    const steady_clock::time_point now = steady_clock::now();
    for (const impl::replica_node& node : impl_->replicas) {
        ::crpc::db::replica_health entry;
        entry.name = node.name;
        entry.alive = node.alive;
        entry.consecutive_failures = node.consecutive_failures;
        entry.last_error = node.last_error;
        entry.last_error_message = node.last_error_message;
        if (!node.alive) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - node.dead_since);
            if (impl_->options.replica_retry_cooldown.count() > 0 &&
                elapsed < impl_->options.replica_retry_cooldown) {
                entry.retry_in = impl_->options.replica_retry_cooldown - elapsed;
            }
        }
        out.push_back(std::move(entry));
    }
    return out;
}

result<bool> cluster::probe_replica(std::size_t index) {
    if (index >= impl_->replicas.size()) {
        return rpc_error{rpc_code::invalid_argument, "cluster replica index is out of range"};
    }
    {
        std::lock_guard<std::mutex> lock(impl_->health_mutex);
        impl_->replicas[index].probing = true;
    }
    const bool reachable = impl_->ping_replica(index);
    std::lock_guard<std::mutex> lock(impl_->health_mutex);
    impl_->replicas[index].probing = false;
    if (reachable) {
        impl_->mark_reachable_locked(index);
        return true;
    }
    // A failed probe starts a fresh cooldown. Ejection rules apply as usual:
    // with a threshold of 0 a live node is not marked dead by a failed probe.
    impl_->mark_failure_locked(
        index, rpc_error{rpc_code::connection_refused,
                         impl_->replicas[index].name + " probe failed"});
    return false;
}

std::size_t cluster::replica_count() const noexcept { return impl_->replicas.size(); }

std::uint64_t cluster::primary_opened() const noexcept {
    return impl_->primary_pool->opened();
}

result<std::uint64_t> cluster::replica_opened(std::size_t index) const {
    if (index >= impl_->replicas.size()) {
        return rpc_error{rpc_code::invalid_argument, "cluster replica index is out of range"};
    }
    return impl_->replicas[index].connections->opened();
}

void cluster::close() noexcept {
    if (!impl_) {
        return;
    }
    impl_->closed = true;
    impl_->primary_pool->close();
    for (impl::replica_node& node : impl_->replicas) {
        node.connections->close();
    }
}

bool cluster::is_closed() const noexcept { return impl_->closed; }

// --- cluster_txn -------------------------------------------------------------

cluster_txn::cluster_txn(pool::lease lease, std::shared_ptr<replica_consistency> consistency,
                         std::shared_ptr<void> write_state)
    : lease_(std::move(lease)),
      consistency_(std::move(consistency)),
      write_state_(std::move(write_state)),
      finished_(false) {}

cluster_txn::~cluster_txn() {
    if (!finished_) {
        (void)finish(false);
    }
}

cluster_txn::cluster_txn(cluster_txn&& other) noexcept
    : lease_(std::move(other.lease_)),
      consistency_(std::move(other.consistency_)),
      write_state_(std::move(other.write_state_)),
      finished_(other.finished_) {
    other.finished_ = true;
}

cluster_txn& cluster_txn::operator=(cluster_txn&& other) noexcept {
    if (this != &other) {
        if (!finished_) {
            (void)finish(false);
        }
        lease_ = std::move(other.lease_);
        consistency_ = std::move(other.consistency_);
        write_state_ = std::move(other.write_state_);
        finished_ = other.finished_;
        other.finished_ = true;
    }
    return *this;
}

result<result_set*> cluster_txn::query(std::string_view sql, span<const parameter> params,
                                       route_hint hint) {
    if (finished_ || !lease_.valid()) {
        return rpc_error{rpc_code::failed_precondition, "transaction is closed"};
    }
    if (hint.reads == route_hint::target::replica) {
        return rpc_error{rpc_code::invalid_argument,
                         "a transaction runs on its primary connection and cannot be "
                         "routed to a replica"};
    }
    result_set* rows = nullptr;
    const status ran = lease_.get()->query(sql, params, &rows);
    if (!ran) {
        return ran.error();
    }
    return rows;
}

status cluster_txn::execute(std::string_view sql, span<const parameter> params) {
    result<result_set*> rows = query(sql, params, {});
    if (!rows) {
        return rows.error();
    }
    delete rows.value();
    return status{};
}

status cluster_txn::commit() { return finish(true); }

status cluster_txn::rollback() { return finish(false); }

status cluster_txn::finish(bool commit_outcome) {
    if (finished_) {
        return status{};
    }
    finished_ = true;
    status outcome = commit_outcome ? lease_->commit() : lease_->rollback();
    // A committed transaction's write position must be sampled before the
    // connection goes back to the pool, or a later consistent read has nothing
    // to wait on. A failed sample clears the token rather than keeping a stale
    // one (see `capture_or_clear`).
    if (commit_outcome && outcome.ok() && consistency_) {
        capture_or_clear(std::static_pointer_cast<write_state>(write_state_), *consistency_,
                         *lease_.get());
    }
    const status released = lease_.release();
    return outcome.ok() ? (released.ok() ? status{} : released) : outcome;
}

} // namespace db
} // namespace crpc