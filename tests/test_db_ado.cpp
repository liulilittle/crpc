// CRPC -- ADO.NET-level database features, scripted against a TDS server.
//
// Five things a database layer needs once it is used like ADO.NET: multiple result
// sets per batch, streaming (a large result that is not materialised), named
// parameters, transaction isolation levels, and command timeouts that actually
// interrupt the server. Each is proved here against a scripted TDS session, byte
// for byte, so the assertion is about the client's state machine; the live-server
// counterparts live in test_db_tds_it.cpp and test_db_mysql_it.cpp.
#include "test_framework.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/db/connection.hpp>
#include <crpc/db/memory_result_set.hpp>
#include <crpc/db/result_set_list.hpp>
#include <crpc/db/tds/connection.hpp>
#include <crpc/db/tds/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::channel;
using crpc::db::result_set;
using crpc::db::statement_guard;
using crpc::db::tds::packet_header;
using crpc::db::tds::packet_status;
using crpc::db::tds::packet_type;
using crpc::db::tds::token_type;

using bytes = std::vector<std::byte>;

[[nodiscard]] std::uint8_t byte_at(const bytes& data, std::size_t index) {
    return static_cast<std::uint8_t>(data[index]);
}

void put_u8(bytes& out, std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }

void put_u16_le(bytes& out, std::uint16_t value) {
    put_u8(out, static_cast<std::uint8_t>(value & 0xffu));
    put_u8(out, static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void put_u32_le(bytes& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        put_u8(out, static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
    }
}

void put_u64_le(bytes& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        put_u8(out, static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
    }
}

void put_utf16(bytes& out, std::string_view text) {
    const bytes encoded = crpc::db::tds::utf16le(text);
    out.insert(out.end(), encoded.begin(), encoded.end());
}

void put_b_varchar(bytes& out, std::string_view text) {
    put_u8(out, static_cast<std::uint8_t>(text.size()));
    put_utf16(out, text);
}

void put_us_varchar(bytes& out, std::string_view text) {
    put_u16_le(out, static_cast<std::uint16_t>(text.size()));
    put_utf16(out, text);
}

[[nodiscard]] bytes frame(packet_type type, const bytes& payload, std::size_t packet_size) {
    bytes out;
    const std::size_t chunk_max = packet_size > 8 ? packet_size - 8 : payload.size();
    std::size_t offset = 0;
    std::uint8_t id = 1;
    do {
        const std::size_t chunk = std::min(payload.size() - offset, chunk_max);
        packet_header header;
        header.type = type;
        header.length = static_cast<std::uint16_t>(8 + chunk);
        header.packet_id = id++;
        header.status = (offset + chunk >= payload.size())
                            ? static_cast<std::uint8_t>(packet_status::eom)
                            : std::uint8_t{0};
        std::byte head[8]{};
        std::size_t written = 0;
        const crpc::status encoded =
            crpc::db::tds::encode_header(crpc::span<std::byte>(head, sizeof head), header, written);
        CRPC_CHECK(static_cast<bool>(encoded));
        CRPC_CHECK_EQ(std::size_t{8}, written);
        out.insert(out.end(), head, head + 8);
        out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset),
                   payload.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
        offset += chunk;
    } while (offset < payload.size());
    return out;
}

void put_done(bytes& out, std::uint16_t status, std::uint64_t row_count) {
    put_u8(out, static_cast<std::uint8_t>(token_type::done));
    put_u16_le(out, status);
    put_u16_le(out, 0);
    put_u64_le(out, row_count);
}

void put_error(bytes& out, std::uint32_t number, std::uint8_t severity,
               std::string_view message) {
    bytes body;
    put_u32_le(body, number);
    put_u8(body, 1);
    put_u8(body, severity);
    put_us_varchar(body, message);
    put_b_varchar(body, "server");
    put_b_varchar(body, "");
    put_u32_le(body, 1);
    put_u8(out, static_cast<std::uint8_t>(token_type::error));
    put_u16_le(out, static_cast<std::uint16_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

void put_u32_be(bytes& out, std::uint32_t value) {
    put_u8(out, static_cast<std::uint8_t>((value >> 24) & 0xffu));
    put_u8(out, static_cast<std::uint8_t>((value >> 16) & 0xffu));
    put_u8(out, static_cast<std::uint8_t>((value >> 8) & 0xffu));
    put_u8(out, static_cast<std::uint8_t>(value & 0xffu));
}

void put_login_ack(bytes& out) {
    bytes body;
    put_u8(body, 1);
    put_u32_be(body, 0x74000004u);
    put_b_varchar(body, "Microsoft SQL Server");
    put_u32_be(body, 0x0F00A1B2u);
    put_u8(out, static_cast<std::uint8_t>(token_type::loginack));
    put_u16_le(out, static_cast<std::uint16_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

void put_packet_size_change(bytes& out, std::string_view new_size) {
    bytes body;
    put_u8(body, static_cast<std::uint8_t>(crpc::db::tds::env_change_type::packet_size));
    put_b_varchar(body, new_size);
    put_b_varchar(body, "4096");
    put_u8(out, static_cast<std::uint8_t>(token_type::envchange));
    put_u16_le(out, static_cast<std::uint16_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

struct column_spec {
    std::uint8_t type;
    std::uint16_t max_length;
    const char* name;
};

void put_colmetadata(bytes& out, const std::vector<column_spec>& columns) {
    put_u8(out, static_cast<std::uint8_t>(token_type::colmetadata));
    put_u16_le(out, static_cast<std::uint16_t>(columns.size()));
    for (const column_spec& column : columns) {
        put_u32_le(out, 0);
        put_u16_le(out, 1);
        put_u8(out, column.type);
        switch (column.type) {
        case 0xE7:
        case 0xA7:
        case 0xA5:
            put_u16_le(out, column.max_length);
            if (column.type != 0xA5) {
                for (int i = 0; i < 5; ++i) {
                    put_u8(out, 0);
                }
            }
            break;
        case 0x26:
        case 0x68:
        case 0x6D:
        case 0x24:
        case 0x28:
        case 0x29:
        case 0x2A:
            if (column.type != 0x28) {
                put_u8(out, static_cast<std::uint8_t>(column.max_length));
            }
            break;
        default:
            break;
        }
        put_b_varchar(out, column.name);
    }
}

void put_int4_row(bytes& out, std::int32_t value) {
    put_u8(out, static_cast<std::uint8_t>(token_type::row));
    put_u32_le(out, static_cast<std::uint32_t>(value));
}

// --- scripted server ---------------------------------------------------------

class scripted_channel final : public channel {
public:
    using responder = std::function<bytes(packet_type, crpc::span<const std::byte>)>;

    explicit scripted_channel(responder respond) : respond_(std::move(respond)) {}

    [[nodiscard]] crpc::result<bytes> read_exact(std::size_t count) override {
        if (closed_) {
            return crpc::rpc_error{crpc::rpc_code::eof, "the scripted channel is closed"};
        }
        if (count > in_.size() - read_pos_) {
            return crpc::rpc_error{crpc::rpc_code::eof,
                                   "the scripted server has no more bytes to send"};
        }
        bytes out(in_.begin() + static_cast<std::ptrdiff_t>(read_pos_),
                  in_.begin() + static_cast<std::ptrdiff_t>(read_pos_ + count));
        read_pos_ += count;
        return out;
    }

    crpc::status write(crpc::span<const std::byte> data) override {
        if (closed_) {
            return crpc::status{crpc::rpc_code::shutting_down, "the scripted channel is closed"};
        }
        written_ += data.size();
        pending_.insert(pending_.end(), data.begin(), data.end());
        drain();
        return crpc::status{};
    }

    void close() noexcept override { closed_ = true; }

    [[nodiscard]] std::size_t bytes_written() const noexcept { return written_; }
    [[nodiscard]] bool is_closed() const noexcept { return closed_; }
    void set_server_packet_size(std::size_t value) noexcept { server_packet_size_ = value; }

private:
    void drain() {
        for (;;) {
            if (pending_.size() - consumed_ < 8) {
                return;
            }
            const std::size_t base = consumed_;
            const std::uint16_t length =
                static_cast<std::uint16_t>((byte_at(pending_, base + 2) << 8) |
                                           byte_at(pending_, base + 3));
            if (pending_.size() - consumed_ < length) {
                return;
            }
            const auto type = static_cast<packet_type>(byte_at(pending_, base));
            const std::uint8_t status = byte_at(pending_, base + 1);
            message_.insert(message_.end(),
                            pending_.begin() + static_cast<std::ptrdiff_t>(base + 8),
                            pending_.begin() + static_cast<std::ptrdiff_t>(base + length));
            consumed_ += length;
            if ((status & static_cast<std::uint8_t>(packet_status::eom)) == 0) {
                continue;
            }
            const bytes reply =
                respond_(type, crpc::span<const std::byte>(message_.data(), message_.size()));
            message_.clear();
            if (!reply.empty()) {
                const bytes framed = frame(packet_type::tabular_result, reply, server_packet_size_);
                in_.insert(in_.end(), framed.begin(), framed.end());
            }
        }
    }

    responder respond_;
    bytes pending_;
    std::size_t consumed_ = 0;
    bytes message_;
    bytes in_;
    std::size_t read_pos_ = 0;
    std::size_t written_ = 0;
    std::size_t server_packet_size_ = 4096;
    bool closed_ = false;
};

// A channel whose first request message is answered asynchronously after a delay,
// so a client-side watchdog can fire while the client waits. ATTENTION is answered
// immediately (with the acknowledgement the real server would send), and a delayed
// reply that was superseded by a cancel is dropped -- modelling a server that
// stopped the work instead of running it to completion.
class delaying_channel final : public channel {
public:
    using responder = std::function<bytes(packet_type, crpc::span<const std::byte>)>;

    delaying_channel(std::chrono::milliseconds delay, responder respond)
        : delay_(delay), respond_(std::move(respond)) {}

    ~delaying_channel() override { close(); }

    [[nodiscard]] crpc::result<bytes> read_exact(std::size_t count) override {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return closed_ || in_.size() - read_pos_ >= count; });
        if (closed_ && in_.size() - read_pos_ < count) {
            return crpc::rpc_error{crpc::rpc_code::eof, "the delaying channel is closed"};
        }
        bytes out(in_.begin() + static_cast<std::ptrdiff_t>(read_pos_),
                  in_.begin() + static_cast<std::ptrdiff_t>(read_pos_ + count));
        read_pos_ += count;
        return out;
    }

    crpc::status write(crpc::span<const std::byte> data) override {
        if (closed_) {
            return crpc::status{crpc::rpc_code::shutting_down, "the delaying channel is closed"};
        }
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.insert(pending_.end(), data.begin(), data.end());
        for (;;) {
            if (pending_.size() - consumed_ < 8) {
                return crpc::status{};
            }
            const std::size_t base = consumed_;
            const std::uint16_t length =
                static_cast<std::uint16_t>((byte_at(pending_, base + 2) << 8) |
                                           byte_at(pending_, base + 3));
            if (pending_.size() - consumed_ < length) {
                return crpc::status{};
            }
            const auto type = static_cast<packet_type>(byte_at(pending_, base));
            const std::uint8_t status = byte_at(pending_, base + 1);
            message_.insert(message_.end(),
                            pending_.begin() + static_cast<std::ptrdiff_t>(base + 8),
                            pending_.begin() + static_cast<std::ptrdiff_t>(base + length));
            consumed_ += length;
            if ((status & static_cast<std::uint8_t>(packet_status::eom)) == 0) {
                continue;
            }
            const auto type_copy = type;
            const bytes body = message_;
            message_.clear();
            if (type_copy == packet_type::attention) {
                // The server answers an ATTENTION at once and marks the request it
                // interrupts (the latest sql_batch/rpc) as cancelled.
                const std::uint64_t sent = next_seq_.load();
                cancel_seq_.store(sent > 0 ? sent - 1
                                           : std::numeric_limits<std::uint64_t>::max());
                deliver(type_copy, body);
                continue;
            }
            // Delay only the first real command (sql_batch/rpc); the login
            // handshake and every later command are answered promptly, so the
            // delay models one long-running statement rather than a slow link.
            const bool is_command =
                type_copy == packet_type::sql_batch || type_copy == packet_type::rpc;
            const bool delay_first = is_command && !delayed_.exchange(true);
            const std::uint64_t seq = next_seq_.fetch_add(1);
            workers_.emplace_back([this, type_copy, body, delay_first, seq] {
                if (delay_first) {
                    std::this_thread::sleep_for(delay_);
                    // The cancelled request's own reply is dropped, so a server
                    // that stopped the work never leaks it into the next reply.
                    if (cancel_seq_.load() == seq) {
                        return;
                    }
                }
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_) {
                    return;
                }
                const bytes reply = respond_(type_copy, crpc::span<const std::byte>(
                                                           body.data(), body.size()));
                if (!reply.empty()) {
                    const bytes framed =
                        frame(packet_type::tabular_result, reply, k_frame_size);
                    in_.insert(in_.end(), framed.begin(), framed.end());
                }
                cv_.notify_all();
            });
        }
    }

    void close() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    void deliver(packet_type type, const bytes& body) {
        const bytes reply =
            respond_(type, crpc::span<const std::byte>(body.data(), body.size()));
        if (!reply.empty()) {
            const bytes framed = frame(packet_type::tabular_result, reply, k_frame_size);
            in_.insert(in_.end(), framed.begin(), framed.end());
        }
        cv_.notify_all();
    }

    static constexpr std::size_t k_frame_size = 4096;

    std::chrono::milliseconds delay_;
    responder respond_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bytes pending_;
    std::size_t consumed_ = 0;
    bytes message_;
    bytes in_;
    std::size_t read_pos_ = 0;
    std::atomic<bool> delayed_{false};
    // The request sequence the ATTENTION interrupted, so only that request's
    // delayed reply is dropped and later commands are answered normally.
    std::atomic<std::uint64_t> next_seq_{0};
    std::atomic<std::uint64_t> cancel_seq_{0};
    std::vector<std::thread> workers_;
    bool closed_ = false;
};

[[nodiscard]] crpc::db::tds::connection_options options_for_tests() {
    crpc::db::tds::connection_options options;
    options.user = "sa";
    options.password = "pw";
    options.database = "master";
    options.packet_size = 4096;
    return options;
}

[[nodiscard]] scripted_channel::responder login_server() {
    return [](packet_type type, crpc::span<const std::byte>) -> bytes {
        bytes reply;
        if (type == packet_type::prelogin) {
            crpc::db::tds::prelogin answer;
            answer.encryption = crpc::db::tds::encryption_mode::not_supported;
            (void)crpc::db::tds::encode_prelogin(reply, answer);
            return reply;
        }
        if (type == packet_type::login7) {
            put_login_ack(reply);
            put_packet_size_change(reply, "8192");
            put_done(reply, 0, 0);
            return reply;
        }
        return reply;
    };
}

template <typename Responder>
[[nodiscard]] std::unique_ptr<crpc::db::tds::connection> open_with(Responder respond) {
    auto link = std::make_unique<scripted_channel>(std::move(respond));
    return std::unique_ptr<crpc::db::tds::connection>(
        new crpc::db::tds::connection(options_for_tests(), std::move(link)));
}

[[nodiscard]] bool contains_utf16(const std::vector<std::byte>& haystack,
                                  std::string_view needle) {
    std::vector<std::byte> encoded;
    encoded.reserve(needle.size() * 2);
    for (const char c : needle) {
        encoded.push_back(static_cast<std::byte>(c));
        encoded.push_back(std::byte{0});
    }
    return std::search(haystack.begin(), haystack.end(), encoded.begin(), encoded.end()) !=
           haystack.end();
}

// The two SETs a batch reply carries: COLMETADATA("a"), one row = 1, DONE(count 1)
// with the "more results" bit, then the same shape for column "b".
[[nodiscard]] bytes two_result_sets(std::uint8_t done_status_first) {
    bytes reply;
    put_colmetadata(reply, {{0x38, 0, "a"}});
    put_int4_row(reply, 1);
    put_done(reply, done_status_first, 1);
    put_colmetadata(reply, {{0x38, 0, "b"}});
    put_int4_row(reply, 2);
    put_done(reply, 0, 1);
    return reply;
}

} // namespace

CRPC_TEST(ado_materialised_next_result_walks_two_result_sets) {
    auto respond = login_server();
    int batch = 0;
    auto session = open_with([respond, &batch](packet_type type, crpc::span<const std::byte> payload) {
        if (type != packet_type::sql_batch) {
            return respond(type, payload);
        }
        ++batch;
        if (batch == 1) {
            return two_result_sets(0x0001);
        }
        bytes reply;
        put_colmetadata(reply, {{0x38, 0, "n"}});
        put_int4_row(reply, 9);
        put_done(reply, 0, 1);
        return reply;
    });

    result_set* rows = nullptr;
    const crpc::status ran = session->query("SELECT 1; SELECT 2", {}, &rows);
    statement_guard guard(rows);
    CRPC_CHECK(static_cast<bool>(ran));
    CRPC_CHECK(rows != nullptr);

    // First set.
    CRPC_CHECK_EQ(std::string("a"), rows->column(0).name);
    CRPC_CHECK(rows->next());
    CRPC_CHECK_EQ(std::int64_t{1}, rows->get_int64(0).value());
    CRPC_CHECK_FALSE(rows->next());

    // Second set, then no more.
    CRPC_CHECK(rows->next_result());
    CRPC_CHECK_EQ(std::string("b"), rows->column(0).name);
    CRPC_CHECK(rows->next());
    CRPC_CHECK_EQ(std::int64_t{2}, rows->get_int64(0).value());
    CRPC_CHECK_FALSE(rows->next());
    CRPC_CHECK_FALSE(rows->next_result());

    // The reply was fully decoded before `query()` returned, so the session serves
    // the next request without a shared half-read stream.
    result_set* again = nullptr;
    const crpc::status second = session->query("SELECT 9", {}, &again);
    statement_guard again_guard(again);
    CRPC_CHECK(static_cast<bool>(second));
    CRPC_CHECK(again != nullptr);
    CRPC_CHECK(again->next());
    CRPC_CHECK_EQ(std::int64_t{9}, again->get_int64(0).value());
}

CRPC_TEST(ado_materialised_next_result_behind_a_skipped_set) {
    // A caller may step forward without exhausting the current set first.
    auto respond = login_server();
    auto session = open_with([respond](packet_type type, crpc::span<const std::byte> payload) {
        if (type != packet_type::sql_batch) {
            return respond(type, payload);
        }
        return two_result_sets(0x0001);
    });
    result_set* rows = nullptr;
    const crpc::status ran = session->query("SELECT 1; SELECT 2", {}, &rows);
    statement_guard guard(rows);
    CRPC_CHECK(static_cast<bool>(ran));
    CRPC_CHECK(rows->next_result());
    CRPC_CHECK_EQ(std::string("b"), rows->column(0).name);
    CRPC_CHECK(rows->next());
    CRPC_CHECK_EQ(std::int64_t{2}, rows->get_int64(0).value());
}

CRPC_TEST(ado_memory_result_set_has_no_next_result) {
    crpc::db::memory_result_set set({}, -1);
    CRPC_CHECK_FALSE(set.next_result());
    CRPC_CHECK_EQ(std::size_t{0}, set.rows_buffered());
    CRPC_CHECK_EQ(std::size_t{0}, set.column_count());
}

CRPC_TEST(ado_streaming_reads_a_large_result_without_materialising) {
    // 20000 rows across packets of 256 bytes: the reply spans many messages, and a
    // streaming cursor must hand rows out one at a time without buffering them.
    constexpr std::int32_t row_count = 20000;
    auto respond = login_server();
    int batch = 0;
    auto session = open_with([respond, &batch](packet_type type,
                                               crpc::span<const std::byte> payload) {
        if (type != packet_type::sql_batch) {
            return respond(type, payload);
        }
        ++batch;
        if (batch > 1) {
            bytes simple;
            put_colmetadata(simple, {{0x38, 0, "n"}});
            put_int4_row(simple, 42);
            put_done(simple, 0, 1);
            return simple;
        }
        bytes reply;
        put_colmetadata(reply, {{0x38, 0, "n"}});
        for (int i = 0; i < row_count; ++i) {
            put_int4_row(reply, i);
        }
        put_done(reply, 0, static_cast<std::uint64_t>(row_count));
        return reply;
    });
    CRPC_CHECK(static_cast<bool>(session->set_command_timeout(std::chrono::milliseconds(0))));

    result_set* rows = nullptr;
    const crpc::status ran = session->query_streaming("SELECT n FROM big", {}, &rows);
    statement_guard guard(rows);
    CRPC_CHECK(static_cast<bool>(ran));
    CRPC_CHECK(rows != nullptr);
    CRPC_CHECK(rows->is_streaming());

    std::int64_t read = 0;
    while (rows->next()) {
        CRPC_CHECK_EQ(read, rows->get_int64(0).value());
        // At no point may more than the current row be buffered.
        CRPC_CHECK(rows->rows_buffered() <= 1);
        ++read;
    }
    CRPC_CHECK_EQ(std::int64_t{row_count}, read);
    CRPC_CHECK_EQ(std::size_t{row_count}, rows->rows_decoded());
    CRPC_CHECK_EQ(std::size_t{0}, rows->rows_buffered());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(rows->last_error().code));

    // The reply was fully drained, so the session serves the next request.
    result_set* after = nullptr;
    const crpc::status second = session->query("SELECT 42", {}, &after);
    statement_guard after_guard(after);
    CRPC_CHECK(static_cast<bool>(second));
    CRPC_CHECK(after != nullptr);
    CRPC_CHECK(after->next());
    CRPC_CHECK_EQ(std::int64_t{42}, after->get_int64(0).value());
}

CRPC_TEST(ado_streaming_next_result_walks_two_result_sets) {
    auto respond = login_server();
    int batch = 0;
    auto session = open_with([respond, &batch](packet_type type,
                                               crpc::span<const std::byte> payload) {
        if (type != packet_type::sql_batch) {
            return respond(type, payload);
        }
        ++batch;
        if (batch == 1) {
            return two_result_sets(0x0001);
        }
        bytes reply;
        put_colmetadata(reply, {{0x38, 0, "n"}});
        put_int4_row(reply, 8);
        put_done(reply, 0, 1);
        return reply;
    });

    result_set* rows = nullptr;
    const crpc::status ran = session->query_streaming("SELECT 1; SELECT 2", {}, &rows);
    statement_guard guard(rows);
    CRPC_CHECK(static_cast<bool>(ran));
    CRPC_CHECK(rows != nullptr);
    CRPC_CHECK(rows->is_streaming());

    CRPC_CHECK_EQ(std::string("a"), rows->column(0).name);
    CRPC_CHECK(rows->next());
    CRPC_CHECK_EQ(std::int64_t{1}, rows->get_int64(0).value());
    CRPC_CHECK(rows->next_result());

    CRPC_CHECK_EQ(std::string("b"), rows->column(0).name);
    CRPC_CHECK(rows->next());
    CRPC_CHECK_EQ(std::int64_t{2}, rows->get_int64(0).value());
    CRPC_CHECK_FALSE(rows->next());
    CRPC_CHECK_FALSE(rows->next_result());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(rows->last_error().code));

    result_set* again = nullptr;
    const crpc::status second = session->query("SELECT 8", {}, &again);
    statement_guard again_guard(again);
    CRPC_CHECK(static_cast<bool>(second));
    CRPC_CHECK(again != nullptr);
    CRPC_CHECK(again->next());
    CRPC_CHECK_EQ(std::int64_t{8}, again->get_int64(0).value());
}

CRPC_TEST(ado_streaming_closing_unread_drains_the_reply) {
    // Closing a half-read streaming cursor must swallow the rest of the reply, or
    // the next request would decode it as its own answer.
    auto respond = login_server();
    int batch = 0;
    auto session = open_with([respond, &batch](packet_type type,
                                               crpc::span<const std::byte> payload) {
        if (type != packet_type::sql_batch) {
            return respond(type, payload);
        }
        ++batch;
        if (batch > 1) {
            bytes simple;
            put_colmetadata(simple, {{0x38, 0, "n"}});
            put_int4_row(simple, 5);
            put_done(simple, 0, 1);
            return simple;
        }
        bytes reply;
        put_colmetadata(reply, {{0x38, 0, "n"}});
        for (int i = 0; i < 100; ++i) {
            put_int4_row(reply, i);
        }
        put_done(reply, 0, 100);
        return reply;
    });

    result_set* rows = nullptr;
    const crpc::status ran = session->query_streaming("SELECT n FROM many", {}, &rows);
    CRPC_CHECK(static_cast<bool>(ran));
    {
        statement_guard guard(rows);
        CRPC_CHECK(rows->next());
        CRPC_CHECK_EQ(std::int64_t{0}, rows->get_int64(0).value());
        // Destroying the guard closes the cursor mid-reply; close() drains it.
    }
    result_set* after = nullptr;
    const crpc::status second = session->query("SELECT 5", {}, &after);
    statement_guard after_guard(after);
    CRPC_CHECK(static_cast<bool>(second));
    CRPC_CHECK(after != nullptr);
    CRPC_CHECK(after->next());
    CRPC_CHECK_EQ(std::int64_t{5}, after->get_int64(0).value());
}

CRPC_TEST(ado_streaming_reports_a_server_error_in_last_error) {
    int batch = 0;
    auto respond = login_server();
    auto session = open_with([respond, &batch](packet_type type,
                                               crpc::span<const std::byte> payload) {
        if (type != packet_type::sql_batch) {
            return respond(type, payload);
        }
        ++batch;
        bytes reply;
        if (batch == 1) {
            put_error(reply, 208, 16, "Invalid object name 'nope'.");
            put_done(reply, 0x0002, 0);
            return reply;
        }
        put_colmetadata(reply, {{0x38, 0, "n"}});
        put_int4_row(reply, 7);
        put_done(reply, 0, 1);
        return reply;
    });

    // The server rejects before producing any row, so the error surfaces when the
    // stream is opened, exactly as the materialised path reports it.
    result_set* rows = nullptr;
    const crpc::status ran = session->query_streaming("SELECT * FROM nope", {}, &rows);
    CRPC_CHECK_FALSE(static_cast<bool>(ran));
    if (!ran) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                      static_cast<int>(ran.error().code));
    }
    // The reply was drained, so the server error leaves the session usable.
    result_set* after = nullptr;
    const crpc::status second = session->query("SELECT 7", {}, &after);
    statement_guard after_guard(after);
    CRPC_CHECK(static_cast<bool>(second));
    CRPC_CHECK(after != nullptr);
    CRPC_CHECK(after->next());
    CRPC_CHECK_EQ(std::int64_t{7}, after->get_int64(0).value());
}

