// CRPC -- shared memory between processes on one machine.
//
// Two things this test is built to catch, because both have bitten real code:
//
//   1. A name that "works" on one platform and not the other. The validator is
//      exercised directly, so the rule is pinned rather than inferred from whatever
//      the OS happened to accept on the machine the test ran on.
//   2. A mapping that looks writable but is not shared. Every write test reads the
//      value back through a *second* handle, because a private copy of the pages
//      would pass a read-back through the same handle.
#include <crpc/fs/fs.hpp>
#include <crpc/fs/shared_memory.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::fs;

namespace {

// A name no other test run can collide with. A monotonic counter plus this process's
// own address keeps two ctest workers apart without needing a platform pid call.
std::string unique_name(const char* stem) {
    static std::atomic<unsigned> counter{0};
    const unsigned tail = counter.fetch_add(1, std::memory_order_relaxed);
    return std::string("crpc_test_") + stem + "_" +
           std::to_string(reinterpret_cast<std::uintptr_t>(&counter)) + "_" +
           std::to_string(tail);
}

std::string as_text(span<std::byte> view) {
    return std::string(reinterpret_cast<const char*>(view.data()), view.size());
}

void write_text(shared_memory& m, std::string_view text) {
    const auto dst = m.bytes();
    const std::size_t n = text.size() < dst.size() ? text.size() : dst.size();
    std::memcpy(dst.data(), text.data(), n);
}

} // namespace

CRPC_TEST(shared_memory_name_rules_are_enforced_before_the_os_sees_them) {
    // Accepted: the documented alphabet, including dots and dashes inside.
    CRPC_CHECK(validate_shared_name("a").ok());
    CRPC_CHECK(validate_shared_name("metrics.v1").ok());
    CRPC_CHECK(validate_shared_name("ring-buffer_2").ok());
    CRPC_CHECK(validate_shared_name(std::string(96, 'x')).ok());

    // Rejected: a separator would mean different things in the two namespaces.
    CRPC_CHECK_FALSE(validate_shared_name("").ok());
    CRPC_CHECK_FALSE(validate_shared_name("a/b").ok());
    CRPC_CHECK_FALSE(validate_shared_name("a\\b").ok());
    CRPC_CHECK_FALSE(validate_shared_name(".hidden").ok());
    CRPC_CHECK_FALSE(validate_shared_name("..").ok());
    CRPC_CHECK_FALSE(validate_shared_name("with space").ok());
    CRPC_CHECK_FALSE(validate_shared_name(std::string(97, 'x')).ok());
    // A NUL would truncate the name at the C boundary; reject it explicitly.
    CRPC_CHECK_FALSE(validate_shared_name(std::string_view("a\0b", 3)).ok());
}

CRPC_TEST(shared_memory_create_is_exclusive_and_open_existing_requires_a_creator) {
    const std::string name = unique_name("excl");
    (void)shared_memory::remove(name);

    // Nobody created it yet.
    auto absent = shared_memory::open(name, 64, shared_memory_mode::open_existing);
    CRPC_CHECK_FALSE(absent.ok());
    if (!absent.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::not_found),
                      static_cast<int>(absent.error().code));
    }

    auto created = shared_memory::open(name, 64, shared_memory_mode::create);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    CRPC_CHECK_EQ(static_cast<u64>(64), created.value().size());
    CRPC_CHECK(created.value().valid());

    // create is exclusive: the second creator must be refused, not silently handed
    // the existing mapping.
    auto again = shared_memory::open(name, 64, shared_memory_mode::create);
    CRPC_CHECK_FALSE(again.ok());
    if (!again.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::failed_precondition),
                      static_cast<int>(again.error().code));
    }

    // The exclusive refusal must not have damaged the mapping the first one holds.
    write_text(created.value(), "survived");
    CRPC_CHECK_EQ(std::string("survived"), as_text(created.value().bytes()).substr(0, 8));

    CRPC_CHECK(shared_memory::remove(name).ok());
}

