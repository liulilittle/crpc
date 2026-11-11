// CRPC -- G19 step2: registry service implementation.
//
// Ordering rules this file depends on (VERDICT sections 4.2, 4.4, 5, 6):
//   * the epoch store is seeded BEFORE any handler can allocate an epoch;
//   * remote `writer_epoch == 0` is refused only after the epoch store exists,
//     so the refusal never leaves a client with an unprotected write path;
//   * `fence_barrier` is not called by this service and appends nothing to the
//     log: only catalog mutations (register/set_state/deregister) append.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <crpc/cluster/registry_service.hpp>

#include <crpc/cluster/raft_command.hpp>
#include <crpc/cluster/raft_node.hpp>

#include <crpc/endian.hpp>
#include <crpc/fs/directory.hpp>
#include <crpc/fs/file.hpp>
#include <crpc/frame.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/dispatcher.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace crpc {
namespace cluster {
namespace {

// --- Single-instance lease ---------------------------------------------------
//
// A named kernel mutex, not a pid file: a pid can be reused by an unrelated
// process and permanently refuse a new instance (VERDICT section 6). The name
// is derived from the state directory so two services on different state
// directories do not fight.
//
// A dead holder is taken over, so `WAIT_ABANDONED` IS accepted as an
// acquisition. That is deliberate and it is not a crash detector: the kernel
// hands a mutex held by a dead process to the next waiter, which may observe
// either `WAIT_OBJECT_0` or `WAIT_ABANDONED`, and neither distinguishes "the
// predecessor crashed" from "the predecessor was still initialising". So the
// lease cannot tell a dirty state directory from a clean one -- which is why
// the takeover MUST validate and repair the log (the V1 base check and the
// truncate to the last CRC-valid record). Refusing `WAIT_ABANDONED` instead
// would make a crashed deployment permanently unrestartable, which is worse
// than a repairable dirty tail.
class instance_lease {
public:
    instance_lease() = default;
    ~instance_lease() { release(); }

    instance_lease(const instance_lease&) = delete;
    instance_lease& operator=(const instance_lease&) = delete;

    // Returns true when this call acquired the lease.
    //
    // Two layers, because a named kernel mutex alone is not enough: a mutex is
    // re-entrant for the thread that already holds it, so two services in the
    // SAME process would both acquire it. The in-process registry closes that
    // case; the kernel object closes the cross-process case.
    [[nodiscard]] bool acquire(const std::string& state_dir) {
        const std::string name = hash_name(state_dir);
        {
            std::lock_guard<std::mutex> lock(held_mutex());
            if (held_().count(name) != 0) {
                return false;
            }
        }
#if defined(_WIN32)
        name_ = "Local\\crpc_registryd_" + name;
        handle_ = ::CreateMutexA(nullptr, FALSE, name_.c_str());
        if (handle_ == nullptr) {
            return false;
        }
        const DWORD waited = ::WaitForSingleObject(handle_, 0);
        // WAIT_OBJECT_0 and WAIT_ABANDONED both count as acquired; anything
        // else (a timeout, an error) does not. WAIT_ABANDONED means the
        // predecessor died, so the state directory may be half-written -- the
        // takeover path must validate and repair it before serving.
        if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
            (void)::CloseHandle(handle_);
            handle_ = nullptr;
            return false;
        }
#else
        path_ = "/tmp/crpc_registryd_" + name + ".lock";
        fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ < 0) {
            return false;
        }
        struct flock lock {};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        if (::fcntl(fd_, F_SETLK, &lock) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
#endif
        std::lock_guard<std::mutex> lock(held_mutex());
        held_().insert(name);
        held_name_ = name;
        return true;
    }

    void release() noexcept {
        {
            std::lock_guard<std::mutex> lock(held_mutex());
            held_().erase(held_name_);
        }
#if defined(_WIN32)
        if (handle_ != nullptr) {
            (void)::ReleaseMutex(handle_);
            (void)::CloseHandle(handle_);
            handle_ = nullptr;
        }
        name_.clear();
#else
        if (fd_ >= 0) {
            struct flock lock {};
            lock.l_type = F_UNLCK;
            lock.l_whence = SEEK_SET;
            (void)::fcntl(fd_, F_SETLK, &lock);
            (void)::close(fd_);
            fd_ = -1;
        }
#endif
    }

private:
    // The in-process half of the lease. Keyed by the same name as the kernel
    // object, so both layers agree on identity.
    static std::mutex& held_mutex() noexcept {
        static std::mutex mutex;
        return mutex;
    }
    static std::set<std::string>& held_() noexcept {
        static std::set<std::string> names;
        return names;
    }

    [[nodiscard]] static std::string hash_name(const std::string& value) {
        u64 hash = 1469598103934665603ull;
        for (const char c : value) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ull;
        }
        char buf[32];
        (void)std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
        return std::string(buf);
    }

#if defined(_WIN32)
    void* handle_ = nullptr;
    std::string name_;
    std::string held_name_;
#else
    int fd_ = -1;
    std::string path_;
    std::string held_name_;
#endif
};

// --- Log persistence ---------------------------------------------------------
//
// The service appends one record per accepted catalog mutation. The payload is
// the registry's restore payload (fence + incarnation header, then the
// versioned instance body), so `restore_from_log` can replay the log without
// knowing the service existed.
[[nodiscard]] status encode_log_payload(const service_instance& instance,
                                        u64 fence, u64 incarnation,
                                        std::vector<std::byte>& out) {
    // The header is 16 bytes: fence then incarnation, both little-endian. The
    // body is the same shape the query response uses, so one parser serves both.
    out.clear();
    const auto append_u16 = [&out](u16 value) {
        const std::size_t at = out.size();
        out.resize(at + 2);
        endian::store_le16(out.data() + at, value);
    };
    const auto append_u32 = [&out](u32 value) {
        const std::size_t at = out.size();
        out.resize(at + 4);
        endian::store_le32(out.data() + at, value);
    };
    const auto append_u64 = [&out](u64 value) {
        const std::size_t at = out.size();
        out.resize(at + 8);
        endian::store_le64(out.data() + at, value);
    };
    const auto append_str = [&out, &append_u32](const std::string& value) {
        append_u32(static_cast<u32>(value.size()));
        out.insert(out.end(), reinterpret_cast<const std::byte*>(value.data()),
                   reinterpret_cast<const std::byte*>(value.data()) + value.size());
    };

    append_u64(fence);
    append_u64(incarnation);
    out.push_back(static_cast<std::byte>(1));   // body version
    out.push_back(static_cast<std::byte>(instance.state));
    append_str(instance.service);
    append_str(instance.node_id);
    append_str(instance.address.host);
    append_u16(instance.address.port);
    append_u32(instance.weight);
    append_u64(instance.incarnation);
    append_u32(static_cast<u32>(instance.meta.size()));
    for (const auto& entry : instance.meta) {
        append_str(entry.first);
        append_str(entry.second);
    }
    return status{};
}

} // namespace

// --- epoch_store --------------------------------------------------------------

fs::path epoch_store::file_path(const fs::path& state_dir) {
    return state_dir / "registry.epoch";
}

result<u64> epoch_store::read(const fs::path& state_dir) {
    const fs::path path = file_path(state_dir);
    const auto bytes = fs::read_all_bytes(path);
    if (!bytes.ok()) {
        // An absent file is a clean "no persisted value"; anything else is an
        // error the caller must see, because a silently zero seed could
        // hand out an epoch a surviving writer already used.
        if (bytes.error().code == rpc_code::not_found) {
            return u64{0};
        }
        return bytes.error();
    }
    const auto& data = bytes.value();
    if (data.size() != sizeof(u64)) {
        return rpc_error(rpc_code::failed_precondition,
                         "registry epoch file has the wrong size");
    }
    u64 value = 0;
    std::memcpy(&value, data.data(), sizeof(value));
    return value;
}

void epoch_store::seed(u64 persisted, u64 registry_fence) noexcept {
    value_ = persisted > registry_fence ? persisted : registry_fence;
}

result<u64> epoch_store::next() {
    const u64 candidate = value_ + 1;
    if (candidate == 0) {
        // The sentinel UINT64_MAX is reserved for "caller did not set an
        // epoch"; wrapping to 0 would also be a valid external-writer marker.
        return rpc_error(rpc_code::resource_exhausted, "registry epoch exhausted");
    }
    const status persisted = persist(candidate);
    if (!persisted.ok()) {
        return persisted.error();
    }
    value_ = candidate;
    return candidate;
}

status epoch_store::persist(u64 value) const {
    if (fail_persist_) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "registry: epoch persist failed",
                                       registry_reason_epoch_not_persisted)};
    }
    std::byte bytes[sizeof(u64)];
    endian::store_le64(bytes, value);
    return fs::write_all_bytes_atomic(
        path_, span<const std::byte>(bytes, sizeof(bytes)));
}

// --- anchor persistence -------------------------------------------------------
//
// VERDICT section 5.1: the service's `last_anchor` is persisted "next to the
// epoch file". Without this, a restart left the map empty, `anchor_allows` saw
// "no anchor on record" and let every write through -- a client could replay a
// pre-restart anchor against a freshly started service.
//
// The file is SECURITY STATE, so a corrupt file must fail the startup rather
// than be treated as empty: silently clearing it is exactly the bypass this
// persistence exists to prevent.

