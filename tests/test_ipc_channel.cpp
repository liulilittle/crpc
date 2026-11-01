// CRPC -- a full-duplex channel between two processes on one machine.
//
// Everything here is in-process: two `channel` objects over one mapping stand in for
// two processes. What that covers is the whole protocol -- layout, readiness, the
// direction each end binds, and the back-pressure behaviour -- and what it does not
// cover is the operating system's part (two real processes, two views, two address
// spaces). The distinction matters: a bug in the layout or the direction shows up
// here, a bug in the mapping shows up in `test_fs_shared_memory`.
//
// The direction test is the one that earned its place: an earlier version rebuilt the
// views after a move as if every end were the creator, so a joined end silently
// swapped its rings. Both ends still worked, just talking to themselves.
#include <crpc/ipc/channel.hpp>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::ipc;

namespace {

std::string unique_name(const char* stem) {
    static std::atomic<unsigned> counter{0};
    const unsigned tail = counter.fetch_add(1, std::memory_order_relaxed);
    return std::string("crpc_chan_") + stem + "_" +
           std::to_string(reinterpret_cast<std::uintptr_t>(&counter)) + "_" +
           std::to_string(tail);
}

struct session {
    std::string name = unique_name("s");
    ~session() { (void)fs::shared_memory::remove(name); }
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

CRPC_TEST(channel_create_and_join_produce_a_working_pair) {
    session s;
    auto creator = channel::create(s.name, 256);
    CRPC_CHECK(creator.ok());
    if (!creator.ok()) {
        return;
    }
    auto joiner = channel::join(s.name);
    CRPC_CHECK(joiner.ok());
    if (!joiner.ok()) {
        return;
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(256), creator.value().capacity());
    CRPC_CHECK_EQ(static_cast<std::size_t>(256), joiner.value().capacity());

    // Creator -> joiner.
    const std::vector<std::byte> hello = bytes_of("hello");
    CRPC_CHECK(creator.value().try_send(span<const std::byte>(hello.data(), hello.size())));
    CRPC_CHECK_EQ(static_cast<std::size_t>(5), joiner.value().pending());

    std::byte out[64];
    const std::size_t got = joiner.value().try_receive(span<std::byte>(out, sizeof(out)));
    CRPC_CHECK_EQ(static_cast<std::size_t>(5), got);
    CRPC_CHECK_EQ(std::string("hello"), text_of(span<const std::byte>(out, got)));

    // Joiner -> creator: the other ring, in the other direction. A channel that wired
    // both ends to one ring would pass the block above and fail here.
    const std::vector<std::byte> back = bytes_of("reply");
    CRPC_CHECK(joiner.value().try_send(span<const std::byte>(back.data(), back.size())));
    CRPC_CHECK_EQ(static_cast<std::size_t>(5), creator.value().pending());
    const std::size_t got_back = creator.value().try_receive(span<std::byte>(out, sizeof(out)));
    CRPC_CHECK_EQ(static_cast<std::size_t>(5), got_back);
    CRPC_CHECK_EQ(std::string("reply"), text_of(span<const std::byte>(out, got_back)));

    // Each direction is independent: a send one way must not show up as pending on
    // the sender's own receive side.
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), creator.value().pending());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), joiner.value().pending());
}

CRPC_TEST(channel_survives_a_move_without_swapping_its_ends) {
    session s;
    auto created = channel::create(s.name, 128);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    auto joined = channel::join(s.name);
    CRPC_CHECK(joined.ok());
    if (!joined.ok()) {
        return;
    }

    // Move both ends the way a caller holding them in an optional/container would.
    channel creator = std::move(created.value());
    channel joiner = std::move(joined.value());

    const std::vector<std::byte> payload = bytes_of("after-move");
    CRPC_CHECK(creator.try_send(span<const std::byte>(payload.data(), payload.size())));

    // If the move had rebuilt the joiner as a creator, this would read its own ring
    // (empty) instead of the creator's.
    std::byte out[32];
    const std::size_t got = joiner.try_receive(span<std::byte>(out, sizeof(out)));
    CRPC_CHECK_EQ(static_cast<std::size_t>(10), got);
    CRPC_CHECK_EQ(std::string("after-move"),
                  text_of(span<const std::byte>(out, got)));
}

