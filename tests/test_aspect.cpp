// CRPC -- AOP aspect chain tests.
//
// These cases pin the contract of `crpc::aspect`:
//
//   * pointcut matching is exact / prefix / suffix / all; there is no regex
//     engine in this framework (include/crpc/text has no regex header) and
//     std::regex is forbidden by the design rules, so the four factories are
//     the whole surface;
//   * the chain is startup-registered, frozen, then read-only and safe for
//     concurrent `invoke()` from any thread;
//   * the hot path allocates nothing: an empty chain is one branch, and a
//     matched chain builds its onion from stack storage only;
//   * the around model is a real onion: order() ascending from the outside in,
//     ties broken by registration order, `proceed()` runs the next layer and
//     the body, and not calling it skips the body;
//   * errors and exceptions both unwind through `after_throwing`, never
//     through `after`, and never escape `invoke()`.
//
// The executable replaces the global allocation functions so the
// "no allocation" claims are measured, not asserted by inspection.
#include <crpc/aspect/aspect.hpp>

#include <crpc/error.hpp>
#include <crpc/observability/interceptor.hpp>

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace aspect = crpc::aspect;
using crpc::rpc_code;

// --- Counting allocator ------------------------------------------------------

namespace {
std::atomic<std::size_t> g_new_calls{0};
}

void* operator new(std::size_t size) {
    g_new_calls.fetch_add(1, std::memory_order_relaxed);
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

// --- Recording advice --------------------------------------------------------

// Appends one line per callback to a shared log. Every override is guarded so
// that a single advice can selectively exercise one phase.
struct recorder_entry {
    std::string name;
    std::string event;
};

class recording_advice final : public aspect::advice {
public:
    recording_advice(std::string name, std::vector<recorder_entry>* log, int order = 0)
        : name_(std::move(name)), log_(log), order_(order) {}

    void before(aspect::join_point&) override { push("before"); }
    void after(aspect::join_point&) override { push("after"); }
    void after_throwing(aspect::join_point&) override { push("after_throwing"); }

    void around(aspect::join_point& jp, std::function<void()> proceed) override {
        push("around_in");
        if (call_proceed_) {
            for (int i = 0; i < proceed_times_; ++i) {
                proceed();
            }
        }
        push("around_out");
        if (force_outcome_ != rpc_code::ok) {
            jp.outcome = force_outcome_;
        }
        if (throw_from_around_) {
            throw std::runtime_error("around boom");
        }
    }

    int order() const noexcept override { return order_; }

    recording_advice& proceed_times(int times) {
        call_proceed_ = true;
        proceed_times_ = times;
        return *this;
    }
    recording_advice& skip_proceed() {
        call_proceed_ = false;
        return *this;
    }
    recording_advice& force_outcome(rpc_code code) {
        force_outcome_ = code;
        return *this;
    }
    recording_advice& throw_from_around(bool value) {
        throw_from_around_ = value;
        return *this;
    }

private:
    void push(const char* event) {
        if (log_ == nullptr) {
            return;
        }
        log_->push_back({name_, event});
    }

    std::string name_;
    std::vector<recorder_entry>* log_;
    int order_ = 0;
    bool call_proceed_ = true;
    int proceed_times_ = 1;
    rpc_code force_outcome_ = rpc_code::ok;
    bool throw_from_around_ = false;
};

std::string join(const std::vector<recorder_entry>& entries) {
    std::string out;
    for (const recorder_entry& entry : entries) {
        if (!out.empty()) {
            out += ' ';
        }
        out += entry.name;
        out += ':';
        out += entry.event;
    }
    return out;
}

// A advice whose `before` aborts the call by clearing join_point::proceed.
class aborting_advice final : public aspect::advice {
public:
    explicit aborting_advice(std::vector<recorder_entry>* log) : log_(log) {}

    void before(aspect::join_point& jp) override {
        log_->push_back({"abort", "before"});
        jp.proceed = false;
    }
    void after(aspect::join_point& jp) override {
        log_->push_back({"abort", "after"});
        observed_proceed = jp.proceed;
        observed_outcome = jp.outcome;
    }
    void after_throwing(aspect::join_point& jp) override {
        log_->push_back({"abort", "after_throwing"});
        observed_proceed = jp.proceed;
        observed_outcome = jp.outcome;
    }

    bool observed_proceed = true;
    rpc_code observed_outcome = rpc_code::ok;

private:
    std::vector<recorder_entry>* log_;
};

// --- Simple interceptor used by the coexistence case -------------------------

class counting_interceptor final : public crpc::obs::interceptor {
public:
    bool on_hook(crpc::obs::hook_point, crpc::obs::call_context&) override {
        ++calls;
        return true;
    }
    int calls = 0;
};

} // namespace

