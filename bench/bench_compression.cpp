// CRPC -- frame compression benchmark.
//
// Measures the real frame path (header + logical-length prefix + codec stream)
// for every codec this build supports:
//
//   none, lz4, deflate, lz77, zip, gzip
//
// Two payload shapes are used because a compression ratio is meaningless
// without the data it was measured on:
//
//   * text   -- repeated request/log lines, the shape an RPC or an HTTP body
//               usually has;
//   * binary -- a xorshift64 stream, incompressible by construction. A codec
//               that cannot make it smaller must fall back to `none`; the
//               benchmark reports the chosen algorithm instead of pretending
//               the requested one was used.
//
// Each case reports the wire size, the compression ratio, and the encode and
// decode throughput. The harness warms up, runs the case several times and
// reports the median; the numbers are single-process loopback and do not
// include a network.
#include <crpc/frame.hpp>
#include <crpc/frame_compression.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bench_harness.hpp"

namespace {

constexpr std::size_t k_small_payload = 4 * 1024;
constexpr std::size_t k_large_payload = 64 * 1024;

const char* algorithm_name(crpc::compression_algorithm algorithm) {
    switch (algorithm) {
    case crpc::compression_algorithm::none: return "none";
    case crpc::compression_algorithm::lz4: return "lz4";
    case crpc::compression_algorithm::zstd: return "zstd";
    case crpc::compression_algorithm::gzip: return "gzip";
    case crpc::compression_algorithm::deflate: return "deflate";
    case crpc::compression_algorithm::lz77: return "lz77";
    case crpc::compression_algorithm::zip: return "zip";
    }
    return "unknown";
}

std::vector<std::byte> make_text(std::size_t size) {
    static const char line[] =
        "GET /api/v1/orders/12345 status=ok latency=17ms user=alice\n";
    const std::size_t line_size = sizeof(line) - 1;
    std::vector<std::byte> out;
    out.reserve(size);
    while (out.size() < size) {
        const std::size_t room = size - out.size();
        const std::size_t take = std::min(room, line_size);
        for (std::size_t i = 0; i < take; ++i) {
            out.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(line[i])));
        }
    }
    return out;
}

std::vector<std::byte> make_binary(std::size_t size) {
    std::vector<std::byte> out(size);
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    for (std::byte& byte : out) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        byte = static_cast<std::byte>(state & 0xFFu);
    }
    return out;
}

crpc::frame make_frame(const std::vector<std::byte>& payload) {
    crpc::frame value;
    value.header.type = crpc::frame_type::request;
    value.header.stream_id = 1;
    value.payload =
        crpc::span<const std::byte>(payload.data(), payload.size());
    return value;
}

std::size_t encode_once(crpc::compression_algorithm algorithm,
                        const crpc::frame& value, crpc::span<std::byte> out,
                        const crpc::compression_limits& limits,
                        crpc::compression_algorithm& chosen) {
    std::size_t written = 0;
    chosen = crpc::compression_algorithm::none;
    const crpc::status encoded = crpc::encode_frame_compressed(
        out, value, algorithm, limits, written, &chosen);
    return encoded.ok() ? written : 0;
}

bool round_trips(const std::vector<std::byte>& wire,
                 const std::vector<std::byte>& plain,
                 const crpc::compression_limits& limits) {
    std::vector<std::byte> decoded(plain.size());
    crpc::frame value;
    const crpc::status status = crpc::decode_frame_compressed(
        crpc::span<const std::byte>(wire.data(), wire.size()), value,
        crpc::span<std::byte>(decoded.data(), decoded.size()),
        crpc::max_frame_default, limits);
    if (!status.ok() || value.payload.size() != plain.size()) {
        return false;
    }
    // An uncompressed frame borrows the input; a compressed frame points into
    // `decoded`. Comparing the returned payload view covers both cases.
    return plain.empty() ||
           std::memcmp(value.payload.data(), plain.data(), plain.size()) == 0;
}