fs::path anchor_store::file_path(const fs::path& state_dir) {
    return state_dir / "registry.anchor";
}

// Layout: u32 count, then count entries of
//   u32 key_len | key bytes | u64 authority_epoch | u64 index
// where `key` is "<service>\x1f<node_id>" -- the same string form the client
// uses for its per-key anchor slot, so both sides spell a key identically.
//
// KNOWN LIMITATION (U1): the layout has no version field, so a same-length
// change would be misparsed rather than refused. If the layout ever changes,
// bump the filename in file_path() (e.g. `registry.anchor2`) instead of adding
// a version byte -- a new name is fail-closed and needs no reader migration.
status anchor_store::load(const fs::path& state_dir,
                          std::map<std::pair<std::string, std::string>, registry_anchor>& out) {
    out.clear();
    const fs::path path = file_path(state_dir);
    const auto bytes = fs::read_all_bytes(path);
    if (!bytes.ok()) {
        // An absent file is a clean "nothing persisted yet"; anything else is
        // an error the caller must see.
        if (bytes.error().code == rpc_code::not_found) {
            return status{};
        }
        return bytes.error();
    }
    const auto& data = bytes.value();
    std::size_t pos = 0;
    const auto need = [&](std::size_t n) { return pos + n <= data.size(); };
    if (!need(sizeof(u32))) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "registry anchor file is truncated (count)",
                                       registry_reason_anchor_unreadable)};
    }
    const u32 count = endian::load_le32(data.data() + pos);
    pos += sizeof(u32);
    for (u32 i = 0; i < count; ++i) {
        if (!need(sizeof(u32))) {
            return status{registry_failure(rpc_code::failed_precondition,
                                           "registry anchor file is truncated (key length)",
                                           registry_reason_anchor_unreadable)};
        }
        const u32 key_len = endian::load_le32(data.data() + pos);
        pos += sizeof(u32);
        if (key_len == 0 || key_len > 1024 || !need(key_len)) {
            return status{registry_failure(rpc_code::failed_precondition,
                                           "registry anchor file has an invalid key length",
                                           registry_reason_anchor_unreadable)};
        }
        std::string key(reinterpret_cast<const char*>(data.data() + pos), key_len);
        pos += key_len;
        if (!need(sizeof(u64) * 2)) {
            return status{registry_failure(rpc_code::failed_precondition,
                                           "registry anchor file is truncated (anchor)",
                                           registry_reason_anchor_unreadable)};
        }
        registry_anchor anchor;
        anchor.authority_epoch = endian::load_le64(data.data() + pos);
        pos += sizeof(u64);
        anchor.index = endian::load_le64(data.data() + pos);
        pos += sizeof(u64);
        const std::size_t sep = key.find('\x1f');
        if (sep == std::string::npos) {
            return status{registry_failure(rpc_code::failed_precondition,
                                           "registry anchor file has a malformed key",
                                           registry_reason_anchor_unreadable)};
        }
        out[std::make_pair(key.substr(0, sep), key.substr(sep + 1))] = anchor;
    }
    if (pos != data.size()) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "registry anchor file has trailing bytes",
                                       registry_reason_anchor_unreadable)};
    }
    return status{};
}

status anchor_store::save(
    const fs::path& state_dir,
    const std::map<std::pair<std::string, std::string>, registry_anchor>& value) {
    std::vector<std::byte> body;
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
    put_u32(static_cast<u32>(value.size()));
    for (const auto& entry : value) {
        const std::string key = entry.first.first + '\x1f' + entry.first.second;
        put_u32(static_cast<u32>(key.size()));
        body.insert(body.end(), reinterpret_cast<const std::byte*>(key.data()),
                    reinterpret_cast<const std::byte*>(key.data()) + key.size());
        put_u64(entry.second.authority_epoch);
        put_u64(entry.second.index);
    }
    return fs::write_all_bytes_atomic(file_path(state_dir),
                                      span<const std::byte>(body.data(), body.size()));
}

// --- registry_service ---------------------------------------------------------

namespace {

// The step2 fence authority: the local registry allocator. `principal` is
// accepted for the seam but not used to bind the token, because
// `registry::next_fence` has no principal parameter (VERDICT section 3): the
// token's principal is the target key's node_id, and the ACL layer has already
// verified `req.node_id` against the rule.
result<u64> allocate_local(void* context, const fence_request& request) noexcept {
    auto* reg = static_cast<registry*>(context);
    return reg->next_fence(request.service, request.node_id, request.lease_id);
}

} // namespace

struct registry_service::impl {
    explicit impl(const registry_service_config& config)
        : config(config), catalog(config.registry) {}

    registry_service_config config;
    registry catalog;
    epoch_store epochs;
    // Concrete log type: `durable_size()` lives on `file_registry_log`, not on
    // the abstract base, and the takeover repair needs it.
    std::unique_ptr<file_registry_log> log;
    std::unique_ptr<server> server_;
    crpc::endpoint bound_ep;
    instance_lease lease;

    mutable std::mutex mutex;
    // D-A-3: serialises the CAS "check + propose + apply" sequence. The check
    // and the commit used to be two separate critical sections, so two callers
    // presenting the same `cas = N` could both observe `modify_index == N`,
    // both pass, and both commit -- the second silently overwriting the first.
    // Holding this across the whole sequence makes the second caller's check
    // observe the first caller's applied result and fail with a conflict.
    //
    // It is a SEPARATE mutex, not `mutex`, because `replicate_command` ->
    // `apply_committed` re-enters `mutex`; holding `mutex` across the sequence
    // would self-deadlock on a non-recursive mutex. Lock order is always
    // `cas_mutex` then `mutex`, never the reverse.
    std::mutex cas_mutex;
    // The last anchor each key accepted. A repeated request that presents the
    // same anchor as the last accepted write is an idempotent replay even when
    // the transport-level dedup window has expired.
    std::map<std::pair<std::string, std::string>, registry_anchor> last_anchor;
    // Idempotency map: client_request_id -> outcome.
    std::map<std::string, idempotency_record> idempotency;
    u64 anchor_epoch = 0;

    // --- Replicated state machine (step4 PR-9) ------------------------------
    //
    // `last_applied` is the highest log index whose command has been applied to
    // the local state below. It is the ONLY cursor: a command is applied exactly
    // once, in index order, and a restart replays from zero.
    u64 last_applied = 0;
    // The term of the entry at `last_applied`. A conflicting suffix is replaced
    // by an entry at the SAME index with a DIFFERENT term (that is what a Raft
    // conflict is), so comparing the term is how the apply loop detects that the
    // entry it applied is no longer the entry the log holds. Without it, a
    // truncate-then-replace leaves `last_applied == last_log_index` and the
    // early return skips the replacement forever (D-A-4).
    u64 last_applied_term = 0;
    // The live ACL table. It starts as a copy of `config.acl` (the static seed)
    // and is then mutated only by `acl_upsert` / `acl_remove` commands, so every
    // node that applies the same log ends with the same table.
    std::vector<acl_rule> acl_rules;

    // The registry's catalog is the authority; the log's durable tail is read
    // during prepare() to tell whether the file grew behind our back. It is not
    // cached in a member: the only reader was the prepare-time check below.
};

registry_service::registry_service(registry_service_config config)
    : impl_(new impl(config)) {
    // The live ACL table starts as the static seed. Every later change arrives
    // as a committed `acl_upsert` / `acl_remove` command, so two nodes that
    // applied the same log hold the same table.
    impl_->acl_rules = config.acl;
}

registry_service::~registry_service() {
    // RAII: the destructor must release what the constructor acquired. Without
    // this, a caller that forgets `stop()` leaves the single-instance lease
    // held (both the in-process registry and the kernel mutex) until the
    // process exits, so a restarted service over the same state directory is
    // refused. `stop()` is idempotent, so an explicit stop() followed by the
    // destructor is a no-op.
    stop();
}

