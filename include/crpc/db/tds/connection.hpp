// CRPC -- TDS connection: prelogin, login, batches, transactions.
//
// One connection is one TDS session. The wire rules that matter here and are easy
// to get wrong:
//
//   * a message larger than the negotiated packet size is split into packets, each
//     with its own header, and is finished by the one whose `eom` bit is set. Both
//     directions need this: a batch over 4 KiB and a result set over 4 KiB are
//     ordinary, not exceptional;
//   * the server sends tokens, and the end of a batch is a DONE token, not the end
//     of a packet. A reply can span several messages, so reading stops at DONE with
//     the "more results" bit clear — a reader that stopped at end-of-packet would
//     truncate any result taller than one packet;
//   * an error arrives as an ERROR token in the middle of the stream and the rest
//     of the reply still has to be drained, or the next query decodes the leftover
//     as its own answer.
//
// Encryption is negotiated in PRELOGIN. This slice refuses a server that requires
// it instead of pretending to encrypt: half a handshake looks like a login failure
// and hides the cause.
#ifndef CRPC_DB_TDS_CONNECTION_HPP
#define CRPC_DB_TDS_CONNECTION_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/bulk.hpp>
#include <crpc/db/channel.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/tds/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace tds {

struct connection_options {
    std::string user;
    std::string password;
    // Selected by LOGIN7. Empty leaves the server default.
    std::string database;
    // Sent in LOGIN7 as the client host name; SQL Server only uses it for auditing
    // and for `HOST_NAME()`, so a wrong value is cosmetic rather than fatal.
    std::string host_name = "crpc";
    std::string application_name = "crpc";
    // Sent in LOGIN7's ServerName field. A caller that leaves it empty gets the
    // endpoint host, which is what the server expects for its own bookkeeping.
    std::string server_name;

    // Requested packet size. The server may lower it; the negotiated value is what
    // framing uses afterwards, not this one.
    std::uint16_t packet_size = default_packet_size;

    // Upper bound on one reassembled message, before it is refused. A result set
    // larger than this fails with `resource_exhausted` instead of growing until the
    // process dies.
    std::size_t max_message_size = default_max_message_size;
    std::size_t max_result_rows = 1'000'000;
    std::size_t max_result_bytes = 256u * 1024u * 1024u;

    // Demand an encrypted session. PRELOGIN then offers ENCRYPT_ON, so a server that
    // refuses TLS fails the connection instead of receiving a plaintext login.
    bool require_encryption = false;

    // Encrypt only the login packet, then continue in the clear. Refused until the TLS
    // path is verified; see `require_encryption`.
    bool encrypt_login_only = false;

    // Used when TLS is negotiated. A production deployment should leave this on; the
    // integration suite turns it off for a self-signed test server, and says so.
    bool verify_server_certificate = true;
    // Trust anchors for verification. Empty uses the system store.
    std::string ca_bundle;
};

// One authenticated TDS session.
class CRPC_API connection final : public db::connection {
public:
    // Performs PRELOGIN and LOGIN7 over `link`. On failure the link is closed and
    // `connect_error()` carries the server's own number and message.
    connection(connection_options options, std::unique_ptr<db::channel> link);
    ~connection() override;

    [[nodiscard]] status ping() override;
    [[nodiscard]] bool in_transaction() const noexcept override;

    [[nodiscard]] status begin() override;
    [[nodiscard]] status commit() override;
    [[nodiscard]] status rollback() override;

    // Runs one SQL batch, or a parameterised statement through `sp_executesql` when
    // `params` is not empty.
    //
    // Parameters are bound by RPC, never spliced into the text: the statement goes as
    // the first argument, the parameter declarations as the second, and the values
    // follow as typed parameters. A `?` in `sql` is replaced by `@P<n>` in order,
    // because TDS addresses parameters by name.
    //
    // `*out` receives the first result set, or a cursor with an affected-row count
    // and no columns. Further result sets are drained and discarded — a reply that
    // is not drained desynchronises the stream.
    [[nodiscard]] status query(std::string_view sql, span<const parameter> params,
                               result_set** out) override;

    // Inserts `rows` with batched `INSERT ... VALUES (...), (...)` statements whose
    // values are all bound.
    //
    // SQL Server caps a single request at 2100 parameters, so a batch is sized by
    // `2100 / column_count` as well as by the caller's `max_rows_per_statement`. Rows
    // per statement are the unit of failure: a batch that fails discards only its own
    // rows, and `affected` accumulates the counts of the batches that succeeded.
    [[nodiscard]] status bulk_insert(const db::insert_target& target,
                                     span<const std::vector<parameter>> rows,
                                     const db::bulk_options& options,
                                     std::uint64_t& affected);

    // Calls a stored procedure by name with RPC, writing OUT/INOUT values back into
    // `params`. The procedure's return code is available from `last_return_status()`.
    [[nodiscard]] status call(std::string_view procedure, span<parameter> params,
                              result_set** out) override;

    // Asks the server to stop the request that is running. Safe to call from another
    // thread while a query is blocked in a read: it writes a single 8-byte ATTENTION
    // packet under the same write lock, and the blocked call then returns
    // `rpc_code::canceled` once the server acknowledges.
    //
    // TDS has no way to cancel from the same thread that is waiting, so a caller that
    // wants a timeout has to arm it elsewhere.
    [[nodiscard]] status cancel();

