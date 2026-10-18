// CRPC -- bounds-checked wire reader and writer.
//
// These are the only two types that touch the byte stream. Keeping them small
// and total makes the frame codec auditable: every read either returns a value
// or a `rpc_code::protocol_error`, and every write either succeeds or reports
// `rpc_code::encode_error`. Neither type can read or write outside the span it
// was given, so a malformed frame cannot corrupt memory.
//
// Wire conventions (see docs/spec, "Wire protocol"):
//   * every multi-byte integer is little-endian,
//   * unsigned integers are base-128 varints (7 bits per byte, LSB first),
//   * signed integers are zigzag-mapped before varint encoding,
//   * a varint is at most 10 bytes for a u64; an 11th continuation bit is an
//     error rather than a silent truncation.
#ifndef CRPC_WIRE_HPP
#define CRPC_WIRE_HPP

#include <cstddef>
#include <cstring>
#include <limits>

#include <crpc/config.hpp>
#include <crpc/endian.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {

// --- Writer ------------------------------------------------------------------

class byte_writer {
public:
    explicit byte_writer(span<std::byte> out) noexcept : out_(out) {}

    std::size_t written() const noexcept { return pos_; }
    std::size_t remaining() const noexcept { return out_.size() - pos_; }
    bool ok() const noexcept { return error_ == rpc_code::ok; }
    rpc_code error() const noexcept { return error_; }

    span<const std::byte> written_bytes() const noexcept {
        return span<const std::byte>(out_.data(), pos_);
    }

    bool put_u8(u8 value) noexcept {
        if (!ensure(1)) {
            return false;
        }
        out_[pos_++] = static_cast<std::byte>(value);
        return true;
    }

    bool put_u16(u16 value) noexcept {
        if (!ensure(2)) {
            return false;
        }
        endian::store_le16(out_.data() + pos_, value);
        pos_ += 2;
        return true;
    }

    bool put_u32(u32 value) noexcept {
        if (!ensure(4)) {
            return false;
        }
        endian::store_le32(out_.data() + pos_, value);
        pos_ += 4;
        return true;
    }

    bool put_u64(u64 value) noexcept {
        if (!ensure(8)) {
            return false;
        }
        endian::store_le64(out_.data() + pos_, value);
        pos_ += 8;
        return true;
    }

    bool put_f32(float value) noexcept { return put_u32(endian::float_to_bits(value)); }
    bool put_f64(double value) noexcept { return put_u64(endian::double_to_bits(value)); }

    // Base-128 varint. The encoder is loop-bounded by the type width, so it
    // cannot emit an over-long encoding.
    bool put_varint(u64 value) noexcept {
        while (value >= 0x80u) {
            if (!put_u8(static_cast<u8>((value & 0x7Fu) | 0x80u))) {
                return false;
            }
            value >>= 7;
        }
        return put_u8(static_cast<u8>(value));
    }

    bool put_varint_i64(i64 value) noexcept {
        const u64 zigzag = (static_cast<u64>(value) << 1) ^
                           static_cast<u64>(value >> 63);
        return put_varint(zigzag);
    }

    bool put_bytes(span<const std::byte> bytes) noexcept {
        if (!ensure(bytes.size())) {
            return false;
        }
        if (!bytes.empty()) {
            std::memcpy(out_.data() + pos_, bytes.data(), bytes.size());
        }
        pos_ += bytes.size();
        return true;
    }

    // Length-prefixed byte string: varint length, then the bytes.
    bool put_length_prefixed(span<const std::byte> bytes) noexcept {
        return put_varint(static_cast<u64>(bytes.size())) && put_bytes(bytes);
    }

    // Reserves `count` bytes and hands them back for the caller to fill. Used
    // for the frame header, whose length field is only known after the payload
    // has been encoded.
    bool put_zeroes(std::size_t count, span<std::byte>& out) noexcept {
        if (!ensure(count)) {
            return false;
        }
        out = span<std::byte>(out_.data() + pos_, count);
        std::memset(out.data(), 0, count);
        pos_ += count;
        return true;
    }

    // Overwrites previously reserved bytes. The caller must have reserved them.
    bool patch_u32(std::size_t offset, u32 value) noexcept {
        if (offset > pos_ || pos_ - offset < 4) {
            error_ = rpc_code::encode_error;
            return false;
        }
        endian::store_le32(out_.data() + offset, value);
        return true;
    }

private:
    bool ensure(std::size_t count) noexcept {
        if (error_ != rpc_code::ok) {
            return false;
        }
        if (count > out_.size() - pos_) {
            error_ = rpc_code::encode_error;
            return false;
        }
        return true;
    }

