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

CRPC_TEST(mongo_aggregate_command_carries_the_pipeline_and_a_cursor) {
    crpc::db::mongo::aggregate_request request;
    request.database = "app";
    request.collection = "orders";

    bson_document match_body;
    match_body.append("state", bson_value::make_text("open"));
    bson_document match_stage;
    match_stage.append("$match", bson_value::make_document(match_body));
    request.pipeline.push_back(std::move(match_stage));

    bson_document group_body;
    group_body.append("_id", bson_value::make_text("$state"));
    bson_document count_body;
    count_body.append("$sum", bson_value::make_int32(1));
    group_body.append("n", bson_value::make_document(count_body));
    bson_document group_stage;
    group_stage.append("$group", bson_value::make_document(group_body));
    request.pipeline.push_back(std::move(group_stage));

    request.batch_size = 2;

    const bson_document command = crpc::db::mongo::make_aggregate(request);
    // The command name is the first field and every command carries its database last.
    CRPC_CHECK_EQ(std::string("aggregate"), command.fields().front().first);
    CRPC_CHECK_EQ(std::string("$db"), command.fields().back().first);
    std::string collection;
    CRPC_CHECK(command.get_text("aggregate", collection));
    CRPC_CHECK_EQ(std::string("orders"), collection);

    const crpc::db::mongo::bson_value* pipeline = command.find("pipeline");
    CRPC_CHECK(pipeline != nullptr);
    if (pipeline != nullptr) {
        // Stages are an ordered array: the server applies them in exactly this order.
        CRPC_CHECK_EQ(static_cast<int>(crpc::db::mongo::bson_type::array),
                      static_cast<int>(pipeline->type));
        CRPC_CHECK_EQ(std::size_t{2}, pipeline->array.size());
        if (pipeline->array.size() == 2) {
            const bson_document* first = pipeline->array[0].document.get();
            CRPC_CHECK(first != nullptr);
            if (first != nullptr) {
                CRPC_CHECK(first->find("$match") != nullptr);
            }
            const bson_document* second = pipeline->array[1].document.get();
            CRPC_CHECK(second != nullptr);
            if (second != nullptr) {
                CRPC_CHECK(second->find("$group") != nullptr);
            }
        }
    }

    // `aggregate` requires a `cursor` sub-document even when the caller leaves the batch
    // size to the server; `batchSize` is what tells the server to hand back a small batch.
    const bson_document* cursor = command.get_document("cursor");
    CRPC_CHECK(cursor != nullptr);
    if (cursor != nullptr) {
        std::int64_t batch_size = 0;
        CRPC_CHECK(cursor->get_int64("batchSize", batch_size));
        CRPC_CHECK_EQ(std::int64_t{2}, batch_size);
    }

    std::string database;
    CRPC_CHECK(command.get_text("$db", database));
    CRPC_CHECK_EQ(std::string("app"), database);
}

