// CRPC -- RAII temporary files and directories.
//
// A temporary file is created exclusively (never overwrites an existing file),
// in the system temp directory unless the caller supplies one, and is deleted
// by its destructor. `commit()` permanently adopts the file -- the destructor
// then leaves it alone. `remove_now()` deletes immediately. The destructor and
// the move assignment operator of the moved-from object are `noexcept` and
// best-effort: a deletion failure is silently dropped there because a
// destructor cannot report a status. Use `remove_now()` when a silent failure
// would matter.
#ifndef CRPC_FS_TEMP_HPP
#define CRPC_FS_TEMP_HPP

#include <string>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/fs/path.hpp>

namespace crpc {
namespace fs {

class CRPC_API temp_file {
public:
    temp_file() = default;
    ~temp_file(); // deletes the owned file unless `commit()` was called

    temp_file(const temp_file&) = delete;
    temp_file& operator=(const temp_file&) = delete;
    temp_file(temp_file&& other) noexcept;
    temp_file& operator=(temp_file&& other) noexcept;

    // Creates an empty temporary file inside `dir` (default: the system temp
    // directory). An empty `prefix` is allowed and produces a random name.
    static result<temp_file> create(const path& dir = path(),
                                    std::string_view prefix = {});

    const path& native() const noexcept { return path_; }
    bool valid() const noexcept { return !path_.empty(); }
    bool committed() const noexcept { return committed_; }

    // Adopts the file: the destructor will not delete it.
    status commit();

    // Deletes the file now and detaches it.
    status remove_now();

private:
    explicit temp_file(path p) : path_(std::move(p)) {}
    path path_;
    bool committed_ = false;
};

class CRPC_API temp_dir {
public:
    temp_dir() = default;
    ~temp_dir(); // recursively deletes unless `commit()` was called

    temp_dir(const temp_dir&) = delete;
    temp_dir& operator=(const temp_dir&) = delete;
    temp_dir(temp_dir&& other) noexcept;
    temp_dir& operator=(temp_dir&& other) noexcept;

    static result<temp_dir> create(const path& dir = path(),
                                   std::string_view prefix = {});

    const path& native() const noexcept { return path_; }
    bool valid() const noexcept { return !path_.empty(); }
    bool committed() const noexcept { return committed_; }

    status commit();
    status remove_now();

private:
    explicit temp_dir(path p) : path_(std::move(p)) {}
    path path_;
    bool committed_ = false;
};

} // namespace fs
} // namespace crpc

#endif // CRPC_FS_TEMP_HPP