// CRPC -- tests for the Redis RTTI binding (rtti.hpp).
//
// Coverage targets:
//   * scalar / container / optional round-trips through redis::value;
//   * reflected DTO -> hash map and back, including a derived type;
//   * HSET / HMGET / HGETALL command shapes built from the reflection table;
//   * the field policy: unknown fields rejected by default and ignored on
//     request, missing fields kept by default and rejected on request;
//   * type mismatches and range errors fail with rpc_code::decode_error;
//   * the resp_value bridge and an end-to-end client::execute round-trip over
//     the in-process backend.
//
// The file is empty when CRPC_HAVE_RTTI is off, matching the RTTI-dependent
// tests in the suite (test_codec / test_mvc).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/redis/rtti.hpp>

#include "test_framework.hpp"

#if CRPC_HAVE_RTTI

#include <chrono>

#include <crpc/io.hpp>
#include <crpc/redis/client.hpp>
#include <crpc/redis/resp.hpp>
#include <crpc/runtime.hpp>

// --- reflected DTOs ----------------------------------------------------------

struct rtti_point {
    double x = 0.0;
    double y = 0.0;
    std::string label;
};
RTTI_DEFINE_TYPE(rtti_point, x, y, label)

struct rtti_profile {
    std::string id;
    int age = 0;
    bool active = false;
    std::optional<std::string> nickname;
    std::vector<std::string> tags;
};
RTTI_DEFINE_TYPE(rtti_profile, id, age, active, nickname, tags)

struct rtti_base {
    std::string id;
};
RTTI_DEFINE_TYPE(rtti_base, id)

struct rtti_derived : rtti_base {
    int n = 0;
    std::string name;
};
RTTI_DEFINE_TYPE_DERIVED(rtti_derived, rtti_base, n, name)

enum class rtti_color : int { red = 1, green = 2, blue = 3 };

using namespace crpc;
using namespace crpc::redis;

namespace {

// --- value helpers -----------------------------------------------------------

value vnull() { return value{nullptr}; }
value vint(i64 v) { return value{v}; }
value vstr(std::string s) { return value{std::move(s)}; }
value varray(std::vector<value> items) {
    // Parentheses, not braces: array_value aliases std::vector<value>, and the
    // braced form is list-initialisation, which prefers the
    // initializer_list<value> constructor and wraps the whole vector as a
    // single element (value is constructible from array_value).
    return value{array_value(std::move(items))};
}
value vmap(std::vector<std::pair<value, value>> entries) {
    return value{map_value{std::move(entries)}};
}
value vset(std::vector<value> items) {
    return value{set_value{std::move(items)}};
}
bool values_equal(const value& a, const value& b) {
    if (a.kind() != b.kind()) {
        return false;
    }
    switch (a.kind()) {
    case value_kind::null:
        return true;
    case value_kind::boolean:
        return a.as_bool() == b.as_bool();
    case value_kind::integer:
        return a.as_int() == b.as_int();
    case value_kind::real:
        return a.as_real() == b.as_real();
    case value_kind::string:
        return a.as_string() == b.as_string();
    case value_kind::array:
    case value_kind::set:
    case value_kind::push: {
        const array_value& x = a.is_array() ? a.as_array()
                            : a.is_set()    ? a.as_set().items
                                            : a.as_push().items;
        const array_value& y = b.is_array() ? b.as_array()
                            : b.is_set()    ? b.as_set().items
                                            : b.as_push().items;
        if (x.size() != y.size()) {
            return false;
        }
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (!values_equal(x[i], y[i])) {
                return false;
            }
        }
        return true;
    }
    case value_kind::map: {
        const map_value& x = a.as_map();
        const map_value& y = b.as_map();
        if (x.size() != y.size()) {
            return false;
        }
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (!values_equal(x[i].first, y[i].first) ||
                !values_equal(x[i].second, y[i].second)) {
                return false;
            }
        }
        return true;
    }
    case value_kind::error:
        return a.as_error().message == b.as_error().message;
    }
    return false;
}

bool status_is_decode_error(const status& st) {
    return !st.ok() && st.code() == rpc_code::decode_error;
}

// --- scalar / container round-trips -----------------------------------------

