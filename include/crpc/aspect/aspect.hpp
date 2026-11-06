// CRPC -- AOP aspect chain (before / after / around / after-throwing advice).
//
// This module is the aspect-oriented counterpart of `obs::interceptor_chain`.
// The interceptor chain answers "may this call proceed?" at a fixed set of hook
// points; an aspect chain answers "what should wrap this call?" for an
// arbitrary named target. The two are independent: a caller may run both, and
// neither knows about the other (see the coexistence case in
// tests/test_aspect.cpp).
//
// Model
// -----
// One `invoke()` is one join point. Every advice whose pointcut matches the
// target contributes one layer of an onion:
//
//     before()  ->  around(proceed)  ->  [next layer ... body]  ->  after()
//                                                                 \-> after_throwing()
//
// * `order()` ascending puts an advice further outside; equal orders keep the
//   registration order (stable insertion sort, sized at compile time).
// * `around()` receives a `proceed` callable. Calling it runs the next inner
//   layer and eventually the body; not calling it skips the body entirely, and
//   calling it twice runs the body twice (both are legal: the callee decides).
// * `before()` may clear `join_point::proceed` to abort the call. The call then
//   does not enter `around()` or the body, and the outcome defaults to
//   `rpc_code::canceled` unless the advice already set a different code.
// * `after()` runs only when the final outcome is `ok`; every other outcome --
//   a non-ok code returned by the body, a code forced by an advice, an
//   exception thrown by the body, an exception thrown by an advice, or an
//   aborted call -- unwinds through `after_throwing()`. Nothing escapes
//   `invoke()`: a thrown exception is converted to `rpc_code::internal`
//   exactly like `obs::interceptor_chain` does.
// * `join_point::elapsed` is measured on `std::chrono::steady_clock` from the
//   start of `invoke()` and refreshed before each `after`/`after_throwing`
//   call, so an advice sees the wall time of everything inside it. It is not
//   meaningful inside `around()` before `proceed()` returns.
//
// Thread contract
// ---------------
// * `add()` and `freeze()` are startup-only and deliberately NOT synchronised:
//   calling `add()` from two threads at the same time is undefined behaviour,
//   not merely racy output -- the table and the onion order would both corrupt.
//   Register from one thread, then freeze.
// * After `freeze()` the chain is immutable and `invoke()` may be called
//   concurrently from any thread. The chain itself takes no lock and keeps no
//   mutable state; an advice that keeps state must synchronise it itself.
// * `invoke()` is exception-neutral at its boundary in one direction only: an
//   exception thrown by an advice or by the body is contained (converted to
//   `rpc_code::internal`, never escaping). What is NOT covered is the
//   construction of the caller-supplied arguments: materialising the
//   `std::function<rpc_code()> body` parameter from a caller lambda happens at
//   the call site before `invoke()` runs, and a `std::bad_alloc` (or any other
//   exception) thrown there propagates out of the call expression untouched.
//
// Advice exception semantics (pinned by tests/test_aspect.cpp)
// ------------------------------------------------------------
//   before() throws        -> call aborted, body not executed, outcome becomes
//                             `internal` (or the advice's pre-set code), this
//                             layer still sees after_throwing();
//   around() throws        -> outcome becomes `internal`, this layer still sees
//                             after_throwing(), inner layers already run keep
//                             their own unwinding;
//   after() throws         -> this layer does NOT get a compensating
//                             after_throwing() call (the unwind phase had
//                             already chosen one callback); the containment
//                             rule overwrites the outcome with `internal`;
//   after_throwing() throws-> outcome becomes `internal`, swallowing the
//                             body's original error code;
//   nothing thrown by any callback ever escapes invoke().
//
// Pointcuts
// ---------
// The framework has no regex engine (`include/crpc/text` ships case, number,
// string and UTF helpers only) and `std::regex` is deliberately not used: it
// allocates, it is slow, and its behaviour differs across standard libraries.
// The supported matchers are therefore exact name, prefix, suffix and
// all. Wildcards are expressible as a prefix and a suffix pair by registering
// two advices; a general regular-expression matcher is not part of this API.
//
// Cost
// ----
// * `freeze()` is startup-only; afterwards the table is immutable and
//   `invoke()` may be called concurrently from any thread. The chain holds no
//   lock and no mutable state.
// * No matching advice: one pass over the registered entries, one short-lived
//   comparator per entry, one `std::function` call for the body. No allocation,
//   no virtual call.
// * Matching advice: the matched subset is gathered into a fixed stack array
//   (`max_aspects` pointers) and stably sorted by `(order, registration)`.
//   The onion is then entered recursively; each layer builds one small
//   `std::function<void()>` whose capture fits the small-object buffer of
//   every supported standard library, so a matched call does not allocate
//   either. One virtual `before` and one virtual `around` per layer.
//
// The borrowed `pointcut` name is a `std::string_view`: it must outlive the
// chain. Registering string literals or other startup-lifetime names is the
// intended use, and it keeps registration allocation-free as well.
#ifndef CRPC_ASPECT_ASPECT_HPP
#define CRPC_ASPECT_ASPECT_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/text/string_ops.hpp>