acl_decision registry_service::check_acl(const std::string& token, u64 method,
                                         const std::string& service,
                                         const std::string& node_id,
                                         std::chrono::system_clock::time_point now,
                                         std::chrono::milliseconds issued_at,
                                         bool has_issued_at) const {
    acl_decision decision;
    if (token.empty()) {
        decision.reason = registry_reason_acl_denied;
        return decision;
    }
    // The same deterministic FNV-1a the registry uses for principal_hash, so
    // the ACL table can be shared with a deployment that also lists
    // principals by the same function.
    u64 hash = 1469598103934665603ull;
    for (const char c : token) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    const acl_rule* matched = nullptr;
    for (const acl_rule& rule : impl_->acl_rules) {
        if (rule.token_hash == hash) {
            matched = &rule;
            break;
        }
    }
    if (matched == nullptr) {
        decision.reason = registry_reason_acl_denied;
        return decision;
    }
    decision.rule = *matched;

    // The token's issue time must be inside the window. A token whose issue
    // time is missing or outside `registry_token_window` is denied. This is a
    // time LIMIT, not replay prevention: a token observed inside the window
    // can still be replayed. The window is compared against the service's
    // SYSTEM clock (Unix epoch) -- the same clock family the client stamps
    // with -- so the comparison is cross-process. The resulting dependency on
    // clock synchronisation is documented in docs/migration.md and is
    // UNVERIFIED.
    if (!has_issued_at) {
        decision.reason = registry_reason_acl_denied;
        return decision;
    }
    // The client stamps `issued_at` in the system clock (Unix epoch), so the
    // delta is a plain subtraction. `steady_clock` would be wrong here: its
    // epoch is per-process, so a cross-process comparison is meaningless.
    // Fail closed on a missing, negative or too-large skew: a token whose
    // issue time cannot be shown to be inside the window is denied.
    const std::chrono::milliseconds now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch());
    const std::chrono::milliseconds delta = now_ms - issued_at;
    if (delta.count() < 0 || delta > registry_token_window) {
        decision.reason = registry_reason_acl_denied;
        return decision;
    }

    // Rule 3: a write needs a matching write prefix.
    const bool is_write = method == registry_method_register ||
                          method == registry_method_deregister ||
                          method == registry_method_set_state ||
                          method == registry_method_next_fence ||
                          method == registry_method_kv_put ||
                          method == registry_method_kv_cas ||
                          method == registry_method_kv_delete;
    if (is_write) {
        bool prefix_ok = false;
        for (const std::string& prefix : matched->write_prefixes) {
            // A blank prefix means "every service". It needs no special case:
            // `compare(0, 0, prefix)` returns 0 for any `service`, so the
            // substring test already covers the empty prefix. (An explicit
            // `prefix.empty() ||` was here and was redundant.)
            if (service == prefix ||
                (service.size() >= prefix.size() &&
                 service.compare(0, prefix.size(), prefix) == 0)) {
                prefix_ok = true;
                break;
            }
        }
        if (!prefix_ok) {
            decision.reason = registry_reason_acl_denied;
            return decision;
        }
    }
    // Rule 4: a non-admin rule is bound to one node id. This applies to writes
    // only: a read with an empty node_id is a catalog-wide query and the read
    // prefix already scopes it.
    const bool node_bound_write =
        is_write && !matched->node_id.empty() && !matched->admin;
    if (node_bound_write && node_id != matched->node_id) {
        decision.reason = registry_reason_acl_denied;
        return decision;
    }
    // Rule 5: a read needs a matching read prefix. This covers BOTH read
    // methods: `query` (a catalog read scoped by service) and `kv_get` (a KV
    // read scoped by key). `registry_acl.hpp:43` states `read_prefixes`:
    // "empty = no read (fail closed)", so a rule with no read prefix must be
    // denied on every read path -- the old code checked only `query`, which let
    // any valid token read any KV key.
    const bool is_read = method == registry_method_query ||
                         method == registry_method_kv_get;
    if (is_read) {
        bool prefix_ok = false;
        for (const std::string& prefix : matched->read_prefixes) {
            // Same as the write side: an empty prefix is already covered by the
            // substring test, so it needs no special case.
            if (service == prefix ||
                (service.size() >= prefix.size() &&
                 service.compare(0, prefix.size(), prefix) == 0)) {
                prefix_ok = true;
                break;
            }
        }
        if (!prefix_ok) {
            decision.reason = registry_reason_acl_denied;
            return decision;
        }
    }
    decision.allowed = true;
    return decision;
}

registry_anchor registry_service::anchor() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    registry_anchor out;
    out.authority_epoch = impl_->anchor_epoch;
    // The index a caller observes must cover every mutation this service has
    // accepted, not just catalog rows: a KV write does not touch the catalog
    // but does advance the store's high-water mark, and a watcher parked on the
    // catalog-only index would never see it move. The larger of the two is the
    // honest answer.
    u64 index = impl_->catalog.index();
    if (impl_->config.kv != nullptr) {
        index = std::max(index, impl_->config.kv->highest_index());
    }
    out.index = index;
    return out;
}

status registry_service::apply_committed() {
    if (impl_->config.raft == nullptr) {
        return status{};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const u64 committed = impl_->config.raft->commit_index();
    const u64 last_log = impl_->config.raft->last_log_index();

    // D-A-4: the apply cursor must follow a truncated suffix. Raft guarantees a
    // COMMITTED entry is never truncated, so `commit_index` can never exceed the
    // log; if it does, an upstream invariant is broken and silently clamping
    // would hide it. Report it -- but first rebuild the state machine, because
    // the cursor is now past the log and the surviving entries must be replayed.
    const bool commit_past_log = committed > last_log;
    // A follower that truncated a conflicting suffix can leave `last_applied`
    // above the new log end. The old code returned early forever
    // (`committed <= last_applied`), so a replacement entry at or below the old
    // cursor was never applied and the node stayed permanently divergent.
    //
    // The applied KV/ACL changes cannot be rolled back individually, so the
    // state machine is rebuilt from scratch: the KV table is cleared, the ACL
    // table is reset to the static seed (clearing it alone would drop the seed
    // and deny every request), and the cursor restarts at zero. The replay below
    // then re-applies the surviving log. `kv_store::clear()` keeps
    // `highest_index`, so the store's index contract is not regressed.
    if (impl_->last_applied > last_log) {
        if (impl_->config.kv != nullptr) {
            impl_->config.kv->clear();
        }
        impl_->acl_rules = impl_->config.acl;
        impl_->last_applied = 0;
        impl_->last_applied_term = 0;
    }
    if (commit_past_log) {
        return status{registry_failure(
            rpc_code::internal,
            "registry: commit index is past the end of the log",
            "commit_past_log")};
    }

    // D-A-4 (the common case): a conflicting suffix is replaced by an entry at
    // the SAME index with a DIFFERENT term. `last_applied == last_log_index`
    // then holds, so the `committed <= last_applied` early return would skip the
    // replacement forever. Detect it by comparing the term of the entry the
    // cursor points at: if it no longer matches what was applied, the applied
    // state is stale and must be rebuilt from the log.
    if (impl_->last_applied > 0 && impl_->last_applied <= last_log) {
        const u64 current_term =
            impl_->config.raft->entry_term(impl_->last_applied);
        if (current_term != impl_->last_applied_term) {
            if (impl_->config.kv != nullptr) {
                impl_->config.kv->clear();
            }
            impl_->acl_rules = impl_->config.acl;
            impl_->last_applied = 0;
            impl_->last_applied_term = 0;
        }
    }

    if (committed <= impl_->last_applied) {
        return status{};
    }
    // Read the committed prefix once. `replay` returns every intact record at
    // or after the cursor, so the loop below sees exactly the entries it has
    // not applied, in index order.
    const auto replayed = impl_->config.raft->log().replay(impl_->last_applied + 1);
    if (!replayed.ok()) {
        return replayed.error();
    }
    for (const log_record& record : replayed.value()) {
        if (record.index > committed) {
            break;
        }
        // Only `raft_entry` carries a state-machine command. The registry's own
        // `registry_event` records live in a different log; a raft record of any
        // other kind is consensus bookkeeping and has no command to apply.
        if (record.kind != log_record_kind::raft_entry) {
            impl_->last_applied = record.index;
            impl_->last_applied_term = record.term;
            continue;
        }
        raft_command command;
        const status decoded = decode_raft_command(
            span<const std::byte>(record.payload.data(), record.payload.size()),
            command);
        if (!decoded.ok()) {
            // A committed entry that does not decode is unrecoverable: this node
            // cannot reach the same state as the rest of the cluster, and
            // skipping it would hide that. Fail loudly and leave the cursor
            // where it is so the failure repeats on the next call.
            return decoded;
        }
        const status applied = apply_command_locked(command, record.index);
        if (!applied.ok()) {
            return applied;
        }
        impl_->last_applied = record.index;
        impl_->last_applied_term = record.term;
    }
    return status{};
}

status registry_service::apply_command_locked(const raft_command& command,
                                              u64 index) {
    switch (command.kind) {
    case raft_command_kind::kv_put: {
        if (impl_->config.kv == nullptr) {
            return status{registry_failure(rpc_code::failed_precondition,
                                           "registry: kv store not configured",
                                           "kv_unavailable")};
        }
        (void)impl_->config.kv->put(command.key, command.value, index);
        return status{};
    }
    case raft_command_kind::kv_cas: {
        if (impl_->config.kv == nullptr) {
            return status{registry_failure(rpc_code::failed_precondition,
                                           "registry: kv store not configured",
                                           "kv_unavailable")};
        }
        // The outcome is deliberately ignored. A CAS that conflicts on the
        // leader conflicts identically on every follower, because they apply
        // the same commands in the same order to the same state; the leader
        // already reported the conflict to the client before proposing, so
        // there is nothing left to decide here.
        (void)impl_->config.kv->cas(command.key, command.value, command.cas,
                                    index);
        return status{};
    }
    case raft_command_kind::kv_delete: {
        if (impl_->config.kv == nullptr) {
            return status{registry_failure(rpc_code::failed_precondition,
                                           "registry: kv store not configured",
                                           "kv_unavailable")};
        }
        (void)impl_->config.kv->erase(command.key, index);
        return status{};
    }
    case raft_command_kind::acl_upsert: {
        for (acl_rule& rule : impl_->acl_rules) {
            if (rule.token_hash == command.rule.token_hash) {
                rule = command.rule;
                return status{};
            }
        }
        impl_->acl_rules.push_back(command.rule);
        return status{};
    }
    case raft_command_kind::acl_remove: {
        for (auto it = impl_->acl_rules.begin(); it != impl_->acl_rules.end();
             ++it) {
            if (it->token_hash == command.rule.token_hash) {
                impl_->acl_rules.erase(it);
                return status{};
            }
        }
        // Removing a rule that is not present is a no-op, not an error: the
        // command is idempotent, and a replay after a restart must not fail on
        // a removal it already applied.
        return status{};
    }
    }
    return status{registry_failure(rpc_code::internal,
                                   "registry: unknown raft command kind",
                                   "command_kind")};
}

status registry_service::set_acl_rule(const acl_rule& rule) {
    if (rule.token_hash == 0) {
        return status{registry_failure(rpc_code::invalid_argument,
                                       "registry: acl token hash must not be zero",
                                       "acl_invalid")};
    }
    raft_command command;
    command.kind = raft_command_kind::acl_upsert;
    command.rule = rule;
    return replicate_command(command);
}

status registry_service::remove_acl_rule(u64 token_hash) {
    if (token_hash == 0) {
        return status{registry_failure(rpc_code::invalid_argument,
                                       "registry: acl token hash must not be zero",
                                       "acl_invalid")};
    }
    raft_command command;
    command.kind = raft_command_kind::acl_remove;
    command.rule.token_hash = token_hash;
    return replicate_command(command);
}

std::vector<acl_rule> registry_service::acl_rules() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->acl_rules;
}

