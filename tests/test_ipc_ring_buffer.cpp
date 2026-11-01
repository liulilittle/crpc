// CRPC -- single-producer single-consumer ring.
//
// Two properties this file exists to pin, both of which a naive implementation gets
// wrong while still passing a simple write-then-read test:
//
//   * **The wrap.** A frame that straddles the end of the buffer must arrive
//     contiguous and complete, not truncated at the boundary or split into two
//     pieces the reader cannot reassemble. Every size from 1 to capacity is written
//     through a buffer of a deliberately awkward size.
//   * **Full versus empty.** A buffer with no free space must refuse the write and
//     leave the queued bytes untouched; a buffer that has been drained completely
//     must accept a full-capacity frame again. The two states are distinguished by
//     the counters, not by a sentinel, so both directions are checked.
#include <crpc/ipc/ring_buffer.hpp>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::ipc;

namespace {

// A region laid out the way a shared-memory mapping would be: the control block
// first, then `capacity` payload bytes.
struct region {
    explicit region(std::size_t capacity)
        : payload(capacity, std::byte{0xCC}) {
        ring_view::initialise(&control, capacity);
        payload_span = span<std::byte>(payload.data(), payload.size());
        producer = ring_view::attach_producer(&control, payload_span);
        consumer = ring_view::attach_consumer(&control, payload_span);
    }

    std::size_t cap() const { return payload.size(); }

    ring_control control{};
    std::vector<std::byte> payload;
    span<std::byte> payload_span{};
    ring_view producer;
    ring_view consumer;
};

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    if (!text.empty()) {
        std::memcpy(out.data(), text.data(), text.size());
    }
    return out;
}

std::string text_of(span<const std::byte> view) {
    return std::string(reinterpret_cast<const char*>(view.data()), view.size());
}

} // namespace

CRPC_TEST(ring_rejects_a_region_that_is_not_a_ring) {
    ring_control blank{};
    CRPC_CHECK_FALSE(ring_view::looks_like_ring(&blank));

    ring_view::initialise(&blank, 64);
    CRPC_CHECK(ring_view::looks_like_ring(&blank));
    // A capacity of zero is not a usable ring: there is no byte to hand out.
    ring_view::initialise(&blank, 0);
    CRPC_CHECK_FALSE(ring_view::looks_like_ring(&blank));
}

CRPC_TEST(ring_capacity_is_the_whole_buffer_with_no_sentinel_slot) {
    region r(64);
    CRPC_CHECK_EQ(static_cast<std::size_t>(64), r.producer.capacity());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), r.producer.size());
    CRPC_CHECK(r.producer.empty());

    // A frame of exactly the capacity must fit. An implementation that spends a slot
    // on the empty/full distinction would refuse this.
    std::vector<std::byte> full(64, std::byte{'f'});
    CRPC_CHECK(r.producer.try_write(span<const std::byte>(full.data(), full.size())));
    CRPC_CHECK_EQ(static_cast<std::size_t>(64), r.producer.size());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), r.producer.free_space());

    // Full is not the same as empty: another byte must be refused.
    const std::byte one{'x'};
    CRPC_CHECK_FALSE(r.producer.try_write(span<const std::byte>(&one, 1)));

    std::vector<std::byte> out(64);
    const std::size_t got = r.consumer.try_read(span<std::byte>(out.data(), out.size()));
    CRPC_CHECK_EQ(static_cast<std::size_t>(64), got);
    CRPC_CHECK_EQ(std::string(64, 'f'), text_of(span<const std::byte>(out.data(), got)));
    CRPC_CHECK(r.consumer.empty());
}

