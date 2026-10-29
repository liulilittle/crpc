#include <crpc/fs/path.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#if defined(CRPC_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif
#include "os.hpp"

namespace crpc {
namespace fs {
namespace {

bool is_sep(char c) noexcept { return c == '/' || c == '\\'; }

bool is_drive_letter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Replaces every separator with '/'.
std::string generic(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    return out;
}

// On Windows, does `s` (generic) start with a drive of the form "X:"?
bool has_drive(const std::string& s) noexcept {
    return s.size() >= 2 && s[0] != '/' && is_drive_letter(s[0]) && s[1] == ':';
}

// Splits a generic path into a root prefix and the remaining segments.
// root is "" when there is no root. Cases (Windows):
//   "C:/a/b"  -> root "C:/", segments [a, b]
//   "C:a"     -> root "C:",   segments [a]
//   "//s/a/b" -> root "//s/a", segments [b]
//   "/a"      -> root "/",    segments [a]      (drive-relative root)
//   "a/b"     -> root "",     segments [a, b]
// POSIX:
//   "/a/b"    -> root "/", segments [a, b]
//   "a/b"     -> root "",  segments [a, b]
struct split_path {
    std::string root;
    std::vector<std::string> segments;
};

split_path split(const std::string& input) {
    const std::string s = generic(input);
    split_path out;
    std::size_t i = 0;
    const std::size_t n = s.size();

#if defined(CRPC_OS_WINDOWS)
    if (n >= 4 && s[0] == '/' && s[1] == '/' && s[2] == '?') {
        // Extended-length prefix "//?/C:/..." or "//?/UNC/...". Treat the first
        // two segments ("?", "C:") as the root so normalization cannot climb
        // above it.
        std::size_t p = 2;
        int seg = 0;
        std::string root("//");
        while (p < n && seg < 2) {
            const std::size_t start = p;
            while (p < n && s[p] != '/') {
                ++p;
            }
            if (p == start) {
                ++p;
                continue;
            }
            if (seg != 0) {
                root.push_back('/');
            }
            root.append(s, start, p - start);
            ++seg;
            ++p;
        }
        out.root = std::move(root);
        if (p < n) {
            ++p; // skip the '/' after the root
        }
        i = p;
    } else if (n >= 2 && s[0] == '/' && s[1] == '/') {
        // UNC "//server/share/...". The root is the server+share pair.
        std::size_t p = 2;
        int seg = 0;
        std::string root("//");
        while (p < n && seg < 2) {
            const std::size_t start = p;
            while (p < n && s[p] != '/') {
                ++p;
            }
            if (p == start) {
                ++p;
                continue;
            }
            if (seg != 0) {
                root.push_back('/');
            }
            root.append(s, start, p - start);
            ++seg;
            ++p;
        }
        out.root = std::move(root);
        i = p;
    } else if (has_drive(s)) {
        // "C:/..." or "C:..." or "C:".
        out.root = s.substr(0, 2);
        if (n > 2 && is_sep(s[2])) {
            out.root.push_back('/');
            i = 3;
        } else {
            i = 2;
        }
    } else if (!s.empty() && s[0] == '/') {
        // A single leading separator is the root of the current drive
        // ("/foo"), which ".." cannot climb above.
        out.root = "/";
        i = 1;
    }
#else
    if (!s.empty() && s[0] == '/') {
        out.root = "/";
        i = 1;
    }
#endif

    while (i < n) {
        while (i < n && s[i] == '/') {
            ++i;
        }
        if (i >= n) {
            break;
        }
        const std::size_t start = i;
        while (i < n && s[i] != '/') {
            ++i;
        }
        out.segments.emplace_back(s, start, i - start);
    }
    return out;
}

// Rebuilds a path from a root and a segment list.
//   POSIX root "/":  "/" + "a/b"  -> "/a/b"; no segments -> "/".
//   Windows "C:/":  "C:/a/b"; no segments -> "C:/".
//   Windows "C:":   "C:a"; no segments -> "C:" (drive-relative keeps "..").
//   UNC "//s/a":    "//s/a/b"; no segments -> "//s/a".
std::string join(const split_path& sp) {
    if (sp.segments.empty()) {
        if (sp.root.empty()) {
            return std::string();
        }
        if (sp.root.size() >= 2 && sp.root.back() == ':') {
            // Bare "C:" drive-relative root.
            return sp.root;
        }
        return sp.root;
    }
    std::string out = sp.root;
    if (sp.root.empty()) {
        out = sp.segments[0];
        for (std::size_t i = 1; i < sp.segments.size(); ++i) {
            out.push_back('/');
            out += sp.segments[i];
        }
        return out;
    }
    // Root always ends with '/', except bare "C:" which joins without one.
    if (out.back() == '/') {
        for (const std::string& seg : sp.segments) {
            out += seg;
            out.push_back('/');
        }
        out.pop_back();
    } else {
        // "C:" + "a" -> "C:a"
        for (const std::string& seg : sp.segments) {
            out += seg;
            out.push_back('/');
        }
        out.pop_back();
    }
    return out;
}

// Case folding for the host's path comparison policy.
char fold(char c) noexcept {
#if defined(CRPC_OS_WINDOWS)
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#else
    return c;
#endif
}

bool folded_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (fold(a[i]) != fold(b[i])) {
            return false;
        }
    }
    return true;
}

#if defined(CRPC_OS_WINDOWS)
// Only the Windows drive/share comparison folds case; POSIX paths are
// case-sensitive by definition.
std::string folded(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = fold(c);
    }
    return out;
}
#endif

