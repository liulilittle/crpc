// CRPC -- microbenchmarks for the wire and codec layers.
//
// These are the two hot paths that run once per RPC on every node, so their
// per-operation cost sets the floor for the whole framework. Everything here is
// measured in-process; there is no network and no thread involved.
#include <crpc/client_facade.hpp>
#include <crpc/codec.hpp>
#include <crpc/endian.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/server.hpp>
#include <crpc/wire.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#endif

#include <rtti/reflection.hpp>

#include "bench_harness.hpp"

// Counting allocator: the send-path case needs a hard before/after number for
// "how many allocations does one outgoing frame cost". It counts every
// allocation in the process, so each measurement takes a snapshot first.
namespace {
std::atomic<std::size_t> g_allocations{0};

// Optional per-allocation stack capture, used to name the low-level call
// path's remaining allocations. Off by default; it costs a back-trace per
// allocation and would dominate the timing.
constexpr int kTraceMax = 100000;
void* g_trace[kTraceMax][8] = {};
std::atomic<int> g_trace_n{0};
std::atomic<int> g_trace_on{0};
} // namespace

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (g_trace_on.load(std::memory_order_relaxed) != 0) {
        const int idx = g_trace_n.fetch_add(1, std::memory_order_relaxed);
        if (idx < kTraceMax) {
            // `unsigned short`, not `USHORT`: the latter is a Windows typedef, and
            // this file is compiled on Linux too (the bench target is built by
            // bench-compile on Windows and by build.sh --bench elsewhere). Using
            // the platform spelling in shared locals made the non-Windows branch
            // fail to compile even though it only assigned 0.
            unsigned short n = 0;
#if defined(_WIN32)
            n = CaptureStackBackTrace(1, 8, g_trace[idx], nullptr);
#endif
            for (unsigned short i = n; i < 8; ++i) {
                g_trace[idx][i] = nullptr;
            }
        }
    }
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

using namespace std::chrono_literals;

// A message shaped like a typical RPC request: a couple of identifiers, a
// string, and a small collection. Deliberately not the smallest possible
// message, because the smallest possible message measures the harness.
// Declared at global scope: RTTI's registration macros specialize templates in
// namespace rtti and would target the wrong namespace inside an anonymous one.
struct bench_request {
    std::uint64_t user_id;
    std::uint32_t flags;
    std::string name;
    std::vector<std::int32_t> values;
};
RTTI_DEFINE_TYPE(bench_request, user_id, flags, name, values)

namespace {

std::vector<std::byte> make_frame_bytes(std::size_t payload_bytes) {
    std::vector<std::byte> payload(payload_bytes, std::byte{0x5A});
    crpc::frame value;
    value.header.type = crpc::frame_type::request;
    value.header.stream_id = 1;
    value.payload = crpc::span<const std::byte>(payload.data(), payload.size());

    std::vector<std::byte> out(crpc::encoded_frame_size(value));
    std::size_t written = 0;
    const crpc::status encoded =
        crpc::encode_frame(crpc::span<std::byte>(out.data(), out.size()), value,
                           written);
    if (!encoded.ok()) {
        out.clear();
    } else {
        out.resize(written);
    }
    return out;
}

// The pre-optimisation per-byte loop, kept so the benchmark reports a
// before/after number for the same input rather than a bare absolute.
std::uint32_t crc32c_byte_reference(crpc::span<const std::byte> data) noexcept {
    static const std::uint32_t* const table = [] {
        static std::uint32_t entries[256];
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) != 0 ? (0x82F63B78u ^ (value >> 1))
                                           : (value >> 1);
            }
            entries[i] = value;
        }
        return entries;
    }();
    std::uint32_t value = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < data.size(); ++i) {
        const std::uint32_t byte =
            static_cast<std::uint32_t>(static_cast<unsigned char>(data[i]));
        value = table[(value ^ byte) & 0xFFu] ^ (value >> 8);
    }
    return value ^ 0xFFFFFFFFu;
}

// A local slicing-by-8 implementation, so the benchmark can tell the portable
// fallback apart from the hardware path the public crc32c() selects at run time.
std::uint32_t crc32c_slicing8_reference(crpc::span<const std::byte> data) noexcept {
    static const std::uint32_t (*const table)[256] = [] {
        static std::uint32_t entries[8][256];
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) != 0 ? (0x82F63B78u ^ (value >> 1))
                                           : (value >> 1);
            }
            entries[0][i] = value;
        }
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = entries[0][i];
            for (int k = 1; k < 8; ++k) {
                value = entries[0][value & 0xFFu] ^ (value >> 8);
                entries[k][i] = value;
            }
        }
        return entries;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    const std::byte* p = data.data();
    std::size_t len = data.size();
    while (len >= 8) {
        const std::uint32_t low = static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) |
                                  (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
                                  (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
                                  (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24);
        const std::uint32_t c = crc ^ low;
        crc = table[7][c & 0xFFu] ^ table[6][(c >> 8) & 0xFFu] ^
              table[5][(c >> 16) & 0xFFu] ^ table[4][(c >> 24) & 0xFFu] ^
              table[3][static_cast<unsigned char>(p[4])] ^
              table[2][static_cast<unsigned char>(p[5])] ^
              table[1][static_cast<unsigned char>(p[6])] ^
              table[0][static_cast<unsigned char>(p[7])];
        p += 8;
        len -= 8;
    }
    while (len--) {
        crc = table[0][(crc ^ static_cast<std::uint32_t>(static_cast<unsigned char>(*p++))) &
                      0xFFu] ^
              (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void bench_endian() {
    std::byte buffer[8] = {};
    std::uint64_t sink = 0;

    crpc_bench::measure("endian/store_le64", 5'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            crpc::endian::store_le64(buffer, sink + i);
        }
    });

    crpc_bench::measure("endian/load_le64", 5'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            sink += crpc::endian::load_le64(buffer);
        }
    });
}

