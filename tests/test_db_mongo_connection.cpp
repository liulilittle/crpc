// CRPC -- MongoDB connection tests against a scripted channel.
//
// These cases need no server: the channel answers hello with a canned replica set
// reply and every later command with a canned document. What they prove is the wire
// contract of the distributed features -- that the lsid is a binary subtype 4 UUID,
// that $readPreference/readConcern/writeConcern lands as top-level command fields,
// that a retryable write replays the same txnNumber, and that writeConcernError is
// surfaced instead of being swallowed.
//
// The replies are built with the same encoder the connection uses to send, which is
// safe: the reply is only examined by the connection's decoder, and the decoder is
// already covered by the framing tests.
#include "test_framework.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <crpc/db/channel.hpp>
#include <crpc/db/mongo/bson.hpp>
#include <crpc/db/mongo/connection.hpp>
#include <crpc/db/mongo/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::mongo::bson_document;
using crpc::db::mongo::bson_value;
using bytes = std::vector<std::byte>;

// OP_MSG messages are parsed with the library itself, so the helpers below only need
// the parts that get patched by hand.

[[nodiscard]] std::int32_t read_i32(const bytes& data, std::size_t at) {
    std::uint32_t bits = 0;
    for (int i = 0; i < 4; ++i) {
        bits |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[at + static_cast<std::size_t>(i)]))
                << (8 * i);
    }
    return static_cast<std::int32_t>(bits);
}

void write_i32(bytes& data, std::size_t at, std::int32_t value) {
    const std::uint32_t bits = static_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) {
        data[at + static_cast<std::size_t>(i)] =
            static_cast<std::byte>((bits >> (8 * i)) & 0xffu);
    }
}

// Wraps a reply body in OP_MSG and points `responseTo` at the request that asked.
[[nodiscard]] bytes make_reply(const bytes& request, bson_document body) {
    const std::int32_t request_id = read_i32(request, 4);
    crpc::result<bytes> encoded = crpc::db::mongo::encode_op_msg(body, 0);
    if (!encoded) {
        return {};
    }
    write_i32(encoded.value(), 8, request_id);
    return std::move(encoded.value());
}