// --- pointcut ----------------------------------------------------------------

CRPC_TEST(pointcut_exact_prefix_suffix_all) {
    const aspect::pointcut exact = aspect::pointcut::on("svc.method");
    CRPC_CHECK(exact.matches("svc.method"));
    CRPC_CHECK_FALSE(exact.matches("svc.method2"));
    CRPC_CHECK_FALSE(exact.matches("svc"));
    CRPC_CHECK_FALSE(exact.matches(""));

    const aspect::pointcut prefix = aspect::pointcut::on_prefix("svc.");
    CRPC_CHECK(prefix.matches("svc.method"));
    CRPC_CHECK(prefix.matches("svc."));
    CRPC_CHECK_FALSE(prefix.matches("svc"));
    CRPC_CHECK_FALSE(prefix.matches("other.method"));

    const aspect::pointcut suffix = aspect::pointcut::on_suffix(".read");
    CRPC_CHECK(suffix.matches("svc.read"));
    CRPC_CHECK(suffix.matches(".read"));
    CRPC_CHECK_FALSE(suffix.matches("svc.read2"));
    CRPC_CHECK_FALSE(suffix.matches("read"));

    const aspect::pointcut all = aspect::pointcut::on_all();
    CRPC_CHECK(all.matches(""));
    CRPC_CHECK(all.matches("anything.at.all"));
}

CRPC_TEST(pointcut_default_matches_nothing) {
    // A default-constructed pointcut is the "none" kind: it never matches, so a
    // default-initialised table entry can never fire by accident.
    const aspect::pointcut none;
    CRPC_CHECK_FALSE(none.matches(""));
    CRPC_CHECK_FALSE(none.matches("svc.method"));
}

// --- registration ------------------------------------------------------------

CRPC_TEST(chain_registration_contract) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    recording_advice advice("a", &log);

    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), nullptr).code() ==
               rpc_code::invalid_argument);
    CRPC_CHECK(chain.add(aspect::pointcut::on(""), &advice).code() ==
               rpc_code::invalid_argument);
    CRPC_CHECK(chain.add(aspect::pointcut::on_prefix(""), &advice).code() ==
               rpc_code::invalid_argument);
    CRPC_CHECK(chain.add(aspect::pointcut::on_suffix(""), &advice).code() ==
               rpc_code::invalid_argument);

    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advice).ok());
    CRPC_CHECK_EQ(std::size_t{1}, chain.count());
    CRPC_CHECK_FALSE(chain.frozen());

    chain.freeze();
    CRPC_CHECK(chain.frozen());
    chain.freeze(); // idempotent
    CRPC_CHECK(chain.frozen());

    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advice).code() ==
               rpc_code::failed_precondition);
    CRPC_CHECK_EQ(std::size_t{1}, chain.count());
}

CRPC_TEST(chain_capacity_is_bounded) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    std::vector<recording_advice> advices;
    advices.reserve(aspect::aspect_chain::max_aspects + 1);
    for (std::size_t i = 0; i <= aspect::aspect_chain::max_aspects; ++i) {
        advices.emplace_back("a" + std::to_string(i), &log);
    }
    for (std::size_t i = 0; i < aspect::aspect_chain::max_aspects; ++i) {
        CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advices[i]).ok());
    }
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advices.back()).code() ==
               rpc_code::resource_exhausted);
    CRPC_CHECK_EQ(aspect::aspect_chain::max_aspects, chain.count());
}

