// CRPC -- tests for the fs path type.
//
// Path semantics differ between Windows and POSIX, so the tests that depend on
// the host are guarded; the rest must hold on both.
#include <crpc/fs/directory.hpp>
#include <crpc/fs/path.hpp>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::fs;

namespace {
void check_eq_str(const char* expected, const path& p) {
    CRPC_CHECK_EQ(std::string(expected), p.str());
}
} // namespace

CRPC_TEST(fs_path_empty_and_basic) {
    const path e;
    CRPC_CHECK(e.empty());
    CRPC_CHECK_EQ(std::string(), e.str());
    CRPC_CHECK_FALSE(e.is_absolute());
    CRPC_CHECK(e.normalized().empty());

    const path a("a/b");
    CRPC_CHECK_FALSE(a.empty());
    CRPC_CHECK(a.has_parent());
    CRPC_CHECK_FALSE(path("single").has_parent());
    CRPC_CHECK(a.is_relative());
}

CRPC_TEST(fs_path_separator_handling) {
    // Both separators are one separator; generic_string always emits '/'.
    CRPC_CHECK_EQ(std::string("a/b"), path("a\\b").generic_string());
    CRPC_CHECK_EQ(std::string("a\\b"), path("a\\b").str());
    // Separators compare equal regardless of spelling.
    CRPC_CHECK(path("a/b") == path("a\\b"));
}

CRPC_TEST(fs_path_absolute_detection) {
    CRPC_CHECK_FALSE(path("a/b").is_absolute());
#if defined(CRPC_OS_WINDOWS)
    // On Windows a lone leading separator is *drive-relative*, not absolute.
    CRPC_CHECK_FALSE(path("/a").is_absolute());
    CRPC_CHECK(path("C:/a").is_absolute());
    CRPC_CHECK(path("c:\\a\\b").is_absolute());
    CRPC_CHECK_FALSE(path("C:a").is_absolute());
    CRPC_CHECK(path("\\\\server\\share\\x").is_absolute());
#else
    CRPC_CHECK(path("/a").is_absolute());
    CRPC_CHECK_FALSE(path("C:/a").is_absolute());
#endif
}

CRPC_TEST(fs_path_root_detection) {
    CRPC_CHECK(path("/").is_root());
    CRPC_CHECK_FALSE(path("/a").is_root());
    CRPC_CHECK_FALSE(path("").is_root());
#if defined(CRPC_OS_WINDOWS)
    CRPC_CHECK(path("C:/").is_root());
    CRPC_CHECK_FALSE(path("C:").is_root());
    CRPC_CHECK(path("C:/a").is_root() == false);
    CRPC_CHECK(path("\\\\server\\share").is_root());
#endif
}

CRPC_TEST(fs_path_normalization) {
    check_eq_str("a/b/c", path("a//b/./c/").normalized());
    check_eq_str("c", path("a/b/../../c").normalized());
    check_eq_str("..", path("..").normalized());
    check_eq_str("../..", path("../..").normalized());
check_eq_str("b", path("a/../b").normalized());
    check_eq_str("", path("a/..").normalized());
    check_eq_str("", path(".").normalized());
#if defined(CRPC_OS_WINDOWS)
    check_eq_str("C:/", path("C:/a/../..").normalized());
    check_eq_str("C:/a/b", path("C:\\a\\b").normalized());
    check_eq_str("C:a", path("C:a").normalized());
    check_eq_str("C:../x", path("C:../x").normalized());
#else
    check_eq_str("/", path("/a/../..").normalized());
    check_eq_str("/a/b", path("/a//b/").normalized());
#endif
}

CRPC_TEST(fs_path_parent) {
    check_eq_str("a", path("a/b").parent());
    check_eq_str(".", path("a").parent());
    check_eq_str("a", path("a/b/").parent());
    check_eq_str("/", path("/a").parent());
    check_eq_str("/a", path("/a/b").parent());
    check_eq_str("/", path("/").parent());
    check_eq_str(".", path("path/").parent());
    check_eq_str("", path("").parent());
#if defined(CRPC_OS_WINDOWS)
    check_eq_str("C:", path("C:foo").parent());
    check_eq_str("C:/", path("C:/a").parent());
    check_eq_str("C:/a", path("C:/a/b").parent());
    check_eq_str("C:/", path("C:/").parent());
#endif
}

CRPC_TEST(fs_path_filename) {
    check_eq_str("b", path("a/b").filename());
    check_eq_str("b", path("a/b/").filename());
    check_eq_str("..", path("..").filename());
    check_eq_str("single", path("single").filename());
    check_eq_str("", path("/").filename());
    check_eq_str("", path("").filename());
#if defined(CRPC_OS_WINDOWS)
    check_eq_str("", path("C:/").filename());
    check_eq_str("foo", path("C:foo").filename());
#endif
}