CRPC_TEST(channel_create_is_exclusive_and_join_requires_a_creator) {
    session s;
    (void)fs::shared_memory::remove(s.name);

    auto missing = channel::join(s.name);
    CRPC_CHECK_FALSE(missing.ok());
    if (!missing.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::not_found),
                      static_cast<int>(missing.error().code));
    }

    auto first = channel::create(s.name, 64);
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }
    // A second creator must be refused: two creators would each believe they own the
    // outbound ring.
    auto second = channel::create(s.name, 64);
    CRPC_CHECK_FALSE(second.ok());
    if (!second.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::failed_precondition),
                      static_cast<int>(second.error().code));
    }
}

CRPC_TEST(channel_rejects_a_zero_capacity_ring) {
    session s;
    auto bad = channel::create(s.name, 0);
    CRPC_CHECK_FALSE(bad.ok());
    if (!bad.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::invalid_argument),
                      static_cast<int>(bad.error().code));
    }
    // A refused creation must not leave a name behind.
    auto exists = fs::shared_memory::exists(s.name);
    CRPC_CHECK(exists.ok());
    if (exists.ok()) {
        CRPC_CHECK_FALSE(exists.value());
    }
}

CRPC_TEST(channel_reports_back_pressure_instead_of_dropping) {
    session s;
    auto creator = channel::create(s.name, 16);
    CRPC_CHECK(creator.ok());
    if (!creator.ok()) {
        return;
    }
    auto joiner = channel::join(s.name);
    CRPC_CHECK(joiner.ok());
    if (!joiner.ok()) {
        return;
    }

    const std::vector<std::byte> sixteen(16, std::byte{'a'});
    CRPC_CHECK(creator.value().try_send(span<const std::byte>(sixteen.data(), sixteen.size())));
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), creator.value().send_capacity());

    // Full: the next send must be refused, not silently truncate or wrap over the
    // unread data.
    const std::byte one{'b'};
    CRPC_CHECK_FALSE(creator.value().try_send(span<const std::byte>(&one, 1)));

    // What was queued is intact.
    std::vector<std::byte> out(16);
    const std::size_t got =
        joiner.value().try_receive(span<std::byte>(out.data(), out.size()));
    CRPC_CHECK_EQ(static_cast<std::size_t>(16), got);
    CRPC_CHECK_EQ(std::string(16, 'a'), text_of(span<const std::byte>(out.data(), got)));

    // Draining restores room.
    CRPC_CHECK_EQ(static_cast<std::size_t>(16), creator.value().send_capacity());
}

CRPC_TEST(channel_carries_a_stream_between_threads_in_order) {
    session s;
    auto created = channel::create(s.name, 512);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    auto joined = channel::join(s.name);
    CRPC_CHECK(joined.ok());
    if (!joined.ok()) {
        return;
    }

    constexpr int kRecords = 300;
    constexpr std::size_t kRecord = 8;
    std::vector<std::uint64_t> received;
    received.reserve(kRecords);
    std::atomic<bool> done{false};

    std::thread consumer([&] {
        std::byte buf[kRecord];
        while (!done.load(std::memory_order_acquire) ||
               static_cast<int>(received.size()) < kRecords) {
            const std::size_t got = joined.value().try_receive(span<std::byte>(buf, kRecord));
            if (got == 0) {
                std::this_thread::yield();
                continue;
            }
            CRPC_CHECK_EQ(kRecord, got);
            std::uint64_t value = 0;
            std::memcpy(&value, buf, sizeof(value));
            received.push_back(value);
            if (static_cast<int>(received.size()) >= kRecords) {
                break;
            }
        }
    });

    for (int i = 0; i < kRecords; ++i) {
        const std::uint64_t value = static_cast<std::uint64_t>(i);
        std::byte record[kRecord] = {};
        std::memcpy(record, &value, sizeof(value));
        // The ring is smaller than the total, so this is genuine back-pressure: the
        // test spins until the consumer drains, which is the behaviour a caller with
        // no retry would see as "full".
        while (!created.value().try_send(span<const std::byte>(record, kRecord))) {
            std::this_thread::yield();
        }
    }
    done.store(true, std::memory_order_release);
    consumer.join();

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

CRPC_TEST_MAIN()