// --- invocation basics -------------------------------------------------------

CRPC_TEST(empty_chain_runs_body_without_allocation) {
    aspect::aspect_chain chain;
    chain.freeze();

    int body_calls = 0;
    auto body = [&body_calls]() {
        ++body_calls;
        return rpc_code::ok;
    };

    // Warm up once so any lazy allocator growth lands outside the measurement.
    (void)chain.invoke("svc.method", nullptr, nullptr, body);

    const std::size_t before = g_new_calls.load(std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i) {
        const auto outcome = chain.invoke("svc.method", nullptr, nullptr, body);
        CRPC_CHECK(outcome.ok());
    }
    const std::size_t after = g_new_calls.load(std::memory_order_relaxed);

    CRPC_CHECK_EQ(std::size_t{0}, after - before);
    CRPC_CHECK_EQ(1001, body_calls);
}

CRPC_TEST(no_match_runs_body_and_allocates_nothing) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    recording_advice advice("a", &log);
    CRPC_CHECK(chain.add(aspect::pointcut::on_prefix("other."), &advice).ok());
    chain.freeze();

    int body_calls = 0;
    auto body = [&body_calls]() {
        ++body_calls;
        return rpc_code::ok;
    };
    (void)chain.invoke("svc.method", nullptr, nullptr, body);

    const std::size_t before = g_new_calls.load(std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i) {
        CRPC_CHECK(chain.invoke("svc.method", nullptr, nullptr, body).ok());
    }
    const std::size_t after = g_new_calls.load(std::memory_order_relaxed);

    CRPC_CHECK_EQ(std::size_t{0}, after - before);
    CRPC_CHECK_EQ(1001, body_calls);
    CRPC_CHECK_EQ(std::size_t{0}, log.size());
}

CRPC_TEST(empty_chain_converts_body_error) {
    aspect::aspect_chain chain;
    chain.freeze();
    const auto outcome = chain.invoke("svc.method", nullptr, nullptr,
                                      []() { return rpc_code::invalid_argument; });
    CRPC_CHECK_FALSE(outcome.ok());
    CRPC_CHECK(outcome.code() == rpc_code::invalid_argument);
}

CRPC_TEST(empty_chain_contains_body_exception) {
    aspect::aspect_chain chain;
    chain.freeze();
    const auto outcome = chain.invoke("svc.method", nullptr, nullptr, []() -> rpc_code {
        throw std::runtime_error("body boom");
    });
    CRPC_CHECK_FALSE(outcome.ok());
    CRPC_CHECK(outcome.code() == rpc_code::internal);
}

CRPC_TEST(matched_chain_does_not_allocate) {
    aspect::aspect_chain chain;
    std::atomic<int> after_calls{0};

    // Non-allocating advices: the recording advice logs into a vector, so it
    // cannot be the probe here. Two layers exercise the outer/inner recursion.
    class counter final : public aspect::advice {
    public:
        explicit counter(std::atomic<int>* calls) : calls_(calls) {}
        void after(aspect::join_point&) override { calls_->fetch_add(1); }
    private:
        std::atomic<int>* calls_;
    } outer(&after_calls), inner(&after_calls);

    CRPC_CHECK(chain.add(aspect::pointcut::on_prefix("svc."), &outer).ok());
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &inner).ok());
    chain.freeze();

    int body_calls = 0;
    auto body = [&body_calls]() {
        ++body_calls;
        return rpc_code::ok;
    };
    // Warm up once: any lazily initialised standard-library storage is not part
    // of the per-call contract.
    (void)chain.invoke("svc.method", nullptr, nullptr, body);

    const std::size_t before = g_new_calls.load(std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i) {
        CRPC_CHECK(chain.invoke("svc.method", nullptr, nullptr, body).ok());
    }
    const std::size_t after = g_new_calls.load(std::memory_order_relaxed);

    CRPC_CHECK_EQ(std::size_t{0}, after - before);
    CRPC_CHECK_EQ(1001, body_calls);
    CRPC_CHECK_EQ(2002, after_calls.load());
}

