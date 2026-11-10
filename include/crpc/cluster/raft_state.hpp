// CRPC -- G19 step3: Raft persistent state (term / votedFor).
//
// Raft requires exactly two values to survive a restart: the current term and
// the candidate voted for in that term. Everything else -- the log, the commit
// index, the role -- is either stored in the log or derivable from it. These
// two are not: a node that forgets its term can vote twice in the same term,
// and a node that forgets its vote can help elect two leaders in the same
// term. Either failure breaks the single-leader guarantee, and both are silent.
//
// File layout (the format is a contract; a reader must be able to rely on it):
//
//   u64 term            little-endian
//   u32 voted_for_len   little-endian, number of bytes that follow
//   u8[voted_for_len]   the node id voted for; empty means no vote cast
//
// Total length is exactly 12 + voted_for_len. There is no version field: like
// the anchor file, a same-length layout change would be misparsed rather than
// refused, so a layout change must bump the filename in file_path().
//
// Fail-closed: `load` returns an error for anything that is not exactly this
// shape -- a short file, a length past the cap, or trailing bytes. A corrupt
// file must never be read as "term 0, no vote", because resetting the term is
// precisely the double-vote this persistence exists to prevent.
#ifndef CRPC_CLUSTER_RAFT_STATE_HPP
#define CRPC_CLUSTER_RAFT_STATE_HPP

#include <cstdint>
#include <string>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/fs/path.hpp>

namespace crpc {
namespace cluster {

// The two fields Raft requires to survive a restart.
struct raft_persistent_state {
    u64 term = 0;
    // Empty means no vote was cast in this term.
    std::string voted_for;
};

// Persists `raft_persistent_state` beside the registry log.
class CRPC_API raft_state_store {
public:
    // "<state_dir>/registry.raftstate"
    [[nodiscard]] static fs::path file_path(const fs::path& state_dir);

    // An absent file is a clean "nothing persisted yet" (term 0, no vote).
    // Anything else -- wrong size, truncated, a length past the cap, trailing
    // bytes -- is an error, never a silent reset.
    [[nodiscard]] static status load(const fs::path& state_dir,
                                     raft_persistent_state& out);

    // Atomic: the bytes are staged to a temp file, flushed, and renamed over
    // the target, so a reader sees either the previous value or the new one.
    [[nodiscard]] static status save(const fs::path& state_dir,
                                     const raft_persistent_state& value);
};

} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_RAFT_STATE_HPP
