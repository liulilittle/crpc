// CRPC -- filesystem path.
//
// `path` is the single path representation used by the fs framework. It wraps
// a UTF-8 byte string and never touches the OS by itself: parsing, joining and
// normalising are pure string operations, so they are safe to call from any
// thread and fail only through the result type where an operation genuinely
// has no answer (see `relative` across drives).
//
// Platform policy
// ---------------
//   * Separators. Both '/' and '\\' are accepted on every platform and treated
//     as identical separators. Output from `generic_string()` uses '/'; the
//     stored form is the caller's original spelling. `path` never rewrites the
//     caller's string in place.
//   * Case. Comparison (`operator==`, `operator<`, the case helpers) is
//     case-insensitive on Windows (the file system is) and case-sensitive on
//     POSIX. This is the one place the class is deliberately platform-
//     dependent; the alternative would be correct-looking code that silently
//     "finds" the wrong case twice in a directory.
//   * Absolute. `is_absolute()` follows the platform's own definition: on
//     POSIX a leading '/' roots the path; on Windows a drive letter followed
//     by a separator ("C:/x"), a UNC root ("//server/share/...") or an
//     extended-length prefix ("//?/C:...") is absolute, while a bare "C:x" or
//     "\\x" is drive-relative and therefore not absolute.
//   * Normalisation (`normalized()`): resolves "." and "..", collapses repeat
//     separators, drops empty segments and removes the trailing separator
//     (keeping the root itself). Pure string logic; nothing is stat'd.
#ifndef CRPC_FS_PATH_HPP
#define CRPC_FS_PATH_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace fs {

class CRPC_API path {
public:
    path() = default;
    path(const char* text) : text_(text != nullptr ? text : "") {}
    path(std::string text) : text_(std::move(text)) {}
    path(std::string_view text) : text_(text) {}
    path(const path&) = default;
    path& operator=(const path&) = default;
    path(path&&) noexcept = default;
    path& operator=(path&&) noexcept = default;

    // The path is empty when it was constructed from nothing or from an empty
    // string. An empty path does not name anything and is not absolute.
    bool empty() const noexcept { return text_.empty(); }

    // True when the stored string has no slash at all. Note this is a syntactic
    // fact: "foo..bar" is a single name, not a parent reference.
    bool has_parent() const noexcept;

    // The caller's original spelling.
    const std::string& str() const noexcept { return text_; }

    // The path with '/' separators on every platform.
    std::string generic_string() const;

    bool is_absolute() const;
    bool is_relative() const noexcept { return !is_absolute(); }

    // A root path ("/", "C:/", "//server/share") is absolute and has no
    // parent-going-anywhere: its parent is itself.
    bool is_root() const;

    // Directory part. A bare file name yields "."; the root yields the root
    // itself; an empty path yields an empty path. Like `dirname(1)`.
    path parent() const;

    // The final component, excluding any trailing separator. The root and the
    // empty path have no filename ("." and ".." keep their spelling).
    path filename() const;

    // The filename without its final extension.
    std::string stem() const;

    // The final extension including the dot (".cpp"). A leading dot of a
    // dotfile (".bashrc") is a name, not an extension; a missing dot yields "".
    std::string extension() const;

    // Resolves "." / "..", collapses repeated separators and the trailing
    // separator. The root is kept. Pure string logic.
    path normalized() const;

    // Resolves the path against the current working directory when it is
    // relative, without touching the file system beyond `getcwd`/`GetFullPath`.
    result<path> absolute() const;

    // Lexical join. `*this / rhs`; an absolute `rhs` replaces `*this`. No
    // normalisation is performed (call `normalized()` when it is wanted). A
    // bare drive ("C:") joins without a separator, like the platform join.
    path operator/(const path& rhs) const;

    // Equality compares logical path spelling: separator-insensitive and, on
    // Windows, case-insensitive. No normalisation is performed.
    friend bool operator==(const path& a, const path& b) noexcept;
    friend bool operator!=(const path& a, const path& b) noexcept;
    friend bool operator<(const path& a, const path& b) noexcept;

private:
    std::string text_;
};

// Case-sensitive comparison, regardless of platform.
CRPC_API bool path_sensitive_equal(const path& a, const path& b) noexcept;

// The relative spelling of `target` from the point of view of `base`, or an
// error when the two live on different Windows drives (no relative form
// exists). Both paths are absolutised first, so relative inputs are fine.
CRPC_API result<path> relative(const path& base, const path& target);

} // namespace fs
} // namespace crpc

#endif // CRPC_FS_PATH_HPP