CRPC_TEST(order_is_sampled_once_per_matching_advice) {
    // The onion order is computed during the match pass and cached; the unwind
    // must not re-enter the virtual order() accessor. A counting advice pins
    // the count to one call per invoke per matching advice.
    class counting_order final : public aspect::advice {
    public:
        explicit counting_order(int* calls) : calls_(calls) {}
        int order() const noexcept override {
            ++*calls_;
            return 0;
        }
    private:
        int* calls_;
    };

    int order_calls = 0;
    counting_order one(&order_calls);
    counting_order two(&order_calls);

    aspect::aspect_chain chain;
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &one).ok());
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &two).ok());
    chain.freeze();

    order_calls = 0;
    (void)chain.invoke("svc.method", nullptr, nullptr, []() { return rpc_code::ok; });
    CRPC_CHECK_EQ(2, order_calls);
}

// --- advice phases -----------------------------------------------------------

CRPC_TEST(single_advice_phase_order) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    recording_advice advice("a", &log);
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advice).ok());
    chain.freeze();

    const auto outcome = chain.invoke("svc.method", nullptr, nullptr, [&log]() {
        log.push_back({"body", "run"});
        return rpc_code::ok;
    });
    CRPC_CHECK(outcome.ok());
    CRPC_CHECK_EQ(std::string("a:before a:around_in body:run a:around_out a:after"), join(log));
}

CRPC_TEST(onion_order_outer_to_inner) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    recording_advice middle("middle", &log, 5);
    recording_advice outer("outer", &log, -5);
    recording_advice inner("inner", &log, 10);
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &middle).ok());
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &inner).ok());
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &outer).ok());
    chain.freeze();

    CRPC_CHECK(chain.invoke("svc.method", nullptr, nullptr, [&log]() {
        log.push_back({"body", "run"});
        return rpc_code::ok;
    }).ok());

    CRPC_CHECK_EQ(std::string("outer:before outer:around_in "
                              "middle:before middle:around_in "
                              "inner:before inner:around_in "
                              "body:run "
                              "inner:around_out inner:after "
                              "middle:around_out middle:after "
                              "outer:around_out outer:after"),
                  join(log));
}

CRPC_TEST(onion_ties_use_registration_order) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    recording_advice first("first", &log, 0);
    recording_advice second("second", &log, 0);
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &first).ok());
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &second).ok());
    chain.freeze();

    CRPC_CHECK(chain.invoke("svc.method", nullptr, nullptr, [&log]() {
        log.push_back({"body", "run"});
        return rpc_code::ok;
    }).ok());

    CRPC_CHECK_EQ(std::string("first:before first:around_in "
                              "second:before second:around_in "
                              "body:run "
                              "second:around_out second:after "
                              "first:around_out first:after"),
                  join(log));
}

CRPC_TEST(around_without_proceed_skips_body) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    recording_advice swallow("swallow", &log);
    swallow.skip_proceed();
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &swallow).ok());
    chain.freeze();

    int body_calls = 0;
    const auto outcome = chain.invoke("svc.method", nullptr, nullptr, [&body_calls]() {
        ++body_calls;
        return rpc_code::ok;
    });
    CRPC_CHECK(outcome.ok());
    CRPC_CHECK_EQ(0, body_calls);
    CRPC_CHECK_EQ(std::string("swallow:before swallow:around_in swallow:around_out swallow:after"),
                  join(log));
}

