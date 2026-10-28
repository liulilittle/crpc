// CRPC -- HBase REST codec tests.
//
// These run without a server. The CellSet shape and the path encoding are the parts that can
// be checked exactly, and they are where the asymmetry lives: JSON carries base64, the path
// carries the raw key.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/db/hbase/rest.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::hbase::cell;
using crpc::db::hbase::row;

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(text[i]);
    }
    return out;
}

// `span` is explicit here, so a vector has to be viewed rather than passed.
[[nodiscard]] crpc::span<const std::byte> view_of(const std::vector<std::byte>& data) noexcept {
    return crpc::span<const std::byte>(data.data(), data.size());
}

[[nodiscard]] std::string text_of(const std::vector<std::byte>& data) {
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

} // namespace

CRPC_TEST(hbase_cell_set_encodes_base64_for_every_field) {
    row value;
    value.key = bytes_of("row1");
    cell name;
    name.family = "cf";
    name.qualifier = "name";
    name.value = bytes_of("ada");
    name.timestamp = 1700000000000LL;
    value.cells.push_back(name);

    const std::string json =
        crpc::db::hbase::encode_cell_set(crpc::span<const row>(&value, 1)).value();
    // "row1" -> cm93MQ==, "cf:name" -> Y2Y6bmFtZQ==, "ada" -> YWRh
    CRPC_CHECK(json.find("\"key\":\"cm93MQ==\"") != std::string::npos);
    CRPC_CHECK(json.find("\"column\":\"Y2Y6bmFtZQ==\"") != std::string::npos);
    CRPC_CHECK(json.find("\"$\":\"YWRh\"") != std::string::npos);
    CRPC_CHECK(json.find("\"timestamp\":1700000000000") != std::string::npos);
    // A family with no qualifier is encoded as just the family.
    row bare;
    bare.key = bytes_of("r2");
    cell family_only;
    family_only.family = "cf";
    family_only.value = bytes_of("v");
    bare.cells.push_back(family_only);
    const std::string bare_json =
        crpc::db::hbase::encode_cell_set(crpc::span<const row>(&bare, 1)).value();
    // "cf" -> Y2Y=
    CRPC_CHECK(bare_json.find("\"column\":\"Y2Y=\"") != std::string::npos);
}

CRPC_TEST(hbase_cell_set_round_trips) {
    row value;
    value.key = bytes_of("key with space");
    cell a;
    a.family = "cf";
    a.qualifier = "q1";
    a.value = bytes_of("first");
    a.timestamp = 42;
    cell b;
    b.family = "cf";
    b.qualifier = "q2";
    // A value that is not text at all: base64 is what makes it survive.
    b.value = {std::byte{0x00}, std::byte{0xFF}, std::byte{0x7F}};
    b.timestamp = 43;
    value.cells.push_back(a);
    value.cells.push_back(b);

    const std::string json =
        crpc::db::hbase::encode_cell_set(crpc::span<const row>(&value, 1)).value();
    const auto decoded = crpc::db::hbase::decode_cell_set(json);
    CRPC_CHECK(static_cast<bool>(decoded));
    if (!decoded) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{1}, decoded.value().size());
    if (decoded.value().empty()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("key with space"), text_of(decoded.value()[0].key));
    CRPC_CHECK_EQ(std::size_t{2}, decoded.value()[0].cells.size());
    if (decoded.value()[0].cells.size() == 2) {
        const cell& first = decoded.value()[0].cells[0];
        CRPC_CHECK_EQ(std::string("cf"), first.family);
        CRPC_CHECK_EQ(std::string("q1"), first.qualifier);
        CRPC_CHECK_EQ(std::string("first"), text_of(first.value));
        CRPC_CHECK_EQ(std::int64_t{42}, first.timestamp);
        const cell& second = decoded.value()[0].cells[1];
        CRPC_CHECK_EQ(std::size_t{3}, second.value.size());
        CRPC_CHECK_EQ(0, static_cast<int>(second.value[0]));
        CRPC_CHECK_EQ(0xFF, static_cast<int>(second.value[1]));
    }
}

CRPC_TEST(hbase_cell_set_decodes_what_the_gateway_sends) {
    // The exact shape a real gateway answered, with `timestamp` before `$` and the members in
    // that order.
    const std::string json =
        "{\"Row\":[{\"key\":\"cjE=\",\"Cell\":[{\"column\":\"Y2Y6bmFtZQ==\",\"timestamp\":"
        "1793139600396,\"$\":\"YWRh\"}]}]}";
    const auto decoded = crpc::db::hbase::decode_cell_set(json);
    CRPC_CHECK(static_cast<bool>(decoded));
    if (!decoded || decoded.value().empty()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("r1"), text_of(decoded.value()[0].key));
    CRPC_CHECK_EQ(std::size_t{1}, decoded.value()[0].cells.size());
    if (!decoded.value()[0].cells.empty()) {
        CRPC_CHECK_EQ(std::string("cf"), decoded.value()[0].cells[0].family);
        CRPC_CHECK_EQ(std::string("name"), decoded.value()[0].cells[0].qualifier);
        CRPC_CHECK_EQ(std::string("ada"), text_of(decoded.value()[0].cells[0].value));
    }
}

