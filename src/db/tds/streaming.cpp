// CRPC -- TDS streaming result set.
//
// The eager path (`read_reply`) reads a whole reply into memory, which is what a
// caller that wants a bounded, pool-returnable cursor gets. `query_streaming` is
// the opposite: the cursor below pulls tokens from the socket as `next()` advances
// and keeps at most one decoded row plus the message payload it came from, so a
// result of any size is consumed row by row without being materialised.
//
// The streaming cursor keeps the connection busy (the request slot stays claimed)
// until it is drained to the final DONE or closed; `close()` drains whatever is
// left so the next statement decodes its own reply.
#include <crpc/db/tds/connection.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <crpc/error.hpp>

#include "../text_value.hpp"

namespace crpc {
namespace db {
namespace tds {

// The pull-driven decoder for one streaming request.
class connection::streaming_cursor final : public result_set {
public:
    explicit streaming_cursor(connection* owner) noexcept : owner_(owner) {}
    ~streaming_cursor() override { close(); }

    // Reads until the server's first reply token arrives. `timer` bounds that wait:
    // once the server starts answering, reading is paced by the caller, like an
    // ADO.NET reader, and the command timeout no longer applies.
    [[nodiscard]] status prime(connection::request_timer& timer);

    // Reads a reply that begins with no result set (INSERT, DDL) or an immediate
    // server error; used only when prime ended the whole reply.
    void finish() noexcept;

    [[nodiscard]] std::size_t column_count() const noexcept override {
        return columns_.size();
    }
    [[nodiscard]] column_info column(std::size_t index) const override {
        if (index >= columns_.size()) {
            return {};
        }
        return columns_[index];
    }

    [[nodiscard]] bool next() override;
    [[nodiscard]] bool next_result() override;

    [[nodiscard]] bool is_null(std::size_t index) const noexcept override;
    [[nodiscard]] result<std::int64_t> get_int64(std::size_t index) override;
    [[nodiscard]] result<std::uint64_t> get_uint64(std::size_t index) override;
    [[nodiscard]] result<double> get_double(std::size_t index) override;
    [[nodiscard]] result<bool> get_bool(std::size_t index) override;
    [[nodiscard]] result<std::string_view> get_text(std::size_t index) override;
    [[nodiscard]] result<std::vector<std::byte>> get_blob(std::size_t index) override;

    [[nodiscard]] std::int64_t affected_rows() const noexcept override { return affected_; }

    [[nodiscard]] bool is_streaming() const noexcept override { return true; }
    [[nodiscard]] rpc_error last_error() const noexcept override { return error_; }

    void close() noexcept override;

    // Proof the stream is not materialising: every decoded row has already been
    // consumed, so at rest at most the current row is held.
    [[nodiscard]] std::size_t rows_decoded() const noexcept override {
        return decoded_rows_;
    }
    [[nodiscard]] std::size_t rows_buffered() const noexcept override {
        return pending_row_ ? 1u : 0u;
    }

private:
    friend class connection;

    [[nodiscard]] status read_next_message();
    // Pulls tokens until a row is buffered, a result-set boundary is reached, or the
    // reply ends. Transport and protocol failures fail the connection and return the
    // error; a server-reported ERROR is recorded in `error_` and draining continues.
    [[nodiscard]] status advance(bool& got_row);
    void clear_row() noexcept;
    [[nodiscard]] std::string_view value_at(std::size_t index) const noexcept;
    void release_request() noexcept;
    [[nodiscard]] bool ended() const noexcept {
        return finished_ || terminated_ || closed_;
    }

    connection* owner_ = nullptr;

    // Token-stream state. `message_` owns the current message payload; `reader_`
    // decodes it. Only one message is held at a time, which is what bounds memory.
    std::vector<std::byte> message_;
    token_reader reader_ = token_reader({});
    bool have_message_ = false;

    // The current result set.
    std::vector<column_info> columns_;
    bool set_open_ = false;

    // The current row (for the current set).
    std::vector<std::string> row_values_;
    std::vector<bool> row_nulls_;
    bool has_row_ = false;
    // A row `prime()` already decoded but `next()` has not handed out yet.
    bool pending_row_ = false;