CRPC_TEST(before_clearing_proceed_aborts_call) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    aborting_advice advice(&log);
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advice).ok());
    chain.freeze();

    int body_calls = 0;
    const auto outcome = chain.invoke("svc.method", nullptr, nullptr, [&body_calls]() {
        ++body_calls;
        return rpc_code::ok;
    });
    CRPC_CHECK_EQ(0, body_calls);
    CRPC_CHECK_FALSE(advice.observed_proceed);
    CRPC_CHECK_FALSE(outcome.ok());
    CRPC_CHECK(outcome.code() == rpc_code::canceled);
    CRPC_CHECK(advice.observed_outcome == rpc_code::canceled);
    CRPC_CHECK_EQ(std::string("abort:before abort:after_throwing"), join(log));
}

CRPC_TEST(around_can_proceed_more_than_once) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    recording_advice repeat("repeat", &log);
    repeat.proceed_times(2);
    (void)repeat;
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &repeat).ok());
    chain.freeze();

    int body_calls = 0;
    CRPC_CHECK(chain.invoke("svc.method", nullptr, nullptr, [&body_calls]() {
        ++body_calls;
        return rpc_code::ok;
    }).ok());
    CRPC_CHECK_EQ(2, body_calls);
}

// --- error unwinding ---------------------------------------------------------

CRPC_TEST(body_error_unwinds_through_after_throwing) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    recording_advice outer("outer", &log, -1);
    recording_advice inner("inner", &log, 1);
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &outer).ok());
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &inner).ok());
    chain.freeze();

    const auto outcome = chain.invoke("svc.method", nullptr, nullptr,
                                      []() { return rpc_code::invalid_argument; });
    CRPC_CHECK_FALSE(outcome.ok());
    CRPC_CHECK(outcome.code() == rpc_code::invalid_argument);
    CRPC_CHECK_EQ(std::string("outer:before outer:around_in "
                              "inner:before inner:around_in "
                              "inner:around_out inner:after_throwing "
                              "outer:around_out outer:after_throwing"),
                  join(log));
}

CRPC_TEST(body_exception_is_converted_to_internal) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    recording_advice advice("a", &log);
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advice).ok());
    chain.freeze();

    rpc_code observed = rpc_code::ok;
    const auto outcome = chain.invoke("svc.method", nullptr, nullptr, [&observed]() -> rpc_code {
        (void)observed;
        throw std::runtime_error("body boom");
    });
    CRPC_CHECK_FALSE(outcome.ok());
    CRPC_CHECK(outcome.code() == rpc_code::internal);
    CRPC_CHECK_EQ(std::string("a:before a:around_in a:around_out a:after_throwing"), join(log));
}

CRPC_TEST(around_forced_outcome_unwinds_with_after_throwing) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    recording_advice advice("a", &log);
    advice.force_outcome(rpc_code::overload);
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advice).ok());
    chain.freeze();

    const auto outcome = chain.invoke("svc.method", nullptr, nullptr,
                                      []() { return rpc_code::ok; });
    CRPC_CHECK(outcome.code() == rpc_code::overload);
    CRPC_CHECK_EQ(std::string("a:before a:around_in a:around_out a:after_throwing"), join(log));
}

CRPC_TEST(around_exception_is_contained) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    recording_advice advice("a", &log);
    advice.throw_from_around(true);
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advice).ok());
    chain.freeze();

    const auto outcome = chain.invoke("svc.method", nullptr, nullptr,
                                      []() { return rpc_code::ok; });
    CRPC_CHECK(outcome.code() == rpc_code::internal);
    CRPC_CHECK_EQ(std::string("a:before a:around_in a:around_out a:after_throwing"), join(log));
}

// --- advice callback exception semantics (pinned to the audited behaviour) ---
//
// The four cases below pin what the second auditor's P4-P7 probes measured.
// Every expectation here is the behaviour of the implementation as it stands;
// each one has a matching mutation in the verification round that turns it red.

