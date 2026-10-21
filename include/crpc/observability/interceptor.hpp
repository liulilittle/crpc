// CRPC -- AOP interceptor chain.
//
// The chain is a fixed-capacity table of interceptor pointers, one table per
// hook point. It allocates nothing, ever, and it is frozen before the server
// starts serving. Two properties are deliberate:
//
//   * Empty hot path: `run()` first loads the per-hook count. When the count is
//     zero it returns true. That is one bounds check, one byte load and one
//     return: no virtual call, no loop, no allocation. A hook with no
//     interceptor therefore costs a single predictable branch.
//   * No std::function: interceptors are plain virtual objects registered at
//     startup. The only indirect call is the virtual `on_hook` of an
//     interceptor that is actually present.
//
// Interceptors may observe, mutate the call context (deadline, priority,
// metadata carried by the caller), or short-circuit by returning false. When
// the translation unit is built with exception support, a hook that throws is
// caught by the chain and converted to `rpc_code::internal`; no exception
// escapes into the RPC path. In a build without C++ exception support
// (`-fno-exceptions`, or MSVC without an exception-enabling `/EH` option) there
// is no try/catch (see `run()`); a throwing hook is then undefined behaviour and
// in practice terminates the process.
//
// Thread safety: `add()` and `freeze()` are startup-only and are not
// synchronised. After `freeze()` the chain is immutable and `run()` may be
// called concurrently from any thread. `call_context` is a per-call value and
// must not be shared between concurrent calls.
#ifndef CRPC_OBSERVABILITY_INTERCEPTOR_HPP
#define CRPC_OBSERVABILITY_INTERCEPTOR_HPP

#include <array>
#include <cstddef>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/observability/context.hpp>

namespace crpc {
namespace obs {

// Hook points cover the client/server call, serialisation, retry, breaker and
// connection lifecycle. `count` is a sentinel, never registered.
enum class hook_point : u8 {
    before_call = 0,
    after_call,
    before_serialize,
    after_deserialize,
    on_retry,
    on_breaker_event,
    on_connect,
    on_close,
    count
};

[[nodiscard]] constexpr std::size_t hook_point_count() noexcept {
    return static_cast<std::size_t>(hook_point::count);
}

[[nodiscard]] inline const char* to_string(hook_point point) noexcept {
    switch (point) {
    case hook_point::before_call:
        return "before_call";
    case hook_point::after_call:
        return "after_call";
    case hook_point::before_serialize:
        return "before_serialize";
    case hook_point::after_deserialize:
        return "after_deserialize";
    case hook_point::on_retry:
        return "on_retry";
    case hook_point::on_breaker_event:
        return "on_breaker_event";
    case hook_point::on_connect:
        return "on_connect";
    case hook_point::on_close:
        return "on_close";
    case hook_point::count:
        return "count";
    }
    return "unknown";
}

// Mutable per-call state visible to every interceptor in a chain.
//
// `service` and `method` are borrowed views: they must outlive the call and
// are never owned. `error_message` is an inline buffer so that a
// short-circuiting interceptor can attach a diagnostic without allocating.
//
// Allocation: none until an interceptor populates `metadata`; the empty list
// does not allocate.
struct call_context {
    trace_context trace;
    obs::deadline deadline;
    u8 priority = 0;
    std::string_view service;
    std::string_view method;
    metadata_list metadata;

    // Set by a short-circuiting interceptor or by the chain when a hook
    // throws. `ok` means "continue".
    rpc_code result = rpc_code::ok;
    char error_message[128] = {};

    void set_error(rpc_code code, std::string_view message = {}) noexcept {
        result = code;
        const std::size_t count =
            message.size() < sizeof(error_message) - 1 ? message.size() : sizeof(error_message) - 1;
        if (count != 0) {
            for (std::size_t i = 0; i < count; ++i) {
                error_message[i] = message[i];
            }
        }
        error_message[count] = '\0';
    }

    [[nodiscard]] bool ok() const noexcept { return result == rpc_code::ok; }
};

// Base class for every interceptor.
//
// `on_hook` returns true to continue the chain and false to short-circuit. It
// must not block; an implementation may throw (for example an assertion in a
// third-party library) and, in an exception-enabled build, the chain converts
// that to `internal`.
class interceptor {
public:
    virtual ~interceptor() = default;