CRPC_TEST(rtti_string_round_trip) {
    result<value> encoded = encode_to_value(std::string("hello"));
    CRPC_CHECK(encoded.ok());
    CRPC_CHECK(encoded.value().is_string());
    CRPC_CHECK_EQ(std::string("hello"), encoded.value().as_string());

    std::string decoded;
    CRPC_CHECK(decode_from_value(encoded.value(), decoded).ok());
    CRPC_CHECK_EQ(std::string("hello"), decoded);
}

CRPC_TEST(rtti_integral_round_trip) {
    for (i64 raw : {i64{0}, i64{-1}, i64{42}, i64{-9000}, i64{1} << 40}) {
        result<value> encoded = encode_to_value(raw);
        CRPC_CHECK(encoded.ok());
        CRPC_CHECK(encoded.value().is_int());
        i64 decoded = 0;
        CRPC_CHECK(decode_from_value(encoded.value(), decoded).ok());
        CRPC_CHECK_EQ(raw, decoded);
    }
    unsigned long long big = 18446744073709551615ull;
    result<value> encoded = encode_to_value(big);
    CRPC_CHECK(encoded.ok());
    // Past i64 the value has no integer spelling, so it is stored as text.
    CRPC_CHECK(encoded.value().is_string());
    unsigned long long decoded = 0;
    CRPC_CHECK(decode_from_value(encoded.value(), decoded).ok());
    CRPC_CHECK_EQ(big, decoded);
}

CRPC_TEST(rtti_enum_round_trip) {
    result<value> encoded = encode_to_value(rtti_color::blue);
    CRPC_CHECK(encoded.ok());
    CRPC_CHECK(encoded.value().is_int());
    CRPC_CHECK_EQ(3, encoded.value().as_int());

    rtti_color decoded = rtti_color::red;
    CRPC_CHECK(decode_from_value(encoded.value(), decoded).ok());
    CRPC_CHECK(decoded == rtti_color::blue);
}

CRPC_TEST(rtti_real_round_trip) {
    result<value> encoded = encode_to_value(3.5);
    CRPC_CHECK(encoded.ok());
    CRPC_CHECK(encoded.value().is_real());
    double decoded = 0.0;
    CRPC_CHECK(decode_from_value(encoded.value(), decoded).ok());
    CRPC_CHECK_EQ(3.5, decoded);

    float narrow = 1.25f;
    CRPC_CHECK(encode_to_value(narrow).value().is_real());
    float back = 0.0f;
    CRPC_CHECK(decode_from_value(encode_to_value(narrow).value(), back).ok());
    CRPC_CHECK_EQ(1.25f, back);
}

CRPC_TEST(rtti_bool_round_trip) {
    CRPC_CHECK(encode_to_value(true).value().is_bool());
    CRPC_CHECK(encode_to_value(false).value().is_bool());

    bool decoded = false;
    CRPC_CHECK(decode_from_value(encode_to_value(true).value(), decoded).ok());
    CRPC_CHECK(decoded);
    CRPC_CHECK(decode_from_value(encode_to_value(false).value(), decoded).ok());
    CRPC_CHECK_FALSE(decoded);

    // Redis spells booleans as 0/1 text on the wire.
    CRPC_CHECK(decode_from_value(vstr("1"), decoded).ok());
    CRPC_CHECK(decoded);
    CRPC_CHECK(decode_from_value(vstr("0"), decoded).ok());
    CRPC_CHECK_FALSE(decoded);
}

CRPC_TEST(rtti_vector_round_trip) {
    std::vector<int> source{1, -2, 3, 4};
    result<value> encoded = encode_to_value(source);
    CRPC_CHECK(encoded.ok());
    CRPC_CHECK(encoded.value().is_array());
    CRPC_CHECK_EQ(std::size_t{4}, encoded.value().as_array().size());

    std::vector<int> decoded;
    CRPC_CHECK(decode_from_value(encoded.value(), decoded).ok());
    CRPC_CHECK(source == decoded);
}