// (a) A `before` that throws aborts the call: the body never runs, the outcome
// becomes `internal`, and this layer still sees `after_throwing`. Nothing
// escapes `invoke()`.
CRPC_TEST(before_exception_is_contained_and_aborts_call) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;

    class throwing_before final : public aspect::advice {
    public:
        explicit throwing_before(std::vector<recorder_entry>* log) : log_(log) {}
        void before(aspect::join_point&) override { throw std::runtime_error("before boom"); }
        void after_throwing(aspect::join_point&) override {
            log_->push_back({"tb", "after_throwing"});
        }
    private:
        std::vector<recorder_entry>* log_;
    } advice(&log);

    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advice).ok());
    chain.freeze();

    int body_calls = 0;
    const auto outcome = chain.invoke("svc.method", nullptr, nullptr, [&body_calls]() {
        ++body_calls;
        return rpc_code::ok;
    });
    CRPC_CHECK_EQ(0, body_calls);
    CRPC_CHECK(outcome.code() == rpc_code::internal);
    CRPC_CHECK_EQ(std::string("tb:after_throwing"), join(log));
}

// (b) A throwing `after` does not earn this layer a compensating
// `after_throwing` call: the unwind phase already picked one callback, and the
// throw is contained there. Pinned as implemented (probe P5): the containment
// rule overwrites the outcome with `internal` on the debug/audit trail even
// though the body itself succeeded and its output is intact.
CRPC_TEST(after_exception_keeps_layer_unwound_once) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;

    class throwing_after final : public aspect::advice {
    public:
        explicit throwing_after(std::vector<recorder_entry>* log) : log_(log) {}
        void after(aspect::join_point&) override { throw std::runtime_error("after boom"); }
        void after_throwing(aspect::join_point&) override {
            log_->push_back({"ta", "after_throwing"});
        }
    private:
        std::vector<recorder_entry>* log_;
    } advice(&log);

    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advice).ok());
    chain.freeze();

    int output = 0;
    const auto outcome = chain.invoke(
        "svc.method", nullptr, &output,
        [&output]() {
            output = 7; // the body's effect happens before the after() throw
            return rpc_code::ok;
        });

    CRPC_CHECK_EQ(7, output); // the body's result was produced
    CRPC_CHECK(outcome.code() == rpc_code::internal); // containment overwrites
    CRPC_CHECK_EQ(std::string(""), join(log)); // no compensating after_throwing
}

// (c) A throwing `after_throwing` is contained the same way, and the
// containment rule swallows the body's original error code: the caller sees
// `internal` (pinned as implemented, probe P6).
CRPC_TEST(after_throwing_exception_swallows_original_code) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;

    class throwing_after_throwing final : public aspect::advice {
    public:
        explicit throwing_after_throwing(std::vector<recorder_entry>* log) : log_(log) {}
        void after_throwing(aspect::join_point&) override {
            log_->push_back({"tat", "after_throwing"});
            throw std::runtime_error("after_throwing boom");
        }
    private:
        std::vector<recorder_entry>* log_;
    } advice(&log);

    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advice).ok());
    chain.freeze();

    const auto outcome = chain.invoke("svc.method", nullptr, nullptr,
                                      []() { return rpc_code::deadline_exceeded; });
    CRPC_CHECK(outcome.code() == rpc_code::internal);
    CRPC_CHECK_EQ(1, static_cast<int>(log.size())); // the callback ran before it threw
}

// (d) A `before` that aborts by clearing `proceed` keeps a code it pre-set:
// the chain only defaults to `canceled` when the advice left `ok` behind.
CRPC_TEST(before_abort_keeps_pre_set_code) {
    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;

    class veto final : public aspect::advice {
    public:
        explicit veto(std::vector<recorder_entry>* log) : log_(log) {}
        void before(aspect::join_point& jp) override {
            jp.outcome = rpc_code::permission_denied;
            jp.proceed = false;
        }
        void after_throwing(aspect::join_point&) override {
            log_->push_back({"veto", "after_throwing"});
        }
    private:
        std::vector<recorder_entry>* log_;
    } advice(&log);

    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advice).ok());
    chain.freeze();

    int body_calls = 0;
    const auto outcome = chain.invoke("svc.method", nullptr, nullptr, [&body_calls]() {
        ++body_calls;
        return rpc_code::ok;
    });
    CRPC_CHECK_EQ(0, body_calls);
    CRPC_CHECK(outcome.code() == rpc_code::permission_denied);
    CRPC_CHECK_EQ(std::string("veto:after_throwing"), join(log));
}

