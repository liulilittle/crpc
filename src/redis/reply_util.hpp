// CRPC Redis -- small reply helpers shared by the feature modules.
//
// RESP2 and RESP3 distinguish a simple string (`+`) from a bulk string (`$`),
// but a command reply is text either way. The feature layer wants "the text of
// this reply" without caring which wire form carried it, so these helpers fold
// the string alternatives together. They also fold the two error alternatives
// (`-` and RESP3 `!`).
#ifndef CRPC_REDIS_REPLY_UTIL_HPP
#define CRPC_REDIS_REPLY_UTIL_HPP

#include <string>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/redis/resp.hpp>

namespace crpc {
namespace redis {
namespace detail {

inline bool is_text(const resp_value& value) noexcept {
    return value.is_simple_string() || value.is_bulk_string() ||
           value.is_verbatim_string() || value.is_big_number();
}

inline const std::string& as_text(const resp_value& value) noexcept {
    if (value.is_simple_string()) {
        return value.as_simple_string();
    }
    if (value.is_bulk_string()) {
        return value.as_bulk_string();
    }
    if (value.is_verbatim_string()) {
        return value.as_verbatim_string();
    }
    return value.as_big_number();
}

inline bool is_error_reply(const resp_value& value) noexcept {
    return value.is_error() || value.is_blob_error();
}

inline const std::string& error_message(const resp_value& value) noexcept {
    return value.is_error() ? value.as_error() : value.as_blob_error();
}

inline rpc_error application_error(const resp_value& value) {
    return rpc_error{rpc_code::application, error_message(value)};
}

inline rpc_error protocol_error(std::string message) {
    return rpc_error{rpc_code::protocol_error, std::move(message)};
}

inline const std::vector<resp_value>* array_items(const resp_value& value) noexcept {
    if (value.is_array()) {
        return &value.as_array();
    }
    if (value.is_push()) {
        return &value.as_push();
    }
    if (value.is_set()) {
        return &value.as_set();
    }
    return nullptr;
}

} // namespace detail
} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_REPLY_UTIL_HPP
