// CRPC -- MongoDB OP_MSG framing tests.
//
// The header layout and the section framing are checked byte for byte against the
// specification, because an off-by-four in `messageLength` produces a message the server
// rejects with no useful explanation.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <crpc/db/mongo/bson.hpp>
#include <crpc/db/mongo/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::mongo::bson_document;
using crpc::db::mongo::bson_value;
using crpc::db::mongo::message_header;

using bytes = std::vector<std::byte>;

[[nodiscard]] crpc::span<const std::byte> view(const bytes& data) noexcept {
    return crpc::span<const std::byte>(data.data(), data.size());
}

[[nodiscard]] std::string hex(const bytes& data) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::byte byte : data) {
        const auto value = static_cast<unsigned>(byte);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 0x0fu]);
    }
    return out;
}

[[nodiscard]] std::int32_t read_i32(const bytes& data, std::size_t at) {
    std::uint32_t bits = 0;
    for (int i = 0; i < 4; ++i) {
        bits |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[at + static_cast<std::size_t>(i)]))
                << (8 * i);
    }
    return static_cast<std::int32_t>(bits);
}

} // namespace

CRPC_TEST(mongo_header_is_little_endian_and_sixteen_bytes) {
    bytes out;
    message_header header;
    header.message_length = 0x01020304;
    header.request_id = 7;
    header.response_to = 9;
    header.code = 2013;
    CRPC_CHECK(static_cast<bool>(crpc::db::mongo::encode_message_header(out, header)));
    CRPC_CHECK_EQ(std::size_t{16}, out.size());
    CRPC_CHECK_EQ(std::string("04030201" "07000000" "09000000" "dd070000"), hex(out));

    message_header decoded;
    CRPC_CHECK(static_cast<bool>(crpc::db::mongo::decode_message_header(view(out), decoded)));
    CRPC_CHECK_EQ(0x01020304, decoded.message_length);
    CRPC_CHECK_EQ(7, decoded.request_id);
    CRPC_CHECK_EQ(9, decoded.response_to);
    CRPC_CHECK(decoded.is_op_msg());
}

CRPC_TEST(mongo_op_msg_wraps_one_kind_zero_section) {
    bson_document command;
    command.append("ping", bson_value::make_int32(1));
    const bytes message = crpc::db::mongo::encode_op_msg(command, 42).value();
    // Header 16 + flags 4 + kind 1 + document 15 = 36 = 0x24
    CRPC_CHECK_EQ(std::size_t{36}, message.size());
    CRPC_CHECK_EQ(36, read_i32(message, 0));
    CRPC_CHECK_EQ(42, read_i32(message, 4));
    CRPC_CHECK_EQ(0, read_i32(message, 8));
    CRPC_CHECK_EQ(2013, read_i32(message, 12));
    CRPC_CHECK_EQ(0, read_i32(message, 16)); // flagBits
    CRPC_CHECK_EQ(0, static_cast<int>(message[20])); // section kind 0
    // The command document begins at offset 21 and is 15 bytes.
    CRPC_CHECK_EQ(15, read_i32(message, 21));
    CRPC_CHECK_EQ(std::string("0f000000") + "10" + "70696e6700" + "01000000" + "00",
                  hex(bytes(message.begin() + 21, message.end())));
}

CRPC_TEST(mongo_op_msg_round_trips_through_the_decoder) {
    bson_document command;
    command.append("find", bson_value::make_text("people"));
    bson_document filter;
    filter.append("age", bson_value::make_int32(30));
    command.append("filter", bson_value::make_document(filter));
    command.append("$db", bson_value::make_text("test"));

    const bytes message = crpc::db::mongo::encode_op_msg(command, 1).value();
    crpc::db::mongo::op_msg_reply reply;
    const crpc::status decoded = crpc::db::mongo::decode_op_msg(view(message), reply);
    CRPC_CHECK(static_cast<bool>(decoded));
    CRPC_CHECK_EQ(1, reply.header.request_id);
    CRPC_CHECK_FALSE(reply.more_to_come());
    std::string name;
    CRPC_CHECK(reply.body.get_text("find", name));
    CRPC_CHECK_EQ(std::string("people"), name);
    const bson_document* inner = reply.body.get_document("filter");
    CRPC_CHECK(inner != nullptr);
    if (inner != nullptr) {
        std::int64_t age = 0;
        CRPC_CHECK(inner->get_int64("age", age));
        CRPC_CHECK_EQ(std::int64_t{30}, age);
    }
}

