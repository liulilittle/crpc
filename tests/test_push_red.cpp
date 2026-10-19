// CRPC -- baseline-compatible push red/green probe.
//
// This file deliberately uses only APIs that predate the push wiring: a raw
// in-process peer sends one hand-built PUSH frame and observes whether the
// client acknowledges it. On the pre-wiring baseline the client's frame
// dispatcher has no PUSH branch, so the frame is silently dropped and this test
// fails. Once the client handles PUSH/PUSH_ACK it passes. Keeping the probe
// independent of the new push APIs makes that red/green transition reproducible
// on either tree.
#include "test_framework.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <crpc/endian.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/span.hpp>

using namespace crpc;

namespace {

class ack_capture final : public io::source {
public:
    std::size_t on_data(span<const std::byte> data) noexcept override {
        decoder_.feed(data);
        for (;;) {
            frame value;
            const frame_decoder::result result = decoder_.next(value);
            if (result == frame_decoder::result::need_more) {
                break;
            }
            if (result == frame_decoder::result::error) {
                break;
            }
            if (value.header.type == frame_type::push_ack) {
                if (value.payload.size() == 8) {
                    seq_.store(endian::load_le64(value.payload.data()),
                               std::memory_order_release);
                    stream_.store(value.header.stream_id, std::memory_order_release);
                    seen_.store(true, std::memory_order_release);
                }
            }
        }
        return data.size();
    }

    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

    [[nodiscard]] bool seen() const noexcept {
        return seen_.load(std::memory_order_acquire);
    }
    [[nodiscard]] u64 seq() const noexcept {
        return seq_.load(std::memory_order_acquire);
    }
    [[nodiscard]] u32 stream() const noexcept {
        return stream_.load(std::memory_order_acquire);
    }

private:
    frame_decoder decoder_{max_frame_default};
    std::atomic<bool> seen_{false};
    std::atomic<u64> seq_{0};
    std::atomic<u32> stream_{0};
};

struct noop_completion final : io::completion {
    void complete(io::io_result) noexcept override {}
};

} // namespace

CRPC_TEST(client_acknowledges_reliable_push) {
    auto pair = inproc_pair::make();
    ack_capture capture;
    pair.second->bind_source(&capture);

    rpc::client cli(std::move(pair.first));

    // PUSH envelope documented by docs/protocol.md: version, mode, topic id,
    // publisher, epoch, seq, topic seq, body length, body.
    std::byte payload[41]{};
    payload[0] = std::byte{1}; // version
    payload[1] = std::byte{0}; // reliable
    endian::store_le16(payload + 2, static_cast<u16>(9));
    endian::store_le64(payload + 4, static_cast<u64>(3));
    endian::store_le64(payload + 12, static_cast<u64>(1));
    endian::store_le64(payload + 20, static_cast<u64>(7));
    endian::store_le64(payload + 28, static_cast<u64>(1));
    endian::store_le32(payload + 36, static_cast<u32>(1));
    payload[40] = std::byte{'x'};

    frame push;
    push.header.type = frame_type::push;
    push.header.stream_id = 5;
    push.payload = span<const std::byte>(payload, sizeof(payload));

    const std::size_t frame_size = encoded_frame_size(push);
    std::unique_ptr<std::byte[]> encoded(new std::byte[frame_size]);
    std::size_t written = 0;
    CRPC_CHECK(encode_frame(span<std::byte>(encoded.get(), frame_size), push, written).ok());
    CRPC_CHECK_EQ(written, frame_size);

    noop_completion done;
    pair.second->sink().push(span<const std::byte>(encoded.get(), written), &done);

    const bool ack = crpc_test::wait_for([&capture] { return capture.seen(); }, 1000);
    CRPC_CHECK(ack);
    if (ack) {
        CRPC_CHECK_EQ(capture.stream(), static_cast<u32>(5));
        CRPC_CHECK_EQ(capture.seq(), static_cast<u64>(7));
    }

    pair.second->bind_source(nullptr);
}

CRPC_TEST_MAIN()