namespace crpc {

// --- result<rpc_code> --------------------------------------------------------
//
// The generic `result<T>` primary template cannot be instantiated with
// `rpc_code`: its value constructor `result(T)` and its convenience error
// constructor `result(rpc_code)` collapse into the same signature (verified:
// MSVC C2535). `aspect_chain::invoke()` returns `result<rpc_code>`, so the
// specialization below makes that type meaningful instead of changing the
// error model. It lives here rather than in error.hpp because this module is
// the only producer of that instantiation and error.hpp is part of the
// documented error ABI surface.
//
// Representation: a bare `rpc_code` plus an OPTIONAL `rpc_error`. The optional
// is engaged only when the caller supplied a full `rpc_error`, which keeps
// every construction allocation-free -- including on the MSVC debug CRT, where
// even an empty `std::string` and an empty `std::vector` each allocate a
// container proxy (an embedded `rpc_error` member would cost two allocations
// per `invoke()` return in a debug build). `code()` is authoritative for the
// failure code; `error()` carries diagnostics only when a full `rpc_error` was
// supplied, and otherwise returns a process-wide default (constructed once,
// first use).
template <>
class [[nodiscard]] result<rpc_code> {
public:
    using value_type = rpc_code;

    result(rpc_code code) noexcept : code_(code) {}

    result(rpc_error error) : code_(error.code) {
        if (error.code != rpc_code::ok) {
            error_.emplace(std::move(error));
        }
    }

    [[nodiscard]] bool ok() const noexcept { return code_ == rpc_code::ok; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] rpc_code code() const noexcept { return code_; }

    // Precondition: ok(). As with the primary template, reading the value of a
    // failure is a programming error; unlike the primary template there is no
    // value slot to read, so the code is returned unchanged.
    [[nodiscard]] rpc_code value() const noexcept { return code_; }

    [[nodiscard]] rpc_code value_or(rpc_code fallback) const noexcept {
        return ok() ? code_ : fallback;
    }

    // The carried error. When no full `rpc_error` was supplied this is a
    // process-wide default (code `ok`, empty message); use `code()` for the
    // failure code itself.
    [[nodiscard]] const rpc_error& error() const noexcept {
        return error_.has_value() ? *error_ : default_error();
    }

    [[nodiscard]] rpc_error error_or(rpc_error fallback) const {
        return error_.has_value() ? *error_ : std::move(fallback);
    }

private:
    static const rpc_error& default_error() noexcept {
        static const rpc_error value(rpc_code::ok);
        return value;
    }

    rpc_code code_ = rpc_code::ok;
    std::optional<rpc_error> error_;
};

namespace aspect {

// --- Join point --------------------------------------------------------------

// The per-call context handed to every advice.
//
// `target` is a borrowed view of the caller's target string. `args` and
// `result` are opaque pointers whose shape is agreed between the caller and the
// advice; the chain never dereferences them. `outcome` is authoritative: the
// body's return code is stored there, and an advice may overwrite it (the
// outermost surviving value is what `invoke()` returns). `elapsed` is refreshed
// from `std::chrono::steady_clock` before each after callback.
//
// Allocation: `join_point` is a plain value with no owning member; constructing
// one and reusing it for a whole call allocates nothing.
struct join_point {
    std::string_view target;
    void* args = nullptr;
    void* result = nullptr;
    rpc_code outcome = rpc_code::ok;
    std::chrono::nanoseconds elapsed{0};
    // Cleared by a `before` advice to abort the call. An `around` advice that
    // wants to stop the body simply does not call `proceed`.
    bool proceed = true;
};

// --- Advice ------------------------------------------------------------------

// Base class for every aspect. All callbacks are optional; the default
// implementation of `around` just proceeds. Advices are registered once at
// startup (a pointer is stored, never copied) and must outlive the chain.
//
// Thread safety: all callbacks are invoked concurrently from any thread that
// calls `invoke()` once the chain is frozen. An implementation that keeps state
// must synchronise it.
class CRPC_API advice {
public:
    virtual ~advice() = default;

