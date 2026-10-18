// CRPC -- trace context, traceparent and baggage implementation.
#include <crpc/observability/context.hpp>

#include <cstring>

namespace crpc {
namespace obs {
namespace {

constexpr bool is_lower_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

constexpr u8 hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<u8>(c - '0');
    }
    return static_cast<u8>(c - 'a' + 10);
}

constexpr char hex_digit(u8 value) noexcept {
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10);
}

bool decode_hex(std::string_view text, std::byte* out, std::size_t byte_count) noexcept {
    if (text.size() != byte_count * 2) {
        return false;
    }
    for (std::size_t i = 0; i < byte_count; ++i) {
        const char high = text[i * 2];
        const char low = text[i * 2 + 1];
        if (!is_lower_hex(high) || !is_lower_hex(low)) {
            return false;
        }
        out[i] = static_cast<std::byte>(static_cast<u8>((hex_value(high) << 4) | hex_value(low)));
    }
    return true;
}

void encode_hex(const std::byte* bytes, std::size_t byte_count, char* out) noexcept {
    for (std::size_t i = 0; i < byte_count; ++i) {
        const u8 value = static_cast<u8>(bytes[i]);
        out[i * 2] = hex_digit(static_cast<u8>(value >> 4));
        out[i * 2 + 1] = hex_digit(static_cast<u8>(value & 0x0F));
    }
}

} // namespace

const char* to_string(traceparent_status status) noexcept {
    switch (status) {
    case traceparent_status::ok:
        return "ok";
    case traceparent_status::invalid_length:
        return "invalid_length";
    case traceparent_status::invalid_format:
        return "invalid_format";
    case traceparent_status::invalid_version:
        return "invalid_version";
    case traceparent_status::invalid_hex:
        return "invalid_hex";
    case traceparent_status::invalid_trace_id:
        return "invalid_trace_id";
    case traceparent_status::invalid_span_id:
        return "invalid_span_id";
    }
    return "unknown";
}

traceparent_result parse_traceparent(std::string_view text) noexcept {
    traceparent_result result;

    if (text.size() != 55) {
        result.status = traceparent_status::invalid_length;
        return result;
    }
    if (text[2] != '-' || text[35] != '-' || text[52] != '-') {
        result.status = traceparent_status::invalid_format;
        return result;
    }
    if (!is_lower_hex(text[0]) || !is_lower_hex(text[1])) {
        result.status = traceparent_status::invalid_hex;
        return result;
    }
    if (text[0] == 'f' && text[1] == 'f') {
        result.status = traceparent_status::invalid_version;
        return result;
    }
    if (!is_lower_hex(text[53]) || !is_lower_hex(text[54])) {
        result.status = traceparent_status::invalid_hex;
        return result;
    }

    trace_context context;
    if (!decode_hex(text.substr(3, 32), context.trace.bytes.data(), context.trace.bytes.size())) {
        result.status = traceparent_status::invalid_hex;
        return result;
    }
    if (!decode_hex(text.substr(36, 16), context.span.bytes.data(), context.span.bytes.size())) {
        result.status = traceparent_status::invalid_hex;
        return result;
    }
    if (!context.trace.valid()) {
        result.status = traceparent_status::invalid_trace_id;
        return result;
    }
    if (!context.span.valid()) {
        result.status = traceparent_status::invalid_span_id;
        return result;
    }

    context.flags = static_cast<u8>((hex_value(text[53]) << 4) | hex_value(text[54]));
    result.context = context;
    result.status = traceparent_status::ok;
    return result;
}

std::size_t format_traceparent(const trace_context& context, char* out,
                               std::size_t capacity) noexcept {
    if (out == nullptr || capacity < 55) {
        return 0;
    }
    if (!context.trace.valid() || !context.span.valid()) {
        return 0;
    }

    out[0] = '0';
    out[1] = '0';
    out[2] = '-';
    encode_hex(context.trace.bytes.data(), context.trace.bytes.size(), out + 3);
    out[35] = '-';
    encode_hex(context.span.bytes.data(), context.span.bytes.size(), out + 36);
    out[52] = '-';
    out[53] = hex_digit(static_cast<u8>(context.flags >> 4));
    out[54] = hex_digit(static_cast<u8>(context.flags & 0x0F));
    if (capacity > 55) {
        out[55] = '\0';
    }
    return 55;
}

// --- Baggage -----------------------------------------------------------------

