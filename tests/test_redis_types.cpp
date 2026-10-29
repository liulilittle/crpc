// CRPC -- tests for the Redis data-type layer (types.hpp).
//
// Coverage targets: the `value` model (every kind, checked and nullable
// accessors), the command alias, RESP3 encode/decode golden bytes and
// round-trips, fail-closed rejection of malformed input, the typed reply
// projections (decode_*), the per-type command builders, and the input
// iterators.
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <crpc/redis/types.hpp>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::redis;

namespace {

// --- value construction helpers ---------------------------------------------

value vnull() { return value{nullptr}; }
value vbool(bool b) { return value{b}; }
value vint(i64 v) { return value{v}; }
value vreal(double v) { return value{v}; }
value vstr(std::string s) { return value{std::move(s)}; }
value varray(std::vector<value> items) {
    // Parentheses, not braces: array_value aliases std::vector<value>, so
    // `array_value{std::move(items)}` is list-initialisation and prefers the
    // initializer_list<value> constructor -- the whole vector converts to a
    // single value (value is constructible from array_value) and ends up
    // wrapped as the vector's only element.
    return value{array_value(std::move(items))};
}
value vmap(std::vector<std::pair<value, value>> entries) {
    return value{map_value{std::move(entries)}};
}
value vset(std::vector<value> items) {
    return value{set_value{std::move(items)}};
}
value vpush(std::vector<value> items) {
    return value{push_value{std::move(items)}};
}
value verr(std::string msg) { return value{error_value{std::move(msg)}}; }

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
    case value_kind::error:
        return a.as_error().message == b.as_error().message;
    case value_kind::array: {
        const array_value& x = a.as_array();
        const array_value& y = b.as_array();
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
    case value_kind::set: {
        const std::vector<value>& x = a.as_set().items;
        const std::vector<value>& y = b.as_set().items;
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
    case value_kind::push: {
        const std::vector<value>& x = a.as_push().items;
        const std::vector<value>& y = b.as_push().items;
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
    }
    return false;
}

// Decodes exactly one value and asserts the whole buffer was consumed.
value decode_one(const std::string& text) {
    value out;
    std::size_t consumed = 0;
    CRPC_CHECK(decode_value(text, out, consumed));
    CRPC_CHECK_EQ(consumed, text.size());
    return out;
}

// Asserts a command equals the expected argument list.
void check_cmd(std::initializer_list<std::string> expected, const command& actual) {
    CRPC_CHECK_EQ(expected.size(), actual.size());
    std::size_t i = 0;
    for (const std::string& item : expected) {
        if (i < actual.size()) {
            CRPC_CHECK_EQ(item, actual[i]);
        }
        ++i;
    }
}

bool throws_bad_variant(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::bad_variant_access&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

} // namespace

// --- command alias ----------------------------------------------------------

CRPC_TEST(types_command_aliases_resp_command) {
    static_assert(std::is_same<command, resp_command>::value,
                  "command must be the wire codec's resp_command");
    static_assert(std::is_same<command, std::vector<std::string>>::value,
                  "command must be a vector of bulk strings");
    CRPC_CHECK(true);
}

// --- value model ------------------------------------------------------------

CRPC_TEST(types_value_kinds_and_accessors) {
    value n = vnull();
    CRPC_CHECK_EQ(static_cast<int>(n.kind()), static_cast<int>(value_kind::null));
    CRPC_CHECK(n.is_null());
    CRPC_CHECK_FALSE(n.is_bool());

    value b = vbool(true);
    CRPC_CHECK_EQ(static_cast<int>(b.kind()), static_cast<int>(value_kind::boolean));
    CRPC_CHECK(b.is_bool());
    CRPC_CHECK(b.as_bool());

    value i = vint(42);
    CRPC_CHECK_EQ(static_cast<int>(i.kind()), static_cast<int>(value_kind::integer));
    CRPC_CHECK(i.is_int());
    CRPC_CHECK_EQ(i.as_int(), i64{42});

    value r = vreal(1.5);
    CRPC_CHECK_EQ(static_cast<int>(r.kind()), static_cast<int>(value_kind::real));
    CRPC_CHECK(r.is_real());
    CRPC_CHECK_EQ(r.as_real(), 1.5);

    value s = vstr("hello");
    CRPC_CHECK_EQ(static_cast<int>(s.kind()), static_cast<int>(value_kind::string));
    CRPC_CHECK(s.is_string());
    CRPC_CHECK_EQ(s.as_string(), std::string("hello"));

    value a = varray({vint(1), vstr("x")});
    CRPC_CHECK_EQ(static_cast<int>(a.kind()), static_cast<int>(value_kind::array));
    CRPC_CHECK(a.is_array());
    CRPC_CHECK_EQ(a.as_array().size(), std::size_t{2});

    value m = vmap({{vstr("k"), vint(1)}});
    CRPC_CHECK_EQ(static_cast<int>(m.kind()), static_cast<int>(value_kind::map));
    CRPC_CHECK(m.is_map());
    CRPC_CHECK_EQ(m.as_map().size(), std::size_t{1});

    value st = vset({vint(1), vint(2)});
    CRPC_CHECK_EQ(static_cast<int>(st.kind()), static_cast<int>(value_kind::set));
    CRPC_CHECK(st.is_set());
    CRPC_CHECK_EQ(st.as_set().items.size(), std::size_t{2});

    value p = vpush({vint(1)});
    CRPC_CHECK_EQ(static_cast<int>(p.kind()), static_cast<int>(value_kind::push));
    CRPC_CHECK(p.is_push());
    CRPC_CHECK_EQ(p.as_push().items.size(), std::size_t{1});

    value e = verr("ERR boom");
    CRPC_CHECK_EQ(static_cast<int>(e.kind()), static_cast<int>(value_kind::error));
    CRPC_CHECK(e.is_error());
    CRPC_CHECK_EQ(e.as_error().message, std::string("ERR boom"));
}

CRPC_TEST(types_value_nullable_accessors) {
    value i = vint(7);
    CRPC_CHECK(i.if_int() != nullptr);
    CRPC_CHECK_EQ(*i.if_int(), i64{7});
    CRPC_CHECK(i.if_string() == nullptr);
    CRPC_CHECK(i.if_real() == nullptr);
    CRPC_CHECK(i.if_bool() == nullptr);

    value s = vstr("x");
    CRPC_CHECK(s.if_string() != nullptr);
    CRPC_CHECK_EQ(*s.if_string(), std::string("x"));
    CRPC_CHECK(s.if_int() == nullptr);

    value n = vnull();
    CRPC_CHECK(n.if_int() == nullptr);
    CRPC_CHECK(n.if_array() == nullptr);
    CRPC_CHECK(n.if_error() == nullptr);
}

CRPC_TEST(types_value_checked_accessors_throw_on_wrong_kind) {
    value s = vstr("not an int");
    CRPC_CHECK(throws_bad_variant([&s] { (void)s.as_int(); }));
    CRPC_CHECK(throws_bad_variant([&s] { (void)s.as_bool(); }));
    CRPC_CHECK(throws_bad_variant([&s] { (void)s.as_array(); }));
    CRPC_CHECK(throws_bad_variant([&s] { (void)s.as_error(); }));

    value i = vint(1);
    CRPC_CHECK(throws_bad_variant([&i] { (void)i.as_string(); }));
}

// --- encode_command --------------------------------------------------------

CRPC_TEST(types_encode_command_is_resp2_bulk_array) {
    command cmd{"SET", "key", "value"};
    CRPC_CHECK_EQ(encode_command(cmd),
                  std::string("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n"));

    command empty;
    CRPC_CHECK_EQ(encode_command(empty), std::string("*0\r\n"));

    command binary{"SET", "k", std::string("a\r\nb", 4)};
    CRPC_CHECK_EQ(encode_command(binary),
                  std::string("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$4\r\na\r\nb\r\n"));
}

// --- encode_value golden bytes ---------------------------------------------

CRPC_TEST(types_encode_value_golden_resp3) {
    CRPC_CHECK_EQ(encode_value(vnull()), std::string("_\r\n"));
    CRPC_CHECK_EQ(encode_value(vbool(true)), std::string("#t\r\n"));
    CRPC_CHECK_EQ(encode_value(vbool(false)), std::string("#f\r\n"));
    CRPC_CHECK_EQ(encode_value(vint(42)), std::string(":42\r\n"));
    CRPC_CHECK_EQ(encode_value(vint(-7)), std::string(":-7\r\n"));
    CRPC_CHECK_EQ(encode_value(vreal(1.5)), std::string(",1.5\r\n"));
    CRPC_CHECK_EQ(encode_value(vstr("hi")), std::string("$2\r\nhi\r\n"));
    CRPC_CHECK_EQ(encode_value(verr("ERR x")), std::string("-ERR x\r\n"));

    CRPC_CHECK_EQ(encode_value(varray({vint(1), vstr("a")})),
                  std::string("*2\r\n:1\r\n$1\r\na\r\n"));
    CRPC_CHECK_EQ(encode_value(vmap({{vstr("k"), vint(1)}})),
                  std::string("%1\r\n$1\r\nk\r\n:1\r\n"));
    CRPC_CHECK_EQ(encode_value(vset({vint(1), vint(2)})),
                  std::string("~2\r\n:1\r\n:2\r\n"));
    CRPC_CHECK_EQ(encode_value(vpush({vint(1)})),
                  std::string(">1\r\n:1\r\n"));
}

// --- decode_value ----------------------------------------------------------

CRPC_TEST(types_decode_value_reads_every_kind) {
    CRPC_CHECK(values_equal(decode_one("_\r\n"), vnull()));
    CRPC_CHECK(values_equal(decode_one("#t\r\n"), vbool(true)));
    CRPC_CHECK(values_equal(decode_one("#f\r\n"), vbool(false)));
    CRPC_CHECK(values_equal(decode_one(":42\r\n"), vint(42)));
    CRPC_CHECK(values_equal(decode_one(",1.5\r\n"), vreal(1.5)));
    CRPC_CHECK(values_equal(decode_one("$2\r\nhi\r\n"), vstr("hi")));
    CRPC_CHECK(values_equal(decode_one("+OK\r\n"), vstr("OK")));
    CRPC_CHECK(values_equal(decode_one("-ERR x\r\n"), verr("ERR x")));
    CRPC_CHECK(values_equal(decode_one("*2\r\n:1\r\n$1\r\na\r\n"),
                            varray({vint(1), vstr("a")})));
    CRPC_CHECK(values_equal(decode_one("%1\r\n$1\r\nk\r\n:1\r\n"),
                            vmap({{vstr("k"), vint(1)}})));
    CRPC_CHECK(values_equal(decode_one("~2\r\n:1\r\n:2\r\n"),
                            vset({vint(1), vint(2)})));
    CRPC_CHECK(values_equal(decode_one(">1\r\n:1\r\n"), vpush({vint(1)})));
}

CRPC_TEST(types_decode_value_resp2_nulls_and_exotic_markers) {
    CRPC_CHECK(values_equal(decode_one("$-1\r\n"), vnull()));
    CRPC_CHECK(values_equal(decode_one("*-1\r\n"), vnull()));
    // Big number and verbatim string have no dedicated alternative; both fold
    // into the string kind. A blob error folds into the error kind.
    CRPC_CHECK(values_equal(decode_one("(12345678901\r\n"),
                            vstr("12345678901")));
    CRPC_CHECK(values_equal(decode_one("=6\r\ntxt:hi\r\n"), vstr("hi")));
    CRPC_CHECK(values_equal(decode_one("!5\r\nERR x\r\n"), verr("ERR x")));
}

CRPC_TEST(types_decode_value_reports_consumed) {
    const std::string buffer = ":1\r\n:2\r\n";
    value first;
    std::size_t consumed = 0;
    CRPC_CHECK(decode_value(buffer, first, consumed));
    CRPC_CHECK_EQ(consumed, std::size_t{4});
    CRPC_CHECK(values_equal(first, vint(1)));

    value second;
    CRPC_CHECK(decode_value(std::string(buffer, consumed), second, consumed));
    CRPC_CHECK_EQ(consumed, std::size_t{4});
    CRPC_CHECK(values_equal(second, vint(2)));
}

CRPC_TEST(types_decode_value_rejects_malformed) {
    const std::string cases[] = {
        std::string(""),
        std::string("$3\r\nab"),           // truncated bulk
        std::string("*2\r\n:1\r\n"),        // truncated array
        std::string("X\r\n"),               // unknown marker
        std::string(":abc\r\n"),            // bad integer
        std::string("#x\r\n"),              // bad boolean
        std::string(",abc\r\n"),            // bad double
        std::string("*abc\r\n"),            // bad count
        std::string("$-2\r\n"),             // negative count other than -1
        std::string("*1\r\nX\r\n"),         // nested unknown marker
        std::string("$1\r\nab\r\n"),        // bulk shorter than declared
    };
    for (const std::string& text : cases) {
        value out = vint(99);
        std::size_t consumed = 123;
        CRPC_CHECK_FALSE(decode_value(text, out, consumed));
        CRPC_CHECK_EQ(consumed, std::size_t{0});
        CRPC_CHECK(values_equal(out, vint(99)));
    }
}

CRPC_TEST(types_decode_value_rejects_excessive_depth) {
    std::string deep;
    for (int i = 0; i < 200; ++i) {
        deep += "*1\r\n";
    }
    deep += ":1\r\n";
    value out;
    std::size_t consumed = 0;
    CRPC_CHECK_FALSE(decode_value(deep, out, consumed));
    CRPC_CHECK_EQ(consumed, std::size_t{0});
}

CRPC_TEST(types_encode_decode_round_trip) {
    const value values[] = {
        vnull(),
        vbool(true),
        vbool(false),
        vint(0),
        vint(-1234567890123LL),
        vreal(-2.25),
        vstr(""),
        vstr("binary\r\npayload"),
        varray({vint(1), vstr("a"), vnull(), vbool(true)}),
        vmap({{vstr("k1"), vint(1)}, {vstr("k2"), varray({vint(2)})}}),
        vset({vint(1), vreal(2.5)}),
        vpush({vstr("a"), vstr("b")}),
        verr("ERR round trip"),
    };
    for (const value& v : values) {
        const std::string encoded = encode_value(v);
        value decoded;
        std::size_t consumed = 0;
        CRPC_CHECK(decode_value(encoded, decoded, consumed));
        CRPC_CHECK_EQ(consumed, encoded.size());
        CRPC_CHECK(values_equal(decoded, v));
        CRPC_CHECK_EQ(encode_value(decoded), encoded);
    }
}

// --- typed reply projections ------------------------------------------------

CRPC_TEST(types_decode_scalar_projections) {
    CRPC_CHECK(decode_bool(decode_one("#t\r\n")));
    CRPC_CHECK(decode_bool(decode_one(":1\r\n")));
    CRPC_CHECK_FALSE(decode_bool(decode_one("#f\r\n")));
    CRPC_CHECK_FALSE(decode_bool(decode_one(":0\r\n")));

    CRPC_CHECK_EQ(decode_int(decode_one(":42\r\n")), i64{42});
    CRPC_CHECK_EQ(decode_int(decode_one(":-7\r\n")), i64{-7});

    CRPC_CHECK_EQ(decode_real(decode_one(",1.5\r\n")), 1.5);
    CRPC_CHECK_EQ(decode_real(decode_one(":3\r\n")), 3.0);

    CRPC_CHECK_EQ(decode_string(decode_one("$3\r\nfoo\r\n")), std::string("foo"));
    CRPC_CHECK_EQ(decode_string(decode_one("+OK\r\n")), std::string("OK"));

    value wrong = vstr("nope");
    CRPC_CHECK(throws_bad_variant([&wrong] { (void)decode_int(wrong); }));
    CRPC_CHECK(throws_bad_variant([&wrong] { (void)decode_real(wrong); }));
    value not_string = vint(1);
    CRPC_CHECK(throws_bad_variant([&not_string] { (void)decode_string(not_string); }));
}

CRPC_TEST(types_decode_string_array) {
    std::vector<std::string> items =
        decode_string_array(decode_one("*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n"));
    CRPC_CHECK_EQ(items.size(), std::size_t{3});
    CRPC_CHECK_EQ(items[0], std::string("a"));
    CRPC_CHECK_EQ(items[2], std::string("c"));

    CRPC_CHECK_EQ(decode_string_array(decode_one("*0\r\n")).size(), std::size_t{0});

    value not_array = vint(1);
    CRPC_CHECK(throws_bad_variant([&not_array] { (void)decode_string_array(not_array); }));
    value mixed = varray({vstr("a"), vint(1)});
    CRPC_CHECK(throws_bad_variant([&mixed] { (void)decode_string_array(mixed); }));
}

CRPC_TEST(types_decode_pairs_accepts_map_and_flat_array) {
    std::vector<std::pair<std::string, std::string>> from_map =
        decode_pairs(decode_one("%2\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nk2\r\n$2\r\nv2\r\n"));
    CRPC_CHECK_EQ(from_map.size(), std::size_t{2});
    CRPC_CHECK_EQ(from_map[0].first, std::string("k"));
    CRPC_CHECK_EQ(from_map[0].second, std::string("v"));
    CRPC_CHECK_EQ(from_map[1].second, std::string("v2"));

    std::vector<std::pair<std::string, std::string>> from_flat = decode_pairs(
        decode_one("*4\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nk2\r\n$2\r\nv2\r\n"));
    CRPC_CHECK_EQ(from_flat.size(), std::size_t{2});
    CRPC_CHECK_EQ(from_flat[1].first, std::string("k2"));
}

CRPC_TEST(types_decode_real_array) {
    std::vector<double> values =
        decode_real_array(decode_one("*3\r\n,1.5\r\n:2\r\n,-0.25\r\n"));
    CRPC_CHECK_EQ(values.size(), std::size_t{3});
    CRPC_CHECK_EQ(values[0], 1.5);
    CRPC_CHECK_EQ(values[1], 2.0);
    CRPC_CHECK_EQ(values[2], -0.25);
}

CRPC_TEST(types_decode_scored_members) {
    std::vector<scored_member> flat =
        decode_scored_members(decode_one("*4\r\n$1\r\na\r\n,1.5\r\n$1\r\nb\r\n:2\r\n"));
    CRPC_CHECK_EQ(flat.size(), std::size_t{2});
    CRPC_CHECK_EQ(flat[0].member, std::string("a"));
    CRPC_CHECK_EQ(flat[0].score, 1.5);
    CRPC_CHECK_EQ(flat[1].member, std::string("b"));
    CRPC_CHECK_EQ(flat[1].score, 2.0);

    std::vector<scored_member> pairs = decode_scored_members(decode_one(
        "*2\r\n*2\r\n$1\r\na\r\n,1.5\r\n*2\r\n$1\r\nb\r\n:2\r\n"));
    CRPC_CHECK_EQ(pairs.size(), std::size_t{2});
    CRPC_CHECK_EQ(pairs[1].member, std::string("b"));
}

CRPC_TEST(types_decode_stream_entries) {
    std::vector<redis_stream_entry> entries = decode_stream_entries(decode_one(
        "*1\r\n*2\r\n$3\r\n1-1\r\n*4\r\n$1\r\nf\r\n$1\r\nv\r\n$1\r\ng\r\n$1\r\nw\r\n"));
    CRPC_CHECK_EQ(entries.size(), std::size_t{1});
    CRPC_CHECK_EQ(entries[0].id, std::string("1-1"));
    CRPC_CHECK_EQ(entries[0].fields.size(), std::size_t{2});
    CRPC_CHECK_EQ(entries[0].fields[0].first, std::string("f"));
    CRPC_CHECK_EQ(entries[0].fields[1].second, std::string("w"));
}

// --- command builders -------------------------------------------------------

CRPC_TEST(types_string_builders) {
    check_cmd({"SET", "k", "v"}, string_type::set("k", vstr("v")));
    check_cmd({"SET", "k", "5"}, string_type::set("k", vint(5)));
    check_cmd({"SET", "k", "1.5"}, string_type::set("k", vreal(1.5)));
    check_cmd({"SET", "k", "1"}, string_type::set("k", vbool(true)));
    check_cmd({"SET", "k", "v", "PX", "1000"},
              string_type::set("k", vstr("v"), std::chrono::milliseconds{1000}));
    check_cmd({"SETNX", "k", "v"}, string_type::setnx("k", vstr("v")));
    check_cmd({"SETEX", "k", "10", "v"},
              string_type::setex("k", std::chrono::seconds{10}, vstr("v")));
    check_cmd({"PSETEX", "k", "1500", "v"},
              string_type::psetex("k", std::chrono::milliseconds{1500}, vstr("v")));
    check_cmd({"GET", "k"}, string_type::get("k"));
    check_cmd({"GETSET", "k", "v"}, string_type::getset("k", vstr("v")));
    check_cmd({"GETDEL", "k"}, string_type::getdel("k"));
    check_cmd({"GETEX", "k"}, string_type::getex("k"));
    check_cmd({"GETEX", "k", "PX", "250"},
              string_type::getex("k", std::chrono::milliseconds{250}));
    check_cmd({"APPEND", "k", "v"}, string_type::append("k", "v"));
    check_cmd({"STRLEN", "k"}, string_type::strlen("k"));
    check_cmd({"INCR", "k"}, string_type::incr("k"));
    check_cmd({"DECR", "k"}, string_type::decr("k"));
    check_cmd({"INCRBY", "k", "5"}, string_type::incrby("k", 5));
    check_cmd({"DECRBY", "k", "5"}, string_type::decrby("k", 5));
    check_cmd({"INCRBYFLOAT", "k", "1.5"}, string_type::incrbyfloat("k", 1.5));
    check_cmd({"MGET", "a", "b", "c"}, string_type::mget({"a", "b", "c"}));
    check_cmd({"MSET", "a", "1", "b", "2"},
              string_type::mset({{"a", "1"}, {"b", "2"}}));
}

CRPC_TEST(types_list_builders) {
    check_cmd({"LPUSH", "k", "a", "b"}, list_type::lpush("k", {vstr("a"), vstr("b")}));
    check_cmd({"RPUSH", "k", "a"}, list_type::rpush("k", {vstr("a")}));
    check_cmd({"LPUSHX", "k", "a"}, list_type::lpushx("k", {vstr("a")}));
    check_cmd({"RPUSHX", "k", "a"}, list_type::rpushx("k", {vstr("a")}));
    check_cmd({"LPOP", "k"}, list_type::lpop("k"));
    check_cmd({"LPOP", "k", "2"}, list_type::lpop("k", 2));
    check_cmd({"RPOP", "k", "3"}, list_type::rpop("k", 3));
    check_cmd({"LRANGE", "k", "0", "-1"}, list_type::lrange("k", 0, -1));
    check_cmd({"LLEN", "k"}, list_type::llen("k"));
    check_cmd({"LINDEX", "k", "2"}, list_type::lindex("k", 2));
    check_cmd({"LSET", "k", "1", "v"}, list_type::lset("k", 1, vstr("v")));
    check_cmd({"LREM", "k", "2", "v"}, list_type::lrem("k", 2, vstr("v")));
    check_cmd({"LTRIM", "k", "0", "9"}, list_type::ltrim("k", 0, 9));
    check_cmd({"LPOS", "k", "v"}, list_type::lpos("k", vstr("v")));
    check_cmd({"LPOS", "k", "v", "RANK", "2", "COUNT", "3", "MAXLEN", "9"},
              list_type::lpos("k", vstr("v"), 2, 3, 9));
    check_cmd({"RPOPLPUSH", "a", "b"}, list_type::rpoplpush("a", "b"));
}

CRPC_TEST(types_hash_builders) {
    check_cmd({"HSET", "k", "f", "v"}, hash_type::hset("k", "f", vstr("v")));
    check_cmd({"HSET", "k", "f", "1", "g", "2"},
              hash_type::hset("k", {{"f", "1"}, {"g", "2"}}));
    check_cmd({"HSETNX", "k", "f", "v"}, hash_type::hsetnx("k", "f", vstr("v")));
    check_cmd({"HGET", "k", "f"}, hash_type::hget("k", "f"));
    check_cmd({"HDEL", "k", "f", "g"}, hash_type::hdel("k", {"f", "g"}));
    check_cmd({"HLEN", "k"}, hash_type::hlen("k"));
    check_cmd({"HEXISTS", "k", "f"}, hash_type::hexists("k", "f"));
    check_cmd({"HKEYS", "k"}, hash_type::hkeys("k"));
    check_cmd({"HVALS", "k"}, hash_type::hvals("k"));
    check_cmd({"HGETALL", "k"}, hash_type::hgetall("k"));
    check_cmd({"HMGET", "k", "f", "g"}, hash_type::hmget("k", {"f", "g"}));
    check_cmd({"HINCRBY", "k", "f", "3"}, hash_type::hincrby("k", "f", 3));
    check_cmd({"HINCRBYFLOAT", "k", "f", "1.5"},
              hash_type::hincrbyfloat("k", "f", 1.5));
    check_cmd({"HSTRLEN", "k", "f"}, hash_type::hstrlen("k", "f"));
    check_cmd({"HRANDFIELD", "k"}, hash_type::hrandfield("k"));
    check_cmd({"HRANDFIELD", "k", "2"}, hash_type::hrandfield("k", 2));
}

CRPC_TEST(types_set_builders) {
    check_cmd({"SADD", "k", "a", "b"}, set_type::sadd("k", {"a", "b"}));
    check_cmd({"SREM", "k", "a"}, set_type::srem("k", {"a"}));
    check_cmd({"SMEMBERS", "k"}, set_type::smembers("k"));
    check_cmd({"SISMEMBER", "k", "a"}, set_type::sismember("k", "a"));
    check_cmd({"SMISMEMBER", "k", "a", "b"}, set_type::smismember("k", {"a", "b"}));
    check_cmd({"SCARD", "k"}, set_type::scard("k"));
    check_cmd({"SINTER", "a", "b"}, set_type::sinter({"a", "b"}));
    check_cmd({"SUNION", "a", "b"}, set_type::sunion({"a", "b"}));
    check_cmd({"SDIFF", "a", "b"}, set_type::sdiff({"a", "b"}));
    check_cmd({"SINTERSTORE", "d", "a", "b"}, set_type::sinterstore("d", {"a", "b"}));
    check_cmd({"SUNIONSTORE", "d", "a"}, set_type::sunionstore("d", {"a"}));
    check_cmd({"SDIFFSTORE", "d", "a"}, set_type::sdiffstore("d", {"a"}));
    check_cmd({"SPOP", "k"}, set_type::spop("k"));
    check_cmd({"SPOP", "k", "2"}, set_type::spop("k", 2));
    check_cmd({"SRANDMEMBER", "k", "2"}, set_type::srandmember("k", 2));
    check_cmd({"SMOVE", "a", "b", "m"}, set_type::smove("a", "b", "m"));
}

CRPC_TEST(types_zset_builders) {
    zset_type::member m1{1.5, "a"};
    zset_type::member m2{2.0, "b"};
    check_cmd({"ZADD", "k", "1.5", "a", "2", "b"},
              zset_type::zadd("k", {m1, m2}));
    check_cmd({"ZADD", "k", "INCR", "1.5", "a"},
              zset_type::zadd_incr("k", 1.5, "a"));
    check_cmd({"ZREM", "k", "a", "b"}, zset_type::zrem("k", {"a", "b"}));
    check_cmd({"ZSCORE", "k", "a"}, zset_type::zscore("k", "a"));
    check_cmd({"ZMSCORE", "k", "a", "b"}, zset_type::zmscore("k", {"a", "b"}));
    check_cmd({"ZCARD", "k"}, zset_type::zcard("k"));
    check_cmd({"ZCOUNT", "k", "-inf", "+inf"}, zset_type::zcount("k", "-inf", "+inf"));
    check_cmd({"ZINCRBY", "k", "1.5", "a"}, zset_type::zincrby("k", 1.5, "a"));
    check_cmd({"ZRANGE", "k", "0", "-1"}, zset_type::zrange("k", 0, -1));
    check_cmd({"ZRANGE", "k", "0", "-1", "WITHSCORES"},
              zset_type::zrange("k", 0, -1, true));
    check_cmd({"ZRANGEBYSCORE", "k", "1", "2"},
              zset_type::zrangebyscore("k", "1", "2"));
    check_cmd({"ZRANGEBYSCORE", "k", "1", "2", "WITHSCORES", "LIMIT", "5", "10"},
              zset_type::zrangebyscore("k", "1", "2", true, 5, 10));
    check_cmd({"ZREVRANGE", "k", "0", "-1"},
              zset_type::zrevrange("k", 0, -1));
    check_cmd({"ZRANK", "k", "a"}, zset_type::zrank("k", "a"));
    check_cmd({"ZREVRANK", "k", "a"}, zset_type::zrevrank("k", "a"));
    check_cmd({"ZREMRANGEBYRANK", "k", "0", "1"},
              zset_type::zremrangebyrank("k", 0, 1));
    check_cmd({"ZREMRANGEBYSCORE", "k", "1", "2"},
              zset_type::zremrangebyscore("k", "1", "2"));
    check_cmd({"ZRANDMEMBER", "k", "2"}, zset_type::zrandmember("k", 2));
}

CRPC_TEST(types_stream_builders) {
    stream_type::fields fields{{"f", "v"}};
    check_cmd({"XADD", "k", "1-1", "f", "v"},
              stream_type::xadd("k", "1-1", fields));
    check_cmd({"XLEN", "k"}, stream_type::xlen("k"));
    check_cmd({"XDEL", "k", "1-1", "2-2"}, stream_type::xdel("k", {"1-1", "2-2"}));
    check_cmd({"XRANGE", "k", "-", "+"}, stream_type::xrange("k", "-", "+"));
    check_cmd({"XRANGE", "k", "-", "+", "COUNT", "5"},
              stream_type::xrange("k", "-", "+", 5));
    check_cmd({"XREVRANGE", "k", "+", "-"}, stream_type::xrevrange("k", "+", "-"));
    check_cmd({"XREAD", "COUNT", "5", "BLOCK", "100", "STREAMS", "k", "0"},
              stream_type::xread(5, 100, {{"k", "0"}}));
    check_cmd({"XGROUP", "CREATE", "k", "g", "0"},
              stream_type::xgroup_create("k", "g", "0"));
    check_cmd({"XGROUP", "CREATE", "k", "g", "0", "MKSTREAM"},
              stream_type::xgroup_create("k", "g", "0", true));
    check_cmd({"XGROUP", "DESTROY", "k", "g"},
              stream_type::xgroup_destroy("k", "g"));
    check_cmd({"XGROUP", "SETID", "k", "g", "0"},
              stream_type::xgroup_setid("k", "g", "0"));
    check_cmd({"XACK", "k", "g", "1-1"}, stream_type::xack("k", "g", {"1-1"}));
    check_cmd({"XPENDING", "k", "g"}, stream_type::xpending("k", "g"));
}

CRPC_TEST(types_bitmap_hyperloglog_geo_builders) {
    check_cmd({"SETBIT", "k", "7", "1"}, bitmap_type::setbit("k", 7, true));
    check_cmd({"SETBIT", "k", "7", "0"}, bitmap_type::setbit("k", 7, false));
    check_cmd({"GETBIT", "k", "7"}, bitmap_type::getbit("k", 7));
    check_cmd({"BITCOUNT", "k"}, bitmap_type::bitcount("k"));
    check_cmd({"BITCOUNT", "k", "0", "9"}, bitmap_type::bitcount("k", 0, 9));
    check_cmd({"BITCOUNT", "k", "0", "9", "BIT"},
              bitmap_type::bitcount("k", 0, 9, true));
    check_cmd({"BITPOS", "k", "1"}, bitmap_type::bitpos("k", true));
    check_cmd({"BITPOS", "k", "0", "0", "9"}, bitmap_type::bitpos("k", false, 0, 9));
    check_cmd({"BITOP", "AND", "d", "a", "b"},
              bitmap_type::bitop("AND", "d", {"a", "b"}));

    check_cmd({"PFADD", "k"}, hyperloglog_type::pfadd("k"));
    check_cmd({"PFADD", "k", "a", "b"},
              hyperloglog_type::pfadd("k", std::vector<std::string>{"a", "b"}));
    check_cmd({"PFCOUNT", "a", "b"}, hyperloglog_type::pfcount({"a", "b"}));
    check_cmd({"PFMERGE", "d", "a", "b"}, hyperloglog_type::pfmerge("d", {"a", "b"}));

    geo_type::point p{1.0, 2.0, "m"};
    check_cmd({"GEOADD", "k", "1", "2", "m"}, geo_type::geoadd("k", {p}));
    check_cmd({"GEOPOS", "k", "m"}, geo_type::geopos("k", {"m"}));
    check_cmd({"GEODIST", "k", "a", "b"}, geo_type::geodist("k", "a", "b"));
    check_cmd({"GEODIST", "k", "a", "b", "km"},
              geo_type::geodist("k", "a", "b", std::string("km")));
    check_cmd({"GEOHASH", "k", "m"}, geo_type::geohash("k", {"m"}));
    check_cmd({"GEORADIUS", "k", "1", "2", "100", "km"},
              geo_type::georadius("k", 1.0, 2.0, 100.0, "km"));
    check_cmd({"GEORADIUS", "k", "1", "2", "100", "km", "WITHDIST"},
              geo_type::georadius("k", 1.0, 2.0, 100.0, "km", true));
    check_cmd({"GEORADIUSBYMEMBER", "k", "m", "100", "km"},
              geo_type::georadiusbymember("k", "m", 100.0, "km"));
}

// --- iterators --------------------------------------------------------------

CRPC_TEST(types_string_iterator) {
    const array_value items{vstr("a"), vstr("b"), vstr("c")};
    string_iterator it(items);
    const string_iterator end;
    std::vector<std::string> seen;
    for (; it != end; ++it) {
        seen.push_back(*it);
    }
    CRPC_CHECK_EQ(seen.size(), std::size_t{3});
    CRPC_CHECK_EQ(seen[0], std::string("a"));
    CRPC_CHECK_EQ(seen[2], std::string("c"));
    CRPC_CHECK(it == end);
    CRPC_CHECK_FALSE(it != end);

    string_iterator empty;
    CRPC_CHECK(empty == string_iterator());
    CRPC_CHECK_FALSE(empty != string_iterator());
}

CRPC_TEST(types_zset_iterator) {
    const array_value items{vstr("a"), vreal(1.5), vstr("b"), vint(2)};
    zset_iterator it(items);
    const zset_iterator end;
    std::vector<scored_member> seen;
    for (; it != end; ++it) {
        seen.push_back(*it);
    }
    CRPC_CHECK_EQ(seen.size(), std::size_t{2});
    CRPC_CHECK_EQ(seen[0].member, std::string("a"));
    CRPC_CHECK_EQ(seen[0].score, 1.5);
    CRPC_CHECK_EQ(seen[1].member, std::string("b"));
    CRPC_CHECK_EQ(seen[1].score, 2.0);
    CRPC_CHECK(it == end);
}

CRPC_TEST(types_stream_iterator) {
    const array_value items{
        varray({vstr("1-1"), varray({vstr("f"), vstr("v")})}),
        varray({vstr("2-2"), varray({vstr("g"), vstr("w")})}),
    };
    stream_iterator it(items);
    const stream_iterator end;
    std::vector<redis_stream_entry> seen;
    for (; it != end; ++it) {
        seen.push_back(*it);
    }
    CRPC_CHECK_EQ(seen.size(), std::size_t{2});
    CRPC_CHECK_EQ(seen[0].id, std::string("1-1"));
    CRPC_CHECK_EQ(seen[1].id, std::string("2-2"));
    CRPC_CHECK_EQ(seen[1].fields[0].first, std::string("g"));
    CRPC_CHECK(it == end);
}

CRPC_TEST_MAIN()
