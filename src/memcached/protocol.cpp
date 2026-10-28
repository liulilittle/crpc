// CRPC -- memcached wire protocol codec implementation.
#include <crpc/memcached/protocol.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

#include <crpc/endian.hpp>

namespace crpc {
namespace memcached {
namespace {

constexpr std::byte cr() noexcept { return std::byte{'\r'}; }
constexpr std::byte lf() noexcept { return std::byte{'\n'}; }

// Splits on single spaces, dropping empty tokens. memcached never emits a double
// space, but an empty run must not turn into an empty token either.
[[nodiscard]] std::vector<std::string_view> split_views(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ') {
            ++i;
        }
        if (i > start) {
            tokens.push_back(line.substr(start, i - start));
        }
    }
    return tokens;
}

// Appends `text` to `out` at `pos`, failing when it would not fit.
[[nodiscard]] bool append(span<std::byte> out, std::size_t& pos, std::string_view text) noexcept {
    if (text.size() > out.size() - pos) {
        return false;
    }
    if (!text.empty()) {
        std::memcpy(out.data() + pos, text.data(), text.size());
        pos += text.size();
    }
    return true;
}

[[nodiscard]] bool append_crlf(span<std::byte> out, std::size_t& pos) noexcept {
    if (out.size() - pos < 2) {
        return false;
    }
    out[pos] = cr();
    out[pos + 1] = lf();
    pos += 2;
    return true;
}

[[nodiscard]] std::string to_decimal(std::uint64_t value) {
    char digits[24];
    std::size_t count = 0;
    if (value == 0) {
        digits[count++] = '0';
    } else {
        while (value != 0) {
            digits[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }
    std::string text;
    text.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        text[i] = digits[count - 1 - i];
    }
    return text;
}

[[nodiscard]] std::size_t decimal_width(std::uint64_t value) noexcept {
    std::size_t width = 1;
    while (value >= 10) {
        value /= 10;
        ++width;
    }
    return width;
}

[[nodiscard]] bool is_digits(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

} // namespace

// --- Text encoder ------------------------------------------------------------

std::size_t text_encoder::storage_size(std::string_view command, std::string_view key,
                                       std::string_view data, std::uint64_t cas) noexcept {
    // "<cmd> <key> <flags> <exptime> <bytes>[ <cas>]\r\n<data>\r\n". The flags and
    // exptime widths are bounded by their integer types, so this estimate uses
    // those maxima and is never smaller than the encoded frame.
    return command.size() + 1 + key.size() + 1 + 10 + 1 + 20 + 1 + 10 + 1 +
           (cas != 0 ? decimal_width(cas) + 1 : 0) + 2 + data.size() + 2;
}

status text_encoder::storage(std::string_view command, std::string_view key,
                             std::uint32_t flags, std::uint64_t exptime,
                             std::string_view data, std::uint64_t cas,
                             span<std::byte> out, std::size_t& written) const {
    written = 0;
    if (key.empty() || key.size() > max_key_size) {
        return status{rpc_code::invalid_argument, "memcached key length is out of range"};
    }
    const std::string flags_text = to_decimal(flags);
    const std::string exptime_text = to_decimal(exptime);
    const std::string bytes_text = to_decimal(data.size());
    const std::string cas_text = cas != 0 ? to_decimal(cas) : std::string{};

    std::size_t pos = 0;
    const bool fits =
        append(out, pos, command) && append(out, pos, " ") && append(out, pos, key) &&
        append(out, pos, " ") && append(out, pos, flags_text) && append(out, pos, " ") &&
        append(out, pos, exptime_text) && append(out, pos, " ") &&
        append(out, pos, bytes_text) &&
        (cas == 0 || (append(out, pos, " ") && append(out, pos, cas_text))) &&
        append_crlf(out, pos) && append(out, pos, data) && append_crlf(out, pos);
    if (!fits) {
        return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
    }
    written = pos;
    return status{};
}

std::size_t text_encoder::retrieval_size(std::string_view command,
                                         const std::vector<std::string_view>& keys,
                                         std::uint64_t exptime) noexcept {
    std::size_t size = command.size() + 2;
    if (command == "gat" || command == "gats") {
        size += decimal_width(exptime) + 1;
    }
    for (const std::string_view key : keys) {
        size += key.size() + 1;
    }
    return size;
}

status text_encoder::retrieval(std::string_view command,
                               const std::vector<std::string_view>& keys,
                               std::uint64_t exptime, span<std::byte> out,
                               std::size_t& written) const {
    written = 0;
    if (keys.empty()) {
        return status{rpc_code::invalid_argument, "memcached retrieval needs at least one key"};
    }
    for (const std::string_view key : keys) {
        if (key.empty() || key.size() > max_key_size) {
            return status{rpc_code::invalid_argument, "memcached key length is out of range"};
        }
    }
    const bool timed = command == "gat" || command == "gats";
    std::size_t pos = 0;
    if (!append(out, pos, command)) {
        return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
    }
    if (timed) {
        if (!append(out, pos, " ") || !append(out, pos, to_decimal(exptime))) {
            return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
        }
    }
    for (const std::string_view key : keys) {
        if (!append(out, pos, " ") || !append(out, pos, key)) {
            return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
        }
    }
    if (!append_crlf(out, pos)) {
        return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
    }
    written = pos;
    return status{};
}

status text_encoder::remove(std::string_view key, std::uint64_t cas, span<std::byte> out,
                            std::size_t& written) const {
    written = 0;
    if (key.empty() || key.size() > max_key_size) {
        return status{rpc_code::invalid_argument, "memcached key length is out of range"};
    }
    std::size_t pos = 0;
    const bool fits =
        append(out, pos, "delete ") && append(out, pos, key) &&
        (cas == 0 || (append(out, pos, " ") && append(out, pos, to_decimal(cas)))) &&
        append_crlf(out, pos);
    if (!fits) {
        return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
    }
    written = pos;
    return status{};
}

status text_encoder::arithmetic(std::string_view command, std::string_view key,
                                std::uint64_t delta, span<std::byte> out,
                                std::size_t& written) const {
    written = 0;
    if (key.empty() || key.size() > max_key_size) {
        return status{rpc_code::invalid_argument, "memcached key length is out of range"};
    }
    std::size_t pos = 0;
    const bool fits = append(out, pos, command) && append(out, pos, " ") &&
                      append(out, pos, key) && append(out, pos, " ") &&
                      append(out, pos, to_decimal(delta)) && append_crlf(out, pos);
    if (!fits) {
        return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
    }
    written = pos;
    return status{};
}

status text_encoder::touch(std::string_view key, std::uint64_t exptime, span<std::byte> out,
                           std::size_t& written) const {
    written = 0;
    if (key.empty() || key.size() > max_key_size) {
        return status{rpc_code::invalid_argument, "memcached key length is out of range"};
    }
    std::size_t pos = 0;
    const bool fits = append(out, pos, "touch ") && append(out, pos, key) &&
                      append(out, pos, " ") && append(out, pos, to_decimal(exptime)) &&
                      append_crlf(out, pos);
    if (!fits) {
        return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
    }
    written = pos;
    return status{};
}

status text_encoder::simple(std::string_view command, std::string_view argument,
                            span<std::byte> out, std::size_t& written) const {
    written = 0;
    std::size_t pos = 0;
    const bool fits =
        append(out, pos, command) &&
        (argument.empty() || (append(out, pos, " ") && append(out, pos, argument))) &&
        append_crlf(out, pos);
    if (!fits) {
        return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
    }
    written = pos;
    return status{};
}

std::size_t text_encoder::meta_size(std::string_view command, std::string_view key,
                                    const std::vector<std::string_view>& flags,
                                    std::string_view data) noexcept {
    std::size_t size = command.size() + 2;
    if (!key.empty()) {
        size += key.size() + 1;
    }
    for (const std::string_view flag : flags) {
        size += flag.size() + 1;
    }
    if (!data.empty()) {
        size += data.size() + 2;
    }
    return size;
}

status text_encoder::meta(std::string_view command, std::string_view key, std::uint64_t size,
                          const std::vector<std::string_view>& flags, std::string_view data,
                          span<std::byte> out, std::size_t& written) const {
    written = 0;
    if (!key.empty() && key.size() > max_key_size) {
        return status{rpc_code::invalid_argument, "memcached key length is out of range"};
    }
    std::size_t pos = 0;
    if (!append(out, pos, command)) {
        return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
    }
    if (!key.empty()) {
        if (!append(out, pos, " ") || !append(out, pos, key)) {
            return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
        }
    }
    if (size != 0) {
        if (!append(out, pos, " ") || !append(out, pos, to_decimal(size))) {
            return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
        }
    }
    for (const std::string_view flag : flags) {
        if (!append(out, pos, " ") || !append(out, pos, flag)) {
            return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
        }
    }
    if (!append_crlf(out, pos)) {
        return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
    }
    if (!data.empty() && !(append(out, pos, data) && append_crlf(out, pos))) {
        return status{rpc_code::encode_error, "memcached command does not fit the buffer"};
    }
    written = pos;
    return status{};
}

// --- Text parser -------------------------------------------------------------

text_parser::text_parser() noexcept = default;

text_parser::text_parser(std::size_t max_value_size) noexcept
    : max_value_size_(max_value_size) {}

text_parser::text_parser(std::size_t max_value_size, std::size_t max_line_size) noexcept
    : max_value_size_(max_value_size), max_line_size_(max_line_size) {}

void text_parser::set_allow_bare_end(bool allowed) noexcept {
    allow_bare_end_ = allowed;
}

void text_parser::feed(span<const std::byte> data) noexcept {
    if (error_ != rpc_code::ok) {
        return;
    }
    try {
        buffer_.insert(buffer_.end(), data.begin(), data.end());
    } catch (...) {
        // A hostile peer can force an allocation failure only after the limits
        // above were exceeded, so this stays a resource error, never a throw.
        fail(rpc_code::resource_exhausted);
    }
}

void text_parser::reset() noexcept {
    buffer_.clear();
    offset_ = 0;
    error_ = rpc_code::ok;
    state_ = state::line;
    line_.clear();
    pending_entries_.clear();
    pending_value_size_ = 0;
    pending_entry_ = value_entry{};
    pending_reply_ = text_reply{};
}

rpc_code text_parser::error() const noexcept { return error_; }

bool text_parser::failed() const noexcept { return error_ != rpc_code::ok; }

std::size_t text_parser::buffered() const noexcept { return buffer_.size() - offset_; }

std::size_t text_parser::max_value_size() const noexcept { return max_value_size_; }

std::size_t text_parser::max_line_size() const noexcept { return max_line_size_; }

void text_parser::fail(rpc_code code) noexcept { error_ = code; }

void text_parser::consume(std::size_t bytes) noexcept {
    offset_ += bytes;
    if (offset_ >= buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    } else if (offset_ > buffer_.size() / 2) {
        maybe_compact();
    }
}

void text_parser::maybe_compact() noexcept {
    if (offset_ == 0) {
        return;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
    offset_ = 0;
}

text_parser::parse_result text_parser::read_line() noexcept {
    const std::size_t available = buffered();
    for (std::size_t i = 0; i + 1 < available; ++i) {
        if (buffer_[offset_ + i] == cr() && buffer_[offset_ + i + 1] == lf()) {
            if (i > max_line_size_) {
                fail(rpc_code::resource_exhausted);
                return parse_result::error;
            }
            line_.assign(reinterpret_cast<const char*>(buffer_.data() + offset_), i);
            consume(i + 2);
            return parse_result::ok;
        }
    }
    if (available > max_line_size_ + 1) {
        fail(rpc_code::resource_exhausted);
        return parse_result::error;
    }
    return parse_result::need_more;
}

text_parser::parse_result text_parser::take_value(std::size_t size, std::string& out) noexcept {
    if (size > max_value_size_) {
        fail(rpc_code::resource_exhausted);
        return parse_result::error;
    }
    if (buffered() < size + 2) {
        return parse_result::need_more;
    }
    if (buffer_[offset_ + size] != cr() || buffer_[offset_ + size + 1] != lf()) {
        fail(rpc_code::protocol_error);
        return parse_result::error;
    }
    out.assign(reinterpret_cast<const char*>(buffer_.data() + offset_), size);
    consume(size + 2);
    return parse_result::ok;
}

text_parser::parse_result text_parser::dispatch(text_reply& out) noexcept {
    const std::vector<std::string_view> tokens = split_views(line_);
    if (tokens.empty()) {
        fail(rpc_code::protocol_error);
        return parse_result::error;
    }
    const std::string_view command = tokens[0];

    // --- value block header ---------------------------------------------------
    if (command == "VALUE") {
        // VALUE <key> <flags> <bytes> [<cas>]
        if (tokens.size() != 4 && tokens.size() != 5) {
            fail(rpc_code::protocol_error);
            return parse_result::error;
        }
        value_entry entry;
        entry.key.assign(tokens[1].data(), tokens[1].size());
        std::uint64_t flags = 0;
        std::uint64_t bytes = 0;
        if (!parse_u64(tokens[2], flags) || flags > std::numeric_limits<std::uint32_t>::max() ||
            !parse_u64(tokens[3], bytes)) {
            fail(rpc_code::protocol_error);
            return parse_result::error;
        }
        if (tokens.size() == 5 && !parse_u64(tokens[4], entry.cas)) {
            fail(rpc_code::protocol_error);
            return parse_result::error;
        }
        if (bytes > max_value_size_) {
            fail(rpc_code::resource_exhausted);
            return parse_result::error;
        }
        entry.flags = static_cast<std::uint32_t>(flags);
        pending_entry_ = std::move(entry);
        pending_value_size_ = static_cast<std::size_t>(bytes);
        state_ = state::value_block;
        return parse_result::need_more;
    }

    if (command == "END") {
        if (tokens.size() != 1) {
            fail(rpc_code::protocol_error);
            return parse_result::error;
        }
        if (!pending_entries_.empty() || pending_reply_.type == text_reply::kind::values) {
            out = text_reply{};
            out.type = text_reply::kind::values;
            out.entries = std::move(pending_entries_);
            pending_entries_.clear();
            pending_reply_ = text_reply{};
            return parse_result::ok;
        }
        if (!pending_reply_.stats_entries.empty()) {
            out = text_reply{};
            out.type = text_reply::kind::stats;
            out.stats_entries = std::move(pending_reply_.stats_entries);
            pending_reply_ = text_reply{};
            return parse_result::ok;
        }
        if (allow_bare_end_) {
            // A retrieval that found no keys answers with exactly `END`. The
            // empty value list is a normal reply, not a malformed stream.
            out = text_reply{};
            out.type = text_reply::kind::values;
            return parse_result::ok;
        }
        // A bare END without a preceding VALUE/STAT is a protocol violation.
        fail(rpc_code::protocol_error);
        return parse_result::error;
    }

    if (command == "STAT") {
        // STAT <name> <value>; the value may contain spaces, so it is the rest
        // of the line rather than a single token.
        if (tokens.size() < 2) {
            fail(rpc_code::protocol_error);
            return parse_result::error;
        }
        const std::size_t value_offset =
            static_cast<std::size_t>(tokens[1].data() - line_.data()) + tokens[1].size();
        std::string value;
        if (value_offset < line_.size()) {
            value.assign(line_, value_offset + 1, std::string::npos);
        }
        pending_reply_.stats_entries.emplace_back(std::string(tokens[1]), std::move(value));
        return parse_result::need_more;
    }

    if (command == "ITEM") {
        // `stats cachedump` reports one ITEM line per key, terminated by END.
        // It is accumulated into the same list as STAT so the caller sees one
        // reply instead of a line-by-line protocol.
        if (tokens.size() < 2) {
            fail(rpc_code::protocol_error);
            return parse_result::error;
        }
        const std::size_t rest =
            static_cast<std::size_t>(tokens[1].data() - line_.data()) + tokens[1].size();
        std::string value;
        if (rest < line_.size()) {
            value.assign(line_, rest + 1, std::string::npos);
        }
        pending_reply_.stats_entries.emplace_back(std::string(tokens[1]), std::move(value));
        return parse_result::need_more;
    }

    if (command == "VERSION") {
        if (tokens.size() != 2) {
            fail(rpc_code::protocol_error);
            return parse_result::error;
        }
        out = text_reply{};
        out.type = text_reply::kind::version;
        out.text.assign(tokens[1].data(), tokens[1].size());
        return parse_result::ok;
    }

    // --- single-word outcomes -------------------------------------------------
    if (command == "STORED" || command == "NOT_STORED" || command == "EXISTS" ||
        command == "NOT_FOUND" || command == "DELETED" || command == "TOUCHED" ||
        command == "OK" || command == "RESET") {
        if (tokens.size() != 1) {
            fail(rpc_code::protocol_error);
            return parse_result::error;
        }
        out = text_reply{};
        out.type = text_reply::kind::status;
        if (command == "STORED") {
            out.store = store_status::stored;
        } else if (command == "NOT_STORED") {
            out.store = store_status::not_stored;
        } else if (command == "EXISTS") {
            out.store = store_status::exists;
        } else if (command == "NOT_FOUND") {
            out.store = store_status::not_found;
        } else if (command == "DELETED") {
            out.store = store_status::deleted;
        } else if (command == "TOUCHED") {
            out.store = store_status::touched;
        } else {
            // OK (verbosity) and RESET (stats reset) carry no payload.
            out.store = store_status::ok;
        }
        return parse_result::ok;
    }

    // --- errors ---------------------------------------------------------------
    if (command == "ERROR") {
        out = text_reply{};
        out.type = text_reply::kind::error;
        out.error = error_kind::error;
        out.text = "ERROR";
        return parse_result::ok;
    }
    if (command == "CLIENT_ERROR" || command == "SERVER_ERROR") {
        out = text_reply{};
        out.type = text_reply::kind::error;
        out.error =
            command == "CLIENT_ERROR" ? error_kind::client_error : error_kind::server_error;
        // Preserve the whole message, which may contain spaces.
        if (line_.size() > command.size() + 1) {
            out.text.assign(line_, command.size() + 1, std::string::npos);
        }
        return parse_result::ok;
    }

    // --- meta protocol --------------------------------------------------------
    if (command == "HD" || command == "EN" || command == "NF" || command == "NS" ||
        command == "EX" || command == "MN" || command == "VA" || command == "ME") {
        text_reply reply;
        reply.type = text_reply::kind::meta_status;
        for (const std::string_view token : tokens) {
            reply.meta_tokens.emplace_back(token);
        }
        // A CAS token ("c<decimal>") may follow the size on any meta reply, so it
        // is collected before the VA branch hands the reply to the value state.
        for (const std::string_view token : tokens) {
            if (token.size() > 1 && token[0] == 'c') {
                std::uint64_t cas = 0;
                if (parse_u64(token.substr(1), cas)) {
                    reply.meta_cas = cas;
                }
            }
        }
        if (command == "VA") {
            // VA <size> [<flags>...] followed by the value and CRLF.
            if (tokens.size() < 2) {
                fail(rpc_code::protocol_error);
                return parse_result::error;
            }
            std::uint64_t bytes = 0;
            if (!parse_u64(tokens[1], bytes)) {
                fail(rpc_code::protocol_error);
                return parse_result::error;
            }
            if (bytes > max_value_size_) {
                fail(rpc_code::resource_exhausted);
                return parse_result::error;
            }
            reply.type = text_reply::kind::meta_hit;
            pending_value_size_ = static_cast<std::size_t>(bytes);
            pending_reply_ = std::move(reply);
            state_ = state::value_block;
            return parse_result::need_more;
        }
        if (command == "EN") {
            reply.type = text_reply::kind::meta_miss;
        }
        out = std::move(reply);
        return parse_result::ok;
    }

    // --- counters -------------------------------------------------------------
    if (is_digits(command)) {
        if (tokens.size() != 1) {
            fail(rpc_code::protocol_error);
            return parse_result::error;
        }
        std::uint64_t value = 0;
        if (!parse_u64(command, value)) {
            fail(rpc_code::protocol_error);
            return parse_result::error;
        }
        out = text_reply{};
        out.type = text_reply::kind::number;
        out.number = value;
        return parse_result::ok;
    }

    fail(rpc_code::protocol_error);
    return parse_result::error;
}

text_parser::result text_parser::next(text_reply& out) noexcept {
    if (error_ != rpc_code::ok) {
        return result::error;
    }
    for (;;) {
        if (state_ == state::value_block) {
            std::string data;
            const parse_result taken = take_value(pending_value_size_, data);
            if (taken == parse_result::need_more) {
                return result::need_more;
            }
            if (taken == parse_result::error) {
                return result::error;
            }
            state_ = state::line;
            if (pending_reply_.type == text_reply::kind::meta_hit) {
                pending_reply_.value = std::move(data);
                out = std::move(pending_reply_);
                pending_reply_ = text_reply{};
                return result::reply_ready;
            }
            pending_entry_.data = std::move(data);
            pending_entries_.push_back(std::move(pending_entry_));
            pending_entry_ = value_entry{};
            pending_value_size_ = 0;
            // A VALUE reply is complete only at END, so record that at least one
            // VALUE was seen and keep reading lines.
            pending_reply_.type = text_reply::kind::values;
            continue;
        }

        const parse_result line = read_line();
        if (line == parse_result::need_more) {
            return result::need_more;
        }
        if (line == parse_result::error) {
            return result::error;
        }
        const parse_result dispatched = dispatch(out);
        if (dispatched == parse_result::error) {
            return result::error;
        }
        if (dispatched == parse_result::ok) {
            return result::reply_ready;
        }
        // need_more here means "a line was consumed, keep going".
    }
}

status text_parser::finish() noexcept {
    if (error_ != rpc_code::ok) {
        return status{error_};
    }
    if (buffered() != 0 || state_ != state::line || !pending_entries_.empty()) {
        return status{rpc_code::protocol_error, "memcached stream ended mid-reply"};
    }
    return status{};
}

// --- Binary protocol ---------------------------------------------------------

status encode_binary_header(span<std::byte> out, const binary_header& header,
                            std::size_t& written) noexcept {
    written = 0;
    if (out.size() < binary_header_size) {
        return status{rpc_code::encode_error, "memcached binary header needs 24 bytes"};
    }
    out[0] = std::byte{0x80}; // request magic
    out[1] = std::byte{header.opcode};
    endian::store_be16(out.data() + 2, header.key_length);
    out[4] = std::byte{header.extras_length};
    out[5] = std::byte{header.data_type};
    endian::store_be16(out.data() + 6, header.status);
    endian::store_be32(out.data() + 8, header.body_length);
    endian::store_be32(out.data() + 12, header.opaque);
    endian::store_be64(out.data() + 16, header.cas);
    written = binary_header_size;
    return status{};
}

status decode_binary_header(span<const std::byte> in, bool response,
                            binary_header& header) noexcept {
    if (in.size() < binary_header_size) {
        return status{rpc_code::protocol_error, "memcached binary header is truncated"};
    }
    const std::uint8_t magic = static_cast<std::uint8_t>(in[0]);
    if (magic != (response ? 0x81 : 0x80)) {
        return status{rpc_code::protocol_error, "memcached binary magic mismatch"};
    }
    header.opcode = static_cast<std::uint8_t>(in[1]);
    header.key_length = endian::load_be16(in.data() + 2);
    header.extras_length = static_cast<std::uint8_t>(in[4]);
    header.data_type = static_cast<std::uint8_t>(in[5]);
    header.status = endian::load_be16(in.data() + 6);
    header.body_length = endian::load_be32(in.data() + 8);
    header.opaque = endian::load_be32(in.data() + 12);
    header.cas = endian::load_be64(in.data() + 16);
    // The extras and key sections are prefixes of the body; a header that claims
    // more than the body holds is a protocol violation, not a size to trust.
    if (static_cast<std::uint32_t>(header.key_length) + header.extras_length >
        header.body_length) {
        return status{rpc_code::protocol_error, "memcached binary sections exceed the body"};
    }
    return status{};
}

rpc_code map_binary_status(std::uint16_t status) noexcept {
    switch (static_cast<binary_status>(status)) {
    case binary_status::ok:
        return rpc_code::ok;
    case binary_status::key_not_found:
        return rpc_code::not_found;
    case binary_status::key_exists:
        return rpc_code::failed_precondition;
    case binary_status::value_too_large:
        return rpc_code::resource_exhausted;
    case binary_status::invalid_arguments:
        return rpc_code::invalid_argument;
    case binary_status::item_not_stored:
        return rpc_code::failed_precondition;
    case binary_status::unknown_command:
        return rpc_code::no_such_method;
    case binary_status::out_of_memory:
        return rpc_code::resource_exhausted;
    }
    return rpc_code::internal;
}

const char* binary_status_name(std::uint16_t status) noexcept {
    switch (static_cast<binary_status>(status)) {
    case binary_status::ok:
        return "ok";
    case binary_status::key_not_found:
        return "key_not_found";
    case binary_status::key_exists:
        return "key_exists";
    case binary_status::value_too_large:
        return "value_too_large";
    case binary_status::invalid_arguments:
        return "invalid_arguments";
    case binary_status::item_not_stored:
        return "item_not_stored";
    case binary_status::unknown_command:
        return "unknown_command";
    case binary_status::out_of_memory:
        return "out_of_memory";
    }
    return "";
}

// --- Helpers -----------------------------------------------------------------

bool parse_u64(std::string_view text, std::uint64_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

std::vector<std::string> split_tokens(std::string_view line) {
    std::vector<std::string> tokens;
    for (const std::string_view view : split_views(line)) {
        tokens.emplace_back(view);
    }
    return tokens;
}

} // namespace memcached
} // namespace crpc