// CRPC -- G19 step3: Raft persistent state (term / votedFor).
//
// The layout is documented in raft_state.hpp and is a contract: 12 bytes of
// header followed by exactly `voted_for_len` bytes. Both directions are
// fail-closed -- `load` refuses anything that is not exactly that shape rather
// than reading it as a clean zero, because a reset term is the double-vote the
// persistence exists to prevent.
#include <crpc/cluster/raft_state.hpp>

#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

#include <crpc/endian.hpp>
#include <crpc/fs/file.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace cluster {

namespace {

// A node id longer than this is not a legitimate vote. The cap bounds what a
// hostile or corrupted file can make the reader allocate before it validates.
constexpr u32 kMaxVotedFor = 1024;
constexpr std::size_t kHeaderSize = sizeof(u64) + sizeof(u32);

} // namespace

fs::path raft_state_store::file_path(const fs::path& state_dir) {
    return state_dir / "registry.raftstate";
}

status raft_state_store::load(const fs::path& state_dir,
                              raft_persistent_state& out) {
    const fs::path path = file_path(state_dir);
    const auto bytes = fs::read_all_bytes(path);
    if (!bytes.ok()) {
        // An absent file is a clean "nothing persisted yet"; anything else is
        // an error the caller must see.
        if (bytes.error().code == rpc_code::not_found) {
            out = raft_persistent_state{};
            return status{};
        }
        // Fail-closed: `out` is left exactly as the caller passed it. A caller
        // that ignores the error code then sees its own sentinel, not a
        // plausible "term 0, no vote" that would look like a clean start and
        // let the node vote twice in a term it has already voted in.
        return bytes.error();
    }
    const auto& data = bytes.value();
    const auto need = [&](std::size_t n) { return n <= data.size(); };

    if (!need(kHeaderSize)) {
        return status(rpc_code::failed_precondition,
                      "raft state file is truncated (header)");
    }
    std::size_t pos = 0;
    raft_persistent_state parsed;
    parsed.term = endian::load_le64(data.data() + pos);
    pos += sizeof(u64);
    const u32 voted_for_len = endian::load_le32(data.data() + pos);
    pos += sizeof(u32);

    // The cap is checked before the length is trusted: a corrupt file must not
    // be able to make the reader walk off the buffer or allocate to match.
    if (voted_for_len > kMaxVotedFor) {
        return status(rpc_code::failed_precondition,
                      "raft state file has an invalid voted_for length");
    }
    if (!need(static_cast<std::size_t>(voted_for_len))) {
        return status(rpc_code::failed_precondition,
                      "raft state file is truncated (voted_for)");
    }
    parsed.voted_for.assign(reinterpret_cast<const char*>(data.data() + pos),
                            voted_for_len);
    pos += voted_for_len;

    // Trailing bytes mean the file is not the shape this reader writes. Reading
    // it as valid would hide a partial overwrite or a longer newer layout.
    if (pos != data.size()) {
        return status(rpc_code::failed_precondition,
                      "raft state file has trailing bytes");
    }
    out = std::move(parsed);
    return status{};
}

status raft_state_store::save(const fs::path& state_dir,
                              const raft_persistent_state& value) {
    if (value.voted_for.size() > kMaxVotedFor) {
        return status(rpc_code::invalid_argument,
                      "raft state: voted_for is too long");
    }
    std::vector<std::byte> body;
    body.reserve(kHeaderSize + value.voted_for.size());
    const auto put_u32 = [&body](u32 v) {
        std::byte bytes[sizeof(u32)];
        endian::store_le32(bytes, v);
        body.insert(body.end(), bytes, bytes + sizeof(bytes));
    };
    const auto put_u64 = [&body](u64 v) {
        std::byte bytes[sizeof(u64)];
        endian::store_le64(bytes, v);
        body.insert(body.end(), bytes, bytes + sizeof(bytes));
    };

    put_u64(value.term);
    put_u32(static_cast<u32>(value.voted_for.size()));
    body.insert(body.end(),
                reinterpret_cast<const std::byte*>(value.voted_for.data()),
                reinterpret_cast<const std::byte*>(value.voted_for.data()) +
                    value.voted_for.size());

    // Staged to a temp file, flushed, then renamed over the target: a reader
    // sees either the previous value or the new one, never a partial write.
    return fs::write_all_bytes_atomic(file_path(state_dir),
                                      span<const std::byte>(body.data(), body.size()));
}

} // namespace cluster
} // namespace crpc