bool insensitive_less(std::string_view a, std::string_view b) noexcept {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        const char ca = fold(a[i]);
        const char cb = fold(b[i]);
        if (ca != cb) {
            return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb);
        }
    }
    return a.size() < b.size();
}

} // namespace

bool path::has_parent() const noexcept {
    return text_.find_first_of("/\\") != std::string::npos;
}

std::string path::generic_string() const { return generic(text_); }

bool path::is_absolute() const {
    const split_path sp = split(text_);
#if defined(CRPC_OS_WINDOWS)
    if (sp.root.size() >= 3 && sp.root.substr(0, 2) == "//") {
        return true; // UNC or extended-length prefix
    }
    if (sp.root.size() >= 3 && sp.root.back() == '/') {
        return is_drive_letter(sp.root[0]) && sp.root[1] == ':'; // "X:/"
    }
    return false;
#else
    return sp.root == "/";
#endif
}

bool path::is_root() const {
    const split_path sp = split(text_);
    if (sp.root.empty()) {
        return false;
    }
    if (!sp.segments.empty()) {
        return false;
    }
    // A bare drive "C:" is drive-relative, not a root.
    if (sp.root.size() == 2 && sp.root.back() == ':') {
        return false;
    }
    return true;
}

path path::parent() const {
    if (text_.empty()) {
        return path();
    }
    if (is_root()) {
        return *this;
    }
    std::string s = generic_string();
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    const split_path sp = split(s);
    const std::string& root = sp.root; // "", "/", "C:/", "//s/a"
    // Drive-relative form ("C:foo") has no separator after its root.
    if (root.size() == 2 && root[1] == ':' && s.find_last_of('/') == std::string::npos) {
        return path(root);
    }
    const std::size_t pos = s.find_last_of('/');
    if (pos == std::string::npos) {
        return path(".");
    }
    if (pos < root.size()) {
        return path(root); // parent of "/a" is "/", of "C:/a" is "C:/"
    }
    return path(s.substr(0, pos));
}

path path::filename() const {
    split_path sp = split(text_);
    if (sp.segments.empty()) {
        return path();
    }
    return path(sp.segments.back());
}

std::string path::stem() const {
    const std::string name = filename().str();
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        return name;
    }
    return name.substr(0, dot);
}

std::string path::extension() const {
    const std::string name = filename().str();
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        return std::string();
    }
    return name.substr(dot);
}

path path::normalized() const {
    split_path sp = split(text_);
    std::vector<std::string> out;
    out.reserve(sp.segments.size());
    for (const std::string& seg : sp.segments) {
        if (seg == ".") {
            continue;
        }
        if (seg == "..") {
            if (!out.empty() && out.back() != "..") {
                out.pop_back();
                continue;
            }
            // A ".." at (or above) an absolute root is dropped -- "../.." from
            // "/" is still "/". It is kept for a relative path and for a
            // drive-relative "C:..", which resolves against the drive's CWD.
            const bool relative_root = sp.root.empty();
            const bool drive_relative_root =
                sp.root.size() == 2 && sp.root[1] == ':';
            if (relative_root || drive_relative_root) {
                out.push_back(seg);
            }
            continue;
        }
        out.push_back(seg);
    }
    split_path resolved = sp;
    resolved.segments = std::move(out);
    return path(join(resolved));
}