CRPC_TEST(rtti_vector_decode_accepts_set_reply) {
    std::vector<std::string> decoded;
    CRPC_CHECK(decode_from_value(vset({vstr("a"), vstr("b")}), decoded).ok());
    CRPC_CHECK_EQ(std::size_t{2}, decoded.size());
    CRPC_CHECK_EQ(std::string("a"), decoded[0]);
    CRPC_CHECK_EQ(std::string("b"), decoded[1]);
}

CRPC_TEST(rtti_map_round_trip) {
    std::map<std::string, int> source{{"a", 1}, {"b", 2}};
    result<value> encoded = encode_to_value(source);
    CRPC_CHECK(encoded.ok());
    CRPC_CHECK(encoded.value().is_map());

    std::map<std::string, int> decoded;
    CRPC_CHECK(decode_from_value(encoded.value(), decoded).ok());
    CRPC_CHECK_EQ(std::size_t{2}, decoded.size());
    CRPC_CHECK_EQ(1, decoded["a"]);
    CRPC_CHECK_EQ(2, decoded["b"]);
}

CRPC_TEST(rtti_map_decode_accepts_flat_array) {
    std::map<std::string, int> decoded;
    const value reply = varray({vstr("a"), vint(1), vstr("b"), vint(2)});
    CRPC_CHECK(decode_from_value(reply, decoded).ok());
    CRPC_CHECK_EQ(std::size_t{2}, decoded.size());
    CRPC_CHECK_EQ(1, decoded["a"]);
}

CRPC_TEST(rtti_optional_round_trip) {
    std::optional<std::string> present("value");
    result<value> encoded = encode_to_value(present);
    CRPC_CHECK(encoded.ok());
    CRPC_CHECK(encoded.value().is_string());

    std::optional<std::string> empty;
    result<value> encoded_empty = encode_to_value(empty);
    CRPC_CHECK(encoded_empty.ok());
    CRPC_CHECK(encoded_empty.value().is_null());

    std::optional<std::string> decoded;
    CRPC_CHECK(decode_from_value(encoded.value(), decoded).ok());
    CRPC_CHECK(decoded.has_value());
    CRPC_CHECK_EQ(std::string("value"), decoded.value());

    CRPC_CHECK(decode_from_value(encoded_empty.value(), decoded).ok());
    CRPC_CHECK_FALSE(decoded.has_value());
}

// --- reflected DTO ----------------------------------------------------------

CRPC_TEST(rtti_dto_encodes_to_hash_map) {
    rtti_point point;
    point.x = 1.5;
    point.y = -2.5;
    point.label = "origin";
    result<value> encoded = encode_to_value(point);
    CRPC_CHECK(encoded.ok());
    CRPC_CHECK(encoded.value().is_map());
    CRPC_CHECK_EQ(std::size_t{3}, encoded.value().as_map().size());

    rtti_point back;
    CRPC_CHECK(decode_hash_reply(encoded.value(), back).ok());
    CRPC_CHECK_EQ(1.5, back.x);
    CRPC_CHECK_EQ(-2.5, back.y);
    CRPC_CHECK_EQ(std::string("origin"), back.label);
}

CRPC_TEST(rtti_hash_set_command_shape) {
    rtti_point point;
    point.x = 1.0;
    point.y = 2.0;
    point.label = "p";
    result<command> cmd = hash_set_command("pt", point);
    CRPC_CHECK(cmd.ok());
    const command& c = cmd.value();
    CRPC_CHECK_EQ(std::size_t{8}, c.size());
    CRPC_CHECK_EQ(std::string("HSET"), c[0]);
    CRPC_CHECK_EQ(std::string("pt"), c[1]);
    CRPC_CHECK_EQ(std::string("x"), c[2]);
    CRPC_CHECK_EQ(std::string("1"), c[3]);
    CRPC_CHECK_EQ(std::string("y"), c[4]);
    CRPC_CHECK_EQ(std::string("2"), c[5]);
    CRPC_CHECK_EQ(std::string("label"), c[6]);
    CRPC_CHECK_EQ(std::string("p"), c[7]);
}

