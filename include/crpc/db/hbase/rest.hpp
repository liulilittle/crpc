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

#include <chrono>
#include <cstddef>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
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

// The comparison the gateway applies in a `SingleColumnValueFilter`. The spelling is the
// server's (`GREATER_OR_EQUAL`), not `CompareFilter::CompareOp`'s Java name, because the
// JSON model is parsed with `CompareOperator.valueOf`.
enum class compare_operator {
    less,
    less_or_equal,
    equal,
    not_equal,
    greater_or_equal,
    greater,
    no_op,
};

// One server-side filter for a scan.
//
// Only filters that the gateway models as flat JSON objects are here: a prefix on the row
// key, a prefix on the qualifier, and a comparison against one column's value. A filter
// document is sent as a *string* inside the scanner request (see `encode_scan_filter`), and
// its row keys, column names and values are base64.
struct scan_filter {
    enum class kind {
        none,
        row_prefix,
        column_prefix,
        column_value,
    };

    kind which = kind::none;
    // `row_prefix`: the key prefix. `column_value`: the compared value. Base64 in JSON.
    std::vector<std::byte> value;
    // `column_prefix`: the qualifier prefix, as plain text.
    std::string qualifier;
    // `column_value`: the family and qualifier of the compared column.
    std::string family;
    compare_operator op = compare_operator::equal;
    // `column_value`: whether a row that lacks the column is filtered out. The HBase default
    // is true; a false here keeps such rows, which is what a caller asking "not equal to X"
    // usually means.
    bool if_missing = true;

    // Matches rows whose key starts with `prefix`.
    [[nodiscard]] static scan_filter row_prefix(span<const std::byte> prefix);
    // Matches cells whose qualifier starts with `prefix`, and keeps the rows that have one.
    [[nodiscard]] static scan_filter column_prefix(std::string_view prefix);
    // Compares the bytes of `family:qualifier` against `value`.
    [[nodiscard]] static scan_filter column_value(std::string family, std::string qualifier,
                                                  compare_operator op,
                                                  span<const std::byte> value,
                                                  bool if_missing = true);
};

// The options a scan request carries. `scan(table, options)` is the general form; the older
// `scan(table, start, limit)` is kept and forwards to it.
struct scan_options {
    // First row to return (inclusive). Empty means the first row of the table.
    std::vector<std::byte> start_row;
    // Row to stop before (exclusive). Empty means no upper bound.
    std::vector<std::byte> end_row;
    // Rows the caller wants. 0 means "as many as the server sends".
    std::size_t limit = 0;
    // Rows per reply. 0 means "the limit, or 1000 when the limit is 0". A small batch makes
    // the client drain the scanner over several round trips, which is what bounds memory.
    std::size_t batch = 0;
    // Versions per cell. 0 leaves the server default.
    std::uint32_t max_versions = 0;
    // Applied by the server. `kind::none` sends no `filter` member at all.
    scan_filter where;
};

// The `filter` member's value: the filter document as compact JSON text. The scanner request
// carries it as a *string*, so the caller does not escape it; `encode_scanner_request` does.
// Returns an error for `kind::none` or an incomplete filter (a column-value filter needs a
// family).
[[nodiscard]] CRPC_API result<std::string> encode_scan_filter(const scan_filter& filter);

// The scanner request body (`PUT /<table>/scanner`). Exposed so the exact bytes, including
// the double encoding of the filter document, can be pinned without a server.
[[nodiscard]] CRPC_API result<std::string> encode_scanner_request(const scan_options& options);

// One REST gateway the client may route to. The endpoints of a deployment are
// interchangeable gateways to the same cluster; a caller that configures
// gateways pointing at *different* clusters is asking failover to move data
// operations between those clusters and must not use this class.
struct CRPC_API endpoint {
    std::string host;
    std::uint16_t port = 8080;
};

// Whether an endpoint currently receives routed traffic.
enum class endpoint_condition : std::uint8_t {
    // Eligible for routing.
    alive,
    // Ejected after `failure_threshold` consecutive transport failures. It is
    // re-admitted only after a successful `GET /version` probe.
    dead
};