void bench_case(const char* kind, const std::vector<std::byte>& plain,
                crpc::compression_algorithm algorithm, std::size_t iterations,
                int runs) {
    crpc::compression_limits limits;
    limits.max_payload = k_large_payload;
    limits.min_payload = 1;
    // The benchmark is about CPU and ratio, not about the hostile-input guard;
    // disabling the ratio check keeps a legitimately tiny stream measurable.
    limits.max_compression_ratio = 0;

    const crpc::frame frame = make_frame(plain);
    const std::size_t bound =
        crpc::encoded_frame_size_compressed(frame, algorithm, limits);
    // The encode measurement must keep the full worst-case buffer: shrinking it
    // to the first result would make every later encode return encode_error
    // before touching the codec, which is the opposite of what is measured.
    std::vector<std::byte> buffer(bound);
    crpc::compression_algorithm chosen = crpc::compression_algorithm::none;
    const std::size_t wire_size = encode_once(
        algorithm, frame, crpc::span<std::byte>(buffer.data(), buffer.size()),
        limits, chosen);
    if (wire_size == 0) {
        std::printf("compression/%s_%zu_%s: encode failed\n", kind,
                    plain.size(), algorithm_name(algorithm));
        return;
    }
    const std::vector<std::byte> wire(buffer.begin(),
                                      buffer.begin() +
                                          static_cast<std::ptrdiff_t>(wire_size));
    if (!round_trips(wire, plain, limits)) {
        std::printf("compression/%s_%zu_%s: round trip failed\n", kind,
                    plain.size(), algorithm_name(algorithm));
        return;
    }

    const double ratio =
        static_cast<double>(wire.size()) / static_cast<double>(plain.size());

    char encode_name[128];
    char decode_name[128];
    char encode_rate[128];
    char decode_rate[128];
    char ratio_name[128];
    std::snprintf(encode_name, sizeof(encode_name),
                  "compression/encode_%s_%zu_%s_ns", kind, plain.size(),
                  algorithm_name(algorithm));
    std::snprintf(decode_name, sizeof(decode_name),
                  "compression/decode_%s_%zu_%s_ns", kind, plain.size(),
                  algorithm_name(algorithm));
    std::snprintf(encode_rate, sizeof(encode_rate),
                  "compression/encode_%s_%zu_%s", kind, plain.size(),
                  algorithm_name(algorithm));
    std::snprintf(decode_rate, sizeof(decode_rate),
                  "compression/decode_%s_%zu_%s", kind, plain.size(),
                  algorithm_name(algorithm));
    std::snprintf(ratio_name, sizeof(ratio_name),
                  "compression/ratio_%s_%zu_%s", kind, plain.size(),
                  algorithm_name(algorithm));

    std::size_t sink = 0;
    crpc::compression_algorithm scratch = crpc::compression_algorithm::none;
    crpc_bench::measure(encode_name, iterations, runs, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            sink += encode_once(
                algorithm, frame,
                crpc::span<std::byte>(buffer.data(), buffer.size()), limits,
                scratch);
        }
    });

    if (crpc_bench::enabled(encode_name)) {
        const double ns = crpc_bench::results().back().value;
        const double mib = (static_cast<double>(plain.size()) /
                            (1024.0 * 1024.0)) /
                           (ns / 1e9);
        crpc_bench::record(encode_rate, "MiB/s", mib);
        crpc_bench::record(ratio_name, "ratio", ratio);
    }

    std::vector<std::byte> decoded(plain.size());
    crpc_bench::measure(decode_name, iterations, runs, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            crpc::frame value;
            const crpc::status status = crpc::decode_frame_compressed(
                crpc::span<const std::byte>(wire.data(), wire.size()), value,
                crpc::span<std::byte>(decoded.data(), decoded.size()),
                crpc::max_frame_default, limits);
            if (status.ok()) {
                sink += value.payload.size();
            }
        }
    });

    if (crpc_bench::enabled(decode_name)) {
        const double ns = crpc_bench::results().back().value;
        const double mib = (static_cast<double>(plain.size()) /
                            (1024.0 * 1024.0)) /
                           (ns / 1e9);
        crpc_bench::record(decode_rate, "MiB/s", mib);
    }
    (void)sink;

    std::printf("compression/%s_%zu: requested=%s chosen=%s plain=%zu "
                "wire=%zu ratio=%.3f\n",
                kind, plain.size(), algorithm_name(algorithm),
                algorithm_name(chosen), plain.size(), wire.size(), ratio);
    std::fflush(stdout);
}

crpc::span<const std::byte> bytes_of(const std::vector<std::byte>& value) {
    return crpc::span<const std::byte>(value.data(), value.size());
}

crpc::span<std::byte> writable_of(std::vector<std::byte>& value) {
    return crpc::span<std::byte>(value.data(), value.size());
}

template <class Body>
double median_ns(int iterations, Body&& body) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    for (int i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        body();
        samples.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start)
                .count()));
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

// One former test gate, reported as a soft threshold: PASS/WARN, never a
// non-zero exit. `WARN` means the codec changed, not that the build failed.
// Only the measurement goes into the harness result; the limit is a property of
// this ad-hoc comparison, not of a sample, so it is printed here and not passed
// to `record` (whose fourth argument is the sample spread).
void soft_budget(const char* name, double value_ns, double limit_ns) {
    char label[128];
    std::snprintf(label, sizeof(label), "compression/budget/%s", name);
    std::printf("%-46s %s  %10.1f ns  limit %10.1f ns\n", label,
                value_ns <= limit_ns ? "PASS" : "WARN", value_ns, limit_ns);
    crpc_bench::record(label, "ns", value_ns);
    std::fflush(stdout);
}