std::size_t baggage::find_index(std::string_view key) const noexcept {
    if (key.empty()) {
        return max_entries;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        const entry& item = entries_[i];
        if (item.key_len == key.size() &&
            std::memcmp(data_.data() + item.key_offset, key.data(), key.size()) == 0) {
            return i;
        }
    }
    return max_entries;
}

bool baggage::append(std::string_view key, std::string_view value) noexcept {
    const std::size_t needed = key.size() + value.size();
    if (count_ >= max_entries || needed > max_bytes - data_size_) {
        truncated_ = true;
        ++dropped_entries_;
        dropped_bytes_ += needed;
        return false;
    }

    entry item{};
    item.key_offset = static_cast<u16>(data_size_);
    item.key_len = static_cast<u16>(key.size());
    if (!key.empty()) {
        std::memcpy(data_.data() + data_size_, key.data(), key.size());
    }
    data_size_ += key.size();

    item.value_offset = static_cast<u16>(data_size_);
    item.value_len = static_cast<u16>(value.size());
    if (!value.empty()) {
        std::memcpy(data_.data() + data_size_, value.data(), value.size());
    }
    data_size_ += value.size();

    entries_[count_] = item;
    ++count_;
    return true;
}

void baggage::remove_at(std::size_t index) noexcept {
    std::array<char, max_bytes> compacted{};
    std::array<entry, max_entries> compacted_entries{};
    std::size_t compacted_size = 0;
    std::size_t compacted_count = 0;

    for (std::size_t i = 0; i < count_; ++i) {
        if (i == index) {
            continue;
        }
        const entry& source = entries_[i];
        entry target{};
        target.key_offset = static_cast<u16>(compacted_size);
        target.key_len = source.key_len;
        std::memcpy(compacted.data() + compacted_size, data_.data() + source.key_offset,
                    source.key_len);
        compacted_size += source.key_len;

        target.value_offset = static_cast<u16>(compacted_size);
        target.value_len = source.value_len;
        std::memcpy(compacted.data() + compacted_size, data_.data() + source.value_offset,
                    source.value_len);
        compacted_size += source.value_len;

        compacted_entries[compacted_count] = target;
        ++compacted_count;
    }

    data_ = compacted;
    entries_ = compacted_entries;
    count_ = compacted_count;
    data_size_ = compacted_size;
}

bool baggage::set(std::string_view key, std::string_view value) noexcept {
    if (key.empty() || key.size() > max_key_size || value.size() > max_value_size) {
        truncated_ = true;
        ++dropped_entries_;
        dropped_bytes_ += key.size() + value.size();
        return false;
    }

    const std::size_t existing = find_index(key);
    if (existing == max_entries) {
        return append(key, value);
    }

    // Replace in place while preserving insertion order: rebuild the inline
    // arena entry by entry, substituting the new value at the same index.
    baggage next;
    next.truncated_ = truncated_;
    next.dropped_entries_ = dropped_entries_;
    next.dropped_bytes_ = dropped_bytes_;
    for (std::size_t i = 0; i < count_; ++i) {
        const entry& item = entries_[i];
        const std::string_view item_key(data_.data() + item.key_offset, item.key_len);
        const std::string_view item_value =
            i == existing ? value
                          : std::string_view(data_.data() + item.value_offset, item.value_len);
        if (!next.append(item_key, item_value)) {
            truncated_ = true;
            ++dropped_entries_;
            dropped_bytes_ += key.size() + value.size();
            return false;
        }
    }
    *this = next;
    return true;
}

const char* baggage::get(std::string_view key, std::size_t* length) const noexcept {
    const std::size_t index = find_index(key);
    if (index == max_entries) {
        return nullptr;
    }
    const entry& item = entries_[index];
    if (length != nullptr) {
        *length = item.value_len;
    }
    return data_.data() + item.value_offset;
}

bool baggage::remove(std::string_view key) noexcept {
    const std::size_t index = find_index(key);
    if (index == max_entries) {
        return false;
    }
    remove_at(index);
    return true;
}

void baggage::clear() noexcept {
    count_ = 0;
    data_size_ = 0;
    truncated_ = false;
    dropped_entries_ = 0;
    dropped_bytes_ = 0;
}

baggage::entry_view baggage::at(std::size_t index) const noexcept {
    const entry& item = entries_[index];
    return entry_view{std::string_view(data_.data() + item.key_offset, item.key_len),
                      std::string_view(data_.data() + item.value_offset, item.value_len)};
}

} // namespace obs
} // namespace crpc
