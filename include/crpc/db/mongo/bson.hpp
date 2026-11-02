// CRPC -- BSON, the document format MongoDB speaks.
//
// BSON is a length-prefixed binary encoding: a document is an int32 byte count (which
// includes itself and the trailing NUL), then a sequence of elements, then 0x00. Every
// element is a type byte, a NUL-terminated key, and a value whose shape depends on the
// type. Two details account for most decode bugs:
//
//   * the length covers the whole document *including* the final NUL, so a decoder that
//     adds one at the end runs past the buffer;
//   * a string carries its own int32 length *including* its terminating NUL, so the
//     usable text is one byte shorter than the number in front of it.
//
// The representation here is deliberately explicit rather than variant-based: a decoded
// document keeps the wire order of its fields (MongoDB treats the first field specially
// in some commands) and every value keeps its exact wire type, because re-encoding a
// value as a different type changes what the server stores.
#ifndef CRPC_DB_MONGO_BSON_HPP
#define CRPC_DB_MONGO_BSON_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace mongo {

// The element type bytes. The names follow the specification, with a trailing
// underscore where the name would collide with a C++ keyword.
enum class bson_type : std::uint8_t {
    real = 0x01,
    text = 0x02,
    document = 0x03,
    array = 0x04,
    binary = 0x05,
    undefined = 0x06,
    object_id = 0x07,
    boolean = 0x08,
    datetime = 0x09,
    null = 0x0A,
    regex = 0x0B,
    db_pointer = 0x0C,
    javascript = 0x0D,
    symbol = 0x0E,
    javascript_scope = 0x0F,
    int32 = 0x10,
    timestamp = 0x11,
    int64 = 0x12,
    decimal128 = 0x13,
    min_key = 0xFF,
    max_key = 0x7F
};

class bson_document;

// One value. `type` says which members are meaningful.
// Exported: the constructors, the copy/move operations and the `make_*` factories are
// all defined out of line in bson.cpp, so a consumer linking against the shared build
// needs them in the import library. The static build hid this by linking the object
// files directly; `test_db_mongo_bson` is the consumer that reported it.
struct CRPC_API bson_value {
    bson_type type = bson_type::null;

    // real
    double real = 0.0;
    // int32, int64, datetime (milliseconds since the epoch) and timestamp (raw
    // seconds/increment pair) all land here; `type` says which interpretation applies.
    std::int64_t integer = 0;
    // text, javascript, symbol, regex (pattern in `text`, options in `extra`)
    std::string text;
    std::string extra;
    // binary payload; object_id is 12 bytes; decimal128 is 16
    std::vector<std::byte> bytes;
    std::uint8_t subtype = 0;
    bool boolean = false;
    // array elements, in order
    std::vector<bson_value> array;
    // embedded document
    std::unique_ptr<bson_document> document;

    bson_value();
    ~bson_value();
    bson_value(const bson_value& other);
    bson_value& operator=(const bson_value& other);
    bson_value(bson_value&& other) noexcept;
    bson_value& operator=(bson_value&& other) noexcept;

    // Factories. Named rather than constructors so the call site says what it builds.
    [[nodiscard]] static bson_value make_real(double value);
    [[nodiscard]] static bson_value make_int32(std::int32_t value);
    [[nodiscard]] static bson_value make_int64(std::int64_t value);
    [[nodiscard]] static bson_value make_text(std::string value);
    [[nodiscard]] static bson_value make_boolean(bool value);
    [[nodiscard]] static bson_value make_null();
    [[nodiscard]] static bson_value make_datetime(std::int64_t millis);
    [[nodiscard]] static bson_value make_binary(std::vector<std::byte> payload,
                                                std::uint8_t binary_subtype = 0);
    // 12 bytes, as the specification defines.
    [[nodiscard]] static bson_value make_object_id(std::vector<std::byte> payload);
    [[nodiscard]] static bson_value make_document(bson_document value);
    [[nodiscard]] static bson_value make_array(std::vector<bson_value> values);
};

// An ordered document: field order is preserved on encode, because it is part of the
// value for commands and for indexes.
class CRPC_API bson_document {
public:
    using field = std::pair<std::string, bson_value>;

    bson_document() = default;

    [[nodiscard]] std::size_t size() const noexcept { return fields_.size(); }
    [[nodiscard]] bool empty() const noexcept { return fields_.empty(); }
    [[nodiscard]] const std::vector<field>& fields() const noexcept { return fields_; }
    [[nodiscard]] std::vector<field>& fields() noexcept { return fields_; }

    // Appends a field. A duplicate key is allowed by the format and by MongoDB, so this
    // does not replace an existing one.
    bson_document& append(std::string key, bson_value value);

    // The first field named `key`, or nullptr. `nullptr` and "present with a null value"
    // are different answers, so this returns the value rather than a bool.
    [[nodiscard]] const bson_value* find(std::string_view key) const noexcept;
    [[nodiscard]] bson_value* find(std::string_view key) noexcept;

    // Typed reads. Each returns false when the field is absent or has another type,
    // rather than coercing: a server that answers with a string where an int was
    // expected is a protocol error, not something to paper over.
    [[nodiscard]] bool get_text(std::string_view key, std::string& out) const;
    [[nodiscard]] bool get_int64(std::string_view key, std::int64_t& out) const;
    [[nodiscard]] bool get_real(std::string_view key, double& out) const;
    [[nodiscard]] bool get_boolean(std::string_view key, bool& out) const;
    [[nodiscard]] const bson_document* get_document(std::string_view key) const;

    // The total encoded size, which the wire protocol needs before encoding.
    [[nodiscard]] std::size_t encoded_size() const noexcept;

private:
    std::vector<field> fields_;
};

// Encodes a document, appending to `out`. The result is exactly `encoded_size()` bytes.
[[nodiscard]] CRPC_API status encode_bson(std::vector<std::byte>& out,
                                          const bson_document& document);

// Decodes one document from the front of `data`, reporting how many bytes it used.
// A truncated or malformed document is an error, never a partial value: half a document
// is indistinguishable from a different document.
[[nodiscard]] CRPC_API status decode_bson(span<const std::byte> data,
                                          bson_document& out, std::size_t& consumed);

// The size the document at the front of `data` claims. Useful for framing before
// decoding; a claim smaller than five bytes is refused.
[[nodiscard]] CRPC_API result<std::size_t> peek_bson_size(span<const std::byte> data);

// Convenience: encode one document into a fresh vector.
[[nodiscard]] CRPC_API result<std::vector<std::byte>> encode_bson(const bson_document& document);

} // namespace mongo
} // namespace db
} // namespace crpc

#endif // CRPC_DB_MONGO_BSON_HPP