CRPC_TEST(fs_path_stem_and_extension) {
    CRPC_CHECK_EQ(std::string("archive.tar"), path("archive.tar.gz").stem());
    CRPC_CHECK_EQ(std::string(".gz"), path("archive.tar.gz").extension());
    CRPC_CHECK_EQ(std::string("a"), path("a.txt").stem());
    CRPC_CHECK_EQ(std::string(".txt"), path("a.txt").extension());
    CRPC_CHECK_EQ(std::string("noext"), path("noext").stem());
    CRPC_CHECK_EQ(std::string(), path("noext").extension());
    CRPC_CHECK_EQ(std::string("a.b"), path("a.b.").stem());
    CRPC_CHECK_EQ(std::string("."), path("a.b.").extension());
    // A leading dot is a name, not an extension.
    CRPC_CHECK_EQ(std::string(".bashrc"), path(".bashrc").stem());
    CRPC_CHECK_EQ(std::string(), path(".bashrc").extension());
    CRPC_CHECK_EQ(std::string("b"), path("a/b").stem());
    CRPC_CHECK_EQ(std::string(), path("a/b").extension());
}

CRPC_TEST(fs_path_join) {
    check_eq_str("a/b", path("a") / path("b"));
    check_eq_str("a/b", path("a/") / path("b"));
    check_eq_str("b", path("") / path("b"));
    check_eq_str("a", path("a") / path(""));
    // A leading separator resets the base on every platform.
    check_eq_str("/b", path("a") / path("/b"));
    check_eq_str("/b", path("") / path("/b"));
    check_eq_str("a/b/c", path("a") / path("b") / path("c"));
    check_eq_str("a\\b", path("a\\") / path("b"));
#if defined(CRPC_OS_WINDOWS)
    check_eq_str("C:foo", path("C:") / path("foo"));
    check_eq_str("C:/x/y", path("C:/x") / path("y"));
#endif
}

CRPC_TEST(fs_path_case_sensitivity) {
    CRPC_CHECK(path("C:/Foo/Bar.txt") == path("c:\\foo\\bar.TXT"));
#if defined(CRPC_OS_WINDOWS)
    CRPC_CHECK_EQ(true, path("C:/a") == path("c:\\A"));
    CRPC_CHECK_EQ(false, path_sensitive_equal(path("C:/a"), path("c:/A")));
#else
    CRPC_CHECK_EQ(false, path("C:/a") == path("c:/A"));
    CRPC_CHECK_EQ(true, path_sensitive_equal(path("C:/a"), path("C:/a")));
    CRPC_CHECK(path("a/b") == path("a\\b"));
    CRPC_CHECK_FALSE(path("a") == path("A"));
#endif
}

CRPC_TEST(fs_path_ordering_is_total) {
    const path a("a"), b("b"), nested("a/b");
    CRPC_CHECK(a < b);
    CRPC_CHECK(a < nested);  // "a" is a strict prefix of "a/b"
    CRPC_CHECK_FALSE(nested < a);
    CRPC_CHECK_FALSE(a < a);
}

CRPC_TEST(fs_path_absolute_resolves_against_cwd) {
    const result<path> empty_abs = path("").absolute();
    CRPC_CHECK_FALSE(empty_abs.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, empty_abs.code());

    result<path> dot = path(".").absolute();
    CRPC_CHECK(dot.ok());
    CRPC_CHECK(dot.value().is_absolute());

    result<path> cwd = current_directory();
    CRPC_CHECK(cwd.ok());
    CRPC_CHECK(path(dot.value()) == path(cwd.value()));

    // A relative path resolves to an absolute path that contains its name.
    result<path> joined = (cwd.value() / path("x").filename()).absolute();
    CRPC_CHECK(joined.ok());
    CRPC_CHECK(joined.value().is_absolute());
}

CRPC_TEST(fs_path_relative) {
    // Under the base, no "..".
    result<path> r1 = relative(path("/a/b"), path("/a/b/c.txt"));
    CRPC_CHECK(r1.ok());
    CRPC_CHECK_EQ(std::string("c.txt"), r1.value().str());

    // Sibling subtree.
    result<path> r2 = relative(path("/a/b"), path("/a/c"));
    CRPC_CHECK(r2.ok());
    CRPC_CHECK_EQ(std::string("../c"), r2.value().str());

    // Deeper: climb over the shared prefix.
    result<path> r3 = relative(path("/a/b/c"), path("/a"));
    CRPC_CHECK(r3.ok());
    CRPC_CHECK_EQ(std::string("../.."), r3.value().str());

    // Same path.
    result<path> r4 = relative(path("/x/y"), path("/x/y"));
    CRPC_CHECK(r4.ok());
    CRPC_CHECK_EQ(std::string("."), r4.value().str());

    // Relative inputs are absolutised first.
    result<path> r5 = relative(path("a/b"), path("a/b/z"));
    CRPC_CHECK(r5.ok());
    CRPC_CHECK_EQ(std::string("z"), r5.value().str());

#if defined(CRPC_OS_WINDOWS)
    // Different drives have no relative spelling.
    result<path> r6 = relative(path("C:/x"), path("D:/y"));
    CRPC_CHECK_FALSE(r6.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, r6.code());
#endif
}
CRPC_TEST_MAIN()