CRPC_TEST(mongo_op_msg_reports_more_to_come) {
    bson_document body;
    body.append("ok", bson_value::make_real(1.0));
    const bytes message =
        crpc::db::mongo::encode_op_msg(body, 3,
                                      static_cast<std::uint32_t>(
                                          crpc::db::mongo::message_flag::more_to_come))
            .value();
    crpc::db::mongo::op_msg_reply reply;
    CRPC_CHECK(static_cast<bool>(crpc::db::mongo::decode_op_msg(view(message), reply)));
    // A client that ignores this bit waits for a reply that will never arrive.
    CRPC_CHECK(reply.more_to_come());
}

CRPC_TEST(mongo_op_msg_refuses_an_unknown_section) {
    bytes message = crpc::db::mongo::encode_op_msg([] {
                        bson_document body;
                        body.append("ok", bson_value::make_int32(1));
                        return body;
                    }(),
                    1)
                        .value();
    // Change the section kind from 0 to an unmodelled value. Its length is unknown, so
    // the decoder must refuse rather than guess where the next section starts.
    message[20] = std::byte{0x7F};
    crpc::db::mongo::op_msg_reply reply;
    const crpc::status decoded = crpc::db::mongo::decode_op_msg(view(message), reply);
    CRPC_CHECK_FALSE(static_cast<bool>(decoded));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(decoded.error().code));
}

CRPC_TEST(mongo_op_msg_refuses_a_truncated_message) {
    bson_document body;
    body.append("ok", bson_value::make_int32(1));
    const bytes message = crpc::db::mongo::encode_op_msg(body, 1).value();
    // The declared length is larger than what arrived: a framing error, not a short read.
    bytes lying = message;
    lying[0] = static_cast<std::byte>(static_cast<int>(lying[0]) + 8);
    crpc::db::mongo::op_msg_reply reply;
    CRPC_CHECK_FALSE(static_cast<bool>(crpc::db::mongo::decode_op_msg(view(lying), reply)));
}

CRPC_TEST(mongo_find_command_puts_its_name_first) {
    crpc::db::mongo::find_request request;
    request.database = "app";
    request.collection = "orders";
    request.filter.append("state", bson_value::make_text("open"));
    request.limit = 10;
    request.skip = 5;
    request.batch_size = 2;
    const bson_document command = crpc::db::mongo::make_find(request);
    // The server reads the command name from the first field, so its position is part of
    // the contract.
    CRPC_CHECK_EQ(std::string("find"), command.fields().front().first);
    CRPC_CHECK_EQ(std::string("$db"), command.fields().back().first);
    std::string collection;
    CRPC_CHECK(command.get_text("find", collection));
    CRPC_CHECK_EQ(std::string("orders"), collection);
    std::int64_t limit = 0;
    CRPC_CHECK(command.get_int64("limit", limit));
    CRPC_CHECK_EQ(std::int64_t{10}, limit);
    CRPC_CHECK(command.get_document("filter") != nullptr);
}