[[nodiscard]] bool bytes_contain(const bytes& data, const char* needle) {
    const std::size_t needle_length = std::strlen(needle);
    if (data.size() < needle_length) {
        return false;
    }
    for (std::size_t i = 0; i + needle_length <= data.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle_length; ++j) {
            if (data[i + j] != static_cast<std::byte>(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

// A canned `hello` reply describing one role.
[[nodiscard]] bson_document hello_reply(bool replica_set, bool maybe_member = true) {
    bson_document hello;
    hello.append("ok", bson_value::make_real(1.0));
    hello.append("isWritablePrimary", bson_value::make_boolean(true));
    hello.append("secondary", bson_value::make_boolean(false));
    hello.append("maxWireVersion", bson_value::make_int32(21));
    hello.append("maxBsonObjectSize", bson_value::make_int32(16 * 1024 * 1024));
    if (replica_set) {
        hello.append("setName", bson_value::make_text("rs0"));
        hello.append("me", bson_value::make_text("127.0.0.1:1"));
        hello.append("primary", bson_value::make_text("127.0.0.1:1"));
        if (maybe_member) {
            hello.append("hosts", bson_value::make_array(
                                      {bson_value::make_text("127.0.0.1:1")}));
        }
    }
    return hello;
}

// How a scripted exchange answers.
struct scripted_answer {
    // The write fails outright (the message never reached the server).
    bool drop_write = false;
    // The write reached the server but the reply is lost (the server may have run it).
    bool drop_read = false;
    // The reply bytes served after the write, when neither drop is set.
    bytes reply_bytes;
};

class scripted_channel final : public crpc::db::channel {
public:
    using responder =
        std::function<scripted_answer(const bytes& request, std::size_t request_index)>;

    explicit scripted_channel(responder respond) : respond_(std::move(respond)) {}

    crpc::result<std::vector<std::byte>> read_exact(std::size_t count) override {
        if (!read_error_.ok()) {
            crpc::rpc_error error = read_error_.error();
            read_error_ = crpc::status{};
            return error;
        }
        bytes out;
        out.reserve(count);
        while (out.size() < count) {
            if (reply_offset_ + (count - out.size()) > reply_buffer_.size()) {
                return crpc::rpc_error{crpc::rpc_code::eof,
                                       "the scripted channel has no reply bytes left"};
            }
            const std::size_t take = count - out.size();
            out.insert(out.end(), reply_buffer_.begin() +
                                      static_cast<std::ptrdiff_t>(reply_offset_),
                       reply_buffer_.begin() +
                           static_cast<std::ptrdiff_t>(reply_offset_ + take));
            reply_offset_ += take;
        }
        return out;
    }

    crpc::status write(crpc::span<const std::byte> data) override {
        ++request_index_;
        bytes request(data.begin(), data.end());
        requests_.push_back(request);
        scripted_answer answer = respond_(request, request_index_);
        if (answer.drop_write) {
            return crpc::status{crpc::rpc_code::connection_reset,
                                "injected write failure"};
        }
        if (answer.drop_read) {
            read_error_ =
                crpc::status{crpc::rpc_code::connection_reset, "injected read failure"};
            return crpc::status{};
        }
        reply_buffer_.insert(reply_buffer_.end(), answer.reply_bytes.begin(),
                             answer.reply_bytes.end());
        return crpc::status{};
    }

    void close() noexcept override { closed_ = true; }

    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] const std::vector<bytes>& requests() const noexcept { return requests_; }

private:
    responder respond_;
    std::vector<bytes> requests_;
    std::size_t request_index_ = 0;
    bytes reply_buffer_;
    std::size_t reply_offset_ = 0;
    crpc::status read_error_;
    bool closed_ = false;
};

// Builds a connection whose scripted channel serves the given commands after hello.
[[nodiscard]] std::unique_ptr<crpc::db::mongo::connection> make_connection(
    crpc::db::mongo::connection_options options,
    std::function<scripted_answer(const bytes&, std::size_t)> serve,
    scripted_channel** out_channel = nullptr) {
    auto channel = std::make_unique<scripted_channel>(std::move(serve));
    if (out_channel != nullptr) {
        *out_channel = channel.get();
    }
    auto connection = std::make_unique<crpc::db::mongo::connection>(
        std::move(options), std::unique_ptr<crpc::db::channel>(std::move(channel)));
    return connection;
}

// The common responder: hello from a single-node replica set, then command replies.
[[nodiscard]] std::function<scripted_answer(const bytes&, std::size_t)> replica_set_server() {
    return [](const bytes& request, std::size_t) -> scripted_answer {
        if (bytes_contain(request, "hello\x00")) {
            return scripted_answer{false, false, make_reply(request, hello_reply(true))};
        }
        if (bytes_contain(request, "endSessions\x00")) {
            bson_document reply;
            reply.append("ok", bson_value::make_real(1.0));
            return scripted_answer{false, false, make_reply(request, std::move(reply))};
        }
        if (bytes_contain(request, "find\x00")) {
            bson_document cursor;
            cursor.append("id", bson_value::make_int64(0));
            cursor.append("ns", bson_value::make_text("crpc_it.c"));
            std::vector<bson_value> first_batch;
            cursor.append("firstBatch", bson_value::make_array(std::move(first_batch)));
            bson_document reply;
            reply.append("ok", bson_value::make_real(1.0));
            reply.append("cursor", bson_value::make_document(std::move(cursor)));
            return scripted_answer{false, false, make_reply(request, std::move(reply))};
        }
        if (bytes_contain(request, "insert\x00")) {
            bson_document reply;
            reply.append("ok", bson_value::make_real(1.0));
            reply.append("n", bson_value::make_int32(1));
            return scripted_answer{false, false, make_reply(request, std::move(reply))};
        }
        // Anything else (ping, drop, ...) answers success empty-handed.
        bson_document reply;
        reply.append("ok", bson_value::make_real(1.0));
        return scripted_answer{false, false, make_reply(request, std::move(reply))};
    };
}

// Decodes one request message and hands back its command document.
[[nodiscard]] crpc::result<bson_document> request_body(const bytes& request) {
    crpc::db::mongo::op_msg_reply parsed;
    const crpc::status decoded = crpc::db::mongo::decode_op_msg(
        crpc::span<const std::byte>(request.data(), request.size()), parsed);
    if (!decoded) {
        return decoded.error();
    }
    return parsed.body;
}

// True when the request document's first field (the command name) is `name`.
[[nodiscard]] bool command_name(const bson_document& document, std::string_view name) {
    if (document.size() != 0) {
        if (document.fields().front().first == name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_lsid(const bson_document& document, std::size_t expected_size) {
    const bson_document* lsid = document.get_document("lsid");
    if (lsid == nullptr) {
        return false;
    }
    const crpc::db::mongo::bson_value* id = lsid->find("id");
    if (id == nullptr || id->type != crpc::db::mongo::bson_type::binary) {
        return false;
    }
    if (static_cast<int>(id->subtype) != 4) {
        return false;
    }
    return id->bytes.size() == expected_size;
}

[[nodiscard]] bool lsid_matches(const bson_document& document,
                                const std::vector<std::byte>& id) {
    const bson_document* lsid = document.get_document("lsid");
    if (lsid == nullptr) {
        return false;
    }
    const crpc::db::mongo::bson_value* value = lsid->find("id");
    if (value == nullptr || value->type != crpc::db::mongo::bson_type::binary) {
        return false;
    }
    return value->bytes == id;
}

// The field a document wants, as a text leaf of an embedded document.
[[nodiscard]] std::string nested_text(const bson_document& document, std::string_view group,
                                      std::string_view key) {
    const bson_document* nested = document.get_document(group);
    std::string out;
    if (nested != nullptr) {
        (void)nested->get_text(key, out);
    }
    return out;
}

} // namespace

CRPC_TEST(mongo_connection_learns_replica_set_topology_from_hello) {
    scripted_channel* channel = nullptr;
    crpc::db::mongo::connection_options options;
    // No user: the SCRAM exchange is skipped, which keeps the scripted server tiny.
    options.user.clear();
    options.database = "crpc_it";
    auto connection =
        make_connection(options, replica_set_server(), &channel);
    CRPC_CHECK(connection.get() != nullptr);
    CRPC_CHECK(connection->is_open());
    CRPC_CHECK(connection->is_replica_set());
    CRPC_CHECK_EQ(std::string("rs0"), connection->topology().set_name);
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mongo::topology_role::primary),
                  static_cast<int>(connection->topology().self_role()));
    CRPC_CHECK_EQ(std::size_t{1}, connection->topology().members.size());
    if (!connection->topology().members.empty()) {
        CRPC_CHECK_EQ(std::string("127.0.0.1:1"), connection->topology().primary);
        CRPC_CHECK_EQ(std::string("127.0.0.1:1"), connection->topology().members[0].host);
        CRPC_CHECK_EQ(static_cast<int>(crpc::db::mongo::topology_role::primary),
                      static_cast<int>(connection->topology().members[0].role));
    }
}

CRPC_TEST(mongo_connection_without_a_replica_set_envoy_has_no_set_name) {
    scripted_channel* channel = nullptr;
    crpc::db::mongo::connection_options options;
    options.user.clear();
    auto serve = [](const bytes& request, std::size_t) -> scripted_answer {
        if (bytes_contain(request, "hello\x00")) {
            return scripted_answer{false, false, make_reply(request, hello_reply(false))};
        }
        bson_document reply;
        reply.append("ok", bson_value::make_real(1.0));
        return scripted_answer{false, false, make_reply(request, std::move(reply))};
    };
    auto connection = make_connection(options, serve, &channel);
    CRPC_CHECK(connection->is_open());
    CRPC_CHECK_FALSE(connection->is_replica_set());
    CRPC_CHECK(connection->topology().set_name.empty());
}

CRPC_TEST(mongo_session_is_a_uuid_on_every_command) {
    scripted_channel* channel = nullptr;
    crpc::db::mongo::connection_options options;
    options.user.clear();
    auto connection =
        make_connection(options, replica_set_server(), &channel);
    CRPC_CHECK(connection->is_open());

    // The session is created lazily with the first application command.
    CRPC_CHECK_FALSE(connection->has_session());
    crpc::db::mongo::find_request request;
    request.database = "crpc_it";
    request.collection = "c";
    const auto found = connection->find(request);
    CRPC_CHECK(static_cast<bool>(found));
    CRPC_CHECK(connection->has_session());
    CRPC_CHECK_EQ(std::size_t{16}, connection->session_id().size());

    // The second command reuses the same lsid.
    const auto again = connection->find(request);
    CRPC_CHECK(static_cast<bool>(again));
    CRPC_CHECK_EQ(channel->requests().size(), std::size_t{3}); // hello, find, find
    if (channel->requests().size() == 3) {
        const auto first = request_body(channel->requests()[1]);
        const auto second = request_body(channel->requests()[2]);
        CRPC_CHECK(static_cast<bool>(first));
        CRPC_CHECK(static_cast<bool>(second));
        if (first && second) {
            CRPC_CHECK(has_lsid(first.value(), 16));
            CRPC_CHECK(lsid_matches(first.value(), connection->session_id()));
            CRPC_CHECK(has_lsid(second.value(), 16));
            CRPC_CHECK(lsid_matches(second.value(), connection->session_id()));
        }
    }
}

CRPC_TEST(mongo_session_lifecycle_starts_sends_and_ends) {
    scripted_channel* channel = nullptr;
    crpc::db::mongo::connection_options options;
    options.user.clear();
    auto connection =
        make_connection(options, replica_set_server(), &channel);
    CRPC_CHECK(connection->is_open());

    crpc::db::mongo::find_request request;
    request.database = "crpc_it";
    request.collection = "c";
    CRPC_CHECK(static_cast<bool>(connection->find(request)));
    const std::vector<std::byte> first_session = connection->session_id();
    CRPC_CHECK_EQ(std::size_t{16}, first_session.size());

    // A fresh explicit session replaces the lazy one.
    const crpc::status restarted = connection->start_session();
    CRPC_CHECK(static_cast<bool>(restarted));
    CRPC_CHECK(connection->has_session());
    CRPC_CHECK(connection->session_id() != first_session);

    // endSessions reaches the server and clears the local id.
    CRPC_CHECK(static_cast<bool>(connection->end_session()));
    CRPC_CHECK_FALSE(connection->has_session());
    CRPC_CHECK(connection->session_id().empty());
    const bool saw_end_sessions =
        std::any_of(channel->requests().begin(), channel->requests().end(),
                    [](const bytes& request) {
                        return bytes_contain(request, "endSessions\x00");
                    });
    CRPC_CHECK(saw_end_sessions);

    // A command after endSession starts a brand new session id.
    CRPC_CHECK(static_cast<bool>(connection->find(request)));
    CRPC_CHECK(connection->has_session());
    CRPC_CHECK_FALSE(connection->session_id().empty());
}

CRPC_TEST(mongo_read_preference_and_concern_are_top_level_fields) {
    scripted_channel* channel = nullptr;
    crpc::db::mongo::connection_options options;
    options.user.clear();
    options.read_preference = crpc::db::mongo::read_preference_mode::secondary_preferred;
    options.read_concern = crpc::db::mongo::read_concern_level::majority;
    auto connection =
        make_connection(options, replica_set_server(), &channel);
    CRPC_CHECK(connection->is_open());

    crpc::db::mongo::find_request request;
    request.database = "crpc_it";
    request.collection = "c";
    CRPC_CHECK(static_cast<bool>(connection->find(request)));

    if (channel->requests().size() >= 2) {
        const auto body = request_body(channel->requests()[1]);
        CRPC_CHECK(static_cast<bool>(body));
        if (body) {
            // The read preference is a $readPreference field directly on the command
            // document, next to $db, not nested anywhere.
            CRPC_CHECK_EQ(std::string("secondaryPreferred"),
                          nested_text(body.value(), "$readPreference", "mode"));
            // readConcern is spelled without a dollar sign.
            CRPC_CHECK_EQ(std::string("majority"),
                          nested_text(body.value(), "readConcern", "level"));
            CRPC_CHECK(has_lsid(body.value(), 16));
            CRPC_CHECK(command_name(body.value(), "find"));
        }
    }
}

CRPC_TEST(mongo_write_concern_is_a_top_level_field_on_writes) {
    scripted_channel* channel = nullptr;
    crpc::db::mongo::connection_options options;
    options.user.clear();
    options.write_concern = crpc::db::mongo::write_concern_options::count(2);
    options.write_concern.wtimeout_ms = 1000;
    auto connection =
        make_connection(options, replica_set_server(), &channel);
    CRPC_CHECK(connection->is_open());

    crpc::db::mongo::bson_document document;
    document.append("_id", bson_value::make_int32(1));
    const auto inserted =
        connection->insert("c", std::vector<bson_document>{std::move(document)});
    CRPC_CHECK(static_cast<bool>(inserted));
    CRPC_CHECK_EQ(std::int64_t{1}, inserted.value());

    if (channel->requests().size() >= 2) {
        const auto body = request_body(channel->requests()[1]);
        CRPC_CHECK(static_cast<bool>(body));
        if (body) {
            const bson_document* concern = body.value().get_document("writeConcern");
            CRPC_CHECK(concern != nullptr);
            if (concern != nullptr) {
                std::int64_t w = 0;
                CRPC_CHECK(concern->get_int64("w", w));
                CRPC_CHECK_EQ(std::int64_t{2}, w);
                std::int64_t timeout = 0;
                CRPC_CHECK(concern->get_int64("wtimeout", timeout));
                CRPC_CHECK_EQ(std::int64_t{1000}, timeout);
            }
            CRPC_CHECK(has_lsid(body.value(), 16));
            CRPC_CHECK(command_name(body.value(), "insert"));
        }
    }
}

CRPC_TEST(mongo_retryable_write_replays_the_same_txn_number) {
    scripted_channel* first_channel = nullptr;
    scripted_channel* second_channel_raw = nullptr;
    bool injected = false;

    crpc::db::mongo::connection_options options;
    options.user.clear();
    const auto server = [&](const bytes& request, std::size_t) -> scripted_answer {
        if (bytes_contain(request, "hello\x00")) {
            return scripted_answer{false, false, make_reply(request, hello_reply(true))};
        }
        if (bytes_contain(request, "insert\x00")) {
            // The first insert dies on the wire after the bytes were accepted; the
            // retried insert succeeds. Both carry the same txnNumber, so the server
            // dedupes even if the first actually ran.
            if (!injected) {
                injected = true;
                return scripted_answer{false, true, {}};
            }
            bson_document reply;
            reply.append("ok", bson_value::make_real(1.0));
            reply.append("n", bson_value::make_int32(1));
            return scripted_answer{false, false, make_reply(request, std::move(reply))};
        }
        bson_document reply;
        reply.append("ok", bson_value::make_real(1.0));
        return scripted_answer{false, false, make_reply(request, std::move(reply))};
    };

    // The reconnector must be in place before the connection is built: it is copied
    // into the connection at construction.
    options.reconnect = [&]() -> crpc::result<std::unique_ptr<crpc::db::channel>> {
        auto channel = std::make_unique<scripted_channel>(server);
        second_channel_raw = channel.get();
        std::unique_ptr<crpc::db::channel> base = std::move(channel);
        return base;
    };

    auto connection = make_connection(options, server, &first_channel);
    CRPC_CHECK(connection->is_open());
    CRPC_CHECK(connection->is_replica_set());

    // run_command on an insert is the same retryable-write path insert() uses, and it
    // reports the retry out of band.
    bson_document command;
    command.append("insert", bson_value::make_text("c"));
    bson_document document;
    document.append("_id", bson_value::make_int32(7));
    std::vector<bson_value> documents;
    documents.push_back(bson_value::make_document(std::move(document)));
    command.append("documents", bson_value::make_array(std::move(documents)));
    const auto answered = connection->run_command(command);
    CRPC_CHECK(static_cast<bool>(answered));
    if (!answered) {
        return;
    }
    CRPC_CHECK(answered.value().ok);
    CRPC_CHECK(answered.value().retried);

    // The dropped first attempt and the successful retry must use the same txnNumber
    // and the same session id: that is what makes the replay idempotent.
    const std::vector<bytes>& first_requests = first_channel->requests();
    CRPC_CHECK(first_requests.size() >= 2);
    const auto first_body = request_body(first_requests[1]);
    CRPC_CHECK(static_cast<bool>(first_body));
    if (!first_body) {
        return;
    }
    CRPC_CHECK(second_channel_raw != nullptr);
    if (second_channel_raw == nullptr) {
        return;
    }
    const std::vector<bytes>& second_requests = second_channel_raw->requests();
    CRPC_CHECK(second_requests.size() >= 2);
    const auto second_body = request_body(second_requests[1]);
    CRPC_CHECK(static_cast<bool>(second_body));
    if (!second_body) {
        return;
    }

    std::int64_t first_txn = 0;
    std::int64_t second_txn = 0;
    CRPC_CHECK(first_body.value().get_int64("txnNumber", first_txn));
    CRPC_CHECK(second_body.value().get_int64("txnNumber", second_txn));
    CRPC_CHECK_EQ(first_txn, second_txn);
    CRPC_CHECK(has_lsid(first_body.value(), 16));
    CRPC_CHECK(has_lsid(second_body.value(), 16));
    const bson_document* first_lsid = first_body.value().get_document("lsid");
    const bson_document* second_lsid = second_body.value().get_document("lsid");
    CRPC_CHECK(first_lsid != nullptr);
    CRPC_CHECK(second_lsid != nullptr);
    if (first_lsid != nullptr && second_lsid != nullptr) {
        // The session id must survive the failed attempt into the replay.
        CRPC_CHECK(lsid_matches(first_body.value(), connection->session_id()));
        CRPC_CHECK(lsid_matches(second_body.value(), connection->session_id()));
    }
}

CRPC_TEST(mongo_txn_number_is_suppressed_without_a_replica_set) {
    scripted_channel* channel = nullptr;
    crpc::db::mongo::connection_options options;
    options.user.clear();
    auto serve = [](const bytes& request, std::size_t) -> scripted_answer {
        if (bytes_contain(request, "hello\x00")) {
            return scripted_answer{false, false, make_reply(request, hello_reply(false))};
        }
        if (bytes_contain(request, "insert\x00")) {
            bson_document reply;
            reply.append("ok", bson_value::make_real(1.0));
            reply.append("n", bson_value::make_int32(1));
            return scripted_answer{false, false, make_reply(request, std::move(reply))};
        }
        bson_document reply;
        reply.append("ok", bson_value::make_real(1.0));
        return scripted_answer{false, false, make_reply(request, std::move(reply))};
    };
    auto connection = make_connection(options, serve, &channel);
    CRPC_CHECK(connection->is_open());
    CRPC_CHECK_FALSE(connection->is_replica_set());

    crpc::db::mongo::bson_document document;
    document.append("_id", bson_value::make_int32(1));
    const auto inserted =
        connection->insert("c", std::vector<bson_document>{std::move(document)});
    CRPC_CHECK(static_cast<bool>(inserted));

    if (channel->requests().size() >= 2) {
        const auto body = request_body(channel->requests()[1]);
        CRPC_CHECK(static_cast<bool>(body));
        if (body) {
            CRPC_CHECK(body.value().find("txnNumber") == nullptr);
            // Sessions still work on a standalone; only txnNumber is refused there.
            CRPC_CHECK(has_lsid(body.value(), 16));
        }
    }
}

CRPC_TEST(mongo_write_concern_error_is_surfaced_not_swallowed) {
    scripted_channel* channel = nullptr;
    crpc::db::mongo::connection_options options;
    options.user.clear();
    auto serve = [](const bytes& request, std::size_t) -> scripted_answer {
        if (bytes_contain(request, "hello\x00")) {
            return scripted_answer{false, false, make_reply(request, hello_reply(true))};
        }
        if (bytes_contain(request, "insert\x00")) {
            // A write concern failure is not a command failure: ok stays 1 and carries
            // a separate writeConcernError document, exactly as a real server answers
            // an unsatisfiable w on a single-node set.
            bson_document concern_error;
            concern_error.append("code", bson_value::make_int32(100));
            concern_error.append("codeName", bson_value::make_text("UnsatisfiableWriteConcern"));
            concern_error.append("errmsg", bson_value::make_text("Not enough data-bearing nodes"));
            bson_document reply;
            reply.append("ok", bson_value::make_real(1.0));
            reply.append("n", bson_value::make_int32(1));
            reply.append("writeConcernError", bson_value::make_document(std::move(concern_error)));
            return scripted_answer{false, false, make_reply(request, std::move(reply))};
        }
        bson_document reply;
        reply.append("ok", bson_value::make_real(1.0));
        return scripted_answer{false, false, make_reply(request, std::move(reply))};
    };
    crpc::db::mongo::connection_options write_options = options;
    write_options.write_concern = crpc::db::mongo::write_concern_options::count(2);
    auto connection =
        make_connection(write_options, serve, &channel);
    CRPC_CHECK(connection->is_open());

    // The insert fails, and the error names the server's own reason.
    crpc::db::mongo::bson_document document;
    document.append("_id", bson_value::make_int32(1));
    const auto inserted =
        connection->insert("c", std::vector<bson_document>{std::move(document)});
    CRPC_CHECK_FALSE(static_cast<bool>(inserted));
    if (!inserted) {
        CRPC_CHECK(inserted.error().message.find("Not enough data-bearing nodes") !=
                   std::string::npos);
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                      static_cast<int>(inserted.error().code));
    }

    // The generic command path carries the same information out of band.
    crpc::db::mongo::bson_document document_two;
    document_two.append("_id", bson_value::make_int32(2));
    bson_document command;
    command.append("insert", bson_value::make_text("c"));
    std::vector<bson_value> documents;
    documents.push_back(bson_value::make_document(std::move(document_two)));
    command.append("documents", bson_value::make_array(std::move(documents)));
    const auto answered = connection->run_command(command);
    CRPC_CHECK(static_cast<bool>(answered));
    if (answered) {
        CRPC_CHECK(answered.value().ok);
        CRPC_CHECK(answered.value().has_write_concern_error);
        CRPC_CHECK_EQ(std::int64_t{100}, answered.value().write_concern_code);
        CRPC_CHECK(
            answered.value().write_concern_message.find("Not enough data-bearing nodes") !=
            std::string::npos);
    }
}

CRPC_TEST_MAIN()