    [[nodiscard]] virtual bool on_hook(hook_point point, call_context& context) = 0;
};

// Fixed-capacity, freeze-once chain.
class interceptor_chain {
public:
    static constexpr std::size_t max_interceptors_per_hook = 8;

    interceptor_chain() noexcept = default;
    interceptor_chain(const interceptor_chain&) = delete;
    interceptor_chain& operator=(const interceptor_chain&) = delete;

    // Startup-only. Returns false for an invalid hook point, a null
    // interceptor, a full table, or after freeze().
    [[nodiscard]] bool add(hook_point point, interceptor* hook) noexcept {
        if (frozen_ || hook == nullptr) {
            return false;
        }
        const std::size_t index = static_cast<std::size_t>(point);
        if (index >= hook_point_count()) {
            return false;
        }
        const u8 count = counts_[index];
        if (count >= max_interceptors_per_hook) {
            return false;
        }
        hooks_[index][count] = hook;
        counts_[index] = static_cast<u8>(count + 1);
        return true;
    }

    // Makes the table read-only. Idempotent.
    void freeze() noexcept { frozen_ = true; }
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    [[nodiscard]] std::size_t count(hook_point point) const noexcept {
        const std::size_t index = static_cast<std::size_t>(point);
        if (index >= hook_point_count()) {
            return 0;
        }
        return counts_[index];
    }

    // Runs every interceptor registered for `point` in registration order.
    //
    // Empty chain: one bounds check plus one count load, then return true.
    // Non-empty chain: one virtual call per registered interceptor. In an
    // exception-enabled build a thrown hook stops the chain, sets `result` to
    // `rpc_code::internal` and returns false. A hook returning false also stops
    // the chain and returns false; the hook is responsible for setting
    // `context.result`. In a build without exception support there is no
    // catch: see the comment on the `#else` branch below.
    [[nodiscard]] bool run(hook_point point, call_context& context) const noexcept {
        const std::size_t index = static_cast<std::size_t>(point);
        if (index >= hook_point_count()) {
            return true;
        }
        const u8 count = counts_[index];
        if (count == 0) {
            return true;
        }

        for (u8 i = 0; i < count; ++i) {
            interceptor* hook = hooks_[index][i];
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            try {
                if (!hook->on_hook(point, context)) {
                    if (context.ok()) {
                        context.set_error(rpc_code::canceled,
                                          "interceptor short-circuited");
                    }
                    return false;
                }
            } catch (...) {
                context.set_error(rpc_code::internal, "interceptor threw");
                return false;
            }
#else
            // Built without exception support (-fno-exceptions, /EHs-c-), and
            // also MSVC with no /EH option at all: `_CPPUNWIND` is undefined in
            // both, so this branch is taken. There is no way to observe a
            // throw, so the hook is called directly. A hook that throws in such
            // a build is undefined behaviour; in practice the implementation
            // calls `std::terminate` (observed with MSVC /EHs-c- and with
            // GCC/Clang -fno-exceptions). The short-circuit path is unchanged.
            // Note: an MSVC build that does not enable C++ exceptions (no /EH
            // option, or /EHc-, /EHs-, /EHa-, /EHs-c-, /EHa-c-) used to reach
            // the catch above: C4530 is only a warning at /W1-/W4 and is silent
            // at /W0. Such a build now lets the exception escape. Under /W4 /WX
            // the old header failed inside this header, while this header
            // compiles, so a consumer that never enabled /EH moves from "does
            // not build" to "builds and terminates on throw".
            if (!hook->on_hook(point, context)) {
                if (context.ok()) {
                    context.set_error(rpc_code::canceled,
                                      "interceptor short-circuited");
                }
                return false;
            }
#endif
        }
        return true;
    }

private:
    std::array<std::array<interceptor*, max_interceptors_per_hook>, hook_point_count()> hooks_{};
    std::array<u8, hook_point_count()> counts_{};
    bool frozen_ = false;
};

} // namespace obs
} // namespace crpc

#endif // CRPC_OBSERVABILITY_INTERCEPTOR_HPP