CRPC_TEST(shared_memory_writes_are_visible_through_a_second_handle) {
    const std::string name = unique_name("share");
    (void)shared_memory::remove(name);

    auto owner = shared_memory::open(name, 128, shared_memory_mode::create);
    CRPC_CHECK(owner.ok());
    if (!owner.ok()) {
        return;
    }

    // The point of shared memory: a second handle sees the first handle's writes.
    // Reading back through `owner` alone would also pass for a private copy.
    auto peer = shared_memory::open(name, 128, shared_memory_mode::open_existing);
    CRPC_CHECK(peer.ok());
    if (!peer.ok()) {
        (void)shared_memory::remove(name);
        return;
    }
    CRPC_CHECK_EQ(static_cast<u64>(128), peer.value().size());

    write_text(owner.value(), "from-owner");
    CRPC_CHECK_EQ(std::string("from-owner"),
                  as_text(peer.value().bytes()).substr(0, 10));

    write_text(peer.value(), "from-peer");
    CRPC_CHECK_EQ(std::string("from-peer"),
                  as_text(owner.value().bytes()).substr(0, 9));

    (void)shared_memory::remove(name);
}

CRPC_TEST(shared_memory_open_or_create_keeps_the_existing_size) {
    const std::string name = unique_name("ooc");
    (void)shared_memory::remove(name);

    auto first = shared_memory::open(name, 256, shared_memory_mode::open_or_create);
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }
    CRPC_CHECK_EQ(static_cast<u64>(256), first.value().size());

    // Asking for a different size must not resize or truncate what exists: the
    // size belongs to the name's state, and silently resizing would corrupt the
    // other holder's view.
    auto second = shared_memory::open(name, 4096, shared_memory_mode::open_or_create);
    CRPC_CHECK(second.ok());
    if (second.ok()) {
        CRPC_CHECK_EQ(static_cast<u64>(256), second.value().size());
        CRPC_CHECK_EQ(static_cast<u64>(256), first.value().size());
    }

    (void)shared_memory::remove(name);
}

CRPC_TEST(shared_memory_remove_reports_whether_something_was_removed) {
    const std::string name = unique_name("rm");
    (void)shared_memory::remove(name); // clear any leftover from an aborted run

    auto created = shared_memory::open(name, 32, shared_memory_mode::create);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    CRPC_CHECK(created.value().valid());
    write_text(created.value(), "before-remove");

    CRPC_CHECK(shared_memory::remove(name).ok());

    // What removal means differs by platform, and the header says so. Assert the
    // actual contract rather than the POSIX one on both: pretending they agree
    // would make this test pass for the wrong reason on Windows.
    if (remove_unlinks_the_name()) {
        // POSIX: the name is gone, so nobody new can attach.
        auto after = shared_memory::open(name, 32, shared_memory_mode::open_existing);
        CRPC_CHECK_FALSE(after.ok());
        if (!after.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(rpc_code::not_found),
                          static_cast<int>(after.error().code));
        }
        // Removing an already-absent name is an error, so a caller can tell the two
        // cases apart.
        auto gone = shared_memory::remove(name);
        CRPC_CHECK_FALSE(gone.ok());
        if (!gone.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(rpc_code::not_found),
                          static_cast<int>(gone.error().code));
        }
    } else {
        // Windows: the named object outlives the call while this handle is open, so
        // the name is still attachable. That is the documented difference, not a
        // pass to gloss over.
        auto after = shared_memory::open(name, 32, shared_memory_mode::open_existing);
        CRPC_CHECK(after.ok());
    }

    // Either way the view that was open before removal stays usable: a mapping is
    // not invalidated by a name operation.
    CRPC_CHECK_EQ(std::string("before-remove"),
                  as_text(created.value().bytes()).substr(0, 13));
    write_text(created.value(), "still-mapped");
    CRPC_CHECK_EQ(std::string("still-mapped"),
                  as_text(created.value().bytes()).substr(0, 12));

    // Drop the last handle so the Windows case does not leak the name into the next
    // run of this binary.
    created.value() = shared_memory{};
    (void)shared_memory::remove(name);
}

CRPC_TEST(shared_memory_rejects_bad_names_and_zero_size) {
    auto bad = shared_memory::open("bad/name", 64, shared_memory_mode::create);
    CRPC_CHECK_FALSE(bad.ok());
    if (!bad.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::invalid_argument),
                      static_cast<int>(bad.error().code));
    }

    const std::string name = unique_name("zero");
    (void)shared_memory::remove(name);
    auto zero = shared_memory::open(name, 0, shared_memory_mode::create);
    CRPC_CHECK_FALSE(zero.ok());
    if (!zero.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::invalid_argument),
                      static_cast<int>(zero.error().code));
    }
    // A refused creation must not leave a name behind.
    auto probe = shared_memory::exists(name);
    CRPC_CHECK(probe.ok());
    if (probe.ok()) {
        CRPC_CHECK_FALSE(probe.value());
    }
}

CRPC_TEST_MAIN()