    virtual void before(join_point&) {}
    virtual void after(join_point&) {}
    virtual void after_throwing(join_point&) {}

    // `proceed` runs the next inner layer (and, at the innermost position, the
    // body). Calling it zero, one or several times is legal.
    virtual void around(join_point&, std::function<void()> proceed) { proceed(); }

    // Ascending order means "further outside"; ties keep registration order.
    virtual int order() const noexcept { return 0; }
};

// --- Pointcut ----------------------------------------------------------------

// Which targets an advice applies to. A default-constructed pointcut matches
// nothing (`kind::none`), so a zero-initialised slot can never fire by
// accident.
class CRPC_API pointcut {
public:
    enum class kind : unsigned char { none = 0, exact, prefix, suffix, all };

    pointcut() noexcept = default;

    // The name must outlive the chain; startup-lifetime names (literals,
    // static strings) are the intended use.
    static pointcut on(std::string_view exact_name);
    static pointcut on_prefix(std::string_view prefix);
    static pointcut on_suffix(std::string_view suffix);
    static pointcut on_all() noexcept;

    // The pattern has a legal form (non-empty for the named kinds).
    [[nodiscard]] bool valid() const noexcept {
        return kind_ == kind::all || (kind_ != kind::none && !name_.empty());
    }

    [[nodiscard]] bool matches(std::string_view target) const noexcept {
        switch (kind_) {
        case kind::exact:
            return target == name_;
        case kind::prefix:
            return text::starts_with(target, name_);
        case kind::suffix:
            return text::ends_with(target, name_);
        case kind::all:
            return true;
        case kind::none:
            break;
        }
        return false;
    }

    [[nodiscard]] kind type() const noexcept { return kind_; }
    [[nodiscard]] std::string_view pattern() const noexcept { return name_; }

private:
    pointcut(kind type, std::string_view name) noexcept : name_(name), kind_(type) {}

    std::string_view name_;
    kind kind_ = kind::none;
};

// --- Chain -------------------------------------------------------------------

class CRPC_API aspect_chain {
public:
    // Fixed capacity: the whole table lives inside the object, so a frozen chain
    // is a single cache-friendly block and `invoke()` never touches the heap.
    static constexpr std::size_t max_aspects = 16;

    aspect_chain() noexcept = default;
    aspect_chain(const aspect_chain&) = delete;
    aspect_chain& operator=(const aspect_chain&) = delete;

    // Startup-only registration.
    //
    //   invalid_argument     null advice, or a pointcut that matches nothing
    //                        (`pointcut::on("")`, a default pointcut, ...)
    //   failed_precondition  the chain is already frozen
    //   resource_exhausted   `max_aspects` entries are already registered
    //   ok                   registered
    //
    // Not synchronised: two threads calling `add()` concurrently is undefined
    // behaviour (see the thread contract in the file comment). Single
    // registration thread, then freeze.
    [[nodiscard]] status add(pointcut pc, advice* adv);

    // Makes the table read-only. Idempotent. After this call `invoke()` is safe
    // to call concurrently from any thread; nothing in the chain mutates.
    void freeze() noexcept { frozen_ = true; }
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    [[nodiscard]] std::size_t count() const noexcept { return count_; }

    // Runs `body` through the onion of matching advices.
    //
    // Empty or non-matching chain: the body runs directly, wrapped in the same
    // exception boundary, with no allocation and no virtual call.
    // Matching chain: advices are gathered and ordered on the stack, then the
    // onion is entered; see the file comment for the exact unwind rules.
    //
    // `body` is taken by value so that a caller's lambda is turned into a
    // `std::function` exactly once; a small capture (the common case: one or
    // two pointers) stays inside the small-object buffer and does not allocate.
    [[nodiscard]] result<rpc_code> invoke(std::string_view target, void* args, void* result_out,
                                          std::function<rpc_code()> body);

private:
    struct entry {
        pointcut pc;
        advice* adv = nullptr;
    };

    struct invoke_state {
        advice* layers[max_aspects] = {};
        int orders[max_aspects] = {};
        std::size_t count = 0;
        join_point* jp = nullptr;
        std::function<rpc_code()>* body = nullptr;
        std::chrono::steady_clock::time_point start{};
    };

    static void run_layer(invoke_state& state, std::size_t index) noexcept;

