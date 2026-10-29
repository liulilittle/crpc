// CRPC -- tests for the fs byte buffer.
#include <crpc/fs/memory.hpp>

#include <limits>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::fs;

namespace {

std::string_view as_text(const byte_buffer& b) { return b.to_string_view(); }

} // namespace

CRPC_TEST(fs_buffer_append_and_growth) {
    byte_buffer b;
    CRPC_CHECK(b.empty());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), b.size());

    CRPC_CHECK(b.append("hello").ok());
    CRPC_CHECK(b.append(std::string_view(" world")).ok());
    CRPC_CHECK(b.append(std::byte{'!'}).ok());
    CRPC_CHECK_EQ(std::string_view("hello world!"), as_text(b));
    CRPC_CHECK_FALSE(b.empty());
    CRPC_CHECK(b.capacity() >= b.size());
}

CRPC_TEST(fs_buffer_growth_matches_contents) {
    byte_buffer b;
    std::string expected;
    for (int i = 0; i < 1000; ++i) {
        const char c = static_cast<char>('a' + (i % 26));
        CRPC_CHECK(b.append(std::byte{static_cast<std::byte>(c)}).ok());
        expected.push_back(c);
        // Contents intact after each grow.
        CRPC_CHECK_EQ(expected, as_text(b));
        CRPC_CHECK_EQ(expected.size(), b.size());
        CRPC_CHECK_EQ(b.capacity() >= b.size(), true);
    }
    CRPC_CHECK_EQ(expected, b.to_string());
}

CRPC_TEST(fs_buffer_string_interop) {
    byte_buffer from_sv(std::string_view("abc"));
    CRPC_CHECK_EQ(std::string_view("abc"), as_text(from_sv));

    byte_buffer from_str(std::string("def"));
    CRPC_CHECK_EQ(std::string("def"), from_str.to_string());

    byte_buffer assigned;
    CRPC_CHECK(assigned.assign("xyz").ok());
    CRPC_CHECK_EQ(std::string("xyz"), assigned.to_string());
    CRPC_CHECK_EQ(std::string_view("xyz"), assigned.to_string_view());

    const byte_buffer v(std::string_view("vvv"));
    CRPC_CHECK_EQ(std::string("vvv"), v.to_string());
}

CRPC_TEST(fs_buffer_bytes_view_round_trip) {
    byte_buffer b("0123456789");
    CRPC_CHECK_EQ(static_cast<std::size_t>(10), b.bytes().size());
    CRPC_CHECK_EQ(static_cast<int>('3'), static_cast<int>(b.bytes()[3]));
}

CRPC_TEST(fs_buffer_slice_clamps) {
    const byte_buffer b("0123456789");
    // In range.
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), b.slice(2, 3).size());
    CRPC_CHECK_EQ(static_cast<int>('2'), static_cast<int>(b.slice(2, 3)[0]));
    // To-the-end default.
    CRPC_CHECK_EQ(static_cast<std::size_t>(8), b.slice(2).size());
    CRPC_CHECK_EQ(static_cast<int>('9'), static_cast<int>(b.slice(2)[7]));
    // Clamped at the end, never out of bounds.
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), b.slice(7, 1000).size());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), b.slice(10).size());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), b.slice(10, 100).size());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), b.slice(999).size());
    // Slice of an empty buffer.
    const byte_buffer e;
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), e.slice(0).size());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), e.slice(5, 5).size());
}

CRPC_TEST(fs_buffer_equality_and_swap) {
    CRPC_CHECK(byte_buffer("abc") == byte_buffer("abc"));
    CRPC_CHECK_FALSE(byte_buffer("abc") == byte_buffer("abd"));
    CRPC_CHECK_FALSE(byte_buffer("abc") == byte_buffer("ab"));
    CRPC_CHECK(byte_buffer() == byte_buffer());

    byte_buffer a("aaa");
    byte_buffer b("bbbb");
    a.swap(b);
    CRPC_CHECK_EQ(std::string_view("bbbb"), as_text(a));
    CRPC_CHECK_EQ(std::string_view("aaa"), as_text(b));
}

CRPC_TEST(fs_buffer_clear_keeps_capacity) {
    byte_buffer b("some content that is not tiny");
    const std::size_t cap = b.capacity();
    b.clear();
    CRPC_CHECK(b.empty());
    CRPC_CHECK_EQ(cap, b.capacity());
    CRPC_CHECK(b.append("again").ok());
    CRPC_CHECK_EQ(std::string_view("again"), as_text(b));
}

CRPC_TEST(fs_buffer_erase_front) {
    byte_buffer b("0123456789");
    b.erase_front(4);
    CRPC_CHECK_EQ(std::string_view("456789"), as_text(b));
    b.erase_front(100);
    CRPC_CHECK(b.empty());
}

CRPC_TEST(fs_buffer_reserve_failure_is_an_error) {
    // A reservation beyond any addressable allocation must fail with a value,
    // not throw and not return a half-initialised buffer.
    byte_buffer b("x");
    status s = b.reserve(std::numeric_limits<std::size_t>::max() / 2);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, s.code());
    // The previous contents stay intact.
    CRPC_CHECK_EQ(std::string_view("x"), as_text(b));
}

CRPC_TEST(fs_buffer_huge_append_refuses_without_trashing) {
    byte_buffer b("keep");
    const status s = b.append(span<const std::byte>(
        static_cast<const std::byte*>(nullptr),
        std::numeric_limits<std::size_t>::max() / 4));
    CRPC_CHECK_FALSE(s.ok());
    // size_ must not have wrapped past size_;
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), b.size());
    CRPC_CHECK_EQ(std::string_view("keep"), as_text(b));
}

// The refusal above must be a policy, not the allocator happening to fail. On a
// 32-bit build `SIZE_MAX / 4` is 1 GiB, which a 32-bit process can actually
// allocate, so without a ceiling the buffer allocated it and then copied from
// the caller's null pointer (access violation 0xC0000005 in memcpy, x86 Debug
// only -- x64 always failed the 4 EiB allocation, which is why the bug hid).
CRPC_TEST(fs_buffer_ceiling_is_a_policy_not_an_allocation_accident) {
    byte_buffer b("keep");
    // One byte over the documented ceiling, with a real source: the request
    // must be refused before anything is allocated or read.
    const std::byte source = std::byte{0x41};
    const status over = b.append(span<const std::byte>(
        &source, byte_buffer::default_max_capacity + 1));
    CRPC_CHECK_FALSE(over.ok());
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, over.code());
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), b.size());
    CRPC_CHECK_EQ(std::string_view("keep"), as_text(b));

    // The same bound applies to an explicit reserve.
    const status reserved =
        b.reserve(byte_buffer::default_max_capacity + 1);
    CRPC_CHECK_FALSE(reserved.ok());
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, reserved.code());
    CRPC_CHECK_EQ(std::string_view("keep"), as_text(b));

    // Exactly at the ceiling is still a size the type will accept; it may fail
    // to allocate on a small machine, which is a different answer than the
    // policy refusal above and must not be conflated with it.
    byte_buffer big;
    const status at = big.reserve(byte_buffer::default_max_capacity);
    if (!at.ok()) {
        CRPC_CHECK_EQ(rpc_code::resource_exhausted, at.code());
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), big.size());
}
CRPC_TEST_MAIN()