// (T4) error_or on a non-engaged result: a code-only failure and a success both
// carry no `rpc_error`, so `error_or` returns the caller's fallback unchanged.
CRPC_TEST(result_rpc_code_error_or_fallback_when_not_engaged) {
    const crpc::result<rpc_code> code_only(rpc_code::deadline_exceeded);
    const crpc::rpc_error fallback(crpc::rpc_code::unavailable, "fallback");
    const crpc::rpc_error returned = code_only.error_or(fallback);
    CRPC_CHECK(returned.code == rpc_code::unavailable);
    CRPC_CHECK_EQ(std::string("fallback"), returned.message);

    const crpc::result<rpc_code> success(rpc_code::ok);
    const crpc::rpc_error returned_ok = success.error_or(fallback);
    CRPC_CHECK(returned_ok.code == rpc_code::unavailable);
    CRPC_CHECK_EQ(std::string("fallback"), returned_ok.message);
}

// --- join point payload ------------------------------------------------------

CRPC_TEST(join_point_fields_are_visible_to_advice) {
    struct payload {
        int argument = 41;
        int output = 0;
    } value;

    class inspector final : public aspect::advice {
    public:
        void before(aspect::join_point& jp) override {
            target = std::string(jp.target);
            args_seen = jp.args;
            result_seen = jp.result;
            initial_proceed = jp.proceed;
            initial_outcome = jp.outcome;
        }
        void after(aspect::join_point& jp) override {
            final_outcome = jp.outcome;
            elapsed = jp.elapsed;
        }

        std::string target;
        void* args_seen = nullptr;
        void* result_seen = nullptr;
        bool initial_proceed = false;
        rpc_code initial_outcome = rpc_code::internal;
        rpc_code final_outcome = rpc_code::internal;
        std::chrono::nanoseconds elapsed{0};
    } inspector;

    aspect::aspect_chain chain;
    CRPC_CHECK(chain.add(aspect::pointcut::on("math.double"), &inspector).ok());
    chain.freeze();

    const auto outcome = chain.invoke(
        "math.double", &value, &value.output,
        [&value]() {
            value.output = value.argument * 2;
            return rpc_code::ok;
        });

    CRPC_CHECK(outcome.ok());
    CRPC_CHECK_EQ(82, value.output);
    CRPC_CHECK_EQ(std::string("math.double"), inspector.target);
    CRPC_CHECK(inspector.args_seen == &value);
    CRPC_CHECK(inspector.result_seen == &value.output);
    CRPC_CHECK(inspector.initial_proceed);
    CRPC_CHECK(inspector.initial_outcome == rpc_code::ok);
    CRPC_CHECK(inspector.final_outcome == rpc_code::ok);
}

CRPC_TEST(join_point_elapsed_tracks_wall_time) {
    class timer final : public aspect::advice {
    public:
        void after(aspect::join_point& jp) override { elapsed = jp.elapsed; }
        std::chrono::nanoseconds elapsed{0};
    } timer;

    aspect::aspect_chain chain;
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &timer).ok());
    chain.freeze();

    (void)chain.invoke("svc.slow", nullptr, nullptr, []() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return rpc_code::ok;
    });

    CRPC_CHECK(timer.elapsed >= std::chrono::milliseconds(2));
}

// --- coexistence and threading ----------------------------------------------