CRPC_TEST(ado_named_parameters_bind_by_name) {
    bytes sent;
    auto respond = login_server();
    auto session = open_with([respond, &sent](packet_type type,
                                              crpc::span<const std::byte> payload) {
        if (type != packet_type::rpc) {
            return respond(type, payload);
        }
        sent.assign(payload.begin(), payload.end());
        bytes reply;
        put_colmetadata(reply, {{0x26, 8, "n"}});
        put_u8(reply, static_cast<std::uint8_t>(token_type::row));
        put_u8(reply, 8);
        put_u32_le(reply, 42);
        put_u32_le(reply, 0);
        put_done(reply, 0, 1);
        return reply;
    });

    // Caller-written `@id` in the text plus a named parameter: the name binds, the
    // statement is not rewritten, and the declaration uses the caller's name.
    crpc::db::parameter param = crpc::db::parameter::from_int64(42);
    param.name = "id";
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran =
        session->query("SELECT @id + 1 AS n",
                       crpc::span<const crpc::db::parameter>(&param, 1), &raw);
    crpc::db::statement_guard guard(raw);
    CRPC_CHECK(static_cast<bool>(ran));
    CRPC_CHECK(raw != nullptr);
    if (raw != nullptr) {
        CRPC_CHECK(raw->next());
        CRPC_CHECK_EQ(std::int64_t{42}, raw->get_int64(0).value());
    }

    CRPC_CHECK(!sent.empty());
    CRPC_CHECK(contains_utf16(sent, "SELECT @id + 1"));
    CRPC_CHECK(contains_utf16(sent, "@id bigint"));
    CRPC_CHECK_FALSE(contains_utf16(sent, "@P1"));
    CRPC_CHECK_FALSE(contains_utf16(sent, "SELECT ? + 1"));
}