    std::array<entry, max_aspects> entries_{};
    std::size_t count_ = 0;
    bool frozen_ = false;
};

// --- Implementation ----------------------------------------------------------

namespace detail {

inline void set_elapsed(join_point& jp, std::chrono::steady_clock::time_point start) noexcept {
    jp.elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start);
}

} // namespace detail

inline void aspect_chain::run_layer(invoke_state& state, std::size_t index) noexcept {
    join_point& jp = *state.jp;

    // Innermost position: run the body. `outcome` is overwritten by the body's
    // return value; an exception becomes `internal`.
    if (index == state.count) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        try {
            jp.outcome = (*state.body)();
        } catch (...) {
            jp.outcome = rpc_code::internal;
        }
#else
        jp.outcome = (*state.body)();
#endif
        detail::set_elapsed(jp, state.start);
        return;
    }

    advice* adv = state.layers[index];

    // before(): an advice that throws is contained; the call is aborted.
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        adv->before(jp);
    } catch (...) {
        jp.proceed = false;
        jp.outcome = rpc_code::internal;
    }
#else
    adv->before(jp);
#endif

    if (!jp.proceed) {
        // Aborted: no around(), no body. The outcome defaults to `canceled`;
        // an advice that set a specific code keeps it.
        if (jp.outcome == rpc_code::ok) {
            jp.outcome = rpc_code::canceled;
        }
    } else {
        auto proceed = [&state, index]() { run_layer(state, index + 1); };
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        try {
            adv->around(jp, std::function<void()>(proceed));
        } catch (...) {
            jp.outcome = rpc_code::internal;
        }
#else
        adv->around(jp, std::function<void()>(proceed));
#endif
    }

    detail::set_elapsed(jp, state.start);

    // Unwind phase. Only a clean `ok` gets `after`; everything else -- an error
    // code, a forced outcome, an aborted call, a contained exception -- gets
    // `after_throwing`. The callback itself is contained for the same reason as
    // `before`.
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        if (jp.outcome == rpc_code::ok) {
            adv->after(jp);
        } else {
            adv->after_throwing(jp);
        }
    } catch (...) {
        jp.outcome = rpc_code::internal;
    }
#else
    if (jp.outcome == rpc_code::ok) {
        adv->after(jp);
    } else {
        adv->after_throwing(jp);
    }
#endif
}

inline result<rpc_code> aspect_chain::invoke(std::string_view target, void* args,
                                             void* result_out,
                                             std::function<rpc_code()> body) {
    join_point jp;
    jp.target = target;
    jp.args = args;
    jp.result = result_out;

    // Fast path: nothing registered at all. One count load and one branch,
    // then the body behind the same exception boundary.
    if (count_ == 0) {
        const auto start = std::chrono::steady_clock::now();
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        try {
            jp.outcome = body();
        } catch (...) {
            jp.outcome = rpc_code::internal;
        }
#else
        jp.outcome = body();
#endif
        detail::set_elapsed(jp, start);
        return result<rpc_code>(jp.outcome);
    }

    invoke_state state;
    state.jp = &jp;
    state.body = &body;

    // Gather matching advices in registration order. Because the scan itself is
    // in registration order, an insertion sort that only moves an entry while
    // the preceding entry has a strictly greater order keeps ties stable.
    for (std::size_t i = 0; i < count_; ++i) {
        const entry& candidate = entries_[i];
        if (!candidate.pc.matches(target)) {
            continue;
        }
        const int order = candidate.adv->order();
        std::size_t position = state.count;
        while (position > 0 && state.orders[position - 1] > order) {
            state.layers[position] = state.layers[position - 1];
            state.orders[position] = state.orders[position - 1];
            --position;
        }
        state.layers[position] = candidate.adv;
        state.orders[position] = order;
        ++state.count;
    }

    if (state.count == 0) {
        // Registered entries exist but none matched this target: still the
        // allocation-free direct-body path, with the same outcome handling.
        const auto start = std::chrono::steady_clock::now();
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        try {
            jp.outcome = body();
        } catch (...) {
            jp.outcome = rpc_code::internal;
        }
#else
        jp.outcome = body();
#endif
        detail::set_elapsed(jp, start);
        return result<rpc_code>(jp.outcome);
    }

    const auto start = std::chrono::steady_clock::now();
    state.start = start;
    run_layer(state, 0);
    detail::set_elapsed(jp, start);
    return result<rpc_code>(jp.outcome);
}

} // namespace aspect
} // namespace crpc

#endif // CRPC_ASPECT_ASPECT_HPP