    // The RETURNSTATUS of the last RPC call, when the procedure returned one.
    [[nodiscard]] std::uint32_t last_return_status() const noexcept {
        return last_return_status_;
    }
    [[nodiscard]] bool has_return_status() const noexcept { return has_return_status_; }

    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept override;

    // Diagnostics.
    [[nodiscard]] std::uint32_t server_version() const noexcept { return server_version_; }
    [[nodiscard]] std::uint16_t negotiated_packet_size() const noexcept {
        return packet_size_;
    }
    [[nodiscard]] const std::string& server_name() const noexcept { return server_name_; }
    [[nodiscard]] bool encryption_negotiated() const noexcept { return encrypted_; }
    // True when only the login packet was encrypted and the session runs in the clear.
    [[nodiscard]] bool login_only_encryption() const noexcept { return login_only_encryption_; }
    [[nodiscard]] const rpc_error& connect_error() const noexcept { return connect_error_; }

private:
    [[nodiscard]] status perform_handshake();
    // Every message after the login starts with an AllHeaders block. Without it a
    // real server answers "the incoming tabular data stream (TDS) protocol stream is
    // incorrect. The TDS headers contained errors." -- which is what happened before
    // this block existed.
    [[nodiscard]] std::vector<std::byte> all_headers() const;
    [[nodiscard]] status send_message(packet_type type, span<const std::byte> payload);
    [[nodiscard]] status read_message(std::vector<std::byte>& payload);
    // Sends one batch and decodes the whole reply, returning the first result set.
    [[nodiscard]] status run_batch(std::string_view sql, result_set** out);
    // Sends one RPC and decodes the whole reply. `results` is the first result set;
    // `written` receives the OUT/INOUT values, addressed by parameter name.
    [[nodiscard]] status run_rpc(const rpc_request& request, result_set** out,
                                 std::vector<return_value>* written);
    // The shared token loop for batch and RPC replies.
    [[nodiscard]] status read_reply(result_set** out, std::vector<return_value>* written);
    // For a query, ? placeholders become @P<n> in an sp_executesql call; for a
    // procedure call the parameters are sent to the procedure itself.
    [[nodiscard]] status call_impl(std::string_view sql, span<const parameter> params,
                                   result_set** out, bool procedure,
                                   std::vector<return_value>& written);
    void fail(const status& error) noexcept;

    connection_options options_;
    std::unique_ptr<db::channel> link_;
    std::string server_name_;
    std::uint32_t server_version_ = 0;
    std::uint16_t packet_size_ = default_packet_size;
    std::uint8_t packet_id_ = 1;
    bool open_ = false;
    bool in_transaction_ = false;
    bool encrypted_ = false;
    bool login_only_encryption_ = false;
    std::size_t received_bytes_ = 0;
    // Echoed from ENVCHANGE(begin_transaction); cleared on commit and rollback.
    std::array<std::uint8_t, transaction_descriptor_size> descriptor_{};
    bool has_descriptor_ = false;
    // Serialises whole messages: an ATTENTION from another thread must not land in
    // the middle of a batch's packets.
    std::mutex write_mutex_;
    // A cancel was asked for and its acknowledgement has not been consumed yet. The
    // acknowledgement is a separate message, so the reply loop has to keep reading.
    // Written by cancel() on another thread while the reply loop reads them.
    std::atomic<bool> attention_pending_{false};
    std::atomic<bool> attention_seen_{false};
    // True between sending a batch/RPC and reading its final token. Only then can an
    // ATTENTION be acknowledged, so only then is one worth waiting for.
    std::atomic<bool> in_request_{false};
    std::uint32_t last_return_status_ = 0;
    bool has_return_status_ = false;
    rpc_error connect_error_;
};

// Opens connections over a caller-supplied channel.
//
// This is the injectable form of the endpoint factory: tests and custom transports
// supply the channel, so the pool plus this adapter can be verified end to end
// without a server.
class CRPC_API channel_factory final : public db::connection_factory {
public:
    using opener = std::function<result<std::unique_ptr<db::channel>>()>;

    channel_factory(connection_options options, opener open);
    ~channel_factory() override;

    [[nodiscard]] result<db::connection*> open() override;
    [[nodiscard]] std::string_view name() const noexcept override;

private:
    connection_options options_;
    opener open_;
};

// Opens one connection to one endpoint. A pool owns the result.
class CRPC_API connection_factory final : public db::connection_factory {
public:
    connection_factory(connection_options options, crpc::backend_factory& backend,
                       crpc::endpoint endpoint, crpc::runtime* shard = nullptr);
    ~connection_factory() override;

    [[nodiscard]] result<db::connection*> open() override;
    [[nodiscard]] std::string_view name() const noexcept override;

private:
    connection_options options_;
    crpc::backend_factory* backend_ = nullptr;
    crpc::endpoint endpoint_;
    crpc::runtime* shard_ = nullptr;
};

} // namespace tds
} // namespace db
} // namespace crpc

#endif // CRPC_DB_TDS_CONNECTION_HPP