    span<std::byte> out_;
    std::size_t pos_ = 0;
    rpc_code error_ = rpc_code::ok;
};

// --- Reader ------------------------------------------------------------------

class byte_reader {
public:
    explicit byte_reader(span<const std::byte> in) noexcept : in_(in) {}

    std::size_t read() const noexcept { return pos_; }
    std::size_t remaining() const noexcept { return in_.size() - pos_; }
    bool ok() const noexcept { return error_ == rpc_code::ok; }
    rpc_code error() const noexcept { return error_; }

    span<const std::byte> remaining_bytes() const noexcept {
        return span<const std::byte>(in_.data() + pos_, in_.size() - pos_);
    }

    bool get_u8(u8& value) noexcept {
        if (!ensure(1)) {
            return false;
        }
        value = static_cast<u8>(in_[pos_++]);
        return true;
    }

    bool get_u16(u16& value) noexcept {
        if (!ensure(2)) {
            return false;
        }
        value = endian::load_le16(in_.data() + pos_);
        pos_ += 2;
        return true;
    }

    bool get_u32(u32& value) noexcept {
        if (!ensure(4)) {
            return false;
        }
        value = endian::load_le32(in_.data() + pos_);
        pos_ += 4;
        return true;
    }

    bool get_u64(u64& value) noexcept {
        if (!ensure(8)) {
            return false;
        }
        value = endian::load_le64(in_.data() + pos_);
        pos_ += 8;
        return true;
    }

    bool get_f32(float& value) noexcept {
        u32 bits = 0;
        if (!get_u32(bits)) {
            return false;
        }
        value = endian::bits_to_float(bits);
        return true;
    }

    bool get_f64(double& value) noexcept {
        u64 bits = 0;
        if (!get_u64(bits)) {
            return false;
        }
        value = endian::bits_to_double(bits);
        return true;
    }

    // Rejects a varint that runs past 10 bytes or whose final byte carries more
    // than the 64th bit: both indicate a malformed or hostile frame.
    bool get_varint(u64& value) noexcept {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            u8 byte = 0;
            if (!get_u8(byte)) {
                return false;
            }
            const u64 payload = static_cast<u64>(byte & 0x7Fu);
            if (shift == 63 && payload > 1u) {
                // Only bit 63 remains; anything else would overflow.
                error_ = rpc_code::protocol_error;
                return false;
            }
            value |= payload << shift;
            if ((byte & 0x80u) == 0) {
                return true;
            }
        }
        error_ = rpc_code::protocol_error;
        return false;
    }

    bool get_varint_i64(i64& value) noexcept {
        u64 zigzag = 0;
        if (!get_varint(zigzag)) {
            return false;
        }
        value = static_cast<i64>((zigzag >> 1) ^ (~(zigzag & 1u) + 1u));
        return true;
    }

    bool get_bytes(std::size_t count, span<const std::byte>& out) noexcept {
        if (!ensure(count)) {
            return false;
        }
        out = span<const std::byte>(in_.data() + pos_, count);
        pos_ += count;
        return true;
    }

    // Reads a varint length and then that many bytes. The length is validated
    // against the remaining input before any pointer arithmetic, so a hostile
    // length cannot produce an out-of-range span.
    bool get_length_prefixed(span<const std::byte>& out) noexcept {
        u64 length = 0;
        if (!get_varint(length)) {
            return false;
        }
        if (length > static_cast<u64>(remaining())) {
            error_ = rpc_code::protocol_error;
            return false;
        }
        return get_bytes(static_cast<std::size_t>(length), out);
    }

    bool skip(std::size_t count) noexcept {
        if (!ensure(count)) {
            return false;
        }
        pos_ += count;
        return true;
    }

private:
    bool ensure(std::size_t count) noexcept {
        if (error_ != rpc_code::ok) {
            return false;
        }
        if (count > in_.size() - pos_) {
            error_ = rpc_code::protocol_error;
            return false;
        }
        return true;
    }

    span<const std::byte> in_;
    std::size_t pos_ = 0;
    rpc_code error_ = rpc_code::ok;
};

} // namespace crpc

#endif // CRPC_WIRE_HPP
