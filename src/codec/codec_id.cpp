// CRPC -- codec identifier names.
#include <crpc/codec_id.hpp>

#include <cstring>

namespace crpc {

const char* to_string(codec_id codec) noexcept {
    switch (codec) {
    case codec_id::unspecified:  return "unspecified";
    case codec_id::crpc_binary:  return "crpc-binary";
    case codec_id::json:         return "json";
    case codec_id::xml:          return "xml";
    case codec_id::ini:          return "ini";
    case codec_id::toml:         return "toml";
    case codec_id::protobuf:     return "protobuf";
    case codec_id::flatbuffers:  return "flatbuffers";
    }
    // An out-of-range value can only come from a hostile or newer peer; the
    // frame codec rejects unknown ids before they reach this function.
    return "unknown";
}

codec_id codec_id_from_string(const char* name) noexcept {
    if (name == nullptr) {
        return codec_id::unspecified;
    }
    struct entry {
        const char* name;
        codec_id id;
    };
    static const entry table[] = {
        {"crpc-binary", codec_id::crpc_binary},
        {"json", codec_id::json},
        {"xml", codec_id::xml},
        {"ini", codec_id::ini},
        {"toml", codec_id::toml},
        {"protobuf", codec_id::protobuf},
        {"flatbuffers", codec_id::flatbuffers},
    };
    for (const entry& e : table) {
        if (std::strcmp(e.name, name) == 0) {
            return e.id;
        }
    }
    return codec_id::unspecified;
}

} // namespace crpc
