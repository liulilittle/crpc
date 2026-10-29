// CRPC -- unit of work: connection ownership and the commit/rollback loops.
#include <crpc/orm/session.hpp>

#include <algorithm>
#include <utility>

namespace crpc {
namespace orm {

session::~session() = default;

result<session> session::open(db::pool& pool, dialect d) {
    result<db::pool::lease> lease = pool.acquire();
    if (!lease) {
        return lease.error();
    }
    session owned;
    owned.pool_ = &pool;
    owned.lease_ = std::move(lease.value());
    owned.dialect_ = d;
    return owned;
}

status session::execute(const std::string& sql, span<const parameter> params) {
    db::connection* conn = lease_.get();
    if (conn == nullptr || !conn->is_open()) {
        return status{rpc_code::shutting_down, "the session connection is closed"};
    }
    const status ran = conn->execute(sql, params);
    if (ran) {
        commit_log_.push_back(sql);
    }
    return ran;
}

result<db::result_set*> session::query(const std::string& sql,
                                       span<const parameter> params) {
    db::connection* conn = lease_.get();
    if (conn == nullptr || !conn->is_open()) {
        return rpc_error{rpc_code::shutting_down, "the session connection is closed"};
    }
    db::result_set* out = nullptr;
    const status ran = conn->query(sql, params, &out);
    if (!ran) {
        return ran.error();
    }
    commit_log_.push_back(sql);
    return out;
}

result<std::int64_t> session::last_insert_id() {
    db::connection* conn = lease_.get();
    if (conn == nullptr || !conn->is_open()) {
        return rpc_error{rpc_code::shutting_down, "the session connection is closed"};
    }
    sql::statement stmt;
    const status built = sql::build_last_insert_id(dialect_, stmt);
    if (!built) {
        return built.error();
    }
    db::result_set* out = nullptr;
    const status ran =
        conn->query(stmt.text, span<const parameter>(stmt.params.data(), stmt.params.size()),
                    &out);
    if (!ran) {
        return ran.error();
    }
    statement_guard guard(out);
    if (!guard.valid() || !guard->next()) {
        return rpc_error{rpc_code::internal, "the last-insert-id query returned no row"};
    }
    return guard->get_int64(0);
}

status session::commit() {
    db::connection* conn = lease_.get();
    if (conn == nullptr || !conn->is_open()) {
        return status{rpc_code::shutting_down, "the session connection is closed"};
    }

    bool has_work = false;
    for (const auto& record : records_) {
        if (record->pending()) {
            has_work = true;
            break;
        }
    }
    if (!has_work) {
        commit_log_.clear();
        return status{};
    }

    commit_log_.clear();
    const status started = conn->begin();
    if (!started) {
        return started;
    }

    // INSERT and UPDATE first, in record order; DELETEs after, so a foreign key
    // that a later insert repoints at is never orphaned mid-transaction.
    for (const auto& record : records_) {
        if (record->state() != entity_state::deleted) {
            const status flushed = record->flush(*this);
            if (!flushed) {
                (void)conn->rollback();
                return flushed;
            }
        }
    }
    for (const auto& record : records_) {
        if (record->state() == entity_state::deleted) {
            const status flushed = record->flush(*this);
            if (!flushed) {
                (void)conn->rollback();
                return flushed;
            }
        }
    }

    records_.erase(std::remove_if(records_.begin(), records_.end(),
                                  [](const std::unique_ptr<record_base>& record) {
                                      return record->state() == entity_state::deleted;
                                  }),
                   records_.end());
    return conn->commit();
}

status session::rollback() {
    db::connection* conn = lease_.get();
    if (conn != nullptr && conn->is_open()) {
        // Best effort: the in-memory revert below must happen even if the
        // connection refuses to roll back.
        (void)conn->rollback();
    }

    auto it = records_.begin();
    while (it != records_.end()) {
        const entity_state st = (*it)->state();
        if (!(*it)->persisted() && (st == entity_state::inserted || st == entity_state::deleted)) {
            it = records_.erase(it);
            continue;
        }
        // Anything else that still differs from (or diverges from) its snapshot
        // is reverted; a committed, untouched record is left alone.
        if ((*it)->pending()) {
            (*it)->revert();
        }
        ++it;
    }
    commit_log_.clear();
    return status{};
}

} // namespace orm
} // namespace crpc