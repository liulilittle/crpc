// CRPC -- HBase REST (Stargate) adapter.
//
// HBase has two protocols: the native one (protobuf over a custom RPC) and the REST gateway
// ("Stargate") that the distribution ships. This adapter speaks the gateway, because it is
// the documented, stable surface; the native protocol needs the cluster's protobuf
// definitions and a ZooKeeper-derived region map.
//
// One asymmetry in the gateway is worth stating up front, because getting it wrong produces
// 404s that look like missing rows:
//
//   * the **path** carries the raw row key, URL-encoded (`GET /table/row1`);
//   * the **JSON body** carries base64 for row keys, column names and cell values.
//
// That was established by probing a real server, not from memory: `/table/cjE=` (base64 of
// "r1") answers 404 while `/table/r1` answers 200 with the same row.
//
// Columns are addressed as `family` or `family:qualifier`; a cell's value is opaque bytes,
// so it is base64 in JSON in both directions.
#ifndef CRPC_DB_HBASE_REST_HPP
#define CRPC_DB_HBASE_REST_HPP

#include <cstddef>
#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/http/client.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace hbase {

// One cell: a family, an optional qualifier, a value and the server's timestamp.
struct cell {
    std::string family;
    std::string qualifier;
    std::vector<std::byte> value;
    // Milliseconds since the epoch, as the server reports it. 0 when the client is writing
    // (the server stamps it).
    std::int64_t timestamp = 0;
};

struct row {
    // The raw key. The path encoding and the JSON encoding are applied where they belong, so
    // a caller never handles base64.
    std::vector<std::byte> key;
    std::vector<cell> cells;
};

// Encodes rows as the CellSet JSON the gateway accepts. Exposed because it is the part worth
// testing without a server.
[[nodiscard]] CRPC_API result<std::string> encode_cell_set(span<const row> rows);

// Decodes a CellSet JSON reply. Strict: an unknown member is ignored, but a malformed one is
// an error, and a `Row` without a `key` is refused rather than silently skipped.
[[nodiscard]] CRPC_API result<std::vector<row>> decode_cell_set(std::string_view json);

// Percent-encodes a path segment. A row key is binary, so every byte outside the unreserved
// set is escaped; leaving `/` or `?` unescaped would change which resource is addressed.
[[nodiscard]] CRPC_API std::string encode_path_segment(span<const std::byte> data);

struct client_options {
    // The gateway's host and port. HBase REST defaults to 8080; this deployment uses 18080.
    std::string host;
    std::uint16_t port = 8080;

    // Sent as `Accept`. The gateway answers XML by default, and a client that forgets this
    // header has to parse XML.
    std::string accept = "application/json";

    // Bound on one reply, so a large scan cannot exhaust memory.
    std::size_t max_response_bytes = 64u * 1024u * 1024u;

    std::uint32_t timeout_seconds = 30;

    // How long `create_table`/`delete_table` wait for the table transition, in
    // milliseconds. Zero disables the wait, which is what a caller that manages the state
    // itself wants.
    std::uint32_t transition_timeout_ms = 30000;
};

// One gateway session. It owns no socket: each call opens a fresh HTTP exchange, because the
// gateway's HTTP/1.1 keep-alive is not worth a state machine here and a REST call is not a
// hot path.
class CRPC_API client final {
public:
    // Opens the gateway connection lazily, through `opener`, so a caller can supply a
    // transport (a test, a TLS wrapper) without this class knowing about sockets.
    using opener = std::function<std::unique_ptr<http::client>()>;

    client(client_options options, opener open);
    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;

    // `GET /version`: the gateway's own version string. Also the cheapest liveness probe.
    [[nodiscard]] result<std::string> version();

    // `GET /`: the table names.
    [[nodiscard]] result<std::vector<std::string>> list_tables();
    [[nodiscard]] result<bool> table_exists(std::string_view table);

    // `PUT /<table>/schema` with the XML the gateway documents, then a wait for the table to
    // appear: a write issued against a table that is still being created fails.
    //
    // Both this and `delete_table` block until the gateway has finished the transition. HBase
    // creates and deletes tables asynchronously and the gateway offers no completion signal,
    // so the alternative is a caller that has to sleep between "delete" and "create" — and a
    // create that races a still-running delete fails with a table-state error that looks like
    // a permissions problem.
    [[nodiscard]] status create_table(std::string_view table,
                                      span<const std::string> families);
    [[nodiscard]] status delete_table(std::string_view table);

    // Writes one row. `PUT /<table>/false` is "do not check for an existing row".
    [[nodiscard]] status put_row(std::string_view table, const row& value);

    // Reads one row. A missing row is `not_found`, which is a result rather than an error at
    // the HBase level but is an error for a caller that asked for that key.
    [[nodiscard]] result<row> get_row(std::string_view table, span<const std::byte> key);

    // Deletes one row.
    [[nodiscard]] status delete_row(std::string_view table, span<const std::byte> key);

    // Reads up to `limit` rows starting at `start` (empty means the beginning).
    //
    // This uses the scanner API rather than repeated single-row reads: it is the documented
    // way to read a range, and one scanner round trip is cheaper than N row reads.
    [[nodiscard]] result<std::vector<row>> scan(std::string_view table,
                                                span<const std::byte> start,
                                                std::size_t limit);

private:
    // Polls `table_exists` until it matches `present` or the timeout expires.
    [[nodiscard]] status wait_for_table(std::string_view table, bool present);
    [[nodiscard]] result<http::response> call(http::method verb, std::string_view target,
                                              std::string_view body = {},
                                              std::string_view content_type = {});
    [[nodiscard]] result<std::unique_ptr<http::client>> open_link();

    client_options options_;
    opener open_;
};

// Maps an HTTP status from the gateway to the library code. The gateway uses plain HTTP
// codes rather than a body-level error object, so the status line is the classification.
[[nodiscard]] CRPC_API rpc_code map_http_status(int status) noexcept;

} // namespace hbase
} // namespace db
} // namespace crpc

#endif // CRPC_DB_HBASE_REST_HPP