void bench_wire() {
    std::byte storage[64] = {};
    std::uint64_t sink = 0;

    crpc_bench::measure("wire/put_varint_small", 5'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            crpc::byte_writer w(crpc::span<std::byte>(storage, sizeof(storage)));
            w.put_varint(static_cast<std::uint64_t>(i & 0x3FFF));
            sink += w.written();
        }
    });

    crpc::byte_writer seed(crpc::span<std::byte>(storage, sizeof(storage)));
    seed.put_varint(0x0123456789ABCDEFull);
    const std::size_t seed_len = seed.written();

    crpc_bench::measure("wire/get_varint_u64", 5'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            crpc::byte_reader r(
                crpc::span<const std::byte>(storage, seed_len));
            std::uint64_t v = 0;
            r.get_varint(v);
            sink += v;
        }
    });
}

void bench_frame() {
    const std::vector<std::byte> small = make_frame_bytes(16);
    const std::vector<std::byte> large = make_frame_bytes(1024);
    std::uint64_t sink = 0;

    crpc_bench::measure("frame/encode_header_16B", 2'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            std::byte out[crpc::frame_header_size] = {};
            crpc::frame_header h;
            h.payload_len = static_cast<std::uint32_t>(i & 0xFFFF);
            h.type = crpc::frame_type::request;
            h.stream_id = static_cast<std::uint32_t>(i);
            (void)crpc::encode_frame_header(crpc::span<std::byte>(out, sizeof(out)), h);
            sink += static_cast<std::uint64_t>(out[0]);
        }
    });

    crpc_bench::measure("frame/decode_header_16B", 2'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            crpc::frame_header h;
            (void)crpc::decode_frame_header(
                crpc::span<const std::byte>(small.data(), crpc::frame_header_size), h);
            sink += h.stream_id;
        }
    });

    // The streaming decoder is what a connection actually uses: feed a buffer
    // holding many frames and pull them out.
    std::vector<std::byte> batch;
    batch.reserve(small.size() * 1024);
    for (int i = 0; i < 1024; ++i) {
        batch.insert(batch.end(), small.begin(), small.end());
    }

    // 1024 frames per iteration: divide by the frame count so the reported
    // number is per frame, not per batch.
    crpc_bench::measure("frame/decoder_small_16B_per_frame", 200 * 1024, 7,
                        [&](std::size_t n) {
        std::size_t frames = 0;
        while (frames < n) {
            crpc::frame_decoder decoder;
            decoder.feed(crpc::span<const std::byte>(batch.data(), batch.size()));
            crpc::frame f;
            while (decoder.next(f) == crpc::frame_decoder::result::frame_ready) {
                sink += f.header.stream_id;
                ++frames;
            }
        }
    });
    crpc_bench::measure("frame/decoder_large_1KiB", 50'000, 7, [&](std::size_t n) {
        std::vector<std::byte> one = large;
        for (std::size_t i = 0; i < n; ++i) {
            crpc::frame_decoder decoder;
            decoder.feed(crpc::span<const std::byte>(one.data(), one.size()));
            crpc::frame f;
            while (decoder.next(f) == crpc::frame_decoder::result::frame_ready) {
                sink += f.payload.size();
            }
        }
    });
    (void)sink;
}