CRPC_TEST(ring_every_size_round_trips_across_the_wrap) {
    // 9 is deliberately not a power of two: the wrap lands on a different offset for
    // almost every size, so an off-by-one in the wrap copy shows up.
    region r(9);
    for (std::size_t size = 1; size <= 9; ++size) {
        // Move the read cursor so the next frame straddles the boundary instead of
        // always starting at offset 0. The ring starts empty each iteration (the
        // frame written below is fully drained), so this cannot loop.
        const std::size_t preamble = (size * 3) % 9;
        if (preamble != 0) {
            std::vector<std::byte> pad(preamble, std::byte{'_'});
            CRPC_CHECK(r.producer.try_write(span<const std::byte>(pad.data(), pad.size())));
            std::vector<std::byte> sink(preamble);
            CRPC_CHECK_EQ(preamble,
                          r.consumer.try_read(span<std::byte>(sink.data(), sink.size())));
        }

        std::string pattern;
        for (std::size_t i = 0; i < size; ++i) {
            pattern.push_back(static_cast<char>('A' + (i % 26)));
        }
        const std::vector<std::byte> frame = bytes_of(pattern);
        CRPC_CHECK(r.producer.try_write(span<const std::byte>(frame.data(), frame.size())));

        std::vector<std::byte> out(size);
        const std::size_t got = r.consumer.try_read(span<std::byte>(out.data(), out.size()));
        CRPC_CHECK_EQ(size, got);
        CRPC_CHECK_EQ(pattern, text_of(span<const std::byte>(out.data(), got)));
    }
}

CRPC_TEST(ring_read_never_crosses_into_bytes_not_yet_written) {
    region r(16);
    const std::vector<std::byte> eight(8, std::byte{'8'});
    CRPC_CHECK(r.producer.try_write(span<const std::byte>(eight.data(), eight.size())));

    // Ask for more than is queued: the read must stop at the queued count instead of
    // handing back the stale 0xCC filling.
    std::vector<std::byte> out(16);
    const std::size_t got = r.consumer.try_read(span<std::byte>(out.data(), out.size()));
    CRPC_CHECK_EQ(static_cast<std::size_t>(8), got);
    CRPC_CHECK_EQ(std::string(8, '8'), text_of(span<const std::byte>(out.data(), got)));

    // And an empty ring reads nothing rather than returning the buffer's history.
    const std::size_t again = r.consumer.try_read(span<std::byte>(out.data(), out.size()));
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), again);
}

CRPC_TEST(ring_readable_run_stops_at_the_wrap) {
    region r(10);
    // Fill 7, drain 6, then queue 6 more: the tail is 4 bytes from the end, so the
    // contiguous run from the read cursor is 4 and the rest is on the other side.
    const std::vector<std::byte> seven(7, std::byte{'a'});
    CRPC_CHECK(r.producer.try_write(span<const std::byte>(seven.data(), seven.size())));
    std::vector<std::byte> sink(6);
    CRPC_CHECK_EQ(static_cast<std::size_t>(6),
                  r.consumer.try_read(span<std::byte>(sink.data(), sink.size())));
    const std::vector<std::byte> six(6, std::byte{'b'});
    CRPC_CHECK(r.producer.try_write(span<const std::byte>(six.data(), six.size())));

    // 1 byte left from the first frame + 6 = 7 queued, at offset 6 of 10.
    CRPC_CHECK_EQ(static_cast<std::size_t>(7), r.producer.size());
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), r.consumer.readable_run());
}

CRPC_TEST(ring_refuses_to_write_through_a_foreign_view) {
    region r(32);
    // A consumer-only view must not be able to produce; that is how a ring ends up
    // with two producers, which breaks every ordering argument in the header.
    const std::vector<std::byte> data(4, std::byte{'z'});
    CRPC_CHECK_FALSE(r.consumer.try_write(span<const std::byte>(data.data(), data.size())));
}

