// CRPC -- BSON codec.
#include <crpc/db/mongo/bson.hpp>

#include <cstring>
#include <limits>
#include <utility>

namespace crpc {
namespace db {
namespace mongo {
namespace {

void put_u8(std::vector<std::byte>& out, std::uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
}

void put_i32(std::vector<std::byte>& out, std::int32_t value) {
    const std::uint32_t bits = static_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) {
        put_u8(out, static_cast<std::uint8_t>((bits >> (8 * i)) & 0xffu));
    }
}

void put_i64(std::vector<std::byte>& out, std::int64_t value) {
    const std::uint64_t bits = static_cast<std::uint64_t>(value);
    for (int i = 0; i < 8; ++i) {
        put_u8(out, static_cast<std::uint8_t>((bits >> (8 * i)) & 0xffu));
    }
}

void put_cstring(std::vector<std::byte>& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    put_u8(out, 0);
}

// A string value: int32 length including the terminating NUL, then the bytes, then NUL.
void put_string(std::vector<std::byte>& out, std::string_view text) {
    put_i32(out, static_cast<std::int32_t>(text.size() + 1));
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    put_u8(out, 0);
}

void put_double(std::vector<std::byte>& out, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_i64(out, static_cast<std::int64_t>(bits));
}

// A cursor over the input with bounds checking. Every read goes through it so a
// malformed length cannot make the decoder read past the buffer.
class reader {
public:
    explicit reader(span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] bool need(std::size_t count) const noexcept {
        return offset_ <= data_.size() && data_.size() - offset_ >= count;
    }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }

    [[nodiscard]] std::uint8_t u8() noexcept {
        if (!need(1)) {
            failed_ = true;
            return 0;
        }
        return static_cast<std::uint8_t>(data_[offset_++]);
    }

    [[nodiscard]] std::int32_t i32() noexcept {
        if (!need(4)) {
            failed_ = true;
            return 0;
        }
        std::uint32_t bits = 0;
        for (int i = 0; i < 4; ++i) {
            bits |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[offset_ + static_cast<std::size_t>(i)]))
                    << (8 * i);
        }
        offset_ += 4;
        return static_cast<std::int32_t>(bits);
    }