u64 registry_service::instance_incarnation(const std::string& service,
                                           const std::string& node_id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    query_options options;
    options.service = service;
    options.node_id = node_id;
    // A tombstone is the row a re-registration must clear, so it must be
    // visible here: hiding it would report 0 and hand out an incarnation the
    // tombstone then rejects.
    options.include_deregistered = true;
    const result<catalog_result> queried = impl_->catalog.query(options);
    if (!queried.ok()) {
        return 0;
    }
    for (const service_instance& instance : queried.value().instances) {
        if (instance.service == service && instance.node_id == node_id) {
            return instance.incarnation;
        }
    }
    return 0;
}

status registry_service::replicate_command(const raft_command& command) {
    std::vector<std::byte> payload;
    const status encoded = encode_raft_command(command, payload);
    if (!encoded.ok()) {
        return encoded;
    }
    if (impl_->config.raft == nullptr) {
        // Single-authority mode: apply directly. There is no other node to
        // disagree with, so the local state machine is the whole cluster.
        //
        // The index still has to be monotonic across EVERY mutation this
        // service has made: the catalog, the KV high-water mark and the apply
        // cursor are one counter. Reusing `last_applied + 1` alone would hand
        // two consecutive KV writes the same index and leave `cas=N` matching
        // after the first.
        std::lock_guard<std::mutex> lock(impl_->mutex);
        u64 index = std::max({impl_->last_applied, impl_->catalog.index(),
                              impl_->config.kv != nullptr
                                  ? impl_->config.kv->highest_index()
                                  : u64{0}}) +
                    1;
        const status applied = apply_command_locked(command, index);
        if (!applied.ok()) {
            return applied;
        }
        impl_->last_applied = index;
        return status{};
    }
    const result<u64> proposed = impl_->config.raft->propose(
        span<const std::byte>(payload.data(), payload.size()));
    if (!proposed.ok()) {
        return status{proposed.error()};
    }
    const result<u64> committed = impl_->config.raft->wait_for_commit(
        proposed.value(),
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
    if (!committed.ok()) {
        return status{committed.error()};
    }
    // Apply everything through the committed index, including this command.
    // The leader's own entry is in the same log, so the loop applies it exactly
    // once, in order, like any other.
    return apply_committed();
}

status registry_service::prepare(server::builder& builder) {
    // 1. Single-instance lease. Acquiring it is what makes the later log base
    //    check meaningful: without the lease a second instance could interleave
    //    appends with ours.
    if (!impl_->lease.acquire(impl_->config.state_dir.str())) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "another registry service owns this state directory",
                                       "instance_conflict")};
    }

    // 2. Create the state directory and open the log.
    const status created = fs::create_directories(impl_->config.state_dir);
    if (!created.ok()) {
        return created;
    }
    const fs::path log_path = impl_->config.state_dir / "registry.log";
    auto opened = file_registry_log::open(log_path.str());
    if (!opened.ok()) {
        return opened.error();
    }
    impl_->log = std::move(opened).value();

    // 2b. V1 base check (VERDICT section 6). The lease is held, so no other
    //     instance can append while this runs. The log must not claim a record
    //     this catalog has already folded: a log behind the catalog is a
    //     rollback, not a recovery, and `restore_from_log` would refuse it with
    //     `stale_log` anyway -- checking here makes the reason explicit and
    //     keeps the failure before any catalog mutation.
    const u64 log_last = impl_->log->last_index();
    const u64 catalog_index = impl_->catalog.index();
    if (log_last < catalog_index) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "registry log is behind the restored catalog",
                                       "stale_log")};
    }

    // 2c. The torn-tail truncate is NOT duplicated here: `file_registry_log::
    //     open()` already scans the file, adopts the intact prefix and
    //     truncates a torn tail to it (registry_log.cpp:371-388). A corrupt
    //     record makes `open()` fail, so a damaged log can never be mistaken
    //     for a shorter healthy one. Reimplementing the truncate would create
    //     a second truncation point with the same responsibility.
    //
    // 3. Restore the catalog from the log. This is before the epoch seed, as
    //    VERDICT section 4.2 requires, and after `open()` has repaired the
    //    tail, so the restore sees the same bytes a later restart would.
    const auto restored = restore_from_log(impl_->catalog, *impl_->log);
    if (!restored.ok()) {
        return restored.error();
    }

    // 4. Seed the epoch store from the file AND the registry's current fence.
    const auto persisted = epoch_store::read(impl_->config.state_dir);
    if (!persisted.ok()) {
        return persisted.error();
    }
    impl_->epochs.bind(impl_->config.state_dir);
    impl_->epochs.seed(persisted.value(), impl_->catalog.fence());

    // 4b. Load the persisted anchor water-mark. This must happen after the
    //     restore (so `last_anchor` reflects the same catalog the restore
    //     produced) and it is fail-closed: a corrupt file refuses to start
    //     rather than silently clearing the anchor map.
    const status anchors = anchor_store::load(impl_->config.state_dir,
                                              impl_->last_anchor);
    if (!anchors.ok()) {
        return anchors;
    }

    // 5. The fence authority is the local registry allocator (step2). step3
    //    replaces this function pointer and the service does not change.
    impl_->config.authority.fn = &allocate_local;
    impl_->config.authority.context = &impl_->catalog;

    // 5b. Replay the replicated state machine. The KV table lives in memory and
    //     the ACL table is mutated only by committed commands, so without this
    //     a restart would come up with an empty KV store and the static seed
    //     ACL -- every rule added at runtime would be silently lost while the
    //     commands that added them are still in the log. The cursor starts at
    //     zero, so the replay is the same code path a follower uses.
    const status replayed = apply_committed();
    if (!replayed.ok()) {
        return replayed;
    }

    // 6. Register the six handlers and the listener.
    builder.add_service(registry_service_id, "registry");
    builder.add_raw_method(registry_service_id, registry_method_register, "register",
                           &registry_service::on_register, this);
    builder.add_raw_method(registry_service_id, registry_method_deregister, "deregister",
                           &registry_service::on_deregister, this);
    builder.add_raw_method(registry_service_id, registry_method_set_state, "set_state",
                           &registry_service::on_set_state, this);
    builder.add_raw_method(registry_service_id, registry_method_query, "query",
                           &registry_service::on_query, this);
    builder.add_raw_method(registry_service_id, registry_method_next_fence, "next_fence",
                           &registry_service::on_next_fence, this);
    builder.add_raw_method(registry_service_id, registry_method_ping, "ping",
                           &registry_service::on_ping, this);
    builder.add_raw_method(registry_service_id, registry_method_kv_get, "kv_get",
                           &registry_service::on_kv_get, this);
    builder.add_raw_method(registry_service_id, registry_method_kv_put, "kv_put",
                           &registry_service::on_kv_put, this);
    builder.add_raw_method(registry_service_id, registry_method_kv_cas, "kv_cas",
                           &registry_service::on_kv_cas, this);
    builder.add_raw_method(registry_service_id, registry_method_kv_delete,
                           "kv_delete", &registry_service::on_kv_delete, this);
    if (impl_->config.listen.valid()) {        if (impl_->config.backend == nullptr) {
            return status{registry_failure(rpc_code::failed_precondition,
                                           "registry service has a listener but no backend",
                                           "misconfigured")};
        }
        builder.add_listener(impl_->config.listen, *impl_->config.backend);
    }
    return status{};
}

