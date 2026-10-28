// CRPC -- the unit of work and the type-safe query builder.
//
// A `session` owns one connection lease for its whole lifetime, so a commit
// runs inside a single server transaction and "only update the dirty columns"
// means exactly that. Entities are tracked by whole-object snapshot: the
// reflected field codecs compare `current` against `original`, so a field that
// was changed twice before commit still yields one UPDATE whose SET list holds
// the net change. An entity that was loaded and then left alone is not written
// at all.
//
// The query builder is type-safe in the sense that matters for an ORM: a
// condition is anchored to a `&Account::email`-style member pointer, which
// fixes both the column (resolved by reflected member-pointer equality) and the
// value type (converted through the field's own codec). Wrong column or wrong
// value type is a compile error; wrong value is a status.
#ifndef CRPC_ORM_SESSION_HPP
#define CRPC_ORM_SESSION_HPP

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/error.hpp>
#include <crpc/orm/fwd.hpp>
#include <crpc/orm/metadata.hpp>
#include <crpc/orm/sql.hpp>
#include <crpc/orm/value.hpp>

namespace crpc {
namespace orm {

class session;
template <typename T>
class query_builder;

enum class entity_state : std::uint8_t {
    clean,
    inserted,
    modified,
    deleted
};

class record_base {
public:
    virtual ~record_base() = default;
    virtual entity_state state() const noexcept = 0;
    virtual bool persisted() const noexcept = 0;
    // True when a flush is needed: queued, or modified against the snapshot.
    virtual bool pending() const noexcept = 0;
    virtual status flush(session& owner) = 0;
    virtual void revert() = 0;
};

template <typename T>
class record final : public record_base {
public:
    T current;
    T original;
    entity_state st_ = entity_state::clean;
    bool persisted_ = false;

    entity_state state() const noexcept override { return st_; }
    bool persisted() const noexcept override { return persisted_; }
    bool pending() const noexcept override {
        if (st_ == entity_state::inserted || st_ == entity_state::deleted) {
            return true;
        }
        const entity_meta* meta = entity_metadata<T>();
        for (const column_meta& column : meta->columns) {
            if (column.differs(&current, &original)) {
                return true;
            }
        }
        return false;
    }
    void revert() override {
        current = original;
        st_ = entity_state::clean;
    }
    status flush(session& owner) override;
};

// Handle to one tracked entity. `*ref` and `ref->` reach the live struct the
// application edits; the reference stays valid until the entity is committed
// away (a delete) or the session is destroyed.
template <typename T>
class entity_ref {
public:
    entity_ref() noexcept = default;

    T& operator*() const noexcept { return record_->current; }
    T* operator->() const noexcept { return &record_->current; }
    T& value() const noexcept { return record_->current; }

    entity_state state() const noexcept {
        return record_ != nullptr ? record_->state() : entity_state::clean;
    }
    bool valid() const noexcept { return record_ != nullptr; }

private:
    friend class session;
    record<T>* record_ = nullptr;
};

// One tracked unit of work over one connection lease.
class CRPC_API session {
public:
    session(session&& other) noexcept = default;
    session& operator=(session&& other) noexcept = default;
    ~session();

    session(const session&) = delete;
    session& operator=(const session&) = delete;

    // Acquires a lease from the pool; the session keeps it until destroyed.
    [[nodiscard]] static result<session> open(db::pool& pool,
                                              dialect d = dialect::mysql);

    // Loads the row with primary key `key` and tracks it. Requires exactly one
    // primary key column; a key whose family does not fit the column is an
    // error rather than a silent no-match.
    template <typename T, typename V>
    [[nodiscard]] result<entity_ref<T>> find(const V& key);

    // Queues `value` for INSERT. The auto-increment primary key (if any) is
    // filled in after the insert and left in `current`.
    template <typename T>
    entity_ref<T> add(T value) {
        auto tracker = std::unique_ptr<record<T>>(new record<T>());
        tracker->current = std::move(value);
        tracker->original = tracker->current;
        tracker->st_ = entity_state::inserted;
        tracker->persisted_ = false;
        record<T>* raw = tracker.get();
        records_.push_back(std::move(tracker));
        entity_ref<T> ref;
        ref.record_ = raw;
        return ref;
    }

