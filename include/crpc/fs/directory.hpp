// CRPC -- directory operations.
//
// The directory layer follows the same contract as file.hpp: every failure is
// a classified `rpc_error` that preserves the native system code. Directory
// deletion is recursive and idempotent (`remove_all` of a missing directory is
// a successful no-op), matching the file layer's `remove_file`.
//
// `list_directory` sorts entries by name so tests and callers see a
// deterministic order; it never returns "." or "..".
#ifndef CRPC_FS_DIRECTORY_HPP
#define CRPC_FS_DIRECTORY_HPP

#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/fs/path.hpp>

namespace crpc {
namespace fs {

struct CRPC_API directory_entry {
    std::string name;
    path full_path;
    bool is_directory = false;
    u64 size = 0; // file size in bytes; 0 for directories
};

// --- Current / temporary directories ---------------------------------------

CRPC_API result<path> current_directory();
CRPC_API status set_current_directory(const path& p);
CRPC_API result<path> temporary_directory();

// --- Creation ---------------------------------------------------------------

// Creates exactly one directory. `failed_precondition` when it already exists.
CRPC_API status create_directory(const path& p);

// Creates `p` and every missing ancestor. Idempotent when `p` already exists.
CRPC_API status create_directories(const path& p);

// --- Deletion ---------------------------------------------------------------

// Removes an empty directory. `failed_precondition` when it is not empty.
// Idempotent when it is already gone.
CRPC_API status remove_directory(const path& p);

// Recursively removes `p` (either a directory and its contents or a single
// file). Idempotent when `p` is already gone.
CRPC_API status remove_all(const path& p);

// --- Enumeration ------------------------------------------------------------

// Lists one level (or, with `recursive`, the whole subtree). Entries are
// sorted by name and never include "." or "..". A missing directory is
// `not_found`.
CRPC_API result<std::vector<directory_entry>> list_directory(const path& p,
                                                             bool recursive = false);

} // namespace fs
} // namespace crpc

#endif // CRPC_FS_DIRECTORY_HPP