CRPC_TEST(mongo_insert_command_carries_ordered_documents) {
    bson_document first;
    first.append("_id", bson_value::make_int32(1));
    bson_document second;
    second.append("_id", bson_value::make_int32(2));
    const bson_document command =
        crpc::db::mongo::make_insert("people", {first, second}, false);
    bool ordered = true;
    CRPC_CHECK(command.get_boolean("ordered", ordered));
    CRPC_CHECK_FALSE(ordered);
    const crpc::db::mongo::bson_value* documents = command.find("documents");
    CRPC_CHECK(documents != nullptr);
    if (documents != nullptr) {
        CRPC_CHECK_EQ(std::size_t{2}, documents->array.size());
        std::int64_t id = 0;
        CRPC_CHECK(documents->array[0].document->get_int64("_id", id));
        CRPC_CHECK_EQ(std::int64_t{1}, id);
        CRPC_CHECK(documents->array[1].document->get_int64("_id", id));
        CRPC_CHECK_EQ(std::int64_t{2}, id);
    }
}

CRPC_TEST(mongo_hello_names_the_client_and_the_database) {
    const bson_document hello = crpc::db::mongo::make_hello("crpc-test");
    std::int64_t flag = 0;
    CRPC_CHECK(hello.get_int64("hello", flag));
    CRPC_CHECK_EQ(std::int64_t{1}, flag);
    // The client metadata document has a fixed shape: `driver` and `os` are documents, and
    // the server refuses a string in their place ("The 'driver' field is required to be a
    // BSON document in the client metadata document").
    const bson_document* client = hello.get_document("client");
    CRPC_CHECK(client != nullptr);
    if (client != nullptr) {
        const bson_document* driver = client->get_document("driver");
        CRPC_CHECK(driver != nullptr);
        if (driver != nullptr) {
            std::string name;
            CRPC_CHECK(driver->get_text("name", name));
            CRPC_CHECK_EQ(std::string("crpc"), name);
            std::string version;
            CRPC_CHECK(driver->get_text("version", version));
            CRPC_CHECK_FALSE(version.empty());
        }
        // `os` is required too, with `type` mandatory.
        const bson_document* os = client->get_document("os");
        CRPC_CHECK(os != nullptr);
        if (os != nullptr) {
            std::string type;
            CRPC_CHECK(os->get_text("type", type));
            CRPC_CHECK_FALSE(type.empty());
        }
        const bson_document* application = client->get_document("application");
        CRPC_CHECK(application != nullptr);
        if (application != nullptr) {
            std::string name;
            CRPC_CHECK(application->get_text("name", name));
            CRPC_CHECK_EQ(std::string("crpc-test"), name);
        }
    }
    std::string database;
    CRPC_CHECK(hello.get_text("$db", database));
    CRPC_CHECK_EQ(std::string("admin"), database);
}

CRPC_TEST(mongo_reply_status_reads_ok_and_the_error_message) {
    bson_document good;
    good.append("ok", bson_value::make_real(1.0));
    CRPC_CHECK(crpc::db::mongo::reply_ok(good));

    bson_document bad;
    bad.append("ok", bson_value::make_real(0.0));
    bad.append("errmsg", bson_value::make_text("no such collection"));
    bad.append("code", bson_value::make_int32(26));
    CRPC_CHECK_FALSE(crpc::db::mongo::reply_ok(bad));
    CRPC_CHECK_EQ(std::string("no such collection"),
                  crpc::db::mongo::reply_error_message(bad));

    // An integer 1 is also success; the field is a number, not a boolean.
    bson_document int_ok;
    int_ok.append("ok", bson_value::make_int32(1));
    CRPC_CHECK(crpc::db::mongo::reply_ok(int_ok));

    // A reply with no `ok` is not a valid command reply.
    bson_document empty;
    CRPC_CHECK_FALSE(crpc::db::mongo::reply_ok(empty));
}

CRPC_TEST(mongo_error_codes_map_to_the_library_codes) {
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(crpc::db::mongo::map_mongo_error(11000)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(crpc::db::mongo::map_mongo_error(26)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                  static_cast<int>(crpc::db::mongo::map_mongo_error(50)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(crpc::db::mongo::map_mongo_error(2)));
}

CRPC_TEST_MAIN()