CRPC_TEST(ado_named_parameters_match_case_insensitively) {
    bytes sent;
    auto respond = login_server();
    auto session = open_with([respond, &sent](packet_type type,
                                              crpc::span<const std::byte> payload) {
        if (type != packet_type::rpc) {
            return respond(type, payload);
        }
        sent.assign(payload.begin(), payload.end());
        bytes reply;
        put_done(reply, 0, 0);
        return reply;
    });
    crpc::db::parameter param = crpc::db::parameter::from_int64(5);
    param.name = "UserId";
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = session->query(
        "SELECT @userid AS n", crpc::span<const crpc::db::parameter>(&param, 1), &raw);
    crpc::db::statement_guard guard(raw);
    CRPC_CHECK(static_cast<bool>(ran));
    CRPC_CHECK(!sent.empty());
    CRPC_CHECK(contains_utf16(sent, "@userid bigint"));
}

CRPC_TEST(ado_mixing_named_and_positional_parameters_is_rejected) {
    bytes sent;
    auto respond = login_server();
    auto session = open_with([respond, &sent](packet_type type,
                                              crpc::span<const std::byte> payload) {
        if (type != packet_type::rpc) {
            return respond(type, payload);
        }
        sent.assign(payload.begin(), payload.end());
        return bytes{};
    });
    crpc::db::parameter params[2] = {crpc::db::parameter::from_int64(1),
                                     crpc::db::parameter::from_int64(2)};
    params[0].name = "a";
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = session->query(
        "SELECT @a + ?", crpc::span<const crpc::db::parameter>(params, 2), &raw);
    CRPC_CHECK_FALSE(static_cast<bool>(ran));
    if (!ran) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(ran.error().code));
    }
    CRPC_CHECK(sent.empty());
}

