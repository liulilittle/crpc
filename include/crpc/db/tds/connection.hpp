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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
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

    // Require the link to be encrypted. This slice has no TLS, so setting it is
    // refused at construction with a clear message rather than silently sending a
    // login in the clear.
    bool require_encryption = false;
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

    // Runs one SQL batch. Parameters are refused: TDS binds them through an RPC
    // call (sp_executesql), which this slice does not encode yet, and splicing them
    // into the text is never acceptable.
    //
    // `*out` receives the first result set, or a cursor with an affected-row count
    // and no columns. Further result sets are drained and discarded — a reply that
    // is not drained desynchronises the stream.
    [[nodiscard]] status query(std::string_view sql, span<const parameter> params,
                               result_set** out) override;

    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept override;

    // Diagnostics.
    [[nodiscard]] std::uint32_t server_version() const noexcept { return server_version_; }
    [[nodiscard]] std::uint16_t negotiated_packet_size() const noexcept {
        return packet_size_;
    }
    [[nodiscard]] const std::string& server_name() const noexcept { return server_name_; }
    [[nodiscard]] bool encryption_negotiated() const noexcept { return encrypted_; }
    [[nodiscard]] const rpc_error& connect_error() const noexcept { return connect_error_; }

private:
    [[nodiscard]] status perform_handshake();
    [[nodiscard]] status send_message(packet_type type, span<const std::byte> payload);
    [[nodiscard]] status read_message(std::vector<std::byte>& payload);
    // Sends one batch and decodes the whole reply, returning the first result set.
    [[nodiscard]] status run_batch(std::string_view sql, result_set** out);
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
    std::size_t received_bytes_ = 0;
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