// A point-in-time view of one endpoint, for `client::health()`.
struct CRPC_API endpoint_health {
    std::string host;
    std::uint16_t port = 8080;
    endpoint_condition condition = endpoint_condition::alive;
    // Transport failures since the last successful exchange. A reply of any
    // status (200, 404, 500, ...) proves the endpoint answered and resets this
    // to zero; only transport-level failures count against an endpoint.
    std::uint32_t consecutive_failures = 0;
    // The most recent transport failure. Retained after recovery as a
    // diagnostic; `consecutive_failures == 0` means the endpoint is healthy.
    rpc_code last_error = rpc_code::ok;
    std::string last_error_message;
    // Time left before the next recovery probe. Zero while alive, or once the
    // cooldown has elapsed and a probe is due.
    std::chrono::milliseconds retry_in{0};
    // The last time this endpoint answered a request, wall clock. Epoch when it
    // has never answered.
    std::chrono::system_clock::time_point last_success_at{};
};

struct client_options {
    // The gateway's host and port. HBase REST defaults to 8080; this deployment uses 18080.
    //
    // These two fields configure the single-endpoint form, which is what the
    // class has always meant. When `endpoints` is non-empty it overrides them
    // and the client routes across every listed gateway.
    std::string host;
    std::uint16_t port = 8080;

    // The complete gateway list, tried in order. An empty list means the single
    // gateway at `host`/`port` (backwards compatible). Endpoints must all serve
    // the same cluster; see `endpoint`.
    std::vector<endpoint> endpoints;

    // --- Failover -------------------------------------------------------------
    //
    // An endpoint is ejected only for transport-level failures: connection
    // refused/reset, broken pipe, EOF, deadline and `unknown_side_effect`. A
    // reply of any HTTP status means the endpoint answered, so it is alive for
    // routing purposes even when the operation failed.

    // Consecutive transport failures before an endpoint is marked dead and
    // skipped. 0 disables ejection; the counters and `health()` still update.
    std::uint32_t failure_threshold = 3;

    // How long a dead endpoint stays out of service before a `GET /version`
    // probe is attempted. Zero means "probe on the next call".
    std::chrono::milliseconds retry_cooldown{30000};

    // Whether a *write* whose request may have been executed (the request was
    // sent and the connection died before the reply arrived) is replayed on the
    // next endpoint.
    //
    // Default false, and that default is a correctness decision. A replay of a
    // possibly-executed `put_row`/`delete_row`/`create_table`/`delete_table`
    // risks a duplicate write or a duplicate delete, because the gateway and
    // the operations behind it are not idempotent and there is no idempotency
    // key to deduplicate on. With the default, such a failure returns
    // `unknown_side_effect` with the side effect explicitly called out, so a
    // caller can reconcile by reading state instead of guessing. Setting this
    // opts into at-most-once semantics.
    //
    // Reads are idempotent and are always retried on the next live endpoint;
    // this flag never changes that. A write that was refused at connect time
    // (no byte could have reached the server) is also always retried, because
    // replaying it cannot duplicate anything.
    bool retry_on_uncertain = false;

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

    // Keep one HTTP/1.1 keep-alive connection per gateway parked between
    // calls, instead of dialing a fresh exchange every time. Off by default:
    // the failover design dials per attempt on purpose, so a transport
    // failure and a scripted transport both see one exchange per connection;
    // a caller that routes sustained traffic through one gateway opts in
    // here. With it on, a parked connection is health-checked before reuse
    // (`client::is_open`, which the transport marks closed on a peer FIN or
    // TLS close_notify), a connection retired by the server is dropped, and a
    // concurrent call finding the slot occupied simply dials its own -- so
    // reuse never serializes callers.
    bool reuse_connection = false;
};

// One gateway session. By default it owns no socket: each call opens a fresh
// HTTP exchange, because the gateway's HTTP/1.1 keep-alive is not worth a state
// machine here and a REST call is not a hot path. `client_options::reuse_connection`
// opts into one parked keep-alive connection per gateway instead.
class CRPC_API client final {
public:
    // Opens the gateway connection lazily, through `opener`, so a caller can supply a
    // transport (a test, a TLS wrapper) without this class knowing about sockets.
    //
    // Two forms exist. `opener` (no argument) is the original single-endpoint form: the
    // caller captures the one gateway it opens. `endpoint_opener` receives the endpoint
    // the client wants to talk to and is what multi-endpoint failover needs: the adapter
    // opens one fresh HTTP link per attempt, so it must be able to address every listed
    // gateway. A multi-endpoint list (`client_options::endpoints` with more than one
    // entry) requires `endpoint_opener`; the single-argument form refuses it rather than
    // silently serving every endpoint through one connection.
    using opener = std::function<std::unique_ptr<http::client>()>;
    using endpoint_opener = std::function<std::unique_ptr<http::client>(const endpoint&)>;