// Zero-copy read path (D3a). feed() copies each chunk into the decoder's
// scratch buffer before parsing it; feed_external() parses straight out of the
// caller's chunk and only copies a trailing partial frame. The two measured
// cases are the same frame stream, one 1 KiB frame per chunk and a 64-frame
// batch, so the difference is the per-chunk copy. The last two records are a
// deterministic proof rather than a timing: a feed() payload points into the
// decoder (one copy) while a feed_external() payload is the chunk's own bytes
// (zero copies).
void bench_frame_zero_copy() {
    const std::vector<std::byte> one = make_frame_bytes(1024);
    std::vector<std::byte> batch;
    batch.reserve(one.size() * 64);
    for (int i = 0; i < 64; ++i) {
        batch.insert(batch.end(), one.begin(), one.end());
    }
    std::uint64_t sink = 0;

    crpc_bench::measure("frame/feed_copy_1KiB_per_frame", 50'000, 7,
                        [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            crpc::frame_decoder decoder;
            decoder.feed(crpc::span<const std::byte>(one.data(), one.size()));
            crpc::frame f;
            while (decoder.next(f) == crpc::frame_decoder::result::frame_ready) {
                sink += f.payload.size();
            }
        }
    });
    crpc_bench::measure("frame/feed_external_1KiB_per_frame", 50'000, 7,
                        [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            crpc::frame_decoder decoder;
            decoder.feed_external(crpc::span<const std::byte>(one.data(), one.size()));
            crpc::frame f;
            while (decoder.next(f) == crpc::frame_decoder::result::frame_ready) {
                sink += f.payload.size();
            }
            decoder.drain_external();
        }
    });
    crpc_bench::measure("frame/feed_copy_batch64_per_frame", 20'000, 7,
                        [&](std::size_t n) {
        std::size_t frames = 0;
        while (frames < n) {
            crpc::frame_decoder decoder;
            decoder.feed(crpc::span<const std::byte>(batch.data(), batch.size()));
            crpc::frame f;
            while (decoder.next(f) == crpc::frame_decoder::result::frame_ready) {
                sink += f.payload.size();
                ++frames;
            }
        }
    });
    crpc_bench::measure("frame/feed_external_batch64_per_frame", 20'000, 7,
                        [&](std::size_t n) {
        std::size_t frames = 0;
        while (frames < n) {
            crpc::frame_decoder decoder;
            decoder.feed_external(crpc::span<const std::byte>(batch.data(), batch.size()));
            crpc::frame f;
            while (decoder.next(f) == crpc::frame_decoder::result::frame_ready) {
                sink += f.payload.size();
                ++frames;
            }
            decoder.drain_external();
        }
    });
    (void)sink;

    const std::byte* const payload_at = one.data() + crpc::frame_header_size;
    crpc::frame_decoder copying;
    copying.feed(crpc::span<const std::byte>(one.data(), one.size()));
    crpc::frame copied_frame;
    const bool copied =
        copying.next(copied_frame) == crpc::frame_decoder::result::frame_ready &&
        copied_frame.payload.data() != payload_at;
    crpc::frame_decoder borrowing;
    borrowing.feed_external(crpc::span<const std::byte>(one.data(), one.size()));
    crpc::frame borrowed_frame;
    const bool borrowed =
        borrowing.next(borrowed_frame) == crpc::frame_decoder::result::frame_ready &&
        borrowed_frame.payload.data() == payload_at;
    borrowing.drain_external();
    crpc_bench::record("frame/feed_payload_copies", "copies/frame",
                       copied ? 1.0 : 0.0);
    crpc_bench::record("frame/feed_external_payload_copies", "copies/frame",
                       borrowed ? 0.0 : 1.0);
}

void bench_crc32c() {
    const std::vector<std::byte> small(256, std::byte{0x5A});
    const std::vector<std::byte> large(16 * 1024, std::byte{0x5A});
    const crpc::span<const std::byte> small_view(small.data(), small.size());
    const crpc::span<const std::byte> large_view(large.data(), large.size());
    std::uint64_t sink = 0;

    crpc_bench::measure("crc32c/byte_256B", 2'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            sink += crc32c_byte_reference(small_view);
        }
    });
    crpc_bench::measure("crc32c/slicing8_256B", 2'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            sink += crc32c_slicing8_reference(small_view);
        }
    });
    crpc_bench::measure("crc32c/optimized_256B", 2'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            sink += crpc::crc32c(small_view);
        }
    });

    crpc_bench::measure("crc32c/byte_16KiB", 5000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            sink += crc32c_byte_reference(large_view);
        }
    });
    crpc_bench::measure("crc32c/slicing8_16KiB", 5000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            sink += crc32c_slicing8_reference(large_view);
        }
    });
    crpc_bench::measure("crc32c/optimized_16KiB", 5000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            sink += crpc::crc32c(large_view);
        }
    });
    (void)sink;
}

void bench_codec() {
    bench_request request{};
    request.user_id = 42;

    crpc::buffer encoded;
    const crpc::status seeded =
        crpc::encode_into(crpc::binary_codec{}, request, encoded);
    if (!seeded.ok()) {
        std::printf("codec seed failed: %s\n", seeded.message().c_str());
        return;
    }
    const std::size_t payload = encoded.size();

    crpc_bench::measure("codec/binary_encode_small", 1'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            crpc::buffer out;
            out.reserve(payload + 64);
            (void)crpc::encode_into(crpc::binary_codec{}, request, out);
        }
    });

    crpc_bench::measure("codec/binary_decode_small", 1'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            bench_request out{};
            (void)crpc::decode_from(crpc::binary_codec{}, encoded.readable(), out);
        }
    });
}

// --- RPC client concurrency (gap P2) ----------------------------------------
//
// `rpc::client` used to serialize its entire path on one recursive lock: a
// caller starting a request could not proceed while the reader delivered a
// response, and a completion callback stalled every writer for its whole
// duration. The path is now split across read/write/map/completion locks. These
// cases measure `start_call` in two situations: with the reader idle, and while
// a slow callback occupies the reader thread. On the old lock the second cost
// was bounded below by the callback duration; on the split locks it tracks the
// first.