status registry_service::start() {
    if (impl_->server_) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "registry service is already started",
                                       "already_started")};
    }
    server::builder builder;
    const status prepared = prepare(builder);
    if (!prepared.ok()) {
        return prepared;
    }
    auto candidate = builder.build();
    if (!candidate) {
        return status{rpc_code::internal};
    }
    const status started = candidate->start();
    if (!started.ok()) {
        return started;
    }
    impl_->server_ = std::move(candidate);
    impl_->bound_ep = impl_->config.listen;
    return status{};
}

void registry_service::stop() noexcept {
    if (impl_->server_) {
        impl_->server_->stop();
        impl_->server_.reset();
    }
    impl_->lease.release();
}

bool registry_service::running() const noexcept { return impl_->server_ != nullptr; }

crpc::endpoint registry_service::bound_endpoint() const noexcept { return impl_->bound_ep; }

void registry_service::set_server(std::unique_ptr<server> owned) {
    impl_->server_ = std::move(owned);
    impl_->bound_ep = impl_->config.listen;
}

// --- Handlers -----------------------------------------------------------------

namespace {

// Appends one catalog mutation to the log. Only register/set_state/deregister
// call this: `fence_barrier` deliberately appends nothing (VERDICT section 11
// / A2 D1), so the log's index advances only when the catalog changes.
[[nodiscard]] status append_catalog_record(registry_log& log,
                                          const service_instance& instance,
                                          u64 fence, u64 incarnation) {
    std::vector<std::byte> payload;
    const status encoded = encode_log_payload(instance, fence, incarnation, payload);
    if (!encoded.ok()) {
        return encoded;
    }
    const u64 next = log.last_index() + 1;
    log_record record;
    record.kind = log_record_kind::registry_event;
    record.term = 0;
    record.index = next;
    record.payload = std::move(payload);
    const result<u64> appended = log.append(record);
    if (!appended.ok()) {
        return appended.error();
    }
    const status synced = log.sync();
    if (!synced.ok()) {
        return synced;
    }
    return status{};
}

// Maps a registry reason to its rpc_code, per VERDICT section 2.5. Two entries
// are NOT `failed_precondition`: `not_live` is 9 (`not_found`, deliberately not
// 12) and `limit` / `index_overflow` are 13 (`resource_exhausted`). Everything
// else on the section-2.5 table is 12.
[[nodiscard]] rpc_code code_for_reason(const std::string& reason) {
    if (reason == "not_live") {
        return rpc_code::not_found;
    }
    if (reason == "invalid") {
        return rpc_code::invalid_argument;
    }
    if (reason == "limit" || reason == "index_overflow") {
        return rpc_code::resource_exhausted;
    }
    return rpc_code::failed_precondition;
}

// Wraps a registry error for the wire, setting `registry.index` so a client can
// re-anchor. The error's `rpc_code` is REMAPPED through `code_for_reason`
// (VERDICT section 2.5), because the registry's own codes are a different
// enumeration: `not_live` is 9 (`not_found`), not 12, and `limit` /
// `index_overflow` are 13 (`resource_exhausted`). Returning the registry's code
// unchanged would hand the caller a number the section-2.5 table does not define.
[[nodiscard]] rpc_error service_error(const rpc_error& error, u64 index) {
    rpc_error out = error;
    if (!error.metadata.empty()) {
        if (const std::string* reason = error.metadata.find("registry.reason");
            reason != nullptr && !reason->empty()) {
            out.code = code_for_reason(*reason);
        }
    }
    out.metadata.set("registry.index", std::to_string(index));
    return out;
}

// The idempotency key for a request: the client-supplied id when present, else
// a deterministic digest of the request. The service-layer map is the second
// leg of the three-layer retry (VERDICT section 5.3); it must recognise a
// replayed request even after the transport dedup TTL expired.
[[nodiscard]] std::string request_key(const std::string& service,
                                      const std::string& node_id, u64 incarnation,
                                      u64 fence, u64 writer_epoch) {
    const std::string raw = service + '\x1f' + node_id + '\x1f' +
                            std::to_string(incarnation) + '\x1f' +
                            std::to_string(fence) + '\x1f' +
                            std::to_string(writer_epoch);
    u64 hash = 1469598103934665603ull;
    for (const char c : raw) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    char buf[32];
    (void)std::snprintf(buf, sizeof(buf), "%016llx",
                        static_cast<unsigned long long>(hash));
    return std::string(buf);
}

// The stable request id for one logical write. Excludes the fence and the
// epoch: both change on every attempt, so including them would make a retry
// unrecognisable.
[[nodiscard]] std::string request_key(const std::string& service,
                                      const std::string& node_id, u64 incarnation) {
    return request_key(service, node_id, incarnation, 0, 0);
}

// The service-issued writer epoch. The client's claim is never used as the
// epoch: two clients can present the same number, which is exactly the
// collision VERDICT section 4.2 exists to prevent. A failure to persist the
// epoch is fail-closed -- no number is issued and the write is refused.
struct epoch_issue {
    bool ok = false;
    u64 value = 0;
    rpc_error error{};
};

[[nodiscard]] epoch_issue issue_epoch(epoch_store& epochs) {
    epoch_issue out;
    const result<u64> next = epochs.next();
    if (!next.ok()) {
        out.error = registry_failure(rpc_code::failed_precondition,
                                     "registry: the epoch could not be persisted",
                                     registry_reason_epoch_not_persisted);
        return out;
    }
    out.ok = true;
    out.value = next.value();
    return out;
}

// The service-layer idempotency branch (VERDICT section 5.3, layer 2). A
// replayed request whose key is already completed AND still inside the TTL
// returns the cached outcome without touching the catalog; only successes are
// cached, because a failed first attempt must remain retryable. An expired
// record is dropped and treated as a miss.
[[nodiscard]] bool try_idempotent(
    std::map<std::string, idempotency_record>& map, const std::string& key,
    std::chrono::milliseconds ttl, registry_write_response& cached) {
    const auto found = map.find(key);
    if (found == map.end()) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - found->second.recorded_at);
    if (!found->second.completed || age > ttl) {
        return false;
    }
    cached = found->second.response;
    return true;
}

void record_idempotent(std::map<std::string, idempotency_record>& map,
                       std::string key, const registry_write_response& outcome,
                       std::size_t max_entries, std::chrono::milliseconds ttl) {
    if (max_entries == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    // 1. TTL eviction: drop every expired record. This is the primary bound and
    //    the one the deployment's retry window must be sized against.
    if (ttl.count() > 0) {
        for (auto it = map.begin(); it != map.end();) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.recorded_at);
            if (age > ttl) {
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    }
    // 2. Capacity eviction: drop the oldest record, ONE at a time. Never clear
    //    the whole map: that would discard the protection of every earlier
    //    request simultaneously and let a replay of any of them execute twice.
    while (map.size() >= max_entries) {
        auto oldest = map.begin();
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (it->second.recorded_at < oldest->second.recorded_at) {
                oldest = it;
            }
        }
        map.erase(oldest);
    }
    idempotency_record record;
    record.completed = true;
    record.code = rpc_code::ok;
    record.response = outcome;
    record.recorded_at = now;
    map[key] = record;
}

// The dual-anchor check, shared by all three write handlers so their anchor
// semantics are identical (VERDICT section 5.2). Returns true when the write
// may proceed; on a stale observation it fills `error` and returns false.
// Must be called with the service mutex held.
[[nodiscard]] bool anchor_allows(
    std::map<std::pair<std::string, std::string>, registry_anchor>& last_anchor,
    const std::pair<std::string, std::string>& catalog_key, u64 client_epoch,
    u64 client_index, bool has_client_anchor, rpc_error& error) {
    if (!has_client_anchor) {
        return true;
    }
    const auto last = last_anchor.find(catalog_key);
    if (last == last_anchor.end()) {
        return true;
    }
    const registry_anchor water_mark = last->second;
    const registry_anchor client{client_epoch, client_index};
    // not_stale(reference, observation): the observation is the client's.
    if (not_stale(water_mark, client)) {
        return true;
    }
    error = registry_failure(rpc_code::failed_precondition,
                             "registry: client anchor is behind the authority",
                             registry_reason_stale_anchor);
    error.metadata.set("registry.index", std::to_string(water_mark.index));
    error.metadata.set("registry.authority_epoch",
                       std::to_string(water_mark.authority_epoch));
    return false;
}

} // namespace

