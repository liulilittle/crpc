#include "bench_harness.hpp"

#include <crpc/ipc/ring_buffer.hpp>
#include <crpc/ipc/spsc_queue.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using namespace crpc;
using namespace crpc::ipc;

namespace {

// Unpaced measurement of one direction: the consumer is a separate thread that spins
// until it has taken everything, and the producer timestamp covers only the pushes.
// Latency of an individual operation is what the callers of these primitives care
// about (a per-message queue on a server), so that is what is reported.
template <class Push, class Pop>
double paced_ns_per_op(std::uint64_t operations, Push&& push, Pop&& pop) {
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t done = 0;
    while (done < operations) {
        if (push()) {
            ++done;
        }
        while (pop()) {
        }
    }
    while (pop()) {
    }
    const auto stop = std::chrono::steady_clock::now();
    const double ns =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                                .count());
    return ns / static_cast<double>(operations);
}

} // namespace

void run_all_cases() {
    if (!crpc_bench::enabled("ipc_ring")) {
        return;
    }

    // --- Byte ring: 64-byte frames, the size of a small control message -----------
    {
        constexpr std::size_t kCapacity = 64 * 1024;
        constexpr std::size_t kFrame = 64;
        constexpr std::uint64_t kOps = 2000000;
        std::vector<std::byte> payload(kCapacity, std::byte{0});
        ring_control control{};
        ring_view::initialise(&control, kCapacity);
        auto producer = ring_view::attach_producer(
            &control, span<std::byte>(payload.data(), payload.size()));
        auto consumer = ring_view::attach_consumer(
            &control, span<std::byte>(payload.data(), payload.size()));
        std::byte frame[kFrame] = {};

        crpc_bench::measure(
            "ipc_ring/64B-frame/push+pop", kOps, 5, [&](std::size_t iterations) {
                (void)paced_ns_per_op(
                    iterations,
                    [&] { return producer.try_write(span<const std::byte>(frame, kFrame)); },
                    [&] {
                        std::size_t got = 0;
                        while (got < kFrame) {
                            const std::size_t n = consumer.try_read(span<std::byte>(
                                frame + got, kFrame - got));
                            if (n == 0) {
                                break;
                            }
                            got += n;
                        }
                        return got == kFrame;
                    });
            });
    }

    // --- Value queue: 8-byte values, the shape of an id/handle queue --------------
    {
        constexpr std::size_t kCapacity = 64 * 1024;
        constexpr std::uint64_t kOps = 4000000;
        spsc_queue<std::uint64_t> q(kCapacity);

        crpc_bench::measure(
            "ipc_spsc/8B-value/push+pop", kOps, 5, [&](std::size_t iterations) {
                std::uint64_t value = 0;
                (void)paced_ns_per_op(
                    iterations, [&] { return q.push(value); },
                    [&] {
                        std::uint64_t out = 0;
                        return q.pop(out);
                    });
            });
    }

    // --- Value queue under real thread separation --------------------------------
    // The single-thread numbers above measure the index arithmetic; these measure it
    // while a second core is actually consuming, which is the configuration a server
    // runs in.
    {
        constexpr std::size_t kCapacity = 4096;
        constexpr std::uint64_t kOps = 2000000;
        spsc_queue<std::uint64_t> q(kCapacity);

        std::uint64_t consumed = 0;
        std::atomic<bool> done{false};
        std::thread consumer([&] {
            std::uint64_t out = 0;
            while (consumed < kOps) {
                if (q.pop(out)) {
                    ++consumed;
                }
            }
            done.store(true, std::memory_order_release);
        });

        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < kOps; ++i) {
            while (!q.push(i)) {
            }
        }
        const auto stop = std::chrono::steady_clock::now();
        consumer.join();
        const double ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
        crpc_bench::record("ipc_spsc/two-thread/8B-value", "ns/op", ns / static_cast<double>(kOps));
    }

    // --- Large frames: where the copy, not the index arithmetic, dominates --------
    // A 1 MiB frame through a 4 MiB ring. At this size the copy is the whole cost,
    // which is the only place a wider copy can help; the 64-byte case above is index
    // arithmetic and cannot benefit from it. Reporting both is what makes the claim
    // checkable instead of assumed.
    {
        constexpr std::size_t kCapacity = 4 * 1024 * 1024;
        constexpr std::size_t kFrame = 1024 * 1024;
        constexpr std::uint64_t kOps = 2000;
        std::vector<std::byte> payload(kCapacity, std::byte{0});
        ring_control control{};
        ring_view::initialise(&control, kCapacity);
        auto producer = ring_view::attach_producer(
            &control, span<std::byte>(payload.data(), payload.size()));
        auto consumer = ring_view::attach_consumer(
            &control, span<std::byte>(payload.data(), payload.size()));
        std::vector<std::byte> frame(kFrame, std::byte{1});
        std::vector<std::byte> sink(kFrame);

        crpc_bench::measure(
            "ipc_ring/1MiB-frame/push+pop", kOps, 5, [&](std::size_t iterations) {
                (void)paced_ns_per_op(
                    iterations,
                    [&] {
                        return producer.try_write(
                            span<const std::byte>(frame.data(), kFrame));
                    },
                    [&] {
                        std::size_t got = 0;
                        while (got < kFrame) {
                            const std::size_t n = consumer.try_read(
                                span<std::byte>(sink.data() + got, kFrame - got));
                            if (n == 0) {
                                break;
                            }
                            got += n;
                        }
                        return got == kFrame;
                    });
            });
    }

    // --- Power-of-two versus not ----------------------------------------------
    // The index reduction has a mask path and a modulo path. If the mask were not
    // actually taken for a power-of-two capacity, these two would measure the same;
    // a visible gap is the evidence that the fast path exists and is reached.
    {
        constexpr std::uint64_t kOps = 2000000;
        const std::uint64_t seed = 12345;
        for (const std::size_t cap : {std::size_t{65536}, std::size_t{65535}}) {
            std::vector<std::byte> payload(cap, std::byte{0});
            ring_control control{};
            ring_view::initialise(&control, cap);
            auto producer = ring_view::attach_producer(
                &control, span<std::byte>(payload.data(), payload.size()));
            auto consumer = ring_view::attach_consumer(
                &control, span<std::byte>(payload.data(), payload.size()));
            std::byte frame[8] = {};
            const auto start = std::chrono::steady_clock::now();
            std::uint64_t done = 0;
            while (done < kOps) {
                if (producer.try_write(span<const std::byte>(frame, sizeof(frame)))) {
                    ++done;
                }
                std::byte out[8];
                (void)consumer.try_read(span<std::byte>(out, sizeof(out)));
            }
            const auto stop = std::chrono::steady_clock::now();
            const double ns = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
            const char* name = (cap == 65536) ? "ipc_ring/index/pow2-capacity"
                                              : "ipc_ring/index/modulo-capacity";
            crpc_bench::record(name, "ns/op", ns / static_cast<double>(kOps));
            (void)seed;
        }
    }

    // --- The mutex-guarded baseline, for comparison ---------------------------
    // `runtime::submit` pushes through a std::mutex + std::deque. Whether replacing
    // that with the SPSC queue is worth anything depends on what the lock actually
    // costs relative to the work around it, so the cost is measured rather than
    // assumed. Uncontended and contended are both reported: a shard is submitted to
    // from its own thread in the simple case and from connection threads when a
    // server is busy.
    {
        constexpr std::uint64_t kOps = 2000000;
        std::mutex m;
        std::deque<std::uint64_t> q;
        crpc_bench::measure(
            "baseline/mutex-deque/push+pop/1thread", kOps, 5,
            [&](std::size_t iterations) {
                for (std::size_t i = 0; i < iterations; ++i) {
                    {
                        std::lock_guard<std::mutex> lk(m);
                        q.push_back(i);
                    }
                    std::uint64_t out = 0;
                    {
                        std::lock_guard<std::mutex> lk(m);
                        out = q.front();
                        q.pop_front();
                    }
                    (void)out;
                }
            });
    }
}

CRPC_BENCH_MAIN()
