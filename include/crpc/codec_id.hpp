// CRPC -- codec identifiers.
//
// This is the single authority for the numeric values that travel on the wire.
// It lives in its own header because two layers need it and neither may include
// the other: the frame codec (which writes the id into the call envelope) and
// the codec registry (which maps an id to an implementation). A duplicate
// definition in either header would be an ODR violation the moment a
// translation unit included both -- which is exactly what happened before this
// file existed.
//
// Rules:
//   - the values are part of the protocol and are never renumbered or reused;
//   - 0 is reserved for "unspecified"; a sender must always write a real id, and
//     a receiver that sees 0 rejects the envelope (fail closed);
//   - new ids are appended.
#ifndef CRPC_CODEC_ID_HPP
#define CRPC_CODEC_ID_HPP

#include <crpc/config.hpp>

namespace crpc {

enum class codec_id : u16 {
    unspecified = 0,
    crpc_binary = 1,
    json = 2,
    xml = 3,
    ini = 4,
    toml = 5,
    protobuf = 6,
    flatbuffers = 7
};

// Highest assigned value. A decoder rejects anything above it rather than
// guessing at a future format.
inline constexpr u16 codec_id_max = static_cast<u16>(codec_id::flatbuffers);

[[nodiscard]] constexpr bool is_known_codec(codec_id codec) noexcept {
    const u16 value = static_cast<u16>(codec);
    return value >= static_cast<u16>(codec_id::crpc_binary) && value <= codec_id_max;
}

// A stable, human-readable name. Never nullptr.
CRPC_API const char* to_string(codec_id codec) noexcept;

// Parses a name produced by to_string(). Returns codec_id::unspecified when the
// name is unknown.
CRPC_API codec_id codec_id_from_string(const char* name) noexcept;

} // namespace crpc

#endif // CRPC_CODEC_ID_HPP