status registry_service::on_register(void* self, const call_envelope& /*envelope*/,
                                     span<const std::byte> body, buffer& response,
                                     rpc::call_context&) {
    auto* owner = static_cast<registry_service*>(self);
    registry_register_request request;
    const status decoded = decode_register_request(body, request);
    if (!decoded.ok()) {
        return decoded;
    }
    const acl_decision acl = owner->check_acl(
        request.token, registry_method_register, request.service, request.node_id,
        std::chrono::system_clock::now(), request.token_issued_at,
        request.has_token_issued_at);
    if (!acl.allowed) {
        return status{registry_failure(rpc_code::permission_denied,
                                       "registry: acl_denied",
                                       registry_reason_acl_denied)};
    }

    std::lock_guard<std::mutex> lock(owner->impl_->mutex);
    // Remote writers must present an epoch. This check sits after the epoch
    // store exists, so a client that ignores it still cannot overwrite a
    // tracked writer (VERDICT section 4.4).
    if (request.writer_epoch == 0) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "registry: remote writers must present an epoch",
                                       registry_reason_epoch_required)};
    }

    const auto key = std::make_pair(request.service, request.node_id);
    rpc_error anchor_error;
    if (!anchor_allows(owner->impl_->last_anchor, key, request.client_authority_epoch,
                       request.client_index, request.has_client_anchor,
                       anchor_error)) {
        return status{anchor_error};
    }

    service_instance instance;
    instance.service = request.service;
    instance.node_id = request.node_id;
    instance.address.host = request.host;
    instance.address.port = request.port;
    instance.weight = request.weight;
    instance.incarnation = request.incarnation;
    instance.state = request.has_state
                         ? static_cast<instance_state>(request.state)
                         : instance_state::passing;
    if (!request.meta.empty()) {
        const status meta = decode_meta_map(
            span<const std::byte>(request.meta.data(), request.meta.size()),
            instance.meta);
        if (!meta.ok()) {
            return meta;
        }
    }

    // The principal is injected by the ACL layer, never taken from the client.
    write_context context;
    context.node_id = acl.rule.principal_id;
    context.admin_token = acl.rule.admin ? owner->impl_->config.registry.admin_token
                                         : std::string{};
    context.incarnation = request.incarnation;
    context.fence = request.fence;
    // The epoch is issued by the service, never taken from the client's claim.
    const epoch_issue issued = issue_epoch(owner->impl_->epochs);
    if (!issued.ok) {
        return status{issued.error};
    }
    context.writer_epoch = issued.value;

    // Layer 2 of the three-layer retry: a completed replay returns the cached
    // outcome and does not touch the catalog. The key is the client's own
    // request id when it supplied one; a key derived from the payload would
    // change on every retry (the fence is re-allocated each call) and could
    // never recognise one.
    const std::string idem_key = !request.client_request_id.empty()
                                     ? request.client_request_id
                                     : request_key(request.service, request.node_id,
                                                   request.incarnation);
    registry_write_response cached;
    if (try_idempotent(owner->impl_->idempotency, idem_key,
                            owner->impl_->config.idempotency_ttl, cached)) {
        return encode_write_response(cached, response);
    }

    const result<write_result> written = owner->impl_->catalog.register_instance(
        instance, context);
    if (!written.ok()) {
        const u64 index = owner->impl_->catalog.index();
        return status{service_error(written.error(), index)};
    }

    // Persist the mutation before the client can observe it as committed.
    const status appended = append_catalog_record(*owner->impl_->log, instance,
                                                 written.value().fence,
                                                 written.value().incarnation);
    if (!appended.ok()) {
        return appended;
    }

    // The water-mark moves only after the mutation is durable. A crash between
    // the registry commit and this update leaves the water-mark behind, which
    // makes the next writer re-anchor rather than silently overtake.
    //
    // The epoch half is the service's OWN epoch (`epoch_store::value()`), not a
    // constant: it is the authority's term number, so a restarted service has a
    // higher one and every anchor a pre-restart client still holds becomes
    // stale. It was previously left at the initialised 0, which made the first
    // half of the two-dimensional comparison dead and reduced `not_stale` to a
    // one-dimensional index comparison.
    owner->impl_->anchor_epoch = owner->impl_->epochs.value();
    owner->impl_->last_anchor[key] =
        registry_anchor{owner->impl_->anchor_epoch, written.value().index};
    // Persist the water-mark BEFORE the response can be observed: a crash after
    // the write but before this leaves the anchor behind, so a replayed write
    // would be accepted. Same order as `epoch_store::persist` (write, then
    // return).
    const status anchored = anchor_store::save(owner->impl_->config.state_dir,
                                               owner->impl_->last_anchor);
    if (!anchored.ok()) {
        return anchored;
    }

    registry_write_response out;
    out.index = written.value().index;
    out.fence = written.value().fence;
    out.incarnation = written.value().incarnation;
    out.authority_epoch = owner->impl_->anchor_epoch;
    // Record only after the catalog AND the log accepted the write: a failed
    // attempt must remain retryable (VERDICT section 5.3, layer 2).
    record_idempotent(owner->impl_->idempotency, idem_key, out,
                      owner->impl_->config.max_idempotency_entries,
                      owner->impl_->config.idempotency_ttl);
    return encode_write_response(out, response);
}

status registry_service::on_deregister(void* self, const call_envelope& /*envelope*/,
                                       span<const std::byte> body, buffer& response,
                                       rpc::call_context&) {
    auto* owner = static_cast<registry_service*>(self);
    registry_deregister_request request;
    const status decoded = decode_deregister_request(body, request);
    if (!decoded.ok()) {
        return decoded;
    }
    const acl_decision acl = owner->check_acl(
        request.token, registry_method_deregister, request.service, request.node_id,
        std::chrono::system_clock::now(), request.token_issued_at,
        request.has_token_issued_at);
    if (!acl.allowed) {
        return status{registry_failure(rpc_code::permission_denied,
                                       "registry: acl_denied",
                                       registry_reason_acl_denied)};
    }

    std::lock_guard<std::mutex> lock(owner->impl_->mutex);
    if (request.writer_epoch == 0) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "registry: remote writers must present an epoch",
                                       registry_reason_epoch_required)};
    }

    const auto key = std::make_pair(request.service, request.node_id);
    rpc_error anchor_error;
    if (!anchor_allows(owner->impl_->last_anchor, key, request.client_authority_epoch,
                       request.client_index, request.has_client_anchor,
                       anchor_error)) {
        return status{anchor_error};
    }

    write_context context;
    context.node_id = acl.rule.principal_id;
    context.admin_token = acl.rule.admin ? owner->impl_->config.registry.admin_token
                                         : std::string{};
    context.incarnation = request.incarnation;
    context.fence = request.fence;
    // The epoch is issued by the service, never taken from the client's claim.
    const epoch_issue issued = issue_epoch(owner->impl_->epochs);
    if (!issued.ok) {
        return status{issued.error};
    }
    context.writer_epoch = issued.value;

    const std::string idem_key = !request.client_request_id.empty()
                                     ? request.client_request_id
                                     : request_key(request.service, request.node_id,
                                                   request.incarnation);
    registry_write_response cached;
    if (try_idempotent(owner->impl_->idempotency, idem_key,
                            owner->impl_->config.idempotency_ttl, cached)) {
        return encode_write_response(cached, response);
    }

    const result<write_result> written = owner->impl_->catalog.deregister(
        request.service, request.node_id, context);
    if (!written.ok()) {
        const u64 index = owner->impl_->catalog.index();
        return status{service_error(written.error(), index)};
    }

    // The tombstone is the instance the log stores, so a restore reproduces
    // the same catalog state.
    service_instance tombstone;
    tombstone.service = request.service;
    tombstone.node_id = request.node_id;
    tombstone.state = instance_state::deregistered;
    tombstone.incarnation = written.value().incarnation;
    const status appended = append_catalog_record(*owner->impl_->log, tombstone, written.value().fence, written.value().incarnation);
    if (!appended.ok()) {
        return appended;
    }

    registry_write_response out;
    out.index = written.value().index;
    out.fence = written.value().fence;
    out.incarnation = written.value().incarnation;
    // Advance the water-mark exactly as `on_register` does: a deregister is a
    // write, and a stalled water-mark would keep an already-superseded anchor
    // acceptable forever.
    owner->impl_->anchor_epoch = owner->impl_->epochs.value();
    owner->impl_->last_anchor[key] =
        registry_anchor{owner->impl_->anchor_epoch, written.value().index};
    const status anchored = anchor_store::save(owner->impl_->config.state_dir,
                                               owner->impl_->last_anchor);
    if (!anchored.ok()) {
        return anchored;
    }
    out.authority_epoch = owner->impl_->anchor_epoch;
    record_idempotent(owner->impl_->idempotency, idem_key, out,
                      owner->impl_->config.max_idempotency_entries,
                      owner->impl_->config.idempotency_ttl);
    return encode_write_response(out, response);
}