CRPC_TEST(rtti_hash_round_trip_flat_array_reply) {
    rtti_point point;
    point.x = 0.5;
    point.y = 1.5;
    point.label = "k";
    result<command> set = hash_set_command("h", point);
    CRPC_CHECK(set.ok());

    const value reply =
        varray({vstr("x"), vstr("0.5"), vstr("y"), vstr("1.5"),
                vstr("label"), vstr("k")});
    rtti_point back;
    CRPC_CHECK(decode_hash_reply(reply, back).ok());
    CRPC_CHECK_EQ(0.5, back.x);
    CRPC_CHECK_EQ(1.5, back.y);
    CRPC_CHECK_EQ(std::string("k"), back.label);
}

CRPC_TEST(rtti_hash_round_trip_map_reply) {
    rtti_point point;
    point.x = 9.0;
    point.y = 8.0;
    point.label = "m";
    const value reply = vmap({{vstr("label"), vstr("m")},
                              {vstr("y"), vstr("8")},
                              {vstr("x"), vstr("9")}});
    rtti_point back;
    CRPC_CHECK(decode_hash_reply(reply, back).ok());
    CRPC_CHECK_EQ(9.0, back.x);
    CRPC_CHECK_EQ(8.0, back.y);
    CRPC_CHECK_EQ(std::string("m"), back.label);
}

CRPC_TEST(rtti_derived_dto_base_fields_first) {
    rtti_derived dto;
    dto.id = "base";
    dto.n = 7;
    dto.name = "leaf";

    result<command> cmd = hash_set_command("d", dto);
    CRPC_CHECK(cmd.ok());
    CRPC_CHECK_EQ(std::string("id"), cmd.value()[2]);
    CRPC_CHECK_EQ(std::string("base"), cmd.value()[3]);
    CRPC_CHECK_EQ(std::string("n"), cmd.value()[4]);
    CRPC_CHECK_EQ(std::string("7"), cmd.value()[5]);
    CRPC_CHECK_EQ(std::string("name"), cmd.value()[6]);
    CRPC_CHECK_EQ(std::string("leaf"), cmd.value()[7]);

    result<value> encoded = encode_to_value(dto);
    CRPC_CHECK(encoded.ok());
    CRPC_CHECK_EQ(std::size_t{3}, encoded.value().as_map().size());

    rtti_derived back;
    CRPC_CHECK(decode_hash_reply(encoded.value(), back).ok());
    CRPC_CHECK_EQ(std::string("base"), back.id);
    CRPC_CHECK_EQ(7, back.n);
    CRPC_CHECK_EQ(std::string("leaf"), back.name);
}

CRPC_TEST(rtti_hash_get_command_is_hgetall) {
    const command cmd = hash_get_command("k");
    CRPC_CHECK_EQ(std::size_t{2}, cmd.size());
    CRPC_CHECK_EQ(std::string("HGETALL"), cmd[0]);
    CRPC_CHECK_EQ(std::string("k"), cmd[1]);
}

CRPC_TEST(rtti_hmget_command_lists_fields_in_order) {
    const command cmd = hash_mget_command<rtti_derived>("d");
    CRPC_CHECK_EQ(std::string("HMGET"), cmd[0]);
    CRPC_CHECK_EQ(std::string("d"), cmd[1]);
    CRPC_CHECK_EQ(std::string("id"), cmd[2]);
    CRPC_CHECK_EQ(std::string("n"), cmd[3]);
    CRPC_CHECK_EQ(std::string("name"), cmd[4]);
    CRPC_CHECK_EQ(std::size_t{5}, cmd.size());
}

CRPC_TEST(rtti_hmget_reply_decodes_in_order) {
    rtti_derived back;
    const value reply = varray({vstr("id-1"), vnull(), vstr("late")});
    bind_options opts;
    opts.on_missing = bind_options::missing_field::keep;
    CRPC_CHECK(decode_hash_values(reply, back, opts).ok());
    CRPC_CHECK_EQ(std::string("id-1"), back.id);
    CRPC_CHECK_EQ(0, back.n);
    CRPC_CHECK_EQ(std::string("late"), back.name);
}

// --- field policy -----------------------------------------------------------

CRPC_TEST(rtti_unknown_field_rejected_by_default) {
    rtti_point back;
    const value reply = varray(
        {vstr("x"), vstr("1"), vstr("y"), vstr("2"), vstr("label"), vstr("p"),
         vstr("extra"), vstr("surprise")});
    const status st = decode_hash_reply(reply, back);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(status_is_decode_error(st));
}