CRPC_TEST(ado_named_parameter_missing_from_the_statement_is_rejected) {
    bytes sent;
    auto respond = login_server();
    auto session = open_with([respond, &sent](packet_type type,
                                              crpc::span<const std::byte> payload) {
        if (type != packet_type::rpc) {
            return respond(type, payload);
        }
        sent.assign(payload.begin(), payload.end());
        return bytes{};
    });
    crpc::db::parameter param = crpc::db::parameter::from_int64(9);
    param.name = "missing";
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = session->query(
        "SELECT @other AS n", crpc::span<const crpc::db::parameter>(&param, 1), &raw);
    CRPC_CHECK_FALSE(static_cast<bool>(ran));
    if (!ran) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(ran.error().code));
        CRPC_CHECK(ran.error().message.find("missing") != std::string::npos);
    }
    CRPC_CHECK(sent.empty());
}

CRPC_TEST(ado_isolation_level_emits_set_transaction) {
    std::vector<std::string> batches;
    auto respond = login_server();
    auto session = open_with([&](packet_type type, crpc::span<const std::byte> payload) {
        if (type != packet_type::sql_batch) {
            return login_server()(type, payload);
        }
        const std::string text = crpc::db::tds::from_utf16le(
            crpc::span<const std::byte>(payload.data(), payload.size()));
        // Only the SET+START batches are interesting; the ROLLBACK that follows
        // is also a batch and would skew the count.
        if (text.find("SET TRANSACTION ISOLATION LEVEL") != std::string::npos) {
            batches.push_back(text);
        }
        bytes reply;
        put_done(reply, 0, 0);
        return reply;
    });

    CRPC_CHECK(static_cast<bool>(session->begin(
        crpc::db::isolation_level::serializable)));
    CRPC_CHECK(session->in_transaction());
    CRPC_CHECK(static_cast<bool>(session->begin(
        crpc::db::isolation_level::read_committed)));
    CRPC_CHECK(static_cast<bool>(session->rollback()));

    CRPC_CHECK_EQ(std::size_t{2}, batches.size());
    CRPC_CHECK(batches[0].find("SET TRANSACTION ISOLATION LEVEL SERIALIZABLE; "
                               "BEGIN TRANSACTION") != std::string::npos);
    CRPC_CHECK(batches[1].find("SET TRANSACTION ISOLATION LEVEL READ COMMITTED; "
                               "BEGIN TRANSACTION") != std::string::npos);
}