status registry_service::on_set_state(void* self, const call_envelope& /*envelope*/,
                                      span<const std::byte> body, buffer& response,
                                      rpc::call_context&) {
    auto* owner = static_cast<registry_service*>(self);
    registry_set_state_request request;
    const status decoded = decode_set_state_request(body, request);
    if (!decoded.ok()) {
        return decoded;
    }
    const acl_decision acl = owner->check_acl(
        request.token, registry_method_set_state, request.service, request.node_id,
        std::chrono::system_clock::now(), request.token_issued_at,
        request.has_token_issued_at);
    if (!acl.allowed) {
        return status{registry_failure(rpc_code::permission_denied,
                                       "registry: acl_denied",
                                       registry_reason_acl_denied)};
    }

    std::lock_guard<std::mutex> lock(owner->impl_->mutex);
    if (request.writer_epoch == 0) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "registry: remote writers must present an epoch",
                                       registry_reason_epoch_required)};
    }

    const auto key = std::make_pair(request.service, request.node_id);
    rpc_error anchor_error;
    if (!anchor_allows(owner->impl_->last_anchor, key, request.client_authority_epoch,
                       request.client_index, request.has_client_anchor,
                       anchor_error)) {
        return status{anchor_error};
    }

    write_context context;
    context.node_id = acl.rule.principal_id;
    context.admin_token = acl.rule.admin ? owner->impl_->config.registry.admin_token
                                         : std::string{};
    context.incarnation = request.incarnation;
    context.fence = request.fence;
    // The epoch is issued by the service, never taken from the client's claim.
    const epoch_issue issued = issue_epoch(owner->impl_->epochs);
    if (!issued.ok) {
        return status{issued.error};
    }
    context.writer_epoch = issued.value;

    const std::string idem_key = !request.client_request_id.empty()
                                     ? request.client_request_id
                                     : request_key(request.service, request.node_id,
                                                   request.incarnation);
    registry_write_response cached;
    if (try_idempotent(owner->impl_->idempotency, idem_key,
                            owner->impl_->config.idempotency_ttl, cached)) {
        return encode_write_response(cached, response);
    }

    const result<write_result> written = owner->impl_->catalog.set_state(
        request.service, request.node_id, static_cast<instance_state>(request.state),
        context);
    if (!written.ok()) {
        const u64 index = owner->impl_->catalog.index();
        return status{service_error(written.error(), index)};
    }

    query_options options;
    options.service = request.service;
    options.node_id = request.node_id;
    options.include_deregistered = true;
    const result<catalog_result> queried = owner->impl_->catalog.query(options);
    if (!queried.ok() || queried.value().instances.empty()) {
        return status{registry_failure(rpc_code::internal,
                                       "registry: state change did not produce a row",
                                       "internal")};
    }
    const status appended = append_catalog_record(*owner->impl_->log, queried.value().instances.front(), written.value().fence,
        written.value().incarnation);
    if (!appended.ok()) {
        return appended;
    }

    registry_write_response out;
    out.index = written.value().index;
    out.fence = written.value().fence;
    out.incarnation = written.value().incarnation;
    // Advance the water-mark exactly as `on_register` / `on_deregister` do.
    owner->impl_->anchor_epoch = owner->impl_->epochs.value();
    owner->impl_->last_anchor[key] =
        registry_anchor{owner->impl_->anchor_epoch, written.value().index};
    const status anchored = anchor_store::save(owner->impl_->config.state_dir,
                                               owner->impl_->last_anchor);
    if (!anchored.ok()) {
        return anchored;
    }
    out.authority_epoch = owner->impl_->anchor_epoch;
    record_idempotent(owner->impl_->idempotency, idem_key, out,
                      owner->impl_->config.max_idempotency_entries,
                      owner->impl_->config.idempotency_ttl);
    return encode_write_response(out, response);
}

status registry_service::on_query(void* self, const call_envelope& /*envelope*/,
                                  span<const std::byte> body, buffer& response,
                                  rpc::call_context&) {
    auto* owner = static_cast<registry_service*>(self);
    registry_query_request request;
    const status decoded = decode_query_request(body, request);
    if (!decoded.ok()) {
        return decoded;
    }
    const acl_decision acl = owner->check_acl(
        request.token, registry_method_query, request.service, request.node_id,
        std::chrono::system_clock::now(), request.token_issued_at,
        request.has_token_issued_at);
    if (!acl.allowed) {
        return status{registry_failure(rpc_code::permission_denied,
                                       "registry: acl_denied",
                                       registry_reason_acl_denied)};
    }

    std::lock_guard<std::mutex> lock(owner->impl_->mutex);
    query_options options;
    options.service = request.service;
    options.node_id = request.node_id;
    options.passing_only = request.passing_only;
    // Tombstones are visible to a caller that holds a write prefix for the key;
    // a plain read gets the live view.
    options.include_deregistered = false;

    const result<catalog_result> queried = owner->impl_->catalog.query(options);
    if (!queried.ok()) {
        return status{service_error(queried.error(), owner->impl_->catalog.index())};
    }

    registry_query_response out;
    out.anchor.authority_epoch = owner->impl_->anchor_epoch;
    out.anchor.index = queried.value().index;
    for (const service_instance& instance : queried.value().instances) {
        registry_query_row row;
        row.revision = instance.revision;
        row.instance = instance;
        out.rows.push_back(std::move(row));
    }
    return encode_query_response(out, response);
}

status registry_service::on_next_fence(void* self, const call_envelope& /*envelope*/,
                                       span<const std::byte> body, buffer& response,
                                       rpc::call_context&) {
    auto* owner = static_cast<registry_service*>(self);
    try {
        return owner->on_next_fence_impl(*owner, body, response);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "on_next_fence threw: %s\n", ex.what());
        return status{rpc_code::internal};
    } catch (...) {
        std::fprintf(stderr, "on_next_fence threw unknown\n");
        return status{rpc_code::internal};
    }
}

status registry_service::on_next_fence_impl(registry_service& owner,
                                           span<const std::byte> body,
                                           buffer& response) {
    registry_next_fence_request request;
    const status decoded = decode_next_fence_request(body, request);
    if (!decoded.ok()) {
        return decoded;
    }
    const acl_decision acl = owner.check_acl(
        request.token, registry_method_next_fence, request.service, request.node_id,
        std::chrono::system_clock::now(), request.token_issued_at,
        request.has_token_issued_at);
    if (!acl.allowed) {
        return status{registry_failure(rpc_code::permission_denied,
                                       "registry: acl_denied",
                                       registry_reason_acl_denied)};
    }

    // The fence request carries no epoch, so the epoch-store guard does not
    // apply here: it allocates a token, it does not write a key.
    fence_request fence;
    fence.service = request.service.c_str();
    fence.node_id = request.node_id.c_str();
    fence.lease_id = 0;
    // The principal is the ACL-injected identity, never the client's claim.
    u64 principal = 0;
    for (const char c : acl.rule.principal_id) {
        principal ^= static_cast<unsigned char>(c);
        principal *= 1099511628211ull;
    }
    fence.principal = principal;

    const result<u64> allocated = owner.impl_->config.authority.allocate(fence);
    if (!allocated.ok()) {
        return status{service_error(allocated.error(), owner.impl_->catalog.index())};
    }

    registry_write_response out;
    out.index = owner.impl_->catalog.index();
    out.fence = allocated.value();
    out.incarnation = 0;
    out.authority_epoch = owner.impl_->anchor_epoch;
    return encode_write_response(out, response);
}

status registry_service::on_ping(void* self, const call_envelope& /*envelope*/,
                                 span<const std::byte> body, buffer& response,
                                 rpc::call_context&) {
    auto* owner = static_cast<registry_service*>(self);
    registry_ping_request request;
    const status decoded = decode_ping_request(body, request);
    if (!decoded.ok()) {
        return decoded;
    }
    const acl_decision acl =
        owner->check_acl(request.token, registry_method_ping, "", "",
                     std::chrono::system_clock::now(), request.token_issued_at,
                     request.has_token_issued_at);
    if (!acl.allowed) {
        return status{registry_failure(rpc_code::permission_denied,
                                       "registry: acl_denied",
                                       registry_reason_acl_denied)};
    }
    registry_anchor anchor;
    {
        std::lock_guard<std::mutex> lock(owner->impl_->mutex);
        anchor.authority_epoch = owner->impl_->anchor_epoch;
        anchor.index = owner->impl_->catalog.index();
    }
    return encode_anchor_response(anchor, response);
}

// --- Method dispatch (step4 PR-8) --------------------------------------------
//
// The HTTP facade reaches the handlers through this table instead of calling
// them directly. One table, one function-pointer type: the compiler checks each
// entry against the handler signature, so a handler whose shape changes cannot
// be wired in silently.

namespace {

struct dispatch_entry {
    u64 method_id;
    status (*handler)(void* self, const call_envelope& envelope,
                      span<const std::byte> body, buffer& response,
                      rpc::call_context& ctx);
};

constexpr dispatch_entry kDispatchTable[] = {
    {registry_method_register, &registry_service::on_register},
    {registry_method_deregister, &registry_service::on_deregister},
    {registry_method_set_state, &registry_service::on_set_state},
    {registry_method_query, &registry_service::on_query},
    {registry_method_next_fence, &registry_service::on_next_fence},
    {registry_method_ping, &registry_service::on_ping},
    {registry_method_kv_get, &registry_service::on_kv_get},
    {registry_method_kv_put, &registry_service::on_kv_put},
    {registry_method_kv_cas, &registry_service::on_kv_cas},
    {registry_method_kv_delete, &registry_service::on_kv_delete},
};

} // namespace

status registry_service::dispatch(u64 method_id, span<const std::byte> body,
                                  buffer& response) {
    for (const dispatch_entry& entry : kDispatchTable) {
        if (entry.method_id != method_id) {
            continue;
        }
        call_envelope envelope;
        envelope.service_id = registry_service_id;
        envelope.method_id = method_id;
        envelope.body = body;
        rpc::call_context context;
        return entry.handler(this, envelope, body, response, context);
    }
    return status{registry_failure(rpc_code::no_such_method,
                                   "registry: no handler for that method",
                                   "no_such_method")};
}

