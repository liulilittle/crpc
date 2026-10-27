// CRPC -- Redis scripting.
//
// Redis runs Lua through EVAL. EVALSHA is the version a server should see in
// production: the body travels once, and later calls send only its 40-character
// digest. That optimisation has one sharp edge, which is the whole reason this
// wrapper exists:
//
//   * a server may not have the script (it restarted, it ran SCRIPT FLUSH, or this
//     is a replica) and answers `NOSCRIPT`. Retrying with the body is the documented
//     recovery, and a caller that treats NOSCRIPT as a fatal error sees a
//     production outage after a failover;
//   * the key count is part of the request. Getting it wrong makes Redis hash the
//     arguments into the wrong slot in a cluster, so it is derived from the key list
//     rather than passed by hand;
//   * arguments are binary-safe strings, never formatted into the Lua source.
#ifndef CRPC_REDIS_SCRIPTING_HPP
#define CRPC_REDIS_SCRIPTING_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/redis/resp.hpp>

namespace crpc {
namespace redis {

// A script body and its digest. The digest is computed once, from the exact bytes
// that will be sent, because Redis checksums the body verbatim.
class CRPC_API script {
public:
    explicit script(std::string source);

    [[nodiscard]] const std::string& source() const noexcept { return source_; }
    [[nodiscard]] const std::string& digest() const noexcept { return digest_; }

private:
    std::string source_;
    std::string digest_;
};

// Runs scripts over one command stream.
class CRPC_API script_runner {
public:
    explicit script_runner(command_executor& executor) noexcept : executor_(&executor) {}

    // EVALSHA, falling back to EVAL once when the server answers NOSCRIPT.
    //
    // `keys` are declared to Redis as KEYS (so a cluster can route the call); `args`
    // are ARGV. Both are binary-safe.
    [[nodiscard]] result<resp_value> run(const script& body, const std::vector<std::string>& keys,
                                        const std::vector<std::string>& args = {});

    // Builds the script from `source` and runs it; the digest is not cached, so a
    // repeated call pays the SHA-1 each time. Use `script` + `run` in a loop.
    [[nodiscard]] result<resp_value> run_source(std::string_view source,
                                                const std::vector<std::string>& keys,
                                                const std::vector<std::string>& args = {});

    // SCRIPT LOAD. Returns the digest the server reports, which must equal the one
    // computed locally; a mismatch means the body was altered in transit.
    [[nodiscard]] result<std::string> load(const script& body);

    // SCRIPT EXISTS for one digest.
    [[nodiscard]] result<bool> exists(const script& body);

    // SCRIPT FLUSH. Every cached digest becomes NOSCRIPT until loaded again.
    [[nodiscard]] status flush();

private:
    [[nodiscard]] result<resp_value> eval(const script& body, const std::vector<std::string>& keys,
                                          const std::vector<std::string>& args);

    command_executor* executor_ = nullptr;
};

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_SCRIPTING_HPP