    // Marks a tracked entity for DELETE.
    template <typename T>
    status erase(const entity_ref<T>& ref) {
        if (ref.record_ == nullptr) {
            return status{rpc_code::invalid_argument, "erasing an empty entity reference"};
        }
        ref.record_->st_ = entity_state::deleted;
        return status{};
    }

    // Starts a SELECT builder over T.
    template <typename T>
    query_builder<T> query();

    // Flushes every queued change in one transaction: INSERTs and UPDATEs in
    // record order first, then DELETEs. Unchanged entities produce no
    // statement. On failure everything is rolled back and the in-memory state
    // is left untouched for the caller to retry or drop.
    [[nodiscard]] status commit();

    // Discards uncommitted work: new entities are dropped, modified entities
    // are restored to their snapshot, delete marks are undone.
    [[nodiscard]] status rollback();

    // Runs a statement on the session's connection. Useful for DDL and ad-hoc
    // work that shares the session's transaction.
    [[nodiscard]] status execute(const std::string& sql, span<const parameter> params);
    // Runs a SELECT; the returned cursor is owned by the caller.
    [[nodiscard]] result<db::result_set*> query(const std::string& sql,
                                                span<const parameter> params);
    // Owning-container overloads (same contract; mirror db::client).
    [[nodiscard]] status execute(const std::string& sql, const std::vector<parameter>& params) {
        return execute(sql, span<const parameter>(params.data(), params.size()));
    }
    [[nodiscard]] result<db::result_set*> query(const std::string& sql,
                                                const std::vector<parameter>& params) {
        return query(sql, span<const parameter>(params.data(), params.size()));
    }

    // Reads back the server-assigned identifier for the last INSERT on this
    // session's connection (dialect-specific).
    [[nodiscard]] result<std::int64_t> last_insert_id();

    [[nodiscard]] dialect session_dialect() const noexcept { return dialect_; }
    [[nodiscard]] db::connection* raw() const noexcept { return lease_.get(); }
    // SQL statements the last commit (or any session statement since) issued,
    // for tests and general logging.
    [[nodiscard]] const std::vector<std::string>& commit_log() const noexcept {
        return commit_log_;
    }

    template <typename U>
    friend class query_builder;

private:
    session() = default;