CRPC_TEST(rtti_unknown_field_ignored_on_request) {
    rtti_point back;
    bind_options opts;
    opts.on_unknown = bind_options::unknown_field::ignore;
    const value reply = varray(
        {vstr("x"), vstr("1"), vstr("y"), vstr("2"), vstr("label"), vstr("p"),
         vstr("extra"), vstr("surprise")});
    CRPC_CHECK(decode_hash_reply(reply, back, opts).ok());
    CRPC_CHECK_EQ(1.0, back.x);
    CRPC_CHECK_EQ(std::string("p"), back.label);
}

CRPC_TEST(rtti_missing_field_kept_by_default) {
    rtti_point back;
    back.x = 5.0;
    const value reply = varray({vstr("y"), vstr("2"), vstr("label"), vstr("p")});
    CRPC_CHECK(decode_hash_reply(reply, back).ok());
    CRPC_CHECK_EQ(5.0, back.x);
    CRPC_CHECK_EQ(2.0, back.y);
}

CRPC_TEST(rtti_missing_field_rejected_on_request) {
    rtti_point back;
    bind_options opts;
    opts.on_missing = bind_options::missing_field::reject;
    const value reply = varray({vstr("y"), vstr("2"), vstr("label"), vstr("p")});
    const status st = decode_hash_reply(reply, back, opts);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(status_is_decode_error(st));
}

// --- type safety ------------------------------------------------------------

CRPC_TEST(rtti_decode_integer_from_text_and_out_of_range) {
    i64 decoded = 0;
    CRPC_CHECK(decode_from_value(vstr("12345"), decoded).ok());
    CRPC_CHECK_EQ(i64{12345}, decoded);

    const status bad = decode_from_value(vstr("not-a-number"), decoded);
    CRPC_CHECK_FALSE(bad.ok());
    CRPC_CHECK(status_is_decode_error(bad));

    short narrow = 0;
    const status overflow = decode_from_value(vint(100000), narrow);
    CRPC_CHECK_FALSE(overflow.ok());
    CRPC_CHECK(status_is_decode_error(overflow));
}

CRPC_TEST(rtti_decode_type_mismatch_fails) {
    std::string decoded;
    const status st = decode_from_value(vint(7), decoded);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(status_is_decode_error(st));

    std::vector<int> list;
    const status shape = decode_from_value(vstr("flat"), list);
    CRPC_CHECK_FALSE(shape.ok());
    CRPC_CHECK(status_is_decode_error(shape));
}

CRPC_TEST(rtti_hash_set_command_rejects_container_field) {
    rtti_profile profile;
    profile.id = "u1";
    profile.tags = {"a", "b"};
    const result<command> cmd = hash_set_command("p", profile);
    CRPC_CHECK_FALSE(cmd.ok());
    CRPC_CHECK(cmd.code() == rpc_code::encode_error);
}

CRPC_TEST(rtti_decode_result_overload) {
    result<rtti_point> decoded = decode_from_value<rtti_point>(
        varray({vstr("x"), vstr("1"), vstr("y"), vstr("2"), vstr("label"),
                vstr("p")}));
    CRPC_CHECK(decoded.ok());
    CRPC_CHECK_EQ(1.0, decoded.value().x);
}

// --- resp_value bridge ------------------------------------------------------

