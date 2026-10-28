// CRPC -- tests for fs directory operations.
#include <crpc/fs/fs.hpp>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::fs;

namespace {
struct scratch {
    result<temp_dir> made = temp_dir::create();
    path dir() const { return made.value().native(); }
    path sub(std::string_view name) const { return dir() / path(std::string(name)); }
};
} // namespace

CRPC_TEST(fs_directory_current_and_temporary) {
    result<path> cwd = current_directory();
    CRPC_CHECK(cwd.ok());
    CRPC_CHECK(cwd.value().is_absolute());
    CRPC_CHECK_FALSE(cwd.value().empty());

    result<path> tmp = temporary_directory();
    CRPC_CHECK(tmp.ok());
    CRPC_CHECK(is_directory(tmp.value()).ok());
    CRPC_CHECK_EQ(true, is_directory(tmp.value()).value());
}

CRPC_TEST(fs_directory_set_current) {
    scratch s;
    const path here = current_directory().value();
    const path target = s.sub("elsewhere");
    CRPC_CHECK(create_directory(target).ok());

    CRPC_CHECK(set_current_directory(target).ok());
    const path now = current_directory().value();
    CRPC_CHECK(path(now) == path(target) ||
               path(now).generic_string().find(target.generic_string()) !=
                   std::string::npos);

    // A relative file created while "inside" the temp dir is really there.
    CRPC_CHECK(write_all_text(path("created-relative.txt"), "r").ok());
    CRPC_CHECK(exists(target / path("created-relative.txt")).ok());

    CRPC_CHECK(set_current_directory(here).ok());
    CRPC_CHECK_EQ(here.generic_string(), current_directory().value().generic_string());
    CRPC_CHECK(remove_file(target / path("created-relative.txt")).ok());
    CRPC_CHECK(remove_directory(target).ok());
}

CRPC_TEST(fs_directory_create_and_duplicate) {
    scratch s;
    const path d = s.sub("made");
    CRPC_CHECK(create_directory(d).ok());
    CRPC_CHECK_EQ(true, is_directory(d).value());

    const status dup = create_directory(d);
    CRPC_CHECK_FALSE(dup.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, dup.code());
}

CRPC_TEST(fs_directory_create_nested) {
    scratch s;
    const path d = s.sub("a/b/c");
    CRPC_CHECK(create_directories(d).ok());
    CRPC_CHECK_EQ(true, is_directory(d).value());

    // Idempotent on the second call.
    CRPC_CHECK(create_directories(d).ok());
    CRPC_CHECK(create_directories(s.sub("a/b")).ok());
}

CRPC_TEST(fs_directory_create_nested_relative_path) {
    // A fully relative target must terminate: the "." ancestor chain stops and
    // every level is created under the current directory.
    scratch s;
    const path here = current_directory().value();
    const path target = s.sub("cd-here");
    CRPC_CHECK(create_directory(target).ok());
    CRPC_CHECK(set_current_directory(target).ok());

    CRPC_CHECK(create_directories(path("rel/p/q")).ok());
    CRPC_CHECK_EQ(true, is_directory(target / path("rel/p/q")).value());
    // Parent levels exist too.
    CRPC_CHECK_EQ(true, is_directory(target / path("rel/p")).value());
    CRPC_CHECK_EQ(true, is_directory(target / path("rel")).value());

    CRPC_CHECK(remove_all(path("rel")).ok());
    CRPC_CHECK(set_current_directory(here).ok());
    CRPC_CHECK(remove_directory(target).ok());
}

CRPC_TEST(fs_directory_listing) {
    scratch s;
    CRPC_CHECK(write_all_text(s.sub("b.txt"), "bb").ok());
    CRPC_CHECK(write_all_text(s.sub("a.txt"), "aaaaa").ok());
    CRPC_CHECK(write_all_text(s.sub("c.txt"), "c").ok());
    CRPC_CHECK(create_directory(s.sub("sub")).ok());
    CRPC_CHECK(write_all_text(s.sub("sub/inner.txt"), "i").ok());

    result<std::vector<directory_entry>> flat = list_directory(s.dir());
    CRPC_CHECK(flat.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), flat.value().size());
    // Sorted by name.
    CRPC_CHECK_EQ(std::string("a.txt"), flat.value()[0].name);
    CRPC_CHECK_EQ(std::string("b.txt"), flat.value()[1].name);
    CRPC_CHECK_EQ(std::string("c.txt"), flat.value()[2].name);
    CRPC_CHECK_EQ(std::string("sub"), flat.value()[3].name);
    // Sizes / kinds are reported.
    CRPC_CHECK_EQ(static_cast<u64>(5), flat.value()[0].size);
    CRPC_CHECK_EQ(true, flat.value()[0].is_directory == false);
    CRPC_CHECK_EQ(true, flat.value()[3].is_directory);
    CRPC_CHECK_EQ(static_cast<u64>(0), flat.value()[3].size);
    CRPC_CHECK_EQ(true, flat.value()[0].full_path.is_absolute());

    result<std::vector<directory_entry>> deep = list_directory(s.dir(), true);
    CRPC_CHECK(deep.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(5), deep.value().size());
}

CRPC_TEST(fs_directory_listing_missing) {
    scratch s;
    result<std::vector<directory_entry>> r = list_directory(s.sub("none"));
    CRPC_CHECK_FALSE(r.ok());
    CRPC_CHECK_EQ(rpc_code::not_found, r.code());
}

CRPC_TEST(fs_directory_remove_empty_and_nonempty) {
    scratch s;
    const path empty = s.sub("empty");
    CRPC_CHECK(create_directory(empty).ok());
    CRPC_CHECK(remove_directory(empty).ok());
    CRPC_CHECK(remove_directory(empty).ok()); // idempotent
    CRPC_CHECK_EQ(false, is_directory(empty).value());

    const path busy = s.sub("busy");
    CRPC_CHECK(create_directory(busy).ok());
    CRPC_CHECK(write_all_text(busy / path("f.txt"), "x").ok());
    const status nonempty = remove_directory(busy);
    CRPC_CHECK_FALSE(nonempty.ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, nonempty.code());
    CRPC_CHECK_EQ(true, is_directory(busy).value());
}

CRPC_TEST(fs_directory_remove_all_tree) {
    scratch s;
    const path root = s.sub("tree");
    CRPC_CHECK(create_directories(root / path("x/y")).ok());
    CRPC_CHECK(write_all_text(root / path("x/a.txt"), "a").ok());
    CRPC_CHECK(write_all_text(root / path("x/y/b.txt"), "b").ok());
    CRPC_CHECK(write_all_text(root / path("top.txt"), "t").ok());

    CRPC_CHECK(remove_all(root).ok());
    CRPC_CHECK_EQ(false, exists(root).value());
    CRPC_CHECK(remove_all(root).ok()); // idempotent on a missing path
    CRPC_CHECK(remove_all(s.sub("definitely-missing")).ok());
}

CRPC_TEST(fs_directory_remove_all_on_a_file) {
    scratch s;
    const path f = s.sub("single-file");
    CRPC_CHECK(write_all_text(f, "x").ok());
    CRPC_CHECK(remove_all(f).ok());
    CRPC_CHECK_EQ(false, exists(f).value());
}
CRPC_TEST_MAIN()

