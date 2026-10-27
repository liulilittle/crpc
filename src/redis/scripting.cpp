// CRPC -- Redis scripting.
#include <crpc/redis/scripting.hpp>

#include <utility>

#include <crpc/crypto/hash.hpp>

namespace crpc {
namespace redis {
namespace {

[[nodiscard]] std::string sha1_hex(std::string_view text) {
    const auto digest = crypto::sha1(span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size()));
    return crypto::to_hex(digest);
}

// True when the reply is the NOSCRIPT error. Redis sends it as
// "NOSCRIPT No matching script. Please use EVAL."; only the prefix is contractual,
// so the rest is not matched.
[[nodiscard]] bool is_noscript(const resp_value& reply) {
    if (!reply.is_error()) {
        return false;
    }
    const std::string& message = reply.as_error();
    return message.size() >= 8 && message.compare(0, 8, "NOSCRIPT") == 0;
}

[[nodiscard]] command build_eval(std::string_view verb, std::string_view body_or_digest,
                                 const std::vector<std::string>& keys,
                                 const std::vector<std::string>& args) {
    command cmd;
    cmd.reserve(3 + keys.size() + args.size());
    cmd.emplace_back(verb);
    cmd.emplace_back(body_or_digest);
    // The key count is derived, never passed: a wrong count makes a cluster hash the
    // arguments into the wrong slot instead of failing loudly.
    cmd.emplace_back(std::to_string(keys.size()));
    for (const std::string& key : keys) {
        cmd.push_back(key);
    }
    for (const std::string& arg : args) {
        cmd.push_back(arg);
    }
    return cmd;
}

} // namespace

script::script(std::string source) : source_(std::move(source)), digest_(sha1_hex(source_)) {}

result<resp_value> script_runner::eval(const script& body, const std::vector<std::string>& keys,
                                       const std::vector<std::string>& args) {
    return executor_->execute(build_eval("EVAL", body.source(), keys, args));
}

result<resp_value> script_runner::run(const script& body, const std::vector<std::string>& keys,
                                      const std::vector<std::string>& args) {
    result<resp_value> reply =
        executor_->execute(build_eval("EVALSHA", body.digest(), keys, args));
    if (!reply) {
        return reply;
    }
    if (!is_noscript(reply.value())) {
        return reply;
    }
    // The server does not have the script: send the body. This is the documented
    // recovery, and doing it here is what makes a failover survivable for a caller
    // that only ever calls EVALSHA.
    return eval(body, keys, args);
}

result<resp_value> script_runner::run_source(std::string_view source,
                                             const std::vector<std::string>& keys,
                                             const std::vector<std::string>& args) {
    const script body{std::string(source)};
    return run(body, keys, args);
}

result<std::string> script_runner::load(const script& body) {
    command cmd{"SCRIPT", "LOAD", body.source()};
    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    if (reply.value().is_error()) {
        return rpc_error{rpc_code::application, reply.value().as_error()};
    }
    const std::string reported = reply.value().is_bulk_string()
                                     ? reply.value().as_bulk_string()
                                     : (reply.value().is_simple_string()
                                            ? reply.value().as_simple_string()
                                            : std::string{});
    if (reported.empty()) {
        return rpc_error{rpc_code::protocol_error, "SCRIPT LOAD returned no digest"};
    }
    // A mismatch means the server hashed different bytes than we did, so a later
    // EVALSHA with our digest would never hit.
    if (reported != body.digest()) {
        return rpc_error{rpc_code::protocol_error,
                         "SCRIPT LOAD digest does not match the local digest"};
    }
    return reported;
}

result<bool> script_runner::exists(const script& body) {
    command cmd{"SCRIPT", "EXISTS", body.digest()};
    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    if (reply.value().is_error()) {
        return rpc_error{rpc_code::application, reply.value().as_error()};
    }
    if (!reply.value().is_array() || reply.value().as_array().empty()) {
        return rpc_error{rpc_code::protocol_error, "SCRIPT EXISTS returned no element"};
    }
    const resp_value& first = reply.value().as_array().front();
    if (!first.is_integer()) {
        return rpc_error{rpc_code::protocol_error, "SCRIPT EXISTS returned a non-integer"};
    }
    return first.as_integer() != 0;
}

status script_runner::flush() {
    command cmd{"SCRIPT", "FLUSH"};
    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    if (reply.value().is_error()) {
        return status{rpc_code::application, reply.value().as_error()};
    }
    return status{};
}

} // namespace redis
} // namespace crpc