CRPC_TEST(rtti_value_from_resp_all_kinds) {
    {
        value out;
        CRPC_CHECK(value_from_resp(
            resp_value{resp_bulk_string{"hi"}}, out).ok());
        CRPC_CHECK(out.is_string());
        CRPC_CHECK_EQ(std::string("hi"), out.as_string());
    }
    {
        value out;
        CRPC_CHECK(value_from_resp(
            resp_value{resp_integer{42}}, out).ok());
        CRPC_CHECK(out.is_int());
        CRPC_CHECK_EQ(i64{42}, out.as_int());
    }
    {
        value out;
        CRPC_CHECK(value_from_resp(
            resp_value{resp_double{1.5}}, out).ok());
        CRPC_CHECK(out.is_real());
        CRPC_CHECK_EQ(1.5, out.as_real());
    }
    {
        value out;
        CRPC_CHECK(value_from_resp(
            resp_value{resp_boolean{true}}, out).ok());
        CRPC_CHECK(out.is_bool());
        CRPC_CHECK(out.as_bool());
    }
    {
        value out;
        CRPC_CHECK(value_from_resp(
            resp_value{resp_array_value{{
                resp_value{resp_bulk_string{"a"}},
                resp_value{resp_integer{1}}}}}, out).ok());
        CRPC_CHECK(out.is_array());
        CRPC_CHECK_EQ(std::size_t{2}, out.as_array().size());
    }
    {
        value out;
        CRPC_CHECK(value_from_resp(
            resp_value{resp_map_value{{
                {resp_value{resp_bulk_string{"k"}},
                 resp_value{resp_integer{9}}}}}}, out).ok());
        CRPC_CHECK(out.is_map());
        CRPC_CHECK_EQ(i64{9}, out.as_map()[0].second.as_int());
    }
    {
        value out;
        CRPC_CHECK(value_from_resp(
            resp_value{resp_simple_string{"ok"}}, out).ok());
        CRPC_CHECK(out.is_string());
    }
    {
        value out;
        CRPC_CHECK(value_from_resp(
            resp_value{resp_null{}}, out).ok());
        CRPC_CHECK(out.is_null());
    }
}

} // namespace

// --- client::execute integration --------------------------------------------

namespace {

struct scripted_reply {
    std::vector<std::string> chunks;
};

using server_handler = std::function<scripted_reply(const command&)>;

std::string bulk(const std::string& text) {
    return "$" + std::to_string(text.size()) + "\r\n" + text + "\r\n";
}

// Minimal in-process Redis server: it parses commands off the client's
// connection and answers HSET/HGETALL/HMGET from an in-memory hash. It is the
// same shape as the fake server in test_redis_client.cpp, trimmed to the
// commands this test needs.
class fake_redis_server final : public crpc::io::source {
public:
    fake_redis_server(crpc::connection* conn, server_handler handler)
        : conn_(conn), handler_(std::move(handler)) {
        conn_->bind_source(this);
    }

    ~fake_redis_server() override { conn_->bind_source(nullptr); }

    fake_redis_server(const fake_redis_server&) = delete;
    fake_redis_server& operator=(const fake_redis_server&) = delete;

    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        try {
            parser_.feed(data);
            for (;;) {
                resp_value value;
                const resp_parser::result decoded = parser_.next(value);
                if (decoded != resp_parser::result::value_ready) {
                    break;
                }
                if (!value.is_array()) {
                    continue;
                }
                command cmd;
                for (const resp_value& item : value.as_array()) {
                    if (item.is_bulk_string()) {
                        cmd.push_back(item.as_bulk_string());
                    } else if (item.is_simple_string()) {
                        cmd.push_back(item.as_simple_string());
                    } else {
                        cmd.clear();
                        break;
                    }
                }
                if (cmd.empty()) {
                    continue;
                }
                const scripted_reply reply = handler_(cmd);
                for (const std::string& chunk : reply.chunks) {
                    write_chunk(chunk);
                }
            }
        } catch (...) {
        }
        return data.size();
    }

    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

private:
    class write_done final : public crpc::io::completion {
    public:
        explicit write_done(std::shared_ptr<std::string> data)
            : data_(std::move(data)) {}
        void complete(crpc::io::io_result) noexcept override { delete this; }

    private:
        std::shared_ptr<std::string> data_;
    };

    void write_chunk(const std::string& body) {
        auto data = std::make_shared<std::string>(body);
        auto* done = new write_done(data);
        conn_->sink().push(
            crpc::span<const std::byte>(
                reinterpret_cast<const std::byte*>(data->data()), data->size()),
            done);
    }

    crpc::connection* conn_;
    server_handler handler_;
    resp_parser parser_;
};

class server_holder final : public crpc::accept_sink {
public:
    explicit server_holder(server_handler handler) : handler_(std::move(handler)) {}

