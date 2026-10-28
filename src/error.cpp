// CRPC -- error code names, std::error_code bridge and HTTP mapping.
#include <crpc/error.hpp>

#include <string>

namespace crpc {
namespace {

class rpc_error_category_impl final : public std::error_category {
public:
    const char* name() const noexcept override { return "crpc.rpc"; }

    std::string message(int value) const override {
        return to_string(static_cast<rpc_code>(value));
    }
};

} // namespace

const char* to_string(rpc_code code) noexcept {
    switch (code) {
    case rpc_code::ok:                  return "ok";
    case rpc_code::canceled:            return "canceled";
    case rpc_code::deadline_exceeded:   return "deadline_exceeded";
    case rpc_code::unavailable:         return "unavailable";
    case rpc_code::connection_refused:  return "connection_refused";
    case rpc_code::connection_reset:    return "connection_reset";
    case rpc_code::broken_pipe:         return "broken_pipe";
    case rpc_code::eof:                 return "eof";
    case rpc_code::overload:            return "overload";
    case rpc_code::not_found:           return "not_found";
    case rpc_code::no_such_method:      return "no_such_method";
    case rpc_code::invalid_argument:    return "invalid_argument";
    case rpc_code::failed_precondition: return "failed_precondition";
    case rpc_code::resource_exhausted:  return "resource_exhausted";
    case rpc_code::protocol_error:      return "protocol_error";
    case rpc_code::decode_error:        return "decode_error";
    case rpc_code::encode_error:        return "encode_error";
    case rpc_code::not_acknowledged:    return "not_acknowledged";
    case rpc_code::unknown_side_effect: return "unknown_side_effect";
    case rpc_code::internal:            return "internal";
    case rpc_code::shutting_down:       return "shutting_down";
    case rpc_code::application:         return "application";
    case rpc_code::permission_denied:   return "permission_denied";
    }
    // An out-of-range value can only arrive from a hostile or newer peer; the
    // frame codec rejects unknown codes before they reach this function.
    return "unknown";
}

const std::error_category& rpc_error_category() noexcept {
    static const rpc_error_category_impl instance;
    return instance;
}

int to_http_status(rpc_code code) noexcept {
    switch (code) {
    case rpc_code::ok:                  return 200;
    case rpc_code::canceled:            return 499; // client closed request
    case rpc_code::deadline_exceeded:   return 504;
    case rpc_code::unavailable:         return 503;
    case rpc_code::connection_refused:  return 503;
    case rpc_code::connection_reset:    return 502;
    case rpc_code::broken_pipe:         return 502;
    case rpc_code::eof:                 return 502;
    case rpc_code::overload:            return 429;
    case rpc_code::not_found:           return 404;
    case rpc_code::no_such_method:      return 404;
    case rpc_code::invalid_argument:    return 400;
    case rpc_code::failed_precondition: return 409;
    case rpc_code::resource_exhausted:  return 429;
    case rpc_code::protocol_error:      return 400;
    case rpc_code::decode_error:        return 400;
    case rpc_code::encode_error:        return 500;
    case rpc_code::not_acknowledged:    return 503;
    case rpc_code::unknown_side_effect: return 504;
    case rpc_code::internal:            return 500;
    case rpc_code::shutting_down:       return 503;
    case rpc_code::application:         return 500;
    case rpc_code::permission_denied:   return 403;
    }
    return 500;
}

} // namespace crpc