CRPC_TEST(mongo_aggregate_cursor_is_present_without_a_batch_size) {
    crpc::db::mongo::aggregate_request request;
    request.database = "app";
    request.collection = "orders";
    bson_document stage;
    stage.append("$limit", bson_value::make_int32(1));
    request.pipeline.push_back(std::move(stage));

    const bson_document command = crpc::db::mongo::make_aggregate(request);
    // The server refuses an aggregate without `cursor`, so an empty one is sent rather
    // than omitted; leaving `batchSize` out is how the server default applies.
    const bson_document* cursor = command.get_document("cursor");
    CRPC_CHECK(cursor != nullptr);
    if (cursor != nullptr) {
        CRPC_CHECK_EQ(std::size_t{0}, cursor->size());
        CRPC_CHECK(cursor->find("batchSize") == nullptr);
    }
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

// --- Distributed (replica set) support --------------------------------------
//
// The wire names below are the strings MongoDB itself accepts. A driver that sends
// "secondarypreferred" or "primary_preferred" is silently treated as owning a read
// preference the server does not know, so the exact spelling is part of the contract.

CRPC_TEST(mongo_read_preference_modes_have_the_wire_names) {
    using crpc::db::mongo::read_preference_mode;
    using crpc::db::mongo::to_string;
    CRPC_CHECK_EQ(std::string("primary"), to_string(read_preference_mode::primary));
    CRPC_CHECK_EQ(std::string("primaryPreferred"),
                  to_string(read_preference_mode::primary_preferred));
    CRPC_CHECK_EQ(std::string("secondary"), to_string(read_preference_mode::secondary));
    CRPC_CHECK_EQ(std::string("secondaryPreferred"),
                  to_string(read_preference_mode::secondary_preferred));
    CRPC_CHECK_EQ(std::string("nearest"), to_string(read_preference_mode::nearest));

    // The command field is always {mode: "..."} and nothing else, which is what the
    // server's $readPreference parser requires.
    for (const read_preference_mode mode :
         {read_preference_mode::primary, read_preference_mode::primary_preferred,
          read_preference_mode::secondary, read_preference_mode::secondary_preferred,
          read_preference_mode::nearest}) {
        const bson_document document = crpc::db::mongo::make_read_preference_document(mode);
        CRPC_CHECK_EQ(std::size_t{1}, document.size());
        std::string name;
        CRPC_CHECK(document.get_text("mode", name));
        CRPC_CHECK_EQ(std::string(to_string(mode)), name);
    }
}

CRPC_TEST(mongo_read_concern_levels_have_the_wire_names) {
    using crpc::db::mongo::read_concern_level;
    using crpc::db::mongo::to_string;
    CRPC_CHECK_EQ(std::string("local"), to_string(read_concern_level::local));
    CRPC_CHECK_EQ(std::string("majority"), to_string(read_concern_level::majority));
    CRPC_CHECK_EQ(std::string("snapshot"), to_string(read_concern_level::snapshot));
    CRPC_CHECK_EQ(std::string("linearizable"), to_string(read_concern_level::linearizable));

    for (const read_concern_level level :
         {read_concern_level::local, read_concern_level::majority,
          read_concern_level::snapshot, read_concern_level::linearizable}) {
        const bson_document document = crpc::db::mongo::make_read_concern_document(level);
        CRPC_CHECK_EQ(std::size_t{1}, document.size());
        std::string name;
        CRPC_CHECK(document.get_text("level", name));
        CRPC_CHECK_EQ(std::string(to_string(level)), name);
    }
}

CRPC_TEST(mongo_write_concern_document_encodes_w_j_and_wtimeout) {
    using crpc::db::mongo::make_write_concern_document;
    using crpc::db::mongo::write_concern_options;

    // An unconfigured concern sends no field at all, so the server's default applies.
    write_concern_options unset;
    CRPC_CHECK_FALSE(unset.configured());
    CRPC_CHECK(make_write_concern_document(unset).empty());

    write_concern_options majority = write_concern_options::majority();
    majority.journal = true;
    majority.wtimeout_ms = 1500;
    CRPC_CHECK(majority.configured());
    const bson_document document = make_write_concern_document(majority);
    std::string w;
    CRPC_CHECK(document.get_text("w", w));
    CRPC_CHECK_EQ(std::string("majority"), w);
    bool journal = false;
    CRPC_CHECK(document.get_boolean("j", journal));
    CRPC_CHECK(journal);
    std::int64_t wtimeout = 0;
    CRPC_CHECK(document.get_int64("wtimeout", wtimeout));
    CRPC_CHECK_EQ(std::int64_t{1500}, wtimeout);

    // A numeric w is an int32 and must not be spelled as a string.
    const bson_document numeric =
        make_write_concern_document(write_concern_options::count(2));
    std::int64_t nodes = 0;
    CRPC_CHECK(numeric.get_int64("w", nodes));
    CRPC_CHECK_EQ(std::int64_t{2}, nodes);
    const crpc::db::mongo::bson_value* numeric_w = numeric.find("w");
    CRPC_CHECK(numeric_w != nullptr);
    if (numeric_w != nullptr) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::db::mongo::bson_type::int32),
                      static_cast<int>(numeric_w->type));
    }
    CRPC_CHECK(numeric.find("j") == nullptr);
    CRPC_CHECK(numeric.find("wtimeout") == nullptr);
}

// Builds a document value from a list of strings, the way hello reports hosts.
[[nodiscard]] bson_value text_array(std::initializer_list<const char*> values) {
    std::vector<bson_value> items;
    for (const char* value : values) {
        items.push_back(bson_value::make_text(value));
    }
    return bson_value::make_array(std::move(items));
}

CRPC_TEST(mongo_topology_reads_a_secondaries_hello) {
    bson_document hello;
    hello.append("ok", bson_value::make_real(1.0));
    hello.append("setName", bson_value::make_text("rs0"));
    hello.append("isWritablePrimary", bson_value::make_boolean(false));
    hello.append("secondary", bson_value::make_boolean(true));
    hello.append("me", bson_value::make_text("b:27018"));
    hello.append("primary", bson_value::make_text("a:27018"));
    hello.append("hosts", text_array({"a:27018", "b:27018", "c:27018"}));

    crpc::db::mongo::replica_set_topology topology;
    const crpc::status parsed =
        crpc::db::mongo::parse_hello_topology(hello, topology);
    CRPC_CHECK(static_cast<bool>(parsed));
    CRPC_CHECK(topology.replica_set);
    CRPC_CHECK_EQ(std::string("rs0"), topology.set_name);
    CRPC_CHECK_FALSE(topology.writable_primary);
    CRPC_CHECK(topology.secondary);
    CRPC_CHECK_FALSE(topology.arbiter);
    CRPC_CHECK_EQ(std::string("b:27018"), topology.me);
    CRPC_CHECK_EQ(std::string("a:27018"), topology.primary);
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mongo::topology_role::secondary),
                  static_cast<int>(topology.self_role()));
    CRPC_CHECK_EQ(std::size_t{3}, topology.members.size());
    // The member named in `primary` is the primary; the other hosts in `hosts` are
    // data-bearing secondaries (hosts never lists arbiters).
    for (const crpc::db::mongo::topology_member& member : topology.members) {
        const crpc::db::mongo::topology_role expected =
            member.host == "a:27018" ? crpc::db::mongo::topology_role::primary
                                     : crpc::db::mongo::topology_role::secondary;
        CRPC_CHECK_EQ(static_cast<int>(expected), static_cast<int>(member.role));
    }
}