    std::int64_t affected_ = -1;
    rpc_error error_{rpc_code::ok, {}};
    bool finished_ = false;
    bool terminated_ = false;
    unsigned int colmetadata_seen_ = 0;
    std::size_t decoded_rows_ = 0;
    bool closed_ = false;
};

status connection::streaming_cursor::read_next_message() {
    message_.clear();
    const status read = owner_->read_message(message_);
    if (!read) {
        error_ = read.error();
        owner_->fail(read);
        return read;
    }
    if (!message_.empty()) {
        reader_ = token_reader(span<const std::byte>(message_.data(), message_.size()));
        have_message_ = true;
    } else {
        have_message_ = false;
    }
    return status{};
}

status connection::streaming_cursor::advance(bool& got_row) {
    got_row = false;
    for (;;) {
        if (finished_ || terminated_) {
            return status{};
        }
        if (!have_message_) {
            const status read = read_next_message();
            if (!read) {
                return read;
            }
        }
        token item;
        const token_reader::result step = reader_.next(item);
        if (step == token_reader::result::end) {
            have_message_ = false;
            continue;
        }
        if (step == token_reader::result::error) {
            const status broken{reader_.error(),
                                reader_.error_message().empty()
                                    ? std::string("a TDS result token could not be decoded")
                                    : reader_.error_message()};
            error_ = broken.error();
            owner_->fail(broken);
            return broken;
        }
        switch (item.type) {
        case token::kind::colmetadata: {
            if (set_open_) {
                const status broken{rpc_code::protocol_error,
                                    "a TDS result set started inside another result set"};
                error_ = broken.error();
                owner_->fail(broken);
                return broken;
            }
            columns_.clear();
            columns_.reserve(item.columns.size());
            for (const column_meta& source : item.columns) {
                column_info info;
                info.name = source.name;
                info.type = tds_column_type(source.type);
                info.precision = source.precision;
                info.scale = source.scale;
                info.nullable = source.nullable;
                columns_.push_back(std::move(info));
            }
            set_open_ = true;
            ++colmetadata_seen_;
            clear_row();
            break;
        }
        case token::kind::row:
            if (!set_open_) {
                const status broken{rpc_code::protocol_error,
                                    "a TDS row arrived before its column metadata"};
                error_ = broken.error();
                owner_->fail(broken);
                return broken;
            }
            clear_row();
            if (item.values.size() != columns_.size()) {
                const status broken{rpc_code::protocol_error,
                                    "a TDS row does not match its column metadata"};
                error_ = broken.error();
                owner_->fail(broken);
                return broken;
            }
            row_values_ = std::move(item.values);
            row_nulls_ = std::move(item.nulls);
            has_row_ = true;
            pending_row_ = true;
            ++decoded_rows_;
            got_row = true;
            return status{};
        case token::kind::done: {
            const bool stray_ack = item.done.attention() && !owner_->attention_pending_.load();
            if (item.done.attention() && owner_->attention_pending_.load()) {
                owner_->attention_seen_ = true;
            }
            if (!stray_ack) {
                if (item.done.has_row_count &&
                    item.raw_type != static_cast<std::uint8_t>(token_type::done_proc)) {
                    if (affected_ < 0) {
                        affected_ = 0;
                    }
                    affected_ += static_cast<std::int64_t>(std::min<std::uint64_t>(
                        item.done.row_count,
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
                }
                set_open_ = false;
                has_row_ = false;
                if (!item.done.more_results()) {
                    finished_ = true;
                }
                // A request the caller stopped surfaces as cancelled.
                if (finished_ && owner_->attention_pending_.load() &&
                    owner_->attention_seen_.load()) {
                    error_ = rpc_error{rpc_code::canceled, "the request was cancelled"};
                }
                if (finished_) {
                    release_request();
                }
                return status{};
            }
            // A stray acknowledgement (watchdog raced the end of the reply) is its
            // own message; skip it and keep pulling the actual reply.
            break;
        }
        case token::kind::error:
            if (error_.code == rpc_code::ok) {
                error_ = rpc_error{map_error_number(item.error.number),
                                   item.error.message.empty()
                                       ? std::string("the server rejected the request")
                                       : item.error.message};
            }
            break;
        case token::kind::info:
            // PRINT and informational messages are not errors.
            break;
        case token::kind::env_change:
            if (item.env.type == env_change_type::packet_size &&
                item.env.new_packet_size >= 512) {
                owner_->packet_size_ = static_cast<std::uint16_t>(
                    std::min<std::uint32_t>(item.env.new_packet_size, 32767u));
            }
            if (item.env.type == env_change_type::begin_transaction) {
                owner_->descriptor_ = item.env.new_descriptor;
                owner_->has_descriptor_ = true;
            } else if (item.env.type == env_change_type::commit_transaction ||
                       item.env.type == env_change_type::rollback_transaction) {
                owner_->has_descriptor_ = false;
                owner_->descriptor_.fill(0);
            }
            break;
        case token::kind::return_status:
            owner_->last_return_status_ = item.ret_status;
            owner_->has_return_status_ = true;
            break;
        case token::kind::return_value:
            break;
        default:
            break;
        }
    }
}

status connection::streaming_cursor::prime(connection::request_timer& timer) {
    bool got_row = false;
    const status step = advance(got_row);
    timer.disarm();
    if (!step) {
        // The transport died (or the decode failed); the connection is already
        // failed by `advance`.
        return step;
    }
    if (finished_ || terminated_) {
        // The whole reply ended during priming: an immediate server error, a
        // statement with no result set, or a request the timeout cancelled. The
        // caller wants that outcome now, not when it first reads a row.
        return error_.code == rpc_code::ok ? status{} : status{error_.code, error_.message};
    }
    return status{};
}

void connection::streaming_cursor::finish() noexcept { release_request(); }

void connection::streaming_cursor::release_request() noexcept {
    owner_->attention_pending_ = false;
    owner_->attention_seen_ = false;
    owner_->in_request_ = false;
}

bool connection::streaming_cursor::next() {
    if (ended()) {
        return false;
    }
    if (pending_row_) {
        // A row `prime()` decoded is the next row; hand it over without a re-read.
        pending_row_ = false;
        return true;
    }
    clear_row();
    bool got_row = false;
    for (;;) {
        const status step = advance(got_row);
        if (!step) {
            terminated_ = true;
            return false;
        }
        if (got_row) {
            // Hand the freshly decoded row out now; the next call re-reads.
            pending_row_ = false;
            return true;
        }
        if (!set_open_ || finished_) {
            // The current set ended; rows live in the next one. ADO.NET's reader
            // stops here too: to keep reading, call `next_result()`.
            return false;
        }
    }
}

bool connection::streaming_cursor::next_result() {
    if (ended()) {
        return false;
    }
    // Drain whatever is left of the current set; its rows are being discarded on
    // purpose, and the stream must reach the boundary before the next set's
    // metadata can be decoded.
    while (set_open_) {
        bool got_row = false;
        const status step = advance(got_row);
        if (!step) {
            terminated_ = true;
            return false;
        }
    }
    if (finished_) {
        return false;
    }
    const unsigned int before = colmetadata_seen_;
    for (;;) {
        bool got_row = false;
        const status step = advance(got_row);
        if (!step) {
            terminated_ = true;
            return false;
        }
        if (colmetadata_seen_ > before) {
            return true;
        }
        if (finished_) {
            return false;
        }
    }
}

bool connection::streaming_cursor::is_null(std::size_t index) const noexcept {
    if (!has_row_ || index >= row_nulls_.size()) {
        return true;
    }
    return row_nulls_[index];
}

result<std::int64_t> connection::streaming_cursor::get_int64(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    return detail::parse_int64_text(text);
}

result<std::uint64_t> connection::streaming_cursor::get_uint64(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    return detail::parse_uint64_text(text);
}

result<double> connection::streaming_cursor::get_double(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    return detail::parse_double_text(text);
}

result<bool> connection::streaming_cursor::get_bool(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    return detail::parse_bool_text(text);
}

result<std::string_view> connection::streaming_cursor::get_text(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    return text;
}

result<std::vector<std::byte>> connection::streaming_cursor::get_blob(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    return detail::blob_from_text(text);
}

void connection::streaming_cursor::close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    // Drain the rest of the reply so the connection stays aligned for the next
    // statement; a partially read reply would be decoded as that reply's head.
    if (owner_ != nullptr && !finished_ && !terminated_ && owner_->is_open()) {
        bool got_row = false;
        while (!finished_ && !terminated_) {
            const status step = advance(got_row);
            if (!step) {
                break;
            }
        }
    }
    release_request();
    row_values_.clear();
    row_nulls_.clear();
    has_row_ = false;
    columns_.clear();
}

std::string_view connection::streaming_cursor::value_at(std::size_t index) const noexcept {
    if (!has_row_ || index >= row_values_.size()) {
        return {};
    }
    if (index < row_nulls_.size() && row_nulls_[index]) {
        return {};
    }
    return row_values_[index];
}

void connection::streaming_cursor::clear_row() noexcept {
    row_values_.clear();
    row_nulls_.clear();
    has_row_ = false;
    pending_row_ = false;
}

// --- entry point -------------------------------------------------------------

status connection::query_streaming(std::string_view sql, span<const parameter> params,
                                   result_set** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (!open_) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    // A streaming query returns rows the caller has not read yet, so like `query()`
    // it can only bind input values. Output parameters belong to `call()`.
    bool any_named = false;
    bool all_named = true;
    for (const parameter& param : params) {
        if (param.is_output()) {
            return status{rpc_code::invalid_argument,
                          "query_streaming() cannot return an output parameter; use call()"};
        }
        if (param.name.empty()) {
            all_named = false;
        } else {
            any_named = true;
        }
    }
    if (any_named && !all_named) {
        return status{rpc_code::invalid_argument,
                      "cannot mix named and positional parameters; "
                      "name every parameter or leave every name empty"};
    }

    std::unique_ptr<streaming_cursor> cursor(new streaming_cursor(this));
    status sent{};
    if (params.empty()) {
        sent = send_batch(sql);
    } else {
        rpc_request request;
        sent = build_request(sql, params, false, request);
        if (sent) {
            sent = send_rpc(request);
        }
    }
    if (!sent) {
        cursor->finish();
        return sent;
    }

    // The command timeout bounds the wait for the server's first token. Once the
    // reply starts flowing, reading is paced by the caller's `next()` calls.
    request_timer timer(command_timeout_);
    timer.arm(*this);
    const status primed = cursor->prime(timer);
    if (!primed) {
        return primed;
    }
    if (cursor->finished_ || cursor->terminated_) {
        // The whole reply ended during priming and `prime` deemed it a success only
        // when no error was recorded (a bare INSERT, say). Hand the cursor over; it
        // reports the affected count and `next()` is already at end.
        if (cursor->last_error().code != rpc_code::ok) {
            return cursor->last_error();
        }
    }
    *out = cursor.release();
    return status{};
}

} // namespace tds
} // namespace db
} // namespace crpc