    void on_accept(std::unique_ptr<crpc::connection> conn) noexcept override {
        try {
            auto server =
                std::make_unique<fake_redis_server>(conn.get(), handler_);
            servers_.push_back(std::move(server));
            conns_.push_back(std::move(conn));
        } catch (...) {
        }
    }

private:
    server_handler handler_;
    std::vector<std::unique_ptr<crpc::connection>> conns_;
    std::vector<std::unique_ptr<fake_redis_server>> servers_;
};

struct harness {
    crpc::inproc_backend backend;
    crpc::endpoint ep{crpc::endpoint::inproc("crpc-redis-rtti")};
    std::unique_ptr<crpc::listener> listener;
    std::unique_ptr<server_holder> server;

    harness(server_handler handler) {
        server = std::make_unique<server_holder>(std::move(handler));
        listener = backend.listen(ep, crpc::listen_options{});
        if (listener != nullptr) {
            listener->start(server.get());
        }
    }

    client::options options() const {
        client::options opts;
        opts.pool.connection.ep = ep;
        opts.pool.connection.operation_timeout = std::chrono::seconds(2);
        return opts;
    }
};

server_handler hash_handler() {
    auto fields = std::make_shared<std::map<std::string, std::string>>();
    return [fields](const command& cmd) -> scripted_reply {
        if (cmd.empty()) {
            return {{"-ERR empty command\r\n"}};
        }
        const std::string& name = cmd[0];
        if (name == "PING") {
            return {{"+PONG\r\n"}};
        }
        if (name == "HSET") {
            for (std::size_t i = 2; i + 1 < cmd.size(); i += 2) {
                (*fields)[cmd[i]] = cmd[i + 1];
            }
            return {{":1\r\n"}};
        }
        if (name == "HGETALL") {
            std::string out = "%" + std::to_string(fields->size()) + "\r\n";
            for (const auto& entry : *fields) {
                out += bulk(entry.first);
                out += bulk(entry.second);
            }
            return {{out}};
        }
        if (name == "HMGET") {
            std::string out =
                "*" + std::to_string(cmd.size() - 2) + "\r\n";
            for (std::size_t i = 2; i < cmd.size(); ++i) {
                auto it = fields->find(cmd[i]);
                if (it == fields->end()) {
                    out += "$-1\r\n";
                } else {
                    out += bulk(it->second);
                }
            }
            return {{out}};
        }
        return {{"-ERR unknown command\r\n"}};
    };
}

} // namespace

CRPC_TEST(rtti_client_execute_dto_round_trip) {
    harness h(hash_handler());
    client c(h.options(), h.backend);
    CRPC_CHECK(c.connect().ok());

    rtti_derived dto;
    dto.id = "row-1";
    dto.n = 19;
    dto.name = "nineteen";

    result<command> set = hash_set_command("row", dto);
    CRPC_CHECK(set.ok());
    CRPC_CHECK(c.execute(set.value()).ok());

    result<resp_value> reply = c.execute(hash_get_command("row"));
    CRPC_CHECK(reply.ok());

    value decoded_value;
    CRPC_CHECK(value_from_resp(reply.value(), decoded_value).ok());
    CRPC_CHECK(decoded_value.is_map());

    rtti_derived back;
    CRPC_CHECK(decode_hash_reply(decoded_value, back).ok());
    CRPC_CHECK_EQ(std::string("row-1"), back.id);
    CRPC_CHECK_EQ(19, back.n);
    CRPC_CHECK_EQ(std::string("nineteen"), back.name);

    result<resp_value> hmget = c.execute(hash_mget_command<rtti_derived>("row"));
    CRPC_CHECK(hmget.ok());
    value positional;
    CRPC_CHECK(value_from_resp(hmget.value(), positional).ok());
    rtti_derived positional_back;
    CRPC_CHECK(decode_hash_values(positional, positional_back).ok());
    CRPC_CHECK_EQ(std::string("row-1"), positional_back.id);
    CRPC_CHECK_EQ(19, positional_back.n);
    CRPC_CHECK_EQ(std::string("nineteen"), positional_back.name);

    c.close();
}

CRPC_TEST_MAIN()

#else

// No RTTI dependency: the whole suite is compiled out, so the executable is an
// empty binary that reports zero cases.
CRPC_TEST_MAIN()

#endif // CRPC_HAVE_RTTI