CRPC_TEST(coexists_with_interceptor_chain) {
    crpc::obs::interceptor_chain hooks;
    counting_interceptor interceptor;
    CRPC_CHECK(hooks.add(crpc::obs::hook_point::before_call, &interceptor));
    hooks.freeze();

    aspect::aspect_chain chain;
    std::vector<recorder_entry> log;
    recording_advice advice("a", &log);
    CRPC_CHECK(chain.add(aspect::pointcut::on_all(), &advice).ok());
    chain.freeze();

    int body_calls = 0;
    const auto outcome = chain.invoke("svc.method", nullptr, nullptr, [&hooks, &body_calls]() {
        crpc::obs::call_context context;
        ++body_calls;
        if (!hooks.run(crpc::obs::hook_point::before_call, context)) {
            return rpc_code::internal;
        }
        return rpc_code::ok;
    });

    CRPC_CHECK(outcome.ok());
    CRPC_CHECK_EQ(1, body_calls);
    CRPC_CHECK_EQ(1, interceptor.calls);
    CRPC_CHECK_EQ(std::size_t{8}, crpc::obs::hook_point_count());
}

// Local helper used only by the case above; keeps the assertion readable.
CRPC_TEST(frozen_chain_is_concurrent_read_only) {
    aspect::aspect_chain chain;
    std::atomic<int> body_calls{0};
    std::atomic<int> after_calls{0};

    class counting_advice final : public aspect::advice {
    public:
        explicit counting_advice(std::atomic<int>* calls) : calls_(calls) {}
        void after(aspect::join_point&) override { calls_->fetch_add(1); }
    private:
        std::atomic<int>* calls_;
    } advice(&after_calls);

    CRPC_CHECK(chain.add(aspect::pointcut::on_prefix("svc."), &advice).ok());
    chain.freeze();

    constexpr int threads = 4;
    constexpr int per_thread = 2000;
    std::vector<std::thread> workers;
    std::atomic<int> failures{0};
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&chain, &body_calls, &failures]() {
            auto body = [&body_calls]() {
                body_calls.fetch_add(1, std::memory_order_relaxed);
                return rpc_code::ok;
            };
            for (int i = 0; i < per_thread; ++i) {
                const auto outcome =
                    chain.invoke(i % 2 == 0 ? "svc.hit" : "other.miss", nullptr, nullptr, body);
                if (!outcome.ok()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    CRPC_CHECK_EQ(0, failures.load());
    CRPC_CHECK_EQ(threads * per_thread, body_calls.load());
    CRPC_CHECK_EQ((threads * per_thread) / 2, after_calls.load());
}

// --- result<rpc_code> specialisation ----------------------------------------

CRPC_TEST(result_rpc_code_maps_success_and_error) {
    const crpc::result<rpc_code> success(rpc_code::ok);
    CRPC_CHECK(success.ok());
    CRPC_CHECK(success.code() == rpc_code::ok);
    CRPC_CHECK(success.value() == rpc_code::ok);
    CRPC_CHECK(success.value_or(rpc_code::internal) == rpc_code::ok);

    const crpc::result<rpc_code> failure(rpc_code::deadline_exceeded);
    CRPC_CHECK_FALSE(failure.ok());
    CRPC_CHECK(failure.code() == rpc_code::deadline_exceeded);
    CRPC_CHECK(failure.value_or(rpc_code::internal) == rpc_code::internal);
    // A code-only failure carries no diagnostics: `code()` is authoritative and
    // `error()` degenerates to the default. This is what keeps the type
    // allocation-free even on the MSVC debug CRT (an embedded rpc_error would
    // allocate a container proxy per string/vector member).
    CRPC_CHECK(failure.error().code == rpc_code::ok);

    crpc::rpc_error error(rpc_code::unavailable, "no peer");
    const crpc::result<rpc_code> from_error(error);
    CRPC_CHECK_FALSE(from_error.ok());
    CRPC_CHECK(from_error.code() == rpc_code::unavailable);
    CRPC_CHECK_EQ(std::string("no peer"), from_error.error().message);
    CRPC_CHECK(from_error.error_or(crpc::rpc_error(rpc_code::ok)).code ==
               rpc_code::unavailable);

    const crpc::result<rpc_code> from_ok_error{crpc::rpc_error(rpc_code::ok)};
    CRPC_CHECK(from_ok_error.ok());
}

CRPC_TEST_MAIN()