CRPC_TEST(hbase_cell_set_refuses_a_malformed_reply) {
    // A row without a key cannot be attributed to anything, so it is refused rather than
    // skipped: skipping would hide a reply this client does not understand.
    CRPC_CHECK_FALSE(static_cast<bool>(
        crpc::db::hbase::decode_cell_set("{\"Row\":[{\"Cell\":[]}]}")));
    // Truncated JSON.
    CRPC_CHECK_FALSE(static_cast<bool>(
        crpc::db::hbase::decode_cell_set("{\"Row\":[{\"key\":\"cjE=\"")));
    // Not an object at all.
    CRPC_CHECK_FALSE(static_cast<bool>(crpc::db::hbase::decode_cell_set("[]")));
    // Trailing bytes after a complete object are a decode error, not extra data.
    CRPC_CHECK_FALSE(static_cast<bool>(
        crpc::db::hbase::decode_cell_set("{\"Row\":[]} trailing")));
    // An empty CellSet is legal: a scan with no matches.
    const auto empty = crpc::db::hbase::decode_cell_set("{\"Row\":[]}");
    CRPC_CHECK(static_cast<bool>(empty));
    if (empty) {
        CRPC_CHECK(empty.value().empty());
    }
}

CRPC_TEST(hbase_cell_set_ignores_members_it_does_not_model) {
    // A newer gateway may add fields; an unknown member must not break the reply.
    const std::string json =
        "{\"Row\":[{\"key\":\"cjE=\",\"extra\":{\"a\":[1,2]},\"Cell\":[{\"column\":"
        "\"Y2Y6bmFtZQ==\",\"$\":\"YWRh\",\"future\":true}]}]}";
    const auto decoded = crpc::db::hbase::decode_cell_set(json);
    CRPC_CHECK(static_cast<bool>(decoded));
    if (decoded && !decoded.value().empty()) {
        CRPC_CHECK_EQ(std::string("r1"), text_of(decoded.value()[0].key));
        CRPC_CHECK_EQ(std::size_t{1}, decoded.value()[0].cells.size());
    }
}

CRPC_TEST(hbase_path_segment_escapes_everything_but_unreserved) {
    // A row key is binary, so a slash must not survive: leaving it would address a different
    // resource.
    CRPC_CHECK_EQ(std::string("row1"),
                  crpc::db::hbase::encode_path_segment(view_of(bytes_of("row1"))));
    CRPC_CHECK_EQ(std::string("a%2Fb"),
                  crpc::db::hbase::encode_path_segment(view_of(bytes_of("a/b"))));
    CRPC_CHECK_EQ(std::string("a%3Fb"),
                  crpc::db::hbase::encode_path_segment(view_of(bytes_of("a?b"))));
    CRPC_CHECK_EQ(std::string("a%20b"),
                  crpc::db::hbase::encode_path_segment(view_of(bytes_of("a b"))));
    CRPC_CHECK_EQ(std::string("%00%FF"),
                  crpc::db::hbase::encode_path_segment(view_of(
                      std::vector<std::byte>{std::byte{0x00}, std::byte{0xFF}})));
    // The unreserved set survives, which is what keeps ordinary keys readable in a log.
    CRPC_CHECK_EQ(std::string("A-z_0.9~"),
                  crpc::db::hbase::encode_path_segment(view_of(bytes_of("A-z_0.9~"))));
}