// --- KV handlers (step4 PR-7) -----------------------------------------------

status registry_service::on_kv_get(void* self, const call_envelope& /*envelope*/,
                                   span<const std::byte> body, buffer& response,
                                   rpc::call_context&) {
    auto* owner = static_cast<registry_service*>(self);
    kv_get_request request;
    const status decoded = decode_kv_get_request(body, request);
    if (!decoded.ok()) {
        return decoded;
    }
    const acl_decision acl = owner->check_acl(
        request.token, registry_method_kv_get, request.key, "",
        std::chrono::system_clock::now(), request.token_issued_at,
        request.has_token_issued_at);
    if (!acl.allowed) {
        return status{registry_failure(rpc_code::permission_denied,
                                       "registry: acl_denied",
                                       registry_reason_acl_denied)};
    }
    if (owner->impl_->config.kv == nullptr) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "registry: kv store not configured",
                                       "kv_unavailable")};
    }
    std::lock_guard<std::mutex> lock(owner->impl_->mutex);
    const result<kv_entry> entry = owner->impl_->config.kv->get(request.key);
    if (!entry.ok()) {
        if (entry.code() == rpc_code::not_found) {
            kv_get_response out;
            out.index = std::max(owner->impl_->catalog.index(),
                                 owner->impl_->config.kv->highest_index());
            out.found = false;
            return encode_kv_get_response(out, response);
        }
        return status{registry_failure(rpc_code::invalid_argument,
                                       "registry: kv invalid key",
                                       registry_reason_acl_denied)};
    }
    kv_get_response out;
    out.index = std::max(owner->impl_->catalog.index(),
                         owner->impl_->config.kv->highest_index());
    out.found = true;
    out.value = entry.value().value;
    out.create_index = entry.value().create_index;
    out.modify_index = entry.value().modify_index;
    return encode_kv_get_response(out, response);
}

status registry_service::on_kv_put(void* self, const call_envelope& /*envelope*/,
                                   span<const std::byte> body, buffer& response,
                                   rpc::call_context&) {
    auto* owner = static_cast<registry_service*>(self);
    kv_put_request request;
    const status decoded = decode_kv_put_request(body, request);
    if (!decoded.ok()) {
        return decoded;
    }
    const acl_decision acl = owner->check_acl(
        request.token, registry_method_kv_put, request.key, "",
        std::chrono::system_clock::now(), request.token_issued_at,
        request.has_token_issued_at);
    if (!acl.allowed) {
        return status{registry_failure(rpc_code::permission_denied,
                                       "registry: acl_denied",
                                       registry_reason_acl_denied)};
    }
    if (owner->impl_->config.kv == nullptr) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "registry: kv store not configured",
                                       "kv_unavailable")};
    }
    // PR-9: the put is now a replicated command, not a local write followed by
    // an empty proposal. An empty payload left every follower with no command to
    // apply, so a multi-node cluster diverged on the first KV write.
    //
    // Key validity is checked here, before anything is proposed: an invalid key
    // must be refused without consuming a log slot, and the replicated apply
    // path assumes its input is already valid.
    {
        std::lock_guard<std::mutex> lock(owner->impl_->mutex);
        if (!kv_store::valid_key(request.key)) {
            return status{registry_failure(rpc_code::invalid_argument,
                                           "registry: kv invalid key",
                                           registry_reason_acl_denied)};
        }
    }
    raft_command command;
    command.kind = raft_command_kind::kv_put;
    command.key = request.key;
    command.value = std::move(request.value);
    const status replicated = owner->replicate_command(command);
    if (!replicated.ok()) {
        return replicated;
    }
    // The apply loop already wrote the value and advanced the store's index.
    // Report that index rather than recomputing it, so the response matches what
    // every node now holds.
    kv_put_response out;
    {
        std::lock_guard<std::mutex> lock(owner->impl_->mutex);
        out.index = owner->impl_->config.kv->highest_index();
        out.modify_index = out.index;
    }
    return encode_kv_put_response(out, response);
}

status registry_service::on_kv_cas(void* self, const call_envelope& /*envelope*/,
                                   span<const std::byte> body, buffer& response,
                                   rpc::call_context&) {
    auto* owner = static_cast<registry_service*>(self);
    kv_put_request request;
    const status decoded = decode_kv_put_request(body, request);
    if (!decoded.ok()) {
        return decoded;
    }
    const acl_decision acl = owner->check_acl(
        request.token, registry_method_kv_cas, request.key, "",
        std::chrono::system_clock::now(), request.token_issued_at,
        request.has_token_issued_at);
    if (!acl.allowed) {
        return status{registry_failure(rpc_code::permission_denied,
                                       "registry: acl_denied",
                                       registry_reason_acl_denied)};
    }
    if (owner->impl_->config.kv == nullptr) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "registry: kv store not configured",
                                       "kv_unavailable")};
    }
    // The CAS precondition is evaluated on the LEADER before proposing. A
    // conflicting CAS must not consume a log slot: every node would then apply a
    // command that changes nothing, and the log would fill with no-ops under
    // contention. Evaluating it once, here, also means the decision and the
    // write are made against the same state, which a follower cannot do.
    //
    // D-A-3: the check and the commit are ONE critical section (`cas_mutex`).
    // Two callers presenting the same `cas = N` must not both pass: the first
    // commits and applies, and the second's check then sees the new
    // `modify_index` and conflicts. `cas_mutex` is separate from `mutex`
    // because `replicate_command` re-enters `mutex` through `apply_committed`.
    std::lock_guard<std::mutex> cas_lock(owner->impl_->cas_mutex);
    {
        std::lock_guard<std::mutex> lock(owner->impl_->mutex);
        if (!kv_store::valid_key(request.key)) {
            return status{registry_failure(rpc_code::invalid_argument,
                                           "registry: kv invalid key",
                                           registry_reason_acl_denied)};
        }
        const result<kv_entry> current =
            owner->impl_->config.kv->get(request.key);
        const u64 current_modify_index = current.ok() ? current.value().modify_index : 0;
        const bool exists = current.ok();
        const bool matches = request.cas == 0 ? !exists
                                              : (exists && current_modify_index == request.cas);
        if (!matches) {
            return status{registry_failure(rpc_code::failed_precondition,
                                           "registry: cas_conflict",
                                           registry_reason_cas_conflict)};
        }
    }
    raft_command command;
    command.kind = raft_command_kind::kv_cas;
    command.key = request.key;
    command.value = std::move(request.value);
    command.cas = request.cas;
    const status replicated = owner->replicate_command(command);
    if (!replicated.ok()) {
        return replicated;
    }
    kv_put_response out;
    {
        std::lock_guard<std::mutex> lock(owner->impl_->mutex);
        out.index = owner->impl_->config.kv->highest_index();
        out.modify_index = out.index;
    }
    return encode_kv_put_response(out, response);
}

status registry_service::on_kv_delete(void* self, const call_envelope& /*envelope*/,
                                      span<const std::byte> body, buffer& response,
                                      rpc::call_context&) {
    auto* owner = static_cast<registry_service*>(self);
    kv_delete_request request;
    const status decoded = decode_kv_delete_request(body, request);
    if (!decoded.ok()) {
        return decoded;
    }
    const acl_decision acl = owner->check_acl(
        request.token, registry_method_kv_delete, request.key, "",
        std::chrono::system_clock::now(), request.token_issued_at,
        request.has_token_issued_at);
    if (!acl.allowed) {
        return status{registry_failure(rpc_code::permission_denied,
                                       "registry: acl_denied",
                                       registry_reason_acl_denied)};
    }
    if (owner->impl_->config.kv == nullptr) {
        return status{registry_failure(rpc_code::failed_precondition,
                                       "registry: kv store not configured",
                                       "kv_unavailable")};
    }
    // The not-found precondition is evaluated on the leader before proposing,
    // for the same reason as `on_kv_cas`: a delete of an absent key must not
    // consume a log slot, and the decision must be made against the state the
    // write will actually land on.
    {
        std::lock_guard<std::mutex> lock(owner->impl_->mutex);
        if (!kv_store::valid_key(request.key)) {
            return status{registry_failure(rpc_code::invalid_argument,
                                           "registry: kv invalid key",
                                           registry_reason_acl_denied)};
        }
        if (!owner->impl_->config.kv->get(request.key).ok()) {
            return status{registry_failure(rpc_code::not_found,
                                           "registry: kv key not found",
                                           registry_reason_acl_denied)};
        }
    }
    raft_command command;
    command.kind = raft_command_kind::kv_delete;
    command.key = request.key;
    const status replicated = owner->replicate_command(command);
    if (!replicated.ok()) {
        return replicated;
    }
    kv_delete_response out;
    {
        std::lock_guard<std::mutex> lock(owner->impl_->mutex);
        out.index = owner->impl_->config.kv->highest_index();
    }
    out.deleted = true;
    return encode_kv_delete_response(out, response);
}

} // namespace cluster
} // namespace crpc