// The absolute time budgets that tests/test_frame_compression.cpp used to gate
// on now live here. They were removed from the test because a wall-clock
// microsecond budget on a shared runner measures the runner, and the Debug
// medians already sat inside the old ceilings, so the gate could not catch the
// regression it named. The constants are unchanged, so a real regression still
// shows up as `WARN` in this report; bench/ is not part of CI
// (bench/CMakeLists.txt), so it never turns a green build red.
//
// The two relative checks (LZ4 against CRC32C over the same buffer, and 32 KiB
// against 256 B) are kept as they were: they compare workloads that are
// instrumented and optimised the same way, so they survive /Od and sanitizers.
void budget_cases() {
    if (!crpc_bench::enabled("compression/budget")) {
        return;
    }

    const std::vector<std::byte> text = make_text(32 * 1024);
    std::vector<std::byte> stream(crpc::compressed_payload_bound(
        crpc::compression_algorithm::lz4, text.size()));
    std::size_t written = 0;
    for (int i = 0; i < 8; ++i) {
        (void)crpc::compress_payload(crpc::compression_algorithm::lz4,
                                     bytes_of(text), writable_of(stream),
                                     written);
    }
    const double encode_ns = median_ns(200, [&] {
        (void)crpc::compress_payload(crpc::compression_algorithm::lz4,
                                     bytes_of(text), writable_of(stream),
                                     written);
    });

    const crpc::span<const std::byte> wire =
        crpc::span<const std::byte>(stream.data(), written);
    std::vector<std::byte> plain_out(text.size());
    std::size_t plain_size = 0;
    const double decode_ns = median_ns(200, [&] {
        (void)crpc::decompress_payload(crpc::compression_algorithm::lz4, wire,
                                       text.size(), writable_of(plain_out),
                                       plain_size);
    });

    const std::vector<std::byte> small = make_text(256);
    std::vector<std::byte> small_stream(crpc::compressed_payload_bound(
        crpc::compression_algorithm::lz4, small.size()));
    std::size_t small_written = 0;
    for (int i = 0; i < 16; ++i) {
        (void)crpc::compress_payload(crpc::compression_algorithm::lz4,
                                     bytes_of(small),
                                     writable_of(small_stream), small_written);
    }
    const double small_ns = median_ns(500, [&] {
        (void)crpc::compress_payload(crpc::compression_algorithm::lz4,
                                     bytes_of(small),
                                     writable_of(small_stream), small_written);
    });

    std::uint32_t crc_sink = 0;
    const double crc_ns = median_ns(200, [&] {
        crc_sink = crpc::crc32c(bytes_of(text));
    });
    (void)crc_sink;

    std::printf("compression/budget: enc32k=%.1f dec32k=%.1f enc256=%.1f "
                "crc32k=%.1f ns\n",
                encode_ns, decode_ns, small_ns, crc_ns);

    soft_budget("lz4_encode_le_crc_x12", encode_ns, crc_ns * 12.0);
    soft_budget("lz4_decode_le_crc_x12", decode_ns, crc_ns * 12.0);
    if (small_ns > 0.0) {
        soft_budget("lz4_encode_le_small_x512", encode_ns,
                    small_ns * 128.0 * 4.0);
    }

#if defined(NDEBUG)
    // Shipping ceilings, MSVC 19.44 x64 /O2: 150 us / 100 us / 2 us.
    soft_budget("lz4_encode_32k_release", encode_ns, 150000.0);
    soft_budget("lz4_decode_32k_release", decode_ns, 100000.0);
    soft_budget("lz4_encode_256_release", small_ns, 2000.0);
#elif defined(_MSC_VER)
    // MSVC Debug /Od ceilings: 400 us / 300 us / 20 us.
    soft_budget("lz4_encode_32k_debug", encode_ns, 400000.0);
    soft_budget("lz4_decode_32k_debug", decode_ns, 300000.0);
    soft_budget("lz4_encode_256_debug", small_ns, 20000.0);
#endif
}

void run_all_cases() {
    budget_cases();

    const crpc::compression_algorithm algorithms[] = {
        crpc::compression_algorithm::none,
        crpc::compression_algorithm::lz4,
        crpc::compression_algorithm::deflate,
        crpc::compression_algorithm::lz77,
        crpc::compression_algorithm::zip,
        crpc::compression_algorithm::gzip,
    };

    const std::vector<std::byte> text_small = make_text(k_small_payload);
    const std::vector<std::byte> text_large = make_text(k_large_payload);
    const std::vector<std::byte> binary_small = make_binary(k_small_payload);
    const std::vector<std::byte> binary_large = make_binary(k_large_payload);

    for (const crpc::compression_algorithm algorithm : algorithms) {
        bench_case("text", text_small, algorithm, 2000, 3);
        bench_case("text", text_large, algorithm, 200, 3);
        bench_case("binary", binary_small, algorithm, 2000, 3);
        bench_case("binary", binary_large, algorithm, 200, 3);
    }
}

} // namespace

CRPC_BENCH_MAIN()