// A backend whose writes complete synchronously (no re-entry into the reader)
// and whose receive side the benchmark feeds itself, so the reader runs on a
// different thread from the writers.
class bench_connection final : public crpc::connection {
public:
    crpc::io::source& source() noexcept override { return source_; }
    crpc::io::sink& sink() noexcept override { return sink_; }
    void bind_source(crpc::io::source*) noexcept override {}
    void resume_read() noexcept override {}
    void close() noexcept override { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
    [[nodiscard]] crpc::endpoint remote() const override {
        return crpc::endpoint::inproc("bench");
    }

    // When non-zero, every write sleeps this long inside `push`, holding the
    // client's write lock for the whole time. Used to model a slow transport.
    void set_write_sleep_us(std::size_t us) noexcept {
        sleep_us_.store(us, std::memory_order_relaxed);
    }
    [[nodiscard]] bool in_slow_write() const noexcept {
        return in_slow_write_.load(std::memory_order_acquire);
    }

private:
    class null_source final : public crpc::io::source {
    public:
        std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
            return data.size();
        }
        void on_eof() noexcept override {}
        void on_error(std::error_code) noexcept override {}
    };
    class null_sink final : public crpc::io::sink {
    public:
        explicit null_sink(bench_connection& owner) noexcept : owner_(owner) {}
        void push(crpc::span<const std::byte> data, crpc::io::completion* c) noexcept override {
            const std::size_t us = owner_.sleep_us_.load(std::memory_order_relaxed);
            if (us != 0) {
                owner_.in_slow_write_.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::microseconds(us));
                owner_.in_slow_write_.store(false, std::memory_order_release);
            }
            c->complete(crpc::io::io_result{{}, data.size()});
        }

    private:
        bench_connection& owner_;
    };

    null_source source_;
    std::atomic<std::size_t> sleep_us_{0};
    std::atomic<bool> in_slow_write_{false};
    null_sink sink_{*this};
    bool open_ = true;
};

// Encodes a success RESPONSE frame with an empty body for `stream_id`.
std::vector<std::byte> bench_response(std::uint32_t stream_id) {
    crpc::buffer payload;
    (void)crpc::rpc::encode_response(crpc::rpc_code::ok, crpc::rpc_error{},
                                     crpc::span<const std::byte>(), payload);
    crpc::frame response;
    response.header.type = crpc::frame_type::response;
    response.header.stream_id = stream_id;
    response.payload = payload.readable();
    std::vector<std::byte> out(crpc::encoded_frame_size(response));
    std::size_t written = 0;
    (void)crpc::encode_frame(crpc::span<std::byte>(out.data(), out.size()), response, written);
    out.resize(written);
    return out;
}

crpc::span<const std::byte> bench_body(const std::string& text) {
    return crpc::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

void bench_rpc_roundtrips(int writers, std::size_t iterations) {
    auto conn = std::make_unique<bench_connection>();
    crpc::rpc::client_options options;
    options.slot_capacity = 4096;
    crpc::rpc::client c(std::move(conn), options);

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::uint32_t> ids;
    bool done = false;
    std::thread responder([&c, &mutex, &cv, &ids, &done] {
        for (;;) {
            std::uint32_t id = 0;
            {
                std::unique_lock<std::mutex> lk(mutex);
                cv.wait(lk, [&] { return !ids.empty() || done; });
                if (ids.empty()) {
                    return;
                }
                id = ids.back();
                ids.pop_back();
            }
            const std::vector<std::byte> response = bench_response(id);
            (void)c.source().on_data(
                crpc::span<const std::byte>(response.data(), response.size()));
        }
    });

    const std::string body = "bench";
    auto run = [&](std::size_t n) {
        std::atomic<std::size_t> next{0};
        auto worker = [&] {
            for (;;) {
                const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) {
                    return;
                }
                auto started = c.start_call<std::string>(1, 1, bench_body(body));
                if (!started.ok()) {
                    continue;
                }
                auto handle = std::move(started).value();
                {
                    std::lock_guard<std::mutex> lk(mutex);
                    ids.push_back(handle.slot()->stream_id);
                }
                cv.notify_one();
                (void)handle.get();
            }
        };
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(writers));
        for (int w = 0; w < writers; ++w) {
            threads.emplace_back(worker);
        }
        for (auto& thread : threads) {
            thread.join();
        }
    };

    const char* name = writers == 1 ? "rpc/client_roundtrip_1writer"
                                    : "rpc/client_roundtrip_4writers";
    crpc_bench::measure(name, iterations, 5, run);

    {
        std::lock_guard<std::mutex> lk(mutex);
        done = true;
    }
    cv.notify_all();
    responder.join();
}

