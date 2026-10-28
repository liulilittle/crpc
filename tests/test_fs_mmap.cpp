// CRPC -- tests for memory-mapped files.
#include <crpc/fs/fs.hpp>

#include <cstring>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::fs;

namespace {

struct scratch {
    result<temp_dir> made = temp_dir::create();
    path dir() const { return made.value().native(); }
    path file(std::string_view name) const { return dir() / path(std::string(name)); }
};

span<const std::byte> bytes_of(const std::string& s) {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

std::string view_text(span<const std::byte> view) {
    return std::string(reinterpret_cast<const char*>(view.data()), view.size());
}

} // namespace

CRPC_TEST(fs_mmap_read_only_whole_file) {
    scratch s;
    const path p = s.file("whole");
    CRPC_CHECK(write_all_text(p, "0123456789").ok());

    result<memory_mapped_file> m = memory_mapped_file::open_read_only(p);
    CRPC_CHECK(m.ok());
    CRPC_CHECK_EQ(static_cast<u64>(10), m.value().size());
    const span<const std::byte> view = m.value().bytes();
    CRPC_CHECK_EQ(static_cast<std::size_t>(10), view.size());
    CRPC_CHECK_EQ(std::string("0123456789"), view_text(view));

    // A read-only map has no writable view.
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), m.value().writable_bytes().size());
    CRPC_CHECK_FALSE(m.value().writable());
}

CRPC_TEST(fs_mmap_offset_size_range) {
    scratch s;
    const path p = s.file("range");
    CRPC_CHECK(write_all_text(p, "0123456789").ok());

    result<memory_mapped_file> m = memory_mapped_file::open_read_only(p, 3, 5);
    CRPC_CHECK(m.ok());
    CRPC_CHECK_EQ(static_cast<u64>(5), m.value().size());
    CRPC_CHECK_EQ(std::string("34567"), view_text(m.value().bytes()));

    // size==0 maps to EOF.
    result<memory_mapped_file> to_eof = memory_mapped_file::open_read_only(p, 4, 0);
    CRPC_CHECK(to_eof.ok());
    CRPC_CHECK_EQ(static_cast<u64>(6), to_eof.value().size());
    CRPC_CHECK_EQ(std::string("456789"), view_text(to_eof.value().bytes()));
}

CRPC_TEST(fs_mmap_non_aligned_offset) {
    // File content starts exactly at the requested byte; the map still works
    // even though the OS view is aligned to a 64 KiB allocation granularity.
    scratch s;
    const std::string content(200, '.');
    const path p = s.file("unaligned");
    CRPC_CHECK(write_all_text(p, content).ok());

    result<memory_mapped_file> m = memory_mapped_file::open_read_only(p, 5, 10);
    CRPC_CHECK(m.ok());
    const span<const std::byte> view = m.value().bytes();
    CRPC_CHECK_EQ(static_cast<std::size_t>(10), view.size());
    for (std::size_t i = 0; i < view.size(); ++i) {
        CRPC_CHECK_EQ(static_cast<int>(content[5 + i]), static_cast<int>(view[i]));
    }
}

CRPC_TEST(fs_mmap_out_of_range_errors) {
    scratch s;
    const path p = s.file("short");
    CRPC_CHECK(write_all_text(p, "abc").ok()); // 3 bytes

    // Offset beyond EOF.
    result<memory_mapped_file> o1 = memory_mapped_file::open_read_only(p, 4, 0);
    CRPC_CHECK_FALSE(o1.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, o1.code());
    // Offset+size beyond EOF.
    result<memory_mapped_file> o2 = memory_mapped_file::open_read_only(p, 1, 3);
    CRPC_CHECK_FALSE(o2.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, o2.code());
    // Same for the read-write form.
    result<memory_mapped_file> o3 = memory_mapped_file::open_read_write(p, 2, 2);
    CRPC_CHECK_FALSE(o3.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, o3.code());

    // The exact EOF with a zero length is an empty (valid) map, not an error.
    result<memory_mapped_file> edge = memory_mapped_file::open_read_only(p, 3, 0);
    CRPC_CHECK(edge.ok());
    CRPC_CHECK_EQ(static_cast<u64>(0), edge.value().size());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), edge.value().bytes().size());
}

CRPC_TEST(fs_mmap_empty_file_is_empty_map) {
    scratch s;
    const path p = s.file("empty");
    CRPC_CHECK(write_all_text(p, "").ok());
    result<memory_mapped_file> m = memory_mapped_file::open_read_only(p);
    CRPC_CHECK(m.ok());
    CRPC_CHECK_EQ(static_cast<u64>(0), m.value().size());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), m.value().bytes().size());
}

CRPC_TEST(fs_mmap_read_write_persists) {
    scratch s;
    const path p = s.file("rw");
    CRPC_CHECK(write_all_text(p, "hello").ok());

    {
        result<memory_mapped_file> m = memory_mapped_file::open_read_write(p);
        CRPC_CHECK(m.ok());
        CRPC_CHECK_EQ(true, m.value().writable());
        span<std::byte> writable = m.value().writable_bytes();
        CRPC_CHECK_EQ(static_cast<std::size_t>(5), writable.size());
writable[1] = std::byte{'E'};
        writable[4] = std::byte{'!'};
        CRPC_CHECK(m.value().flush().ok());
    } // unmaps on destruction

    CRPC_CHECK_EQ(std::string("hEll!"), read_all_text(p).value());
}

CRPC_TEST(fs_mmap_read_only_file_refuses_write_map) {
    scratch s;
    const path p = s.file("ro");
    CRPC_CHECK(write_all_text(p, "readonly").ok());
    CRPC_CHECK(set_read_only(p, true).ok());

    result<memory_mapped_file> m = memory_mapped_file::open_read_write(p);
    CRPC_CHECK_FALSE(m.ok());
    CRPC_CHECK_EQ(rpc_code::permission_denied, m.code());

    // Reading it is still fine.
    result<memory_mapped_file> r = memory_mapped_file::open_read_only(p);
    CRPC_CHECK(r.ok());
    CRPC_CHECK_EQ(std::string("readonly"), view_text(r.value().bytes()));

    CRPC_CHECK(set_read_only(p, false).ok());
}

CRPC_TEST(fs_mmap_missing_file_and_directory) {
    scratch s;
    result<memory_mapped_file> missing = memory_mapped_file::open_read_only(s.file("ghost"));
    CRPC_CHECK_FALSE(missing.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, missing.code());

    const path d = s.file("adirectory");
    CRPC_CHECK(create_directory(d).ok());
    result<memory_mapped_file> dir = memory_mapped_file::open_read_only(d);
    CRPC_CHECK_FALSE(dir.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, dir.code());
}

CRPC_TEST(fs_mmap_move_semantics) {
    scratch s;
    const path p = s.file("move");
    CRPC_CHECK(write_all_text(p, "abc").ok());

    result<memory_mapped_file> m = memory_mapped_file::open_read_only(p, 1, 2);
    CRPC_CHECK(m.ok());
    memory_mapped_file moved(std::move(m.value()));
    // The moved-from object no longer owns a mapping and cannot be used.
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), m.value().bytes().size());
    CRPC_CHECK_EQ(static_cast<u64>(2), moved.size());
    CRPC_CHECK_EQ(std::string("bc"), view_text(moved.bytes()));
}
CRPC_TEST_MAIN()