    [[nodiscard]] std::int64_t i64() noexcept {
        if (!need(8)) {
            failed_ = true;
            return 0;
        }
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            bits |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data_[offset_ + static_cast<std::size_t>(i)]))
                    << (8 * i);
        }
        offset_ += 8;
        return static_cast<std::int64_t>(bits);
    }

    [[nodiscard]] double real() noexcept {
        const std::int64_t bits = i64();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // Reads up to the next NUL. A missing terminator is a failure: without it the key
    // would run into the value bytes.
    [[nodiscard]] std::string cstring() {
        std::string out;
        for (;;) {
            if (!need(1)) {
                failed_ = true;
                return out;
            }
            const char c = static_cast<char>(static_cast<std::uint8_t>(data_[offset_++]));
            if (c == '\0') {
                return out;
            }
            out.push_back(c);
        }
    }

    // A string value: the declared length includes the NUL.
    [[nodiscard]] std::string string_value() {
        const std::int32_t declared = i32();
        if (failed_ || declared < 1) {
            failed_ = true;
            return {};
        }
        const std::size_t bytes = static_cast<std::size_t>(declared);
        if (!need(bytes)) {
            failed_ = true;
            return {};
        }
        // The last byte must be the NUL; a length that says otherwise is malformed.
        if (static_cast<std::uint8_t>(data_[offset_ + bytes - 1]) != 0) {
            failed_ = true;
            return {};
        }
        std::string out(reinterpret_cast<const char*>(data_.data() + offset_), bytes - 1);
        offset_ += bytes;
        return out;
    }

    [[nodiscard]] bool take(std::size_t count, std::vector<std::byte>& out) {
        if (!need(count)) {
            failed_ = true;
            return false;
        }
        out.assign(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
                   data_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
        offset_ += count;
        return true;
    }

    void skip(std::size_t count) noexcept {
        if (!need(count)) {
            failed_ = true;
            return;
        }
        offset_ += count;
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    span<const std::byte> data_;
    std::size_t offset_ = 0;
    bool failed_ = false;
};

class reader;

[[nodiscard]] status decode_document_body(reader& in, std::size_t body_length,
                                          bson_document& out);
[[nodiscard]] status decode_embedded(reader& in, bson_document& out);

// Decodes one value whose type byte has already been read.
[[nodiscard]] status decode_value(reader& in, std::uint8_t type, bson_value& value) {
    value = bson_value{};
    value.type = static_cast<bson_type>(type);
    switch (value.type) {
    case bson_type::real:
        value.real = in.real();
        break;
    case bson_type::text:
    case bson_type::javascript:
    case bson_type::symbol:
        value.text = in.string_value();
        break;
    case bson_type::document: {
        bson_document nested;
        const status decoded = decode_embedded(in, nested);
        if (!decoded) {
            return decoded;
        }
        value.document.reset(new bson_document(std::move(nested)));
        break;
    }
    case bson_type::array: {
        // An array is a document whose keys are "0", "1", ...; the keys are ignored on
        // decode and the values taken in order, which is what the format guarantees.
        bson_document nested;
        const status decoded = decode_embedded(in, nested);
        if (!decoded) {
            return decoded;
        }
        value.array.reserve(nested.size());
        for (const auto& item : nested.fields()) {
            value.array.push_back(item.second);
        }
        break;
    }
    case bson_type::binary: {
        const std::int32_t declared = in.i32();
        if (in.failed() || declared < 0) {
            return status{rpc_code::decode_error, "a BSON binary length is impossible"};
        }
        value.subtype = in.u8();
        if (in.failed()) {
            return status{rpc_code::decode_error, "a BSON binary subtype is missing"};
        }
        if (!in.take(static_cast<std::size_t>(declared), value.bytes)) {
            return status{rpc_code::decode_error, "a BSON binary value runs past its document"};
        }
        break;
    }
    case bson_type::object_id:
        if (!in.take(12, value.bytes)) {
            return status{rpc_code::decode_error, "a BSON ObjectId is not twelve bytes"};
        }
        break;
    case bson_type::decimal128:
        if (!in.take(16, value.bytes)) {
            return status{rpc_code::decode_error, "a BSON decimal128 is not sixteen bytes"};
        }
        break;
    case bson_type::boolean: {
        const std::uint8_t raw = in.u8();
        if (in.failed()) {
            return status{rpc_code::decode_error, "a BSON boolean is missing"};
        }
        // The format allows only 0 and 1; anything else is a malformed document, and
        // treating 2 as true would hide it.
        if (raw > 1) {
            return status{rpc_code::decode_error, "a BSON boolean is neither 0 nor 1"};
        }
        value.boolean = raw != 0;
        break;
    }
    case bson_type::datetime:
    case bson_type::int64:
    case bson_type::timestamp:
        value.integer = in.i64();
        break;
    case bson_type::int32:
        value.integer = in.i32();
        break;
    case bson_type::regex:
        value.text = in.cstring();
        value.extra = in.cstring();
        break;
    case bson_type::db_pointer:
        value.text = in.string_value();
        if (!in.take(12, value.bytes)) {
            return status{rpc_code::decode_error, "a BSON DBPointer is not twelve bytes"};
        }
        break;
    case bson_type::null:
    case bson_type::undefined:
    case bson_type::min_key:
    case bson_type::max_key:
        // No payload at all.
        break;
    case bson_type::javascript_scope: {
        // An int32 total, then a string and a document. The total is not needed because
        // both parts are self-describing; skipping the field keeps the stream aligned.
        const std::int32_t total = in.i32();
        if (in.failed() || total < 14) {
            return status{rpc_code::decode_error, "a BSON code-with-scope is malformed"};
        }
        (void)in.string_value();
        bson_document scope;
        const status decoded = decode_embedded(in, scope);
        if (!decoded) {
            return decoded;
        }
        break;
    }
    }
    if (in.failed()) {
        return status{rpc_code::decode_error, "a BSON value is truncated"};
    }
    return status{};
}

// Decodes a document body (the elements and the trailing NUL), given the total length.
[[nodiscard]] status decode_document_body(reader& in, std::size_t body_length,
                                          bson_document& out) {
    const std::size_t end = in.offset() + body_length;
    while (in.offset() < end) {
        const std::uint8_t type = in.u8();
        if (in.failed()) {
            return status{rpc_code::decode_error, "a BSON element type is missing"};
        }
        if (type == 0) {
            // The terminator must be the last byte the length covers; bytes after it
            // mean the length and the contents disagree.
            if (in.offset() != end) {
                return status{rpc_code::decode_error,
                              "a BSON document has bytes after its terminator"};
            }
            return status{};
        }
        std::string key = in.cstring();
        if (in.failed()) {
            return status{rpc_code::decode_error, "a BSON key is unterminated"};
        }
        bson_value value;
        const status decoded = decode_value(in, type, value);
        if (!decoded) {
            return decoded;
        }
        if (in.offset() > end) {
            return status{rpc_code::decode_error, "a BSON element runs past its document"};
        }
        out.append(std::move(key), std::move(value));
    }
    return status{rpc_code::decode_error, "a BSON document has no terminator"};
}

[[nodiscard]] status decode_embedded(reader& in, bson_document& out) {
    const std::int32_t declared = in.i32();
    if (in.failed() || declared < 5) {
        return status{rpc_code::decode_error, "a BSON sub-document length is impossible"};
    }
    const std::size_t length = static_cast<std::size_t>(declared);
    if (!in.need(length - 4)) {
        return status{rpc_code::decode_error, "a BSON sub-document runs past its parent"};
    }
    return decode_document_body(in, length - 4, out);
}

} // namespace

bson_value::bson_value() = default;
bson_value::~bson_value() = default;
bson_value::bson_value(bson_value&& other) noexcept = default;
bson_value& bson_value::operator=(bson_value&& other) noexcept = default;

bson_value::bson_value(const bson_value& other)
    : type(other.type),
      real(other.real),
      integer(other.integer),
      text(other.text),
      extra(other.extra),
      bytes(other.bytes),
      subtype(other.subtype),
      boolean(other.boolean),
      array(other.array) {
    if (other.document) {
        document.reset(new bson_document(*other.document));
    }
}

bson_value& bson_value::operator=(const bson_value& other) {
    if (this == &other) {
        return *this;
    }
    type = other.type;
    real = other.real;
    integer = other.integer;
    text = other.text;
    extra = other.extra;
    bytes = other.bytes;
    subtype = other.subtype;
    boolean = other.boolean;
    array = other.array;
    if (other.document) {
        document.reset(new bson_document(*other.document));
    } else {
        document.reset();
    }
    return *this;
}

bson_value bson_value::make_real(double value) {
    bson_value out;
    out.type = bson_type::real;
    out.real = value;
    return out;
}

bson_value bson_value::make_int32(std::int32_t value) {
    bson_value out;
    out.type = bson_type::int32;
    out.integer = value;
    return out;
}

bson_value bson_value::make_int64(std::int64_t value) {
    bson_value out;
    out.type = bson_type::int64;
    out.integer = value;
    return out;
}

bson_value bson_value::make_text(std::string value) {
    bson_value out;
    out.type = bson_type::text;
    out.text = std::move(value);
    return out;
}

bson_value bson_value::make_boolean(bool value) {
    bson_value out;
    out.type = bson_type::boolean;
    out.boolean = value;
    return out;
}

bson_value bson_value::make_null() { return bson_value{}; }

bson_value bson_value::make_datetime(std::int64_t millis) {
    bson_value out;
    out.type = bson_type::datetime;
    out.integer = millis;
    return out;
}

bson_value bson_value::make_binary(std::vector<std::byte> payload, std::uint8_t binary_subtype) {
    bson_value out;
    out.type = bson_type::binary;
    out.bytes = std::move(payload);
    out.subtype = binary_subtype;
    return out;
}

bson_value bson_value::make_object_id(std::vector<std::byte> payload) {
    bson_value out;
    out.type = bson_type::object_id;
    out.bytes = std::move(payload);
    return out;
}

bson_value bson_value::make_document(bson_document value) {
    bson_value out;
    out.type = bson_type::document;
    out.document.reset(new bson_document(std::move(value)));
    return out;
}

bson_value bson_value::make_array(std::vector<bson_value> values) {
    bson_value out;
    out.type = bson_type::array;
    out.array = std::move(values);
    return out;
}

bson_document& bson_document::append(std::string key, bson_value value) {
    fields_.emplace_back(std::move(key), std::move(value));
    return *this;
}

const bson_value* bson_document::find(std::string_view key) const noexcept {
    for (const field& item : fields_) {
        if (item.first == key) {
            return &item.second;
        }
    }
    return nullptr;
}

bson_value* bson_document::find(std::string_view key) noexcept {
    for (field& item : fields_) {
        if (item.first == key) {
            return &item.second;
        }
    }
    return nullptr;
}

bool bson_document::get_text(std::string_view key, std::string& out) const {
    const bson_value* value = find(key);
    if (value == nullptr || value->type != bson_type::text) {
        return false;
    }
    out = value->text;
    return true;
}

bool bson_document::get_int64(std::string_view key, std::int64_t& out) const {
    const bson_value* value = find(key);
    if (value == nullptr) {
        return false;
    }
    // Both integer widths are integers to a caller; a double is not, because accepting
    // one would silently lose precision on a large value.
    if (value->type != bson_type::int32 && value->type != bson_type::int64) {
        return false;
    }
    out = value->integer;
    return true;
}

bool bson_document::get_real(std::string_view key, double& out) const {
    const bson_value* value = find(key);
    if (value == nullptr) {
        return false;
    }
    if (value->type == bson_type::real) {
        out = value->real;
        return true;
    }
    if (value->type == bson_type::int32 || value->type == bson_type::int64) {
        out = static_cast<double>(value->integer);
        return true;
    }
    return false;
}

bool bson_document::get_boolean(std::string_view key, bool& out) const {
    const bson_value* value = find(key);
    if (value == nullptr || value->type != bson_type::boolean) {
        return false;
    }
    out = value->boolean;
    return true;
}

const bson_document* bson_document::get_document(std::string_view key) const {
    const bson_value* value = find(key);
    if (value == nullptr || value->type != bson_type::document || !value->document) {
        return nullptr;
    }
    return value->document.get();
}

namespace {

void encode_value(std::vector<std::byte>& out, const bson_value& value);

void encode_document_into(std::vector<std::byte>& out, const bson_document& document) {
    const std::size_t length_at = out.size();
    put_i32(out, 0); // patched below
    for (const auto& item : document.fields()) {
        put_u8(out, static_cast<std::uint8_t>(item.second.type));
        put_cstring(out, item.first);
        encode_value(out, item.second);
    }
    put_u8(out, 0);
    const std::size_t total = out.size() - length_at;
    const std::uint32_t bits = static_cast<std::uint32_t>(total);
    for (int i = 0; i < 4; ++i) {
        out[length_at + static_cast<std::size_t>(i)] =
            static_cast<std::byte>((bits >> (8 * i)) & 0xffu);
    }
}

void encode_value(std::vector<std::byte>& out, const bson_value& value) {
    switch (value.type) {
    case bson_type::real:
        put_double(out, value.real);
        break;
    case bson_type::text:
    case bson_type::javascript:
    case bson_type::symbol:
        put_string(out, value.text);
        break;
    case bson_type::document:
        if (value.document) {
            encode_document_into(out, *value.document);
        } else {
            encode_document_into(out, bson_document{});
        }
        break;
    case bson_type::array: {
        // An array is a document whose keys are "0", "1", ... in order.
        bson_document wrapper;
        for (std::size_t i = 0; i < value.array.size(); ++i) {
            wrapper.append(std::to_string(i), value.array[i]);
        }
        encode_document_into(out, wrapper);
        break;
    }
    case bson_type::binary:
        put_i32(out, static_cast<std::int32_t>(value.bytes.size()));
        put_u8(out, value.subtype);
        out.insert(out.end(), value.bytes.begin(), value.bytes.end());
        break;
    case bson_type::object_id:
        // Exactly twelve bytes; anything else would be refused by the server, so pad or
        // truncate nothing and encode what was given.
        out.insert(out.end(), value.bytes.begin(), value.bytes.end());
        break;
    case bson_type::decimal128:
        out.insert(out.end(), value.bytes.begin(), value.bytes.end());
        break;
    case bson_type::boolean:
        put_u8(out, value.boolean ? 1 : 0);
        break;
    case bson_type::datetime:
        put_i64(out, value.integer);
        break;
    case bson_type::timestamp:
        put_i64(out, value.integer);
        break;
    case bson_type::null:
    case bson_type::undefined:
    case bson_type::min_key:
    case bson_type::max_key:
        break;
    case bson_type::int32:
        put_i32(out, static_cast<std::int32_t>(value.integer));
        break;
    case bson_type::int64:
        put_i64(out, value.integer);
        break;
    case bson_type::regex:
        put_cstring(out, value.text);
        put_cstring(out, value.extra);
        break;
    case bson_type::db_pointer:
        put_string(out, value.text);
        out.insert(out.end(), value.bytes.begin(), value.bytes.end());
        break;
    case bson_type::javascript_scope:
        // Unsupported in this slice; encoded as an empty scope rather than as garbage.
        put_i32(out, 5);
        put_u8(out, 0);
        put_i32(out, 5);
        put_u8(out, 0);
        break;
    }
}

} // namespace

std::size_t bson_document::encoded_size() const noexcept {
    std::vector<std::byte> scratch;
    encode_document_into(scratch, *this);
    return scratch.size();
}

status encode_bson(std::vector<std::byte>& out, const bson_document& document) {
    encode_document_into(out, document);
    return status{};
}

result<std::vector<std::byte>> encode_bson(const bson_document& document) {
    std::vector<std::byte> out;
    out.reserve(64);
    const status encoded = encode_bson(out, document);
    if (!encoded) {
        return encoded.error();
    }
    return out;
}

result<std::size_t> peek_bson_size(span<const std::byte> data) {
    if (data.size() < 5) {
        return rpc_error{rpc_code::decode_error, "a BSON document needs at least five bytes"};
    }
    std::uint32_t bits = 0;
    for (int i = 0; i < 4; ++i) {
        bits |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[static_cast<std::size_t>(i)]))
                << (8 * i);
    }
    const std::int32_t declared = static_cast<std::int32_t>(bits);
    if (declared < 5) {
        return rpc_error{rpc_code::decode_error, "a BSON document length is impossible"};
    }
    return static_cast<std::size_t>(declared);
}

status decode_bson(span<const std::byte> data, bson_document& out, std::size_t& consumed) {
    consumed = 0;
    out = bson_document{};
    result<std::size_t> size = peek_bson_size(data);
    if (!size) {
        return size.error();
    }
    if (size.value() > data.size()) {
        return status{rpc_code::decode_error, "a BSON document runs past the buffer"};
    }
    reader in(data);
    const std::int32_t declared = in.i32();
    if (in.failed()) {
        return status{rpc_code::decode_error, "a BSON document length could not be read"};
    }
    const std::size_t length = static_cast<std::size_t>(declared);
    const status body = decode_document_body(in, length - 4, out);
    if (!body) {
        out = bson_document{};
        return body;
    }
    if (in.offset() != length) {
        out = bson_document{};
        return status{rpc_code::decode_error,
                      "a BSON document length disagrees with its contents"};
    }
    consumed = length;
    return status{};
}

} // namespace mongo
} // namespace db
} // namespace crpc