// Measures `start_call` while a completion callback spins on the reader thread.
// The callback is bounded so the benchmark terminates even on a build that
// still serializes the whole path.
void bench_rpc_start_call_during_callback() {
    auto conn = std::make_unique<bench_connection>();
    crpc::rpc::client_options options;
    options.slot_capacity = 200000;
    crpc::rpc::client c(std::move(conn), options);

    std::atomic<bool> entered{false};
    std::atomic<bool> stop{false};
    const std::string body = "bench";

    auto started = c.start_call<std::string>(1, 1, bench_body(body));
    if (!started.ok()) {
        return;
    }
    auto handle = std::move(started).value();
    handle.then([&entered, &stop](crpc::rpc::call<std::string>&) {
        entered.store(true, std::memory_order_release);
        // Long enough that a build which serialized `start_call` behind the
        // callback would report a cost near this bound rather than the
        // callback-free cost.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (!stop.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::yield();
        }
    });

    const std::vector<std::byte> response = bench_response(handle.slot()->stream_id);
    std::thread reader([&c, &response] {
        (void)c.source().on_data(
            crpc::span<const std::byte>(response.data(), response.size()));
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    constexpr std::size_t iterations = 20000;
    std::vector<crpc::rpc::call<std::string>> handles;
    handles.reserve(iterations);
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        auto other = c.start_call<std::string>(1, 1, bench_body(body));
        if (other.ok()) {
            handles.push_back(std::move(other).value());
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    crpc_bench::record("rpc/client_start_call_during_callback", "ns/op",
                       total_ns / static_cast<double>(iterations));

    stop.store(true, std::memory_order_release);
    reader.join();
    handles.clear();
}

// Measures how long the reader takes to deliver a response while a writer
// thread is stuck inside a slow `push` (holding the write lock). With one lock
// for the whole path the reader blocks until the write returns; with the split
// locks the read path never touches the write lock and the delivery stays fast.
void bench_rpc_reader_during_slow_write() {
    auto conn = std::make_unique<bench_connection>();
    bench_connection* raw = conn.get();
    crpc::rpc::client_options options;
    options.slot_capacity = 64;
    crpc::rpc::client c(std::move(conn), options);

    auto started = c.start_call<std::string>(1, 1, bench_body("x"));
    if (!started.ok()) {
        return;
    }
    auto handle = std::move(started).value();
    const std::vector<std::byte> response = bench_response(handle.slot()->stream_id);

    raw->set_write_sleep_us(20000);
    std::unique_ptr<crpc::rpc::call<std::string>> other;
    std::thread writer([&c, &other] {
        auto s = c.start_call<std::string>(1, 1, bench_body("x"));
        if (s.ok()) {
            other = std::make_unique<crpc::rpc::call<std::string>>(std::move(s).value());
        }
    });

    const auto wait_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (!raw->in_slow_write() && std::chrono::steady_clock::now() < wait_until) {
        std::this_thread::yield();
    }
    const bool writer_was_slow = raw->in_slow_write();

    const auto t0 = std::chrono::steady_clock::now();
    (void)c.source().on_data(crpc::span<const std::byte>(response.data(), response.size()));
    const auto t1 = std::chrono::steady_clock::now();

    raw->set_write_sleep_us(0);
    writer.join();
    other.reset();

    if (writer_was_slow) {
        const double ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        crpc_bench::record("rpc/client_response_delivery_during_slow_write", "ns/op", ns);
    }
}

void bench_rpc_client() {
    bench_rpc_roundtrips(1, 20000);
    bench_rpc_roundtrips(4, 20000);
    bench_rpc_start_call_during_callback();
    bench_rpc_reader_during_slow_write();
}

// --- send-path record pool (gap P3) -----------------------------------------
//
// `server_connection::send_payload` used to allocate one `send_op` record and
// one `std::byte[]` buffer per outgoing frame. It now borrows a pooled record
// and its reusable buffer. This case runs a real in-process round trip and reads
// the pool counters: after warm-up a call creates no record and grows no buffer,
// so the per-frame allocation is zero. The reference loop reproduces the old
// pattern (one allocation for the record, one for the buffer) so the red number
// is printed next to the green one.

namespace {

constexpr crpc::u64 kSendPoolService = 950;
constexpr crpc::u64 kSendPoolEcho = 1;

struct bench_echo_service {
    std::string echo(const std::string& value) { return value; }
};

struct bench_send_env {
    crpc::inproc_backend backend;
    bench_echo_service service;
    std::unique_ptr<crpc::server> srv;
    std::unique_ptr<crpc::client> cli;
    std::string name = "crpc-bench-send-pool";

    ~bench_send_env() {
        if (cli) {
            cli->close();
        }
        if (srv) {
            srv->stop();
        }
    }
};

bool bench_send_start(bench_send_env& env) {
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc(env.name), env.backend);
    sb.add_service(kSendPoolService, "bench");
    sb.add_method<bench_echo_service, &bench_echo_service::echo>(
        kSendPoolService, kSendPoolEcho, "echo", env.service);
    sb.drain_timeout(2000ms);
    env.srv = sb.build();
    if (!env.srv || !env.srv->start().ok()) {
        return false;
    }
    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc(env.name), env.backend);
    cb.pool_size(4);
    cb.max_inflight_per_connection(1024);
    cb.default_deadline(5s);
    env.cli = cb.build();
    return env.cli != nullptr;
}

bool bench_send_one(crpc::client& cli, const std::string& request,
                    std::string& out) {
    auto stub =
        cli.service<std::string, std::string>(kSendPoolService, kSendPoolEcho);
    auto started = stub.call(request);
    if (!started.ok()) {
        return false;
    }
    auto result = std::move(started).value().get();
    if (!result.ok()) {
        return false;
    }
    out = std::move(result).value();
    return out == request;
}

} // namespace