    db::pool* pool_ = nullptr;
    db::pool::lease lease_;
    dialect dialect_ = dialect::mysql;
    std::vector<std::unique_ptr<record_base>> records_;
    std::vector<std::string> commit_log_;
};

// --- Session template members ------------------------------------------------

template <typename T, typename V>
result<entity_ref<T>> session::find(const V& key) {
    const entity_meta* meta = entity_metadata<T>();
    if (!meta->valid) {
        return rpc_error{rpc_code::invalid_argument, meta->error};
    }
    if (meta->primary_key.size() != 1) {
        return rpc_error{rpc_code::invalid_argument,
                         "find() requires exactly one primary key column; use query() "
                         "for composite keys"};
    }
    parameter encoded;
    status st = values::encode_scalar(key, encoded);
    if (!st) {
        return st.error();
    }
    st = sql::check_key_compatible(encoded, meta->columns[meta->primary_key[0]].type);
    if (!st) {
        return st.error();
    }

    sql::statement select;
    st = sql::build_select_by_key(*meta, dialect_, meta->primary_key[0], encoded, select);
    if (!st) {
        return st.error();
    }
    result<db::result_set*> rows =
        query(select.text,
              span<const parameter>(select.params.data(), select.params.size()));
    if (!rows) {
        return rows.error();
    }
    statement_guard guard(rows.value());
    if (!guard.valid() || !guard->next()) {
        return rpc_error{rpc_code::not_found, "no row for the requested primary key"};
    }
    T loaded{};
    st = decode_row(*meta, *guard.get(), loaded);
    if (!st) {
        return st.error();
    }

    auto tracker = std::unique_ptr<record<T>>(new record<T>());
    tracker->current = loaded;
    tracker->original = loaded;
    tracker->st_ = entity_state::clean;
    tracker->persisted_ = true;
    record<T>* raw = tracker.get();
    records_.push_back(std::move(tracker));

    entity_ref<T> ref;
    ref.record_ = raw;
    return ref;
}

template <typename T>
query_builder<T> session::query() {
    return query_builder<T>(this);
}

// --- The SQL builder over a tracked entity -----------------------------------

namespace detail {

template <typename T>
status flush_record(session& owner, record<T>& rec, const entity_meta& meta) {
    sql::statement stmt;
    status st;
    switch (rec.st_) {
    case entity_state::inserted: {
        std::vector<std::size_t> columns;
        columns.reserve(meta.columns.size());
        for (const column_meta& column : meta.columns) {
            if (!column.auto_increment) {
                columns.push_back(column.index);
            }
        }
        st = sql::build_insert(meta, owner.session_dialect(), &rec.current, columns, stmt);
        if (!st) {
            return st;
        }
        st = owner.execute(stmt.text, stmt.params);
        if (!st) {
            return st;
        }
        if (meta.auto_increment_index != orm_npos) {
            result<std::int64_t> id = owner.last_insert_id();
            if (!id) {
                return id.error();
            }
            const column_meta& pk = meta.columns[meta.auto_increment_index];
            st = pk.decode_value(&rec.current, db_value::from_int64(id.value()));
            if (!st) {
                return st;
            }
        }
        rec.original = rec.current;
        rec.st_ = entity_state::clean;
        rec.persisted_ = true;
        return status{};
    }
    case entity_state::deleted: {
        if (!rec.persisted_) {
            return status{};
        }
        st = sql::build_delete(meta, owner.session_dialect(), &rec.original, stmt);
        if (!st) {
            return st;
        }
        return owner.execute(stmt.text, stmt.params);
    }
    default: {
        // A clean or modified record: the application may have edited `current`
        // directly after load, so "dirty" is decided by comparing against the
        // snapshot, never by any setter hook.
        std::vector<std::size_t> dirty;
        for (const column_meta& column : meta.columns) {
            if (column.differs(&rec.current, &rec.original)) {
                dirty.push_back(column.index);
            }
        }
        if (dirty.empty()) {
            rec.st_ = entity_state::clean;
            return status{};
        }
        st = sql::build_update(meta, owner.session_dialect(), &rec.current, &rec.original,
                               dirty, stmt);
        if (!st) {
            return st;
        }
        st = owner.execute(stmt.text, stmt.params);
        if (!st) {
            return st;
        }
        rec.original = rec.current;
        rec.st_ = entity_state::clean;
        return status{};
    }
    }
}

} // namespace detail

template <typename T>
status record<T>::flush(session& owner) {
    const entity_meta* meta = entity_metadata<T>();
    if (!meta->valid) {
        return status{rpc_code::invalid_argument, meta->error};
    }
    return detail::flush_record(owner, *this, *meta);
}

// --- The type-safe query builder ---------------------------------------------

template <typename T>
class query_builder {
public:
    template <typename P>
    query_builder& where_null(P member) {
        const std::size_t index = resolve(member);
        if (index != orm_npos) {
            conditions_.push_back({index, sql::comp::is_null, {}});
        }
        return *this;
    }

    template <typename P, typename V>
    query_builder& where_eq(P member, const V& value) {
        return add_condition(member, sql::comp::eq, value);
    }
    template <typename P, typename V>
    query_builder& where_ne(P member, const V& value) {
        return add_condition(member, sql::comp::ne, value);
    }
    template <typename P, typename V>
    query_builder& where_gt(P member, const V& value) {
        return add_condition(member, sql::comp::gt, value);
    }
    template <typename P, typename V>
    query_builder& where_ge(P member, const V& value) {
        return add_condition(member, sql::comp::ge, value);
    }
    template <typename P, typename V>
    query_builder& where_lt(P member, const V& value) {
        return add_condition(member, sql::comp::lt, value);
    }
    template <typename P, typename V>
    query_builder& where_le(P member, const V& value) {
        return add_condition(member, sql::comp::le, value);
    }

    template <typename P, typename V>
    query_builder& where_in(P member, std::initializer_list<V> values) {
        const std::size_t index = resolve(member);
        if (index == orm_npos) {
            return *this;
        }
        using M = typename detail::member_traits<P>::member_type;
        static_assert(values::is_supported<M>::value,
                      "crpc::orm: the where_in member has an unsupported field type");
        static_assert(std::is_convertible<V, M>::value,
                      "crpc::orm: where_in value is not convertible to the field type");
        std::vector<parameter> params;
        params.reserve(values.size());
        for (const V& value : values) {
            const M converted(value);
            parameter encoded;
            status st = values::encode_value(converted, encoded);
            if (!st) {
                error_ = st.message();
                return *this;
            }
            params.push_back(std::move(encoded));
        }
        conditions_.push_back({index, sql::comp::in_, std::move(params)});
        return *this;
    }