CRPC_TEST(ado_isolation_level_unspecified_forwards_to_begin) {
    auto respond = login_server();
    auto session = open_with([respond](packet_type type, crpc::span<const std::byte> payload) {
        if (type != packet_type::sql_batch) {
            return respond(type, payload);
        }
        bytes reply;
        put_done(reply, 0, 0);
        return reply;
    });
    CRPC_CHECK(static_cast<bool>(session->begin(crpc::db::isolation_level::unspecified)));
    CRPC_CHECK(session->in_transaction());
    CRPC_CHECK(static_cast<bool>(session->rollback()));
}

CRPC_TEST(ado_command_timeout_cancels_a_slow_statement_and_stays_usable) {
    // The first batch is answered only after 2s. With a 300ms command timeout the
    // watchdog must cancel it and the query must report `canceled` well before the
    // delayed answer could have arrived.
    auto respond = login_server();
    auto link = std::make_unique<delaying_channel>(
        std::chrono::milliseconds(2000),
        [respond](packet_type type, crpc::span<const std::byte> payload) -> bytes {
            if (type == packet_type::attention) {
                bytes ack;
                put_done(ack, 0x0020, 0); // DONE_ATTN
                return ack;
            }
            if (type != packet_type::sql_batch) {
                return respond(type, payload);
            }
            bytes reply;
            put_colmetadata(reply, {{0x38, 0, "n"}});
            put_int4_row(reply, 1);
            put_done(reply, 0, 1);
            return reply;
        });
    crpc::db::tds::connection session(options_for_tests(), std::move(link));
    CRPC_CHECK(session.is_open());

    crpc::status settled = session.set_command_timeout(std::chrono::milliseconds(300));
    CRPC_CHECK(static_cast<bool>(settled));
    CRPC_CHECK_EQ(std::chrono::milliseconds(300).count(), session.command_timeout().count());

    const auto began = std::chrono::steady_clock::now();
    result_set* rows = nullptr;
    const crpc::status ran = session.query("SLOW", {}, &rows);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - began)
                             .count();
    statement_guard guard(rows);
    CRPC_CHECK_FALSE(static_cast<bool>(ran));
    if (!ran) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::canceled),
                      static_cast<int>(ran.error().code));
    }
    // Cancelled well before the server "would have" answered at 2s.
    CRPC_CHECK(elapsed < 1900);
    CRPC_CHECK(session.is_open());

    // The session still answers: the acknowledgement was consumed and the delayed
    // reply was dropped, so the stream is clean.
    result_set* after = nullptr;
    const crpc::status second = session.query("SELECT 1", {}, &after);
    statement_guard after_guard(after);
    CRPC_CHECK(static_cast<bool>(second));
    if (after != nullptr) {
        CRPC_CHECK(after->next());
        CRPC_CHECK_EQ(std::int64_t{1}, after->get_int64(0).value());
    }
}