void bench_send_pool() {
    bench_send_env env;
    if (!bench_send_start(env)) {
        std::printf("send/pool: environment did not start\n");
        return;
    }
    const std::string request(64, 'x');
    std::string out;
    for (int i = 0; i < 2000; ++i) {
        (void)bench_send_one(*env.cli, request, out);
    }

    // Red: the pre-optimisation per-frame pattern, measured with the counting
    // allocator. One record allocation plus one buffer allocation per frame.
    constexpr int kReferenceFrames = 20000;
    const std::size_t red_before =
        g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < kReferenceFrames; ++i) {
        std::unique_ptr<std::byte[]> record(new (std::nothrow) std::byte[64]);
        std::unique_ptr<std::byte[]> buffer(new (std::nothrow) std::byte[256]);
        (void)record;
        (void)buffer;
    }
    const std::size_t red =
        g_allocations.load(std::memory_order_relaxed) - red_before;

    // Green: the pooled server path, measured through the pool counters.
    const crpc::server::send_pool_counters before = env.srv->send_pool_stats();
    constexpr int kCalls = 50000;
    const auto t0 = std::chrono::steady_clock::now();
    int ok = 0;
    for (int i = 0; i < kCalls; ++i) {
        if (bench_send_one(*env.cli, request, out)) {
            ++ok;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const crpc::server::send_pool_counters after = env.srv->send_pool_stats();
    const std::size_t ops = after.ops_allocated - before.ops_allocated;
    const std::size_t buffers = after.buffers_allocated - before.buffers_allocated;
    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    const double reference = static_cast<double>(red) / kReferenceFrames;

    std::printf("send/pool: reference allocs/frame=%.2f  pooled ops/call=%.4f  "
                "buffers/call=%.4f  calls=%d ok=%d time=%.4fs\n",
                reference, static_cast<double>(ops) / kCalls,
                static_cast<double>(buffers) / kCalls, kCalls, ok, seconds);
    crpc_bench::record("send/pool_allocations_per_call", "allocs/call",
                       static_cast<double>(ops + buffers) / kCalls);
    crpc_bench::record("send/pool_reference_allocations_per_frame", "allocs",
                       reference);
}

// The facade call-state pool (P1). The red path is the pre-optimisation
// per-call pair -- one `make_shared<call_state>` plus one
// `make_unique<retry_sequence>` -- counted with the process allocator. The green
// path is the pooled allocation, counted with the pool's own counters: after
// warm-up it reuses a block and never reaches the system allocator.
void bench_call_state_pool() {
    using state_type = crpc::client_detail::call_state<int>;
    auto pool = std::make_shared<crpc::client_detail::call_state_pool>();
    crpc::client_detail::call_state_allocator<state_type> alloc(pool);

    // Warm up so the pooled path reuses blocks instead of growing the pool.
    for (int i = 0; i < 1000; ++i) {
        (void)std::allocate_shared<state_type>(alloc);
    }

    // Red: the pre-optimisation per-call pair, counted with the allocator.
    // (The reference is larger than two on a Debug build because the STL's
    // iterator-debugging proxies allocate for each `std::vector` member.)
    constexpr int kReferenceCalls = 20000;
    const std::size_t red_before =
        g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < kReferenceCalls; ++i) {
        auto state = std::make_shared<state_type>();
        (void)std::make_unique<crpc::resilience::retry_sequence>(
            crpc::resilience::retry_policy{}, nullptr);
    }
    const std::size_t red =
        g_allocations.load(std::memory_order_relaxed) - red_before;

    // Green: the pooled path, counted with the allocator (total) and the pool's
    // own counters (the pool's share).
    auto& counters = crpc::client_detail::g_call_state_pool_counters;
    counters.system_alloc.store(0, std::memory_order_relaxed);
    counters.reuse.store(0, std::memory_order_relaxed);
    const std::size_t green_before =
        g_allocations.load(std::memory_order_relaxed);
    constexpr int kCalls = 50000;
    for (int i = 0; i < kCalls; ++i) {
        (void)std::allocate_shared<state_type>(alloc);
    }
    const std::size_t green =
        g_allocations.load(std::memory_order_relaxed) - green_before;
    const std::size_t system_alloc =
        counters.system_alloc.load(std::memory_order_relaxed);
    const std::size_t reuse =
        counters.reuse.load(std::memory_order_relaxed);

    std::printf("client/call_state_pool: reference allocs/call=%.2f  "
                "pooled allocs/call=%.2f  pool system allocs/call=%.4f  "
                "reuse/call=%.4f  calls=%d\n",
                static_cast<double>(red) / kReferenceCalls,
                static_cast<double>(green) / kCalls,
                static_cast<double>(system_alloc) / kCalls,
                static_cast<double>(reuse) / kCalls, kCalls);
    crpc_bench::record("client/call_state_pool_reference_allocations_per_call",
                       "allocs/call", static_cast<double>(red) / kReferenceCalls);
    crpc_bench::record("client/call_state_pool_pooled_allocations_per_call",
                       "allocs/call", static_cast<double>(green) / kCalls);
    crpc_bench::record("client/call_state_pool_system_allocations_per_call",
                       "allocs/call", static_cast<double>(system_alloc) / kCalls);
    crpc_bench::record("client/call_state_pool_reuse_per_call", "reuse/call",
                       static_cast<double>(reuse) / kCalls);
}

// --- D3b: low-level rpc::call + request buffer allocation (P3b) ---------------
//
// The facade used to allocate the low-level `rpc::call<Resp>` handle with
// `make_unique` and to grow `core.request` into a fresh heap block on every
// call. Both are now inline/persistent: the handle lives in `call_state` and
// the request buffer is retained by the pool. The reference loop reproduces
// the old pair so the red number sits next to the green one.

struct d3b_stalled_source final : crpc::io::source {
    std::size_t on_data(crpc::span<const std::byte>) noexcept override { return 0; }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}
};

struct d3b_stalled_sink final : crpc::io::sink {
    // Acknowledge every write synchronously so the low-level client's write
    // loop stays healthy and the connection is never saturated. The bytes are
    // dropped: the call never completes, which is what isolates the setup cost.
    void push(crpc::span<const std::byte> data, crpc::io::completion* c) noexcept override {
        if (c != nullptr) {
            crpc::io::io_result result;
            result.bytes = data.size();
            c->complete(result);
        }
    }
};

