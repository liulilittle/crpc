// CRPC -- unified file / directory / memory I/O framework.
//
// The application framework's file system plumbing: paths, whole-file and
// atomic I/O, directories, RAII temporaries, memory-mapped files and the one
// owned byte buffer. See the per-module headers for the error contract.
#ifndef CRPC_FS_HPP
#define CRPC_FS_HPP

#include <crpc/fs/path.hpp>
#include <crpc/fs/memory.hpp>
#include <crpc/fs/file.hpp>
#include <crpc/fs/directory.hpp>
#include <crpc/fs/temp.hpp>
#include <crpc/fs/mmap.hpp>

#endif // CRPC_FS_HPP