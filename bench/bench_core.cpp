// CRPC -- microbenchmarks for the wire and codec layers.
//
// These are the two hot paths that run once per RPC on every node, so their
// per-operation cost sets the floor for the whole framework. Everything here is
// measured in-process; there is no network and no thread involved.
#include <crpc/codec.hpp>
#include <crpc/endian.hpp>
#include <crpc/frame.hpp>
#include <crpc/wire.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <rtti/reflection.hpp>

#include "bench_harness.hpp"

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

} // namespace

void run_all_cases() {
    bench_endian();
    bench_wire();
    bench_frame();
    bench_codec();
}

CRPC_BENCH_MAIN()