struct d3b_stalled_connection final : crpc::connection {
    d3b_stalled_source source_;
    d3b_stalled_sink sink_;
    std::atomic<bool> open{true};

    crpc::io::source& source() noexcept override { return source_; }
    crpc::io::sink& sink() noexcept override { return sink_; }
    void bind_source(crpc::io::source*) noexcept override {}
    void resume_read() noexcept override {}
    void close() noexcept override { open.store(false, std::memory_order_release); }
    [[nodiscard]] bool is_open() const noexcept override {
        return open.load(std::memory_order_acquire);
    }
    [[nodiscard]] crpc::endpoint remote() const override {
        return crpc::endpoint::inproc("d3b-stalled");
    }
};

struct d3b_stalled_backend final : crpc::backend_factory {
    std::unique_ptr<crpc::connection> connect(const crpc::endpoint&,
                                              const crpc::connect_options&) override {
        return std::make_unique<d3b_stalled_connection>();
    }
    std::unique_ptr<crpc::listener> listen(const crpc::endpoint&,
                                           const crpc::listen_options&) override {
        return nullptr;
    }
};

void bench_low_level_call_alloc() {
    d3b_stalled_backend backend;
    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("d3b-stalled"), backend);
    cb.pool_size(1);
    auto cli = cb.build();
    if (!cli) {
        std::printf("client/low_level_call_alloc: client did not build\n");
        return;
    }

    const std::vector<std::byte> request(64, std::byte{0x5A});
    const crpc::span<const std::byte> request_span(request.data(), request.size());

    for (int i = 0; i < 500; ++i) {
        (void)cli->start_encoded<int>(1, 1, request_span);
    }

    auto& counters = crpc::client_detail::g_call_state_pool_counters;
    counters.system_alloc.store(0, std::memory_order_relaxed);
    counters.reuse.store(0, std::memory_order_relaxed);

    // Red: the pre-optimisation per-call pair -- one `make_unique<rpc::call<int>>`
    // plus one request buffer growth.
    constexpr int kReferenceCalls = 20000;
    const std::size_t red_before =
        g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < kReferenceCalls; ++i) {
        auto inner = std::make_unique<crpc::rpc::call<int>>();
        crpc::buffer request_buffer;
        (void)request_buffer.append(request_span);
    }
    const std::size_t red =
        g_allocations.load(std::memory_order_relaxed) - red_before;

    // Green: the facade's own path, measured with the counting allocator.
    constexpr int kCalls = 50000;

    // Component A: the pooled `call_state` block (the P1 allocation).
    auto pool = std::make_shared<crpc::client_detail::call_state_pool>();
    using d3b_state_type = crpc::client_detail::call_state<int>;
    crpc::client_detail::call_state_allocator<d3b_state_type> d3b_alloc(pool);
    for (int i = 0; i < 1000; ++i) {
        (void)std::allocate_shared<d3b_state_type>(d3b_alloc);
    }
    const std::size_t state_before =
        g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < kCalls; ++i) {
        (void)std::allocate_shared<d3b_state_type>(d3b_alloc);
    }
    const std::size_t state_allocs =
        g_allocations.load(std::memory_order_relaxed) - state_before;

    // Component B: the low-level `rpc::call<int>` handle.
    const std::size_t inner_before =
        g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < kCalls; ++i) {
        auto inner = std::make_unique<crpc::rpc::call<int>>();
        (void)inner;
    }
    const std::size_t inner_allocs =
        g_allocations.load(std::memory_order_relaxed) - inner_before;

    // Component C: the request buffer growth.
    const std::size_t buffer_before =
        g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < kCalls; ++i) {
        crpc::buffer request_buffer;
        (void)request_buffer.append(request_span);
    }
    const std::size_t buffer_allocs =
        g_allocations.load(std::memory_order_relaxed) - buffer_before;

    // Component D: the low-level `rpc::client` start_call path.
    auto raw = std::make_unique<d3b_stalled_connection>();
    crpc::rpc::client_options d3b_co;
    d3b_co.slot_capacity = 4096;
    crpc::rpc::client low(std::move(raw), d3b_co);
    for (int i = 0; i < 1000; ++i) {
        auto h = low.start_call<int>(1, 1, request_span);
        (void)h;
    }
    const std::size_t low_before =
        g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < kCalls; ++i) {
        auto h = low.start_call<int>(1, 1, request_span);
        (void)h;
    }
    const std::size_t low_allocs =
        g_allocations.load(std::memory_order_relaxed) - low_before;

    // Component D2: split the low-level path into setup (handle retained) and
    // teardown (handle destroyed -> cancel).
    std::vector<crpc::rpc::call<int>> d3b_retained;
    d3b_retained.reserve(512);
    for (int i = 0; i < 512; ++i) {
        auto h = low.start_call<int>(1, 1, request_span);
        if (h.ok()) {
            d3b_retained.push_back(std::move(h).value());
        }
    }
    const std::size_t setup_before =
        g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < 512; ++i) {
        auto h = low.start_call<int>(1, 1, request_span);
        if (h.ok()) {
            d3b_retained.push_back(std::move(h).value());
        }
    }
    const std::size_t setup_allocs =
        g_allocations.load(std::memory_order_relaxed) - setup_before;
    const std::size_t teardown_before =
        g_allocations.load(std::memory_order_relaxed);
    d3b_retained.clear();
    const std::size_t teardown_allocs =
        g_allocations.load(std::memory_order_relaxed) - teardown_before;

    // Components E/F: the STL maps the low-level client inserts into per call.
    std::unordered_map<crpc::u32, int> d3b_map;
    const std::size_t map_before =
        g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < kCalls; ++i) {
        d3b_map.emplace(static_cast<crpc::u32>(i), i);
        d3b_map.erase(static_cast<crpc::u32>(i));
    }
    const std::size_t map_allocs =
        g_allocations.load(std::memory_order_relaxed) - map_before;

    std::multimap<int, int> d3b_mmap;
    const std::size_t mmap_before =
        g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < kCalls; ++i) {
        d3b_mmap.emplace(i, i);
        d3b_mmap.erase(i);
    }
    const std::size_t mmap_allocs =
        g_allocations.load(std::memory_order_relaxed) - mmap_before;

    const std::size_t green_before =
        g_allocations.load(std::memory_order_relaxed);
    int failures = 0;
    crpc::rpc_code first_failure = crpc::rpc_code::ok;
    for (int i = 0; i < kCalls; ++i) {
        auto started = cli->start_encoded<int>(1, 1, request_span);
        if (!started.ok()) {
            if (failures == 0) {
                first_failure = started.code();
            }
            ++failures;
        }
    }
    const std::size_t green =
        g_allocations.load(std::memory_order_relaxed) - green_before;
    const crpc::client::statistics stats = cli->stats();

    // Trace the facade path's allocations (first 2000 calls) and print the
    // stacks as raw addresses for offline symbolization.
    g_trace_n.store(0, std::memory_order_relaxed);
    g_trace_on.store(1, std::memory_order_relaxed);
    for (int i = 0; i < 2000; ++i) {
        auto started = cli->start_encoded<int>(1, 1, request_span);
        (void)started;
    }
    g_trace_on.store(0, std::memory_order_relaxed);
    {
        const int n = g_trace_n.load(std::memory_order_relaxed);
        // The module base makes a raw frame address readable in a debugger
        // (subtract it to get an RVA). Only Windows can ask for it, so the other
        // platforms print a null base rather than not compiling.
        void* module_base = nullptr;
#if defined(_WIN32)
        module_base = reinterpret_cast<void*>(GetModuleHandle(nullptr));
#endif
        std::printf("trace: %d allocations, module base=%p\n", n, module_base);
        for (int i = 0; i < n && i < 40; ++i) {
            std::printf("  #%d:", i);
            for (int j = 0; j < 8; ++j) {
                void* frame = g_trace[i][j];
                if (frame == nullptr) {
                    break;
                }
                std::printf(" %p", frame);
            }
            std::printf("\n");
        }
    }
    const std::size_t system_alloc =
        counters.system_alloc.load(std::memory_order_relaxed);
    const std::size_t reuse =
        counters.reuse.load(std::memory_order_relaxed);

    std::printf("client/low_level_call_alloc: reference allocs/call=%.4f  "
                "facade allocs/call=%.4f  pool system allocs/call=%.4f  "
                "reuse/call=%.4f  calls=%d failures=%d first_failure=%s "
                "started=%zu retries=%zu\n",
                static_cast<double>(red) / kReferenceCalls,
                static_cast<double>(green) / kCalls,
                static_cast<double>(system_alloc) / kCalls,
                static_cast<double>(reuse) / kCalls, kCalls, failures,
                crpc::to_string(first_failure), stats.calls_started,
                stats.retries);
    std::printf("client/low_level_call_alloc[parts]: call_state allocs/call=%.4f  "
                "rpc::call allocs/call=%.4f  request buffer allocs/call=%.4f  "
                "low-level start_call allocs/call=%.4f  setup allocs/call=%.4f  "
                "teardown allocs/call=%.4f  unordered_map allocs/call=%.4f  "
                "multimap allocs/call=%.4f\n",
                static_cast<double>(state_allocs) / kCalls,
                static_cast<double>(inner_allocs) / kCalls,
                static_cast<double>(buffer_allocs) / kCalls,
                static_cast<double>(low_allocs) / kCalls,
                static_cast<double>(setup_allocs) / 512,
                static_cast<double>(teardown_allocs) / 512,
                static_cast<double>(map_allocs) / kCalls,
                static_cast<double>(mmap_allocs) / kCalls);
    crpc_bench::record(
        "client/low_level_call_alloc_reference_allocations_per_call",
        "allocs/call", static_cast<double>(red) / kReferenceCalls);
    crpc_bench::record(
        "client/low_level_call_alloc_facade_allocations_per_call", "allocs/call",
        static_cast<double>(green) / kCalls);
    crpc_bench::record(
        "client/low_level_call_alloc_pool_system_allocations_per_call",
        "allocs/call", static_cast<double>(system_alloc) / kCalls);
    crpc_bench::record("client/low_level_call_alloc_reuse_per_call", "reuse/call",
                       static_cast<double>(reuse) / kCalls);
}

} // namespace

void run_all_cases() {
    bench_endian();
    bench_wire();
    bench_frame();
    bench_frame_zero_copy();
    bench_crc32c();
    bench_codec();
    bench_rpc_client();
    bench_send_pool();
    bench_call_state_pool();
    bench_low_level_call_alloc();
}

CRPC_BENCH_MAIN()