// The gateway's ScannerModel takes `filter` as a *string* whose contents are themselves a
// JSON document (see the probe recorded in rest.cpp). These cases pin the exact bytes, because
// a nested object or an unescaped string is refused by the server with a bare 400/500.
CRPC_TEST(hbase_scan_filter_encodes_the_gateway_string_form) {
    using crpc::db::hbase::compare_operator;
    using crpc::db::hbase::scan_filter;

    const auto row = scan_filter::row_prefix(view_of(bytes_of("f03")));
    const auto row_json = crpc::db::hbase::encode_scan_filter(row);
    CRPC_CHECK(static_cast<bool>(row_json));
    if (row_json) {
        CRPC_CHECK_EQ(std::string("{\"type\":\"PrefixFilter\",\"value\":\"ZjAz\"}"),
                      row_json.value());
    }

    const auto column = scan_filter::column_prefix("name");
    const auto column_json = crpc::db::hbase::encode_scan_filter(column);
    CRPC_CHECK(static_cast<bool>(column_json));
    if (column_json) {
        CRPC_CHECK_EQ(std::string("{\"type\":\"ColumnPrefixFilter\",\"value\":\"bmFtZQ==\"}"),
                      column_json.value());
    }

    // The field names are `op` and `ifMissing`, not `compareOp`/`filterIfMissing`: the
    // gateway answers 400 for the latter, which was the first shape tried against it.
    const auto value = scan_filter::column_value("cf", "n", compare_operator::greater_or_equal,
                                                 view_of(bytes_of("7")), false);
    const auto value_json = crpc::db::hbase::encode_scan_filter(value);
    CRPC_CHECK(static_cast<bool>(value_json));
    if (value_json) {
        CRPC_CHECK_EQ(
            std::string("{\"type\":\"SingleColumnValueFilter\",\"family\":\"Y2Y=\","
                        "\"qualifier\":\"bg==\",\"op\":\"GREATER_OR_EQUAL\","
                        "\"comparator\":{\"type\":\"BinaryComparator\",\"value\":\"Nw==\"},"
                        "\"ifMissing\":false}"),
            value_json.value());
    }

    // A family is mandatory for a column-value filter; an empty one would silently compare
    // against every family the server knows.
    const auto no_family = scan_filter::column_value("", "n", compare_operator::equal,
                                                     view_of(bytes_of("7")));
    CRPC_CHECK_FALSE(static_cast<bool>(crpc::db::hbase::encode_scan_filter(no_family)));
}

CRPC_TEST(hbase_scanner_request_carries_rows_filter_and_options) {
    using crpc::db::hbase::compare_operator;
    using crpc::db::hbase::scan_filter;
    using crpc::db::hbase::scan_options;

    scan_options options;
    options.start_row = bytes_of("k02");
    options.end_row = bytes_of("k05");
    options.limit = 7;
    options.batch = 3;
    options.max_versions = 2;
    options.where = scan_filter::row_prefix(view_of(bytes_of("f03")));

    const auto body = crpc::db::hbase::encode_scanner_request(options);
    CRPC_CHECK(static_cast<bool>(body));
    if (body) {
        CRPC_CHECK_EQ(
            std::string("{\"batch\":3,\"startRow\":\"azAy\",\"endRow\":\"azA1\","
                        "\"maxVersions\":2,\"filter\":\"{\\\"type\\\":\\\"PrefixFilter\\\","
                        "\\\"value\\\":\\\"ZjAz\\\"}\"}"),
            body.value());
    }

    // With no filter the member is absent, not null and not an empty string: the gateway
    // parses an empty string as a filter document and refuses it.
    scan_options plain;
    plain.limit = 5;
    const auto plain_body = crpc::db::hbase::encode_scanner_request(plain);
    CRPC_CHECK(static_cast<bool>(plain_body));
    if (plain_body) {
        CRPC_CHECK_EQ(std::string("{\"batch\":5}"), plain_body.value());
        CRPC_CHECK(plain_body.value().find("filter") == std::string::npos);
    }

    // Batch defaults preserve the behaviour of the old `scan(start, limit)`: the limit is the
    // batch when it is set, and a plain scan still asks for a bounded page.
    scan_options unlimited;
    const auto unlimited_body = crpc::db::hbase::encode_scanner_request(unlimited);
    CRPC_CHECK(static_cast<bool>(unlimited_body));
    if (unlimited_body) {
        CRPC_CHECK_EQ(std::string("{\"batch\":1000}"), unlimited_body.value());
    }
    scan_options explicit_batch;
    explicit_batch.limit = 9;
    explicit_batch.batch = 2;
    const auto explicit_body = crpc::db::hbase::encode_scanner_request(explicit_batch);
    CRPC_CHECK(static_cast<bool>(explicit_body));
    if (explicit_body) {
        CRPC_CHECK_EQ(std::string("{\"batch\":2}"), explicit_body.value());
    }
}

CRPC_TEST(hbase_http_status_maps_to_library_codes) {
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(crpc::db::hbase::map_http_status(200)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(crpc::db::hbase::map_http_status(201)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(crpc::db::hbase::map_http_status(404)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(crpc::db::hbase::map_http_status(400)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(crpc::db::hbase::map_http_status(403)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::overload),
                  static_cast<int>(crpc::db::hbase::map_http_status(429)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(crpc::db::hbase::map_http_status(503)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                  static_cast<int>(crpc::db::hbase::map_http_status(504)));
}

CRPC_TEST_MAIN()