CRPC_TEST(ado_streaming_timeout_cancels_a_slow_first_token) {
    auto respond = login_server();
    auto link = std::make_unique<delaying_channel>(
        std::chrono::milliseconds(2000),
        [respond](packet_type type, crpc::span<const std::byte> payload) -> bytes {
            if (type == packet_type::attention) {
                bytes ack;
                put_done(ack, 0x0020, 0);
                return ack;
            }
            if (type != packet_type::sql_batch) {
                return respond(type, payload);
            }
            bytes reply;
            put_colmetadata(reply, {{0x38, 0, "n"}});
            put_int4_row(reply, 5);
            put_done(reply, 0, 1);
            return reply;
        });
    crpc::db::tds::connection session(options_for_tests(), std::move(link));
    CRPC_CHECK(session.is_open());
    CRPC_CHECK(static_cast<bool>(session.set_command_timeout(std::chrono::milliseconds(300))));

    const auto began = std::chrono::steady_clock::now();
    result_set* rows = nullptr;
    const crpc::status ran = session.query_streaming("SLOW", {}, &rows);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - began)
                             .count();
    statement_guard guard(rows);
    CRPC_CHECK_FALSE(static_cast<bool>(ran));
    if (!ran) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::canceled),
                      static_cast<int>(ran.error().code));
    }
    CRPC_CHECK(elapsed < 1900);
    CRPC_CHECK(session.is_open());

    result_set* after = nullptr;
    const crpc::status second = session.query("SELECT 2", {}, &after);
    statement_guard after_guard(after);
    CRPC_CHECK(static_cast<bool>(second));
    if (after != nullptr) {
        CRPC_CHECK(after->next());
        // The scripted responder always emits the row 5; what matters is that the
        // reply is the scripted one, not a tail of the cancelled request.
        CRPC_CHECK_EQ(std::int64_t{5}, after->get_int64(0).value());
    }
}

CRPC_TEST_MAIN()