    client(client_options options, opener open);
    client(client_options options, endpoint_opener open);
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

    // The general form: a row range, a page size and a server-side filter. The filter is
    // evaluated by the region servers, so the reply is already narrowed; nothing is filtered
    // after it arrives.
    [[nodiscard]] result<std::vector<row>> scan(std::string_view table,
                                                const scan_options& options);

    // --- Failover state ------------------------------------------------------
    //
    // A snapshot of every endpoint in configured order. Cheap and local: it never
    // contacts a gateway, so a dead endpoint with an elapsed cooldown still reads `dead`
    // here until a routed call or `probe_endpoint` actually probes it.
    [[nodiscard]] std::vector<endpoint_health> health() const;

    // Probes one endpoint immediately with `GET /version` (the gateway's own, cheapest
    // liveness check), regardless of the cooldown. Returns true when the endpoint
    // answered — a reply of any kind makes it alive for routing — and false when the
    // probe failed at the transport level. With ejection disabled
    // (`failure_threshold == 0`) a failed probe does not mark a live endpoint dead, but
    // the return value still reports the failed probe. An out-of-range index is
    // `invalid_argument`.
    [[nodiscard]] result<bool> probe_endpoint(std::size_t index);

private:
    // Endpoint health, defined in rest.cpp.
    struct endpoint_state;

    // Polls `table_exists` until it matches `present` or the timeout expires.
    [[nodiscard]] status wait_for_table(std::string_view table, bool present);

    // Sends one HTTP exchange to one endpoint, by list index, and records the outcome in
    // the health state. Every transport-level call — routed methods, scanner drains,
    // probes — funnels through here so the state cannot drift.
    [[nodiscard]] result<http::response> send_on(std::size_t index, http::method verb,
                                                  std::string_view target,
                                                  std::string_view body = {},
                                                  std::string_view content_type = {});

    // The failover loop. Tries live endpoints in order starting from the last successful
    // one; a read failure moves to the next endpoint, a write failure moves only when the
    // request cannot have been executed (or `retry_on_uncertain` is set), and a reply of
    // any HTTP status stops the loop because it is an authoritative answer.
    //
    // `idempotent` selects the write-side rule described on
    // `client_options::retry_on_uncertain`. A non-idempotent call whose request may have
    // been executed on a dying connection returns `unknown_side_effect` instead of
    // rewinding to the next endpoint.
    [[nodiscard]] result<http::response> call_routed(http::method verb,
                                                      std::string_view target,
                                                      std::string_view body,
                                                      std::string_view content_type,
                                                      bool idempotent);

    // Runs one full scan (open, drain, release) against one endpoint, filling `out`.
    // A transport failure leaves `out` partially filled and is reported so the scan
    // loop can restart the whole scan on the next endpoint — re-running a read from
    // scratch is safe, and restarting is the only correct recovery for a scanner, which
    // is host-local state. An HTTP status reply fails the scan without retrying.
    [[nodiscard]] status scan_attempt(std::size_t index, std::string_view table,
                                      const scan_options& options, std::vector<row>& out);

    [[nodiscard]] result<std::unique_ptr<http::client>> make_link(const endpoint& ep);
    // Probes every dead endpoint whose cooldown elapsed, on this call's behalf.
    void maybe_probe();
    void mark_reachable(std::size_t index);
    void mark_failure(std::size_t index, const rpc_error& error);
    void resolve_endpoints();

    client_options options_;
    opener open_;
    endpoint_opener open_multi_;
    std::vector<endpoint> endpoints_;
    std::vector<endpoint_state> states_;
    // Index of the last endpoint that answered a request; the next routed call starts
    // the search here.
    std::size_t preferred_ = 0;
    mutable std::mutex mutex_;
};

// Maps an HTTP status from the gateway to the library code. The gateway uses plain HTTP
// codes rather than a body-level error object, so the status line is the classification.
[[nodiscard]] CRPC_API rpc_code map_http_status(int status) noexcept;

} // namespace hbase
} // namespace db
} // namespace crpc

#endif // CRPC_DB_HBASE_REST_HPP