CRPC_TEST(mongo_topology_reads_a_primary_hello_with_an_arbiter) {
    bson_document hello;
    hello.append("setName", bson_value::make_text("rs1"));
    hello.append("isWritablePrimary", bson_value::make_boolean(true));
    hello.append("secondary", bson_value::make_boolean(false));
    hello.append("me", bson_value::make_text("a:1"));
    hello.append("primary", bson_value::make_text("a:1"));
    hello.append("hosts", text_array({"a:1", "b:1", "c:1"}));
    hello.append("arbiters", text_array({"d:1"}));

    crpc::db::mongo::replica_set_topology topology;
    CRPC_CHECK(static_cast<bool>(crpc::db::mongo::parse_hello_topology(hello, topology)));
    CRPC_CHECK(topology.replica_set);
    CRPC_CHECK(topology.writable_primary);
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mongo::topology_role::primary),
                  static_cast<int>(topology.self_role()));
    CRPC_CHECK_EQ(std::size_t{4}, topology.members.size());
    bool saw_arbiter = false;
    for (const crpc::db::mongo::topology_member& member : topology.members) {
        if (member.host == "d:1") {
            saw_arbiter = true;
            CRPC_CHECK_EQ(static_cast<int>(crpc::db::mongo::topology_role::arbiter),
                          static_cast<int>(member.role));
        }
    }
    CRPC_CHECK(saw_arbiter);
}

CRPC_TEST(mongo_topology_recognises_an_arbiter_self) {
    bson_document hello;
    hello.append("setName", bson_value::make_text("rs0"));
    hello.append("arbiterOnly", bson_value::make_boolean(true));
    hello.append("secondary", bson_value::make_boolean(false));
    hello.append("hosts", text_array({"a:1"}));
    hello.append("arbiters", text_array({"d:1"}));

    crpc::db::mongo::replica_set_topology topology;
    CRPC_CHECK(static_cast<bool>(crpc::db::mongo::parse_hello_topology(hello, topology)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mongo::topology_role::arbiter),
                  static_cast<int>(topology.self_role()));
}

CRPC_TEST(mongo_topology_on_a_standalone_is_not_a_replica_set) {
    // The standalone hello has no setName and no hosts; treating it as a one-node set
    // would make the client send txnNumber, which a standalone refuses outright.
    bson_document hello;
    hello.append("ok", bson_value::make_real(1.0));
    hello.append("isWritablePrimary", bson_value::make_boolean(true));
    hello.append("maxWireVersion", bson_value::make_int32(21));

    crpc::db::mongo::replica_set_topology topology;
    CRPC_CHECK(static_cast<bool>(crpc::db::mongo::parse_hello_topology(hello, topology)));
    CRPC_CHECK_FALSE(topology.replica_set);
    CRPC_CHECK(topology.set_name.empty());
    CRPC_CHECK(topology.writable_primary);
    CRPC_CHECK(topology.members.empty());
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mongo::topology_role::primary),
                  static_cast<int>(topology.self_role()));
}

CRPC_TEST(mongo_end_sessions_carries_uuid_binaries) {
    // The server refuses a session id that is not binary subtype 4:
    //   "BSON field 'endSessions.endSessionsFromClient.id' is the wrong binData
    //    type 'general', expected type 'UUID'".
    const std::vector<std::byte> id(16, std::byte{0x5A});
    const bson_document command = crpc::db::mongo::make_end_sessions({id});
    CRPC_CHECK_EQ(std::string("endSessions"), command.fields().front().first);
    const crpc::db::mongo::bson_value* sessions = command.find("endSessions");
    CRPC_CHECK(sessions != nullptr);
    if (sessions != nullptr) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::db::mongo::bson_type::array),
                      static_cast<int>(sessions->type));
        CRPC_CHECK_EQ(std::size_t{1}, sessions->array.size());
        if (sessions->array.size() == 1) {
            const bson_document* entry = sessions->array[0].document.get();
            CRPC_CHECK(entry != nullptr);
            if (entry != nullptr) {
                const crpc::db::mongo::bson_value* value = entry->find("id");
                CRPC_CHECK(value != nullptr);
                if (value != nullptr) {
                    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mongo::bson_type::binary),
                                  static_cast<int>(value->type));
                    CRPC_CHECK_EQ(static_cast<int>(4), static_cast<int>(value->subtype));
                    CRPC_CHECK_EQ(std::size_t{16}, value->bytes.size());
                }
            }
        }
    }
    std::string database;
    CRPC_CHECK(command.get_text("$db", database));
    CRPC_CHECK_EQ(std::string("admin"), database);
}

CRPC_TEST_MAIN()