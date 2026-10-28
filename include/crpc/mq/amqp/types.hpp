// CRPC -- AMQP 0-9-1 data types shared by the codec and the connection.
//
// An AMQP field table is a recursive value type: a table contains values, an
// array contains values, and a value may itself be a table or an array. The
// variant below keeps the *wire* type of every value instead of collapsing the
// integer widths into one `int64_t`. That matters for a round trip: a server
// that sent an `I` (int32) must be able to receive the same `I` back, and a
// re-encoder that promoted it to `L` would change what the peer sees even though
// the numeric value is unchanged.
//
// The recursive table is held through a `unique_ptr` with a hand-written copy,
// so `field_value` keeps value semantics. Sharing the child table between two
// copies would make an edit through one copy visible through the other.
#ifndef CRPC_MQ_AMQP_TYPES_HPP
#define CRPC_MQ_AMQP_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>

namespace crpc {
namespace mq {
namespace amqp {

// The type octet of a field-table value. The numeric value is the ASCII letter
// from the AMQP 0-9-1 specification, so a captured frame can be compared with
// the specification byte for byte.
enum class field_type : std::uint8_t {
    boolean = 't',
    int8 = 'b',
    uint8 = 'B',
    int16 = 'U',
    uint16 = 'u',
    int32 = 'I',
    uint32 = 'i',
    int64 = 'L',
    uint64 = 'l',
    float32 = 'f',
    float64 = 'd',
    decimal = 'D',
    short_string = 's',
    long_string = 'S',
    array = 'A',
    timestamp = 'T',
    table = 'F',
    null_value = 'V',
    bytes = 'x'
};

class field_table;

// One field-table value. Which payload member is meaningful is selected by
// `type`; the other members are left at their defaults.
class CRPC_API field_value {
public:
    field_type type = field_type::null_value;
    bool boolean = false;
    std::int64_t signed_integer = 0;
    std::uint64_t unsigned_integer = 0;
    double real = 0.0;
    std::uint8_t decimal_scale = 0;
    std::uint32_t decimal_value = 0;
    // Short and long strings, and the raw form of a byte array.
    std::string text;
    std::uint64_t timestamp = 0;
    std::vector<field_value> array;
    std::unique_ptr<field_table> table;
    std::vector<std::byte> bytes;

    field_value();
    ~field_value();
    field_value(const field_value& other);
    field_value& operator=(const field_value& other);
    field_value(field_value&& other) noexcept;
    field_value& operator=(field_value&& other) noexcept;

    [[nodiscard]] bool operator==(const field_value& other) const noexcept;
    [[nodiscard]] bool operator!=(const field_value& other) const noexcept {
        return !(*this == other);
    }

    static field_value make_bool(bool value);
    static field_value make_int8(std::int8_t value);
    static field_value make_uint8(std::uint8_t value);
    static field_value make_int16(std::int16_t value);
    static field_value make_uint16(std::uint16_t value);
    static field_value make_int32(std::int32_t value);
    static field_value make_uint32(std::uint32_t value);
    static field_value make_int64(std::int64_t value);
    static field_value make_uint64(std::uint64_t value);
    static field_value make_float(float value);
    static field_value make_double(double value);
    static field_value make_decimal(std::uint8_t scale, std::uint32_t value);
    static field_value make_short_string(std::string value);
    static field_value make_string(std::string value);
    static field_value make_array(std::vector<field_value> values);
    static field_value make_timestamp(std::uint64_t value);
    static field_value make_table(field_table value);
    static field_value make_null();
    static field_value make_bytes(std::vector<std::byte> value);
};

// An ordered key/value list. AMQP field tables are ordered on the wire and a
// caller may care about that order (RabbitMQ echoes headers to consumers in the
// order they were published), so this is a list rather than a map.
class CRPC_API field_table {
public:
    using entry_type = std::pair<std::string, field_value>;

    field_table() = default;

    void set(std::string key, field_value value);
    [[nodiscard]] const field_value* find(std::string_view key) const noexcept;
    [[nodiscard]] bool contains(std::string_view key) const noexcept {
        return find(key) != nullptr;
    }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    void clear() noexcept { entries_.clear(); }

    [[nodiscard]] const std::vector<entry_type>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::vector<entry_type>& entries() noexcept { return entries_; }

private:
    std::vector<entry_type> entries_;
};

[[nodiscard]] CRPC_API bool operator==(const field_table& left,
                                       const field_table& right) noexcept;
[[nodiscard]] CRPC_API bool operator!=(const field_table& left,
                                       const field_table& right) noexcept;

// The Basic content properties. A scalar that has no meaningful "absent" value
// is optional: `delivery_mode == 0` could mean unset or a (nonsensical) zero, and
// the property flags must say which one the peer sent.
struct message_properties {
    std::string content_type;
    std::string content_encoding;
    field_table headers;
    std::optional<std::uint8_t> delivery_mode;
    std::optional<std::uint8_t> priority;
    std::string correlation_id;
    std::string reply_to;
    std::string expiration;
    std::string message_id;
    std::optional<std::uint64_t> timestamp;
    std::string type;
    std::string user_id;
    std::string app_id;
    std::string cluster_id;
};

[[nodiscard]] CRPC_API bool operator==(const message_properties& left,
                                       const message_properties& right) noexcept;
[[nodiscard]] CRPC_API bool operator!=(const message_properties& left,
                                       const message_properties& right) noexcept;

// One Basic.Deliver. `present` distinguishes "the wait timed out" from a real
// delivery, so a caller does not need an optional wrapper around `result<T>`.
struct delivery {
    bool present = false;
    std::string consumer_tag;
    std::uint64_t delivery_tag = 0;
    bool redelivered = false;
    std::string exchange;
    std::string routing_key;
    message_properties properties;
    std::vector<std::byte> body;
};

// One Basic.Get. `empty` is the Basic.GetEmpty outcome and is a normal result,
// not an error.
struct get_result {
    bool empty = true;
    std::uint64_t delivery_tag = 0;
    bool redelivered = false;
    std::string exchange;
    std::string routing_key;
    std::uint32_t message_count = 0;
    message_properties properties;
    std::vector<std::byte> body;
};

// One Basic.Return: a mandatory publication the broker could not route.
struct returned_message {
    bool present = false;
    std::uint16_t reply_code = 0;
    std::string reply_text;
    std::string exchange;
    std::string routing_key;
    message_properties properties;
    std::vector<std::byte> body;
};

// The result of Queue.Declare. `queue` is the server-generated name when the
// caller declared an empty one.
struct queue_declare_result {
    std::string queue;
    std::uint32_t message_count = 0;
    std::uint32_t consumer_count = 0;
};

} // namespace amqp
} // namespace mq
} // namespace crpc

#endif // CRPC_MQ_AMQP_TYPES_HPP