result<path> path::absolute() const {
    if (text_.empty()) {
        return rpc_error(rpc_code::invalid_argument,
                         "fs: absolute: an empty path has no absolute form");
    }
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> wide = detail::utf8_to_wide(text_);
    if (!wide) {
        return wide.error();
    }
    const DWORD needed = GetFullPathNameW(wide.value().c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return detail::classify_win32(GetLastError(), "absolute", text_);
    }
    std::wstring buffer(needed, L'\0');
    const DWORD written =
        GetFullPathNameW(wide.value().c_str(), needed, &buffer[0], nullptr);
    if (written == 0) {
        return detail::classify_win32(GetLastError(), "absolute", text_);
    }
    buffer.resize(written);
    return path(detail::wide_to_utf8(buffer));
#else
    if (is_absolute()) {
        return normalized();
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
        return detail::classify_errno(errno, "absolute", text_);
    }
    return (path(cwd) / *this).normalized();
#endif
}

path path::operator/(const path& rhs) const {
    if (rhs.empty()) {
        return *this;
    }
    // A driver or a leading separator resets the base on every platform
    // (posixpath.join semantics): "/b" starts a new path from the root.
    if (rhs.is_absolute() ||
        (!rhs.str().empty() && (rhs.str()[0] == '/' || rhs.str()[0] == '\\'))) {
        return rhs;
    }
    if (empty()) {
        return rhs;
    }
    const std::string& lhs = text_;
    const std::string& r = rhs.text_;
    // A bare drive ("C:") joins without a separator: "C:" + "foo" -> "C:foo".
    if (lhs.size() == 2 && lhs[1] == ':' && has_drive(lhs)) {
        return path(lhs + r);
    }
    std::string out = lhs;
    if (!is_sep(out.back())) {
        out.push_back('/');
    }
    out += r;
    return path(std::move(out));
}

bool operator==(const path& a, const path& b) noexcept {
    return folded_equal(a.generic_string(), b.generic_string());
}

bool operator!=(const path& a, const path& b) noexcept { return !(a == b); }

bool operator<(const path& a, const path& b) noexcept {
    return insensitive_less(a.generic_string(), b.generic_string());
}

bool path_sensitive_equal(const path& a, const path& b) noexcept {
    return a.generic_string() == b.generic_string();
}

result<path> relative(const path& base, const path& target) {
    result<path> ab = base.absolute();
    if (!ab) {
        return ab.error();
    }
    result<path> at = target.absolute();
    if (!at) {
        return at.error();
    }
    split_path b = split(ab.value().str());
    split_path t = split(at.value().str());

#if defined(CRPC_OS_WINDOWS)
    // Different drives / different UNC shares have no relative form. Both
    // roots are "X:/", "//server/share" or "" (relative), so compare folded.
    auto root_key = [](const std::string& r) -> std::string {
        std::string k = folded(r);
        while (!k.empty() && k.back() == '/') {
            k.pop_back();
        }
        return k;
    };
    if (root_key(b.root) != root_key(t.root) ||
        b.root.empty() != t.root.empty()) {
        return rpc_error(rpc_code::invalid_argument,
                         "fs: relative: base and target are on different "
                         "Windows drives or shares");
    }
#endif

    // Common prefix length (case policy matches path::operator==).
    std::size_t common = 0;
    const std::size_t n = b.segments.size() < t.segments.size()
                              ? b.segments.size()
                              : t.segments.size();
    while (common < n && folded_equal(b.segments[common], t.segments[common])) {
        ++common;
    }

    std::string out;
    for (std::size_t i = common; i < b.segments.size(); ++i) {
        out += "..";
        out.push_back('/');
    }
    for (std::size_t i = common; i < t.segments.size(); ++i) {
        out += t.segments[i];
        out.push_back('/');
    }
    if (!out.empty()) {
        out.pop_back();
    }
    if (out.empty()) {
        out = ".";
    }
    return path(std::move(out));
}

} // namespace fs
} // namespace crpc