CRPC_TEST(ring_survives_a_thread_handoff_in_order) {
    // The two views are used from different threads, which is the shape a real
    // cross-process channel has. This cannot prove the memory ordering (that needs a
    // weakly-ordered CPU), but it does catch a view that quietly shares the caller's
    // stack or a counter that is not actually atomic.
    //
    // The ring hands out *bytes*, not frames: a read takes what fits in the caller's
    // buffer. This test therefore uses fixed-width 8-byte records so a reader that
    // takes 8 bytes at a time lands exactly on record boundaries -- the framing is
    // the caller's job, and pretending the ring does it would test the wrong thing.
    constexpr int kRecords = 200;
    constexpr std::size_t kRecord = 8;
    region r(kRecords * kRecord);
    std::atomic<int> produced{0};
    std::vector<std::uint64_t> received;
    received.reserve(kRecords);

    std::thread reader([&] {
        std::byte buf[kRecord];
        while (static_cast<int>(received.size()) < kRecords) {
            const std::size_t got = r.consumer.try_read(span<std::byte>(buf, kRecord));
            if (got == 0) {
                std::this_thread::yield();
                continue;
            }
            // A partial record would mean the ring split one the caller had already
            // committed; with a fixed width and an empty-at-start ring that cannot
            // happen, so treat it as a failure rather than skipping it.
            CRPC_CHECK_EQ(kRecord, got);
            std::uint64_t value = 0;
            std::memcpy(&value, buf, sizeof(value));
            received.push_back(value);
        }
    });

    for (int i = 0; i < kRecords; ++i) {
        const std::uint64_t value = static_cast<std::uint64_t>(i);
        std::byte record[kRecord] = {};
        std::memcpy(record, &value, sizeof(value));
        // Every record fits in the total capacity and no other producer competes, so
        // this cannot fail.
        CRPC_CHECK(r.producer.try_write(span<const std::byte>(record, kRecord)));
        produced.fetch_add(1, std::memory_order_relaxed);
    }
    reader.join();

    CRPC_CHECK_EQ(kRecords, produced.load());
    CRPC_CHECK_EQ(static_cast<std::size_t>(kRecords), received.size());
    bool in_order = true;
    for (int i = 0; i < kRecords; ++i) {
        if (received[static_cast<std::size_t>(i)] != static_cast<std::uint64_t>(i)) {
            in_order = false;
            break;
        }
    }
    CRPC_CHECK(in_order);
}

// The index reduction has two paths: a mask for power-of-two capacities and a
// remainder for everything else. Only the mask path can be wrong in a way that a
// power-of-two test misses -- an off-by-one in the mask (`& capacity` instead of
// `& (capacity - 1)`) reads slot `capacity`, which is outside the buffer, and with
// the counters below capacity that index never comes up. So the two paths are
// driven to the same cursor values and required to agree; that is the property, not
// "both happened to work on the capacities this file used before".
CRPC_TEST(ring_mask_and_modulo_paths_agree_at_every_index) {
    // A write immediately followed by a read keeps both cursors equal, and equal
    // cursors make `% cap` and `& (cap - 1)` agree for every value -- which is why
    // the first version of this case could not tell the two paths apart. The ring is
    // therefore kept partly full: seed it, then write and read one byte at a time, so
    // the cursors stay offset by the fill level and every low-bit pattern of the
    // cursor is visited while the slots still have to line up.
    //
    // The value that comes out is the oldest queued byte, not the one just written:
    // the queue is FIFO, so the expectation is a small model of the ring rather than
    // "the marker must come back".
    for (const std::size_t cap : {std::size_t{8}, std::size_t{7},
                                  std::size_t{16}, std::size_t{15}}) {
        for (std::size_t fill = 1; fill < cap; ++fill) {
            region r(cap);
            // Model: the bytes currently queued, oldest first.
            std::vector<std::byte> model;
            for (std::size_t i = 0; i < fill; ++i) {
                const std::byte b{static_cast<unsigned char>(0x40 + (i & 0x3F))};
                model.push_back(b);
                CRPC_CHECK(r.producer.try_write(span<const std::byte>(&b, 1)));
            }
            const std::size_t rounds = cap * 3 + 5;
            for (std::size_t i = 0; i < rounds; ++i) {
                const std::byte marker{static_cast<unsigned char>(i & 0xFF)};
                CRPC_CHECK(r.producer.try_write(span<const std::byte>(&marker, 1)));
                model.push_back(marker);
                std::byte out{};
                CRPC_CHECK_EQ(static_cast<std::size_t>(1),
                              r.consumer.try_read(span<std::byte>(&out, 1)));
                // FIFO: the byte read must be the oldest one still queued.
                CRPC_CHECK_EQ(static_cast<unsigned char>(model.front()),
                              static_cast<unsigned char>(out));
                model.erase(model.begin());
            }
            // Drain what is left and keep checking the same model.
            std::byte out{};
            while (r.consumer.try_read(span<std::byte>(&out, 1)) == 1) {
                CRPC_CHECK_FALSE(model.empty());
                if (!model.empty()) {
                    CRPC_CHECK_EQ(static_cast<unsigned char>(model.front()),
                                  static_cast<unsigned char>(out));
                    model.erase(model.begin());
                }
            }
            CRPC_CHECK(model.empty());
        }
    }
}

CRPC_TEST_MAIN()