    template <typename P>
    query_builder& order_by(P member, bool descending = false) {
        const std::size_t index = resolve(member);
        if (index != orm_npos) {
            order_.push_back({index, descending});
        }
        return *this;
    }

    query_builder& limit(std::size_t count) {
        limited_ = true;
        limit_ = count;
        return *this;
    }
    query_builder& offset(std::size_t count) {
        offset_ = count;
        return *this;
    }

    // Executes the SELECT and decodes every row.
    [[nodiscard]] result<std::vector<T>> list() const {
        if (!error_.empty()) {
            return rpc_error{rpc_code::invalid_argument, error_};
        }
        const entity_meta* meta = entity_metadata<T>();
        if (!meta->valid) {
            return rpc_error{rpc_code::invalid_argument, meta->error};
        }
        sql::statement stmt;
        status st = sql::build_select(*meta, session_->session_dialect(), conditions_,
                                      order_, limited_, limit_, offset_, stmt);
        if (!st) {
            return st.error();
        }
        result<db::result_set*> rows =
            session_->query(stmt.text,
                            span<const parameter>(stmt.params.data(), stmt.params.size()));
        if (!rows) {
            return rows.error();
        }
        statement_guard guard(rows.value());
        std::vector<T> result_rows;
        if (!guard.valid()) {
            return result_rows;
        }
        while (guard->next()) {
            T row{};
            st = decode_row(*meta, *guard.get(), row);
            if (!st) {
                return st.error();
            }
            result_rows.push_back(std::move(row));
        }
        return result_rows;
    }

    // The first row, or std::nullopt when nothing matches.
    [[nodiscard]] result<std::optional<T>> first() const {
        query_builder limited_copy(*this);
        limited_copy.limited_ = true;
        limited_copy.limit_ = 1;
        result<std::vector<T>> rows = limited_copy.list();
        if (!rows) {
            return rows.error();
        }
        if (rows.value().empty()) {
            return std::optional<T>{};
        }
        return std::optional<T>(std::move(rows.value().front()));
    }

    [[nodiscard]] result<std::int64_t> count() const {
        if (!error_.empty()) {
            return rpc_error{rpc_code::invalid_argument, error_};
        }
        const entity_meta* meta = entity_metadata<T>();
        if (!meta->valid) {
            return rpc_error{rpc_code::invalid_argument, meta->error};
        }
        sql::statement stmt;
        status st = sql::build_count(*meta, session_->session_dialect(), conditions_, stmt);
        if (!st) {
            return st.error();
        }
        result<db::result_set*> rows =
            session_->query(stmt.text,
                            span<const parameter>(stmt.params.data(), stmt.params.size()));
        if (!rows) {
            return rows.error();
        }
        statement_guard guard(rows.value());
        if (!guard.valid() || !guard->next()) {
            return rpc_error{rpc_code::internal, "count query returned no row"};
        }
        result<std::int64_t> count = guard->get_int64(0);
        if (!count) {
            return count.error();
        }
        return count.value();
    }

private:
    friend class session;
    explicit query_builder(session* owner) : session_(owner) {}

    template <typename P>
    std::size_t resolve(P member) {
        const std::size_t index = member_field_index<T>(member);
        if (index == orm_npos) {
            error_ = "no reflected member of this entity matches the given member pointer";
        }
        return index;
    }

    template <typename P, typename V>
    query_builder& add_condition(P member, sql::comp op, const V& value) {
        const std::size_t index = resolve(member);
        if (index == orm_npos) {
            return *this;
        }
        using M = typename detail::member_traits<P>::member_type;
        static_assert(values::is_supported<M>::value,
                      "crpc::orm: the condition member has an unsupported field type");
        static_assert(std::is_convertible<V, M>::value,
                      "crpc::orm: condition value is not convertible to the field type");
        const M converted(value);
        parameter encoded;
        status st = values::encode_value(converted, encoded);
        if (!st) {
            error_ = st.message();
            return *this;
        }
        conditions_.push_back({index, op, {std::move(encoded)}});
        return *this;
    }

    session* session_ = nullptr;
    std::vector<sql::condition> conditions_;
    std::vector<sql::order_term> order_;
    bool limited_ = false;
    std::size_t limit_ = 0;
    std::size_t offset_ = 0;
    std::string error_;
};

} // namespace orm
} // namespace crpc

#endif // CRPC_ORM_SESSION_HPP