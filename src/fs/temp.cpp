#include <crpc/fs/temp.hpp>
#include <crpc/fs/directory.hpp>
#include <crpc/fs/file.hpp>

#include <utility>

#include "os.hpp"

namespace crpc {
namespace fs {
namespace {

result<path> temp_parent(const path& dir) {
    if (!dir.empty()) {
        return dir;
    }
    return temporary_directory();
}

} // namespace

// --- temp_file --------------------------------------------------------------

temp_file::~temp_file() {
    if (!path_.empty() && !committed_) {
        // Best effort: a destructor cannot surface a failure.
        (void)remove_file(path_);
    }
}

temp_file::temp_file(temp_file&& other) noexcept
    : path_(std::move(other.path_)), committed_(other.committed_) {
    other.path_ = path();
    other.committed_ = false;
}

temp_file& temp_file::operator=(temp_file&& other) noexcept {
    if (this != &other) {
        if (!path_.empty() && !committed_) {
            (void)remove_file(path_);
        }
        path_ = std::move(other.path_);
        committed_ = other.committed_;
        other.path_ = path();
        other.committed_ = false;
    }
    return *this;
}

result<temp_file> temp_file::create(const path& dir, std::string_view prefix) {
    result<path> parent = temp_parent(dir);
    if (!parent) {
        return parent.error();
    }
    // Exclusive-create loop: the name is random, but a collision (or an
    // attacker racing us in the same directory) must not clobber anything.
    for (int attempt = 0; attempt < 16; ++attempt) {
        const path candidate = parent.value() /
                               path(std::string(prefix) + "crpc_tmp_" +
                                    crpc::fs::detail::unique_name("") + ".tmp");
        // create_new_empty_file uses exclusive-create: it never truncates or
        // replaces an existing file, so the loop is race-free.
        status created = detail::create_new_empty_file(candidate);
        if (created) {
            return temp_file(candidate);
        }
        if (created.code() == rpc_code::failed_precondition) {
            continue; // the name raced into existence; try another
        }
        return created.error();
    }
    return rpc_error(rpc_code::resource_exhausted,
                     "fs: temp_file: could not find a free name in '" +
                         parent.value().str() + "'");
}

status temp_file::commit() {
    if (path_.empty()) {
        return rpc_error(rpc_code::failed_precondition,
                         "fs: temp_file::commit: no owned file");
    }
    committed_ = true;
    return status();
}

status temp_file::remove_now() {
    if (path_.empty()) {
        return status(); // nothing owned
    }
    status s = remove_file(path_);
    if (s) {
        path_ = path();
        committed_ = false;
    }
    return s;
}

// --- temp_dir ---------------------------------------------------------------

temp_dir::~temp_dir() {
    if (!path_.empty() && !committed_) {
        (void)remove_all(path_);
    }
}

temp_dir::temp_dir(temp_dir&& other) noexcept
    : path_(std::move(other.path_)), committed_(other.committed_) {
    other.path_ = path();
    other.committed_ = false;
}

temp_dir& temp_dir::operator=(temp_dir&& other) noexcept {
    if (this != &other) {
        if (!path_.empty() && !committed_) {
            (void)remove_all(path_);
        }
        path_ = std::move(other.path_);
        committed_ = other.committed_;
        other.path_ = path();
        other.committed_ = false;
    }
    return *this;
}

result<temp_dir> temp_dir::create(const path& dir, std::string_view prefix) {
    result<path> parent = temp_parent(dir);
    if (!parent) {
        return parent.error();
    }
    for (int attempt = 0; attempt < 16; ++attempt) {
        const path candidate = parent.value() /
                               path(std::string(prefix) + "crpc_tmpdir_" +
                                    crpc::fs::detail::unique_name(""));
        result<bool> exists_now = exists(candidate);
        if (exists_now.ok() && exists_now.value()) {
            continue;
        }
        status made = create_directory(candidate);
        if (made) {
            return temp_dir(candidate);
        }
        if (made.code() == rpc_code::failed_precondition) {
            continue; // raced into existence
        }
        return made.error();
    }
    return rpc_error(rpc_code::resource_exhausted,
                     "fs: temp_dir: could not find a free name in '" +
                         parent.value().str() + "'");
}

status temp_dir::commit() {
    if (path_.empty()) {
        return rpc_error(rpc_code::failed_precondition,
                         "fs: temp_dir::commit: no owned directory");
    }
    committed_ = true;
    return status();
}

status temp_dir::remove_now() {
    if (path_.empty()) {
        return status();
    }
    status s = remove_all(path_);
    if (s) {
        path_ = path();
        committed_ = false;
    }
    return s;
}

} // namespace fs
} // namespace crpc