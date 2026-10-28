// CRPC -- tests for RAII temporary files and directories.
#include <crpc/fs/fs.hpp>
#include <crpc/fs/temp.hpp>

#include <utility>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::fs;

namespace {
span<const std::byte> bytes_of(const std::string& s) {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}
} // namespace

CRPC_TEST(fs_temp_file_lives_in_temp_and_is_removed) {
    const path tmp = temporary_directory().value();
    path p;
    {
        result<temp_file> t = temp_file::create();
        CRPC_CHECK(t.ok());
        p = t.value().native();
        CRPC_CHECK_FALSE(p.empty());
        CRPC_CHECK(exists(p).ok());
        CRPC_CHECK_EQ(true, is_file(p).value());
        CRPC_CHECK(write_all_bytes(p, bytes_of("payload")).ok());
        CRPC_CHECK_EQ(std::string("payload"), read_all_text(p).value());

        // The file is inside the system temp directory, not the CWD.
        CRPC_CHECK(p.is_absolute());
        const path parent = p.parent();
        CRPC_CHECK_EQ(tmp.generic_string(), parent.generic_string());
    }
    // Destructor ran and deleted the file.
    CRPC_CHECK_EQ(false, exists(p).value());
}

CRPC_TEST(fs_temp_file_committed_survives) {
    path p;
    {
        result<temp_file> t = temp_file::create();
        CRPC_CHECK(t.ok());
        p = t.value().native();
        CRPC_CHECK_EQ(false, t.value().committed());
        CRPC_CHECK(t.value().commit().ok());
        CRPC_CHECK_EQ(true, t.value().committed());
    }
    CRPC_CHECK_EQ(true, exists(p).value());
    CRPC_CHECK(remove_file(p).ok());
}

CRPC_TEST(fs_temp_file_custom_directory) {
    const path cwd = current_directory().value();
    result<temp_file> t = temp_file::create(cwd, "fs-temp-prefix-");
    CRPC_CHECK(t.ok());
    CRPC_CHECK(t.value().native().str().find("fs-temp-prefix-") != std::string::npos);
    CRPC_CHECK_EQ(true, exists(t.value().native()).value());
}

CRPC_TEST(fs_temp_file_remove_now) {
    path p;
    {
        result<temp_file> t = temp_file::create();
        CRPC_CHECK(t.ok());
        p = t.value().native();
        CRPC_CHECK(t.value().remove_now().ok());
        CRPC_CHECK(t.value().native().empty());
        CRPC_CHECK_EQ(false, exists(p).value());
    }
    // Removing again (destructor) is a harmless no-op.
    CRPC_CHECK_EQ(false, exists(p).value());
}

CRPC_TEST(fs_temp_file_move_transfers_ownership) {
    result<temp_file> t = temp_file::create();
    CRPC_CHECK(t.ok());
    temp_file moved(std::move(t.value()));
    // The moved-from object owns nothing.
    CRPC_CHECK(t.value().native().empty());
    CRPC_CHECK_EQ(true, exists(moved.native()).value());
    // `moved` is destroyed here and deletes the file.
}

CRPC_TEST(fs_temp_dir_removed_with_contents) {
    path p;
    {
        result<temp_dir> d = temp_dir::create();
        CRPC_CHECK(d.ok());
        p = d.value().native();
        CRPC_CHECK_EQ(true, is_directory(p).value());
        CRPC_CHECK(write_all_text(p / path("inner.txt"), "inside").ok());
    }
    CRPC_CHECK_EQ(false, exists(p).value());
}

CRPC_TEST(fs_temp_dir_committed_survives) {
    path p;
    {
        result<temp_dir> d = temp_dir::create();
        CRPC_CHECK(d.ok());
        p = d.value().native();
        CRPC_CHECK(d.value().commit().ok());
    }
    CRPC_CHECK_EQ(true, is_directory(p).value());
    CRPC_CHECK(remove_all(p).ok());
}
CRPC_TEST_MAIN()

