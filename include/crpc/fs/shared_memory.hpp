// CRPC -- shared memory between processes on one machine.
//
// A named mapping is what two local processes use to hand each other a buffer
// without a socket in between: a frame queue for a renderer, a metrics page for a
// supervisor, a staging area for a payload whose copy over TCP would dominate. The
// name is the rendezvous, so the name rules are part of the contract.
//
// Two platform differences this API does not hide
// -----------------------------------------------
// 1. Length. Windows rounds a mapping up to the page size and has no supported way
//    to ask for the requested length afterwards, while POSIX reports the exact
//    ftruncate size. Rather than report "the page size" on one platform and "your
//    request" on the other, the mapping carries a small header holding the length
//    the creator asked for, and `bytes()` starts after it. A peer therefore always
//    gets the creator's number.
//
// 2. Removal. POSIX `shm_unlink` takes the name out of the namespace, so no new
//    process can attach while existing mappings keep working -- the
//    unlink-after-open pattern. Windows has no equivalent: a named object lives
//    while any handle is open and vanishes when the last one closes, so `remove`
//    cannot stop a third process from attaching. `remove_unlinks_the_name()`
//    reports which behaviour the running platform has; code that depends on the
//    POSIX guarantee must check it instead of assuming, and the test asserts
//    accordingly rather than pretending the two agree.
//
// The view shape mirrors `memory_mapped_file` on purpose: same `bytes()` accessor,
// so code that already handles a mapped file needs no second mental model.
#ifndef CRPC_FS_SHARED_MEMORY_HPP
#define CRPC_FS_SHARED_MEMORY_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace fs {

// How a mapping is obtained.
enum class shared_memory_mode {
    // Create it; `failed_precondition` when the name already exists. This is the
    // race-free form: exactly one creator wins, everyone else is told no.
    create,
    // Open an existing mapping; `not_found` when nobody created it. `size` is
    // ignored -- the creator's length wins.
    open_existing,
    // Create when absent, open when present. `size` is honoured only on creation,
    // so a caller that asks for 4096 and finds an existing 256-byte mapping gets
    // 256. The size belongs to the name's state, not to the request.
    open_or_create
};

// Rules for a shared-memory name, checked before the OS sees it. Kept callable
// because a caller building a name from external input must validate it the same
// way the implementation does.
//
//   * 1..96 bytes. Windows object names allow far more, but a POSIX name has to fit
//     in `/dev/shm/<name>` and 96 is safely below both limits;
//   * letters, digits, '.', '_', '-' only. No separator: the two platforms map the
//     name into different namespaces, so a `/` would mean one thing here and another
//     there;
//   * no leading '.', which also excludes "." and "..".
[[nodiscard]] CRPC_API status validate_shared_name(std::string_view name);

// True when `remove` takes the name out of the namespace, so a later open fails
// (POSIX). False when a named object persists while any handle is open and cannot be
// unlinked by name (Windows). Measured on the running host, not compiled in.
[[nodiscard]] CRPC_API bool remove_unlinks_the_name() noexcept;

// One process's view of a named mapping.
class CRPC_API shared_memory {
public:
    // Declared, not defaulted inline: `impl` is still incomplete here, and an
    // inline default constructor would force unique_ptr<impl>'s deleter to be
    // instantiated inside the class definition.
    shared_memory();
    ~shared_memory();

    shared_memory(const shared_memory&) = delete;
    shared_memory& operator=(const shared_memory&) = delete;
    shared_memory(shared_memory&& other) noexcept;
    shared_memory& operator=(shared_memory&& other) noexcept;

    // Obtains `name`. `size` is the byte length to create with, and must be greater
    // than zero when creating. `bytes().size()` is the creator's length, which on
    // `open_existing` / `open_or_create` may differ from `size`.
    [[nodiscard]] static result<shared_memory> open(std::string_view name, u64 size,
                                                    shared_memory_mode mode);

    // Releases the name. On POSIX this is `shm_unlink`; on Windows it reports
    // whether the name existed and does not prevent further attaches (see
    // `remove_unlinks_the_name()`). `not_found` when no such mapping exists, so a
    // caller can tell "I removed it" from "it was already gone".
    [[nodiscard]] static status remove(std::string_view name);

    // True when a mapping with this name currently exists.
    [[nodiscard]] static result<bool> exists(std::string_view name);

    // The creator's byte length, starting after the length header this class keeps.
    [[nodiscard]] span<std::byte> bytes() const noexcept {
        return span<std::byte>(base_, static_cast<std::size_t>(size_));
    }
    [[nodiscard]] u64 size() const noexcept { return size_; }
    [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    struct impl;

    // Defined in the implementation after impl is complete: a destructor cannot
    // instantiate unique_ptr<impl>'s deleter while the type is still incomplete,
    // and a free function cannot name a private nested type.
    static void release(std::byte* base, impl* state) noexcept;

    // The header layout lives in the implementation; the header only needs to know
    // it exists so the pointer arithmetic stays in one place.
    std::unique_ptr<impl> impl_;
    std::byte* base_ = nullptr;
    u64 size_ = 0;
    std::string name_;
};

} // namespace fs
} // namespace crpc

#endif // CRPC_FS_SHARED_MEMORY_HPP
