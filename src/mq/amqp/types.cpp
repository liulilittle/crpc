// CRPC -- AMQP field-table value types.
#include <crpc/mq/amqp/types.hpp>

#include <utility>

namespace crpc {
namespace mq {
namespace amqp {

field_value::field_value() = default;
field_value::~field_value() = default;

field_value::field_value(const field_value& other)
    : type(other.type), boolean(other.boolean), signed_integer(other.signed_integer),
      unsigned_integer(other.unsigned_integer), real(other.real),
      decimal_scale(other.decimal_scale), decimal_value(other.decimal_value),
      text(other.text), timestamp(other.timestamp), array(other.array),
      bytes(other.bytes) {
    if (other.table != nullptr) {
        table = std::unique_ptr<field_table>(new field_table(*other.table));
    }
}

field_value& field_value::operator=(const field_value& other) {
    if (this == &other) {
        return *this;
    }
    type = other.type;
    boolean = other.boolean;
    signed_integer = other.signed_integer;
    unsigned_integer = other.unsigned_integer;
    real = other.real;
    decimal_scale = other.decimal_scale;
    decimal_value = other.decimal_value;
    text = other.text;
    timestamp = other.timestamp;
    array = other.array;
    bytes = other.bytes;
    table.reset();
    if (other.table != nullptr) {
        table = std::unique_ptr<field_table>(new field_table(*other.table));
    }
    return *this;
}

field_value::field_value(field_value&& other) noexcept = default;
field_value& field_value::operator=(field_value&& other) noexcept = default;

bool field_value::operator==(const field_value& other) const noexcept {
    if (type != other.type) {
        return false;
    }
    switch (type) {
    case field_type::boolean:
        return boolean == other.boolean;
    case field_type::int8:
    case field_type::int16:
    case field_type::int32:
    case field_type::int64:
        return signed_integer == other.signed_integer;
    case field_type::uint8:
    case field_type::uint16:
    case field_type::uint32:
    case field_type::uint64:
        return unsigned_integer == other.unsigned_integer;
    case field_type::float32:
    case field_type::float64:
        return real == other.real;
    case field_type::decimal:
        return decimal_scale == other.decimal_scale && decimal_value == other.decimal_value;
    case field_type::short_string:
    case field_type::long_string:
        return text == other.text;
    case field_type::array:
        return array == other.array;
    case field_type::timestamp:
        return timestamp == other.timestamp;
    case field_type::table:
        if (table == nullptr || other.table == nullptr) {
            return table == other.table;
        }
        return *table == *other.table;
    case field_type::null_value:
        return true;
    case field_type::bytes:
        return bytes == other.bytes;
    }
    return false;
}

namespace {

[[nodiscard]] field_value make_signed(field_type type, std::int64_t value) {
    field_value out;
    out.type = type;
    out.signed_integer = value;
    return out;
}

[[nodiscard]] field_value make_unsigned(field_type type, std::uint64_t value) {
    field_value out;
    out.type = type;
    out.unsigned_integer = value;
    return out;
}

} // namespace

field_value field_value::make_bool(bool value) {
    field_value out;
    out.type = field_type::boolean;
    out.boolean = value;
    return out;
}

field_value field_value::make_int8(std::int8_t value) {
    return make_signed(field_type::int8, value);
}

field_value field_value::make_uint8(std::uint8_t value) {
    return make_unsigned(field_type::uint8, value);
}

field_value field_value::make_int16(std::int16_t value) {
    return make_signed(field_type::int16, value);
}

field_value field_value::make_uint16(std::uint16_t value) {
    return make_unsigned(field_type::uint16, value);
}

field_value field_value::make_int32(std::int32_t value) {
    return make_signed(field_type::int32, value);
}

field_value field_value::make_uint32(std::uint32_t value) {
    return make_unsigned(field_type::uint32, value);
}

field_value field_value::make_int64(std::int64_t value) {
    return make_signed(field_type::int64, value);
}

field_value field_value::make_uint64(std::uint64_t value) {
    return make_unsigned(field_type::uint64, value);
}

field_value field_value::make_float(float value) {
    field_value out;
    out.type = field_type::float32;
    out.real = static_cast<double>(value);
    return out;
}

field_value field_value::make_double(double value) {
    field_value out;
    out.type = field_type::float64;
    out.real = value;
    return out;
}

field_value field_value::make_decimal(std::uint8_t scale, std::uint32_t value) {
    field_value out;
    out.type = field_type::decimal;
    out.decimal_scale = scale;
    out.decimal_value = value;
    return out;
}

field_value field_value::make_short_string(std::string value) {
    field_value out;
    out.type = field_type::short_string;
    out.text = std::move(value);
    return out;
}

field_value field_value::make_string(std::string value) {
    field_value out;
    out.type = field_type::long_string;
    out.text = std::move(value);
    return out;
}

field_value field_value::make_array(std::vector<field_value> values) {
    field_value out;
    out.type = field_type::array;
    out.array = std::move(values);
    return out;
}

field_value field_value::make_timestamp(std::uint64_t value) {
    field_value out;
    out.type = field_type::timestamp;
    out.timestamp = value;
    return out;
}

field_value field_value::make_table(field_table value) {
    field_value out;
    out.type = field_type::table;
    out.table = std::unique_ptr<field_table>(new field_table(std::move(value)));
    return out;
}

field_value field_value::make_null() {
    field_value out;
    out.type = field_type::null_value;
    return out;
}

field_value field_value::make_bytes(std::vector<std::byte> value) {
    field_value out;
    out.type = field_type::bytes;
    out.bytes = std::move(value);
    return out;
}

void field_table::set(std::string key, field_value value) {
    for (auto& entry : entries_) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    }
    entries_.emplace_back(std::move(key), std::move(value));
}

const field_value* field_table::find(std::string_view key) const noexcept {
    for (const auto& entry : entries_) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

bool operator==(const field_table& left, const field_table& right) noexcept {
    return left.entries() == right.entries();
}

bool operator!=(const field_table& left, const field_table& right) noexcept {
    return !(left == right);
}

bool operator==(const message_properties& left, const message_properties& right) noexcept {
    return left.content_type == right.content_type &&
           left.content_encoding == right.content_encoding &&
           left.headers == right.headers && left.delivery_mode == right.delivery_mode &&
           left.priority == right.priority &&
           left.correlation_id == right.correlation_id && left.reply_to == right.reply_to &&
           left.expiration == right.expiration && left.message_id == right.message_id &&
           left.timestamp == right.timestamp && left.type == right.type &&
           left.user_id == right.user_id && left.app_id == right.app_id &&
           left.cluster_id == right.cluster_id;
}

bool operator!=(const message_properties& left, const message_properties& right) noexcept {
    return !(left == right);
}

} // namespace amqp
} // namespace mq
} // namespace crpc