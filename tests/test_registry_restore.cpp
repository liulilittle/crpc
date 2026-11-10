// CRPC -- G19 step1 S-B2: registry::restore_from_log.
//
// The contract these cases pin (verdict 7.1.6 / 7.1.6b, W5-1):
//
//   D1  the restore folds duplicate log indexes last-wins: a synthesised
//       stream [{1,A},{2,B},{2,C}] leaves 2=C in the catalog. The real file
//       backend already folds (C2 truncates a conflicting suffix), so this
//       case must feed a custom backend straight to restore_from_log; a
//       folding test built on a real log is vacuous (7.1.6b).
//   D2  a stream whose indexes are dense after folding is accepted. This is
//       the positive control for D5: only D5 would be fooled by an
//       implementation that never validates at all.
//   D3  the registry index is the folded maximum, not the record count.
//   D4  duplicate indexes do not double-apply: the catalog has one row per
//       (service, node_id) and the folded record count is the dense count.
//   D5  a truncated tail (registry events 1 then 3 with no record at 2) is
//       rejected, and the registry is left completely untouched. G19 step3
//       PR-1 narrowed this to "strictly increasing" because the consensus
//       layer's own kinds may legally occupy the missing index, so a gap is
//       only a corruption when nothing at all carries the index.
//
// Fail-closed requirements (R1-R9 as narrowed by B-verify-sb2-criteria):
//   R1  a torn tail is the prefix replay() returns; it is accepted as-is.
//   R4  the same (service, node_id, incarnation) with a different payload is
//       a conflict, not a silent overwrite.
//   R5  a stream whose fence water-mark is below the target registry's is
//       rejected (a fence regression would resurrect an old holder).
//   R8  a CRC-corrupt frame surfaces replay()'s own error verbatim.
//   R9  a failed restore never zeroes the registry index.
//
// ABI constraint: restore_from_log is a friend free function, so `registry`
// gains no data member and `sizeof(registry)` stays 432 (Debug) / 384
// (Release).
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <crpc/cluster/registry.hpp>
#include <crpc/cluster/registry_log.hpp>
#include <crpc/fs/file.hpp>
#include <crpc/fs/temp.hpp>
#include <crpc/span.hpp>

namespace cl = crpc::cluster;
namespace fs = crpc::fs;
using crpc::rpc_code;
using crpc::u64;

namespace {

// --- Payload codec -----------------------------------------------------------
//
// version(u8) | state(u8) | service(len u32 + bytes) | node_id(len u32 + bytes)
// | host(len u32 + bytes) | port(u16) | weight(u32) | incarnation(u64)
// | meta_count(u32) then per entry key(len u32 + bytes) value(len u32 + bytes)
//
// Every length is a u32 and the decoder rejects a length that would run past
// the end of the buffer, so a truncated payload is an error rather than a
// partial instance.
constexpr std::uint8_t kVersion = 1;

class payload_writer {
public:
    void u8(std::uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v) {
        for (int i = 0; i < 2; ++i) {
            out_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
        }
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
        }
    }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            out_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFull));
        }
    }
    void str(const std::string& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        for (const char c : value) {
            out_.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
    }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(out_); }

private:
    std::vector<std::byte> out_;
};

std::vector<std::byte> encode_payload(const cl::service_instance& instance) {
    payload_writer w;
    w.u8(kVersion);
    w.u8(static_cast<std::uint8_t>(instance.state));
    w.str(instance.service);
    w.str(instance.node_id);
    w.str(instance.address.host);
    w.u16(instance.address.port);
    w.u32(instance.weight);
    w.u64(instance.incarnation);
    w.u32(static_cast<std::uint32_t>(instance.meta.size()));
    for (const auto& entry : instance.meta) {
        w.str(entry.first);
        w.str(entry.second);
    }
    return w.take();
}

class payload_reader {
public:
    explicit payload_reader(const std::vector<std::byte>& bytes) : bytes_(bytes) {}

    bool u8(std::uint8_t& out) {
        if (pos_ + 1 > bytes_.size()) {
            return false;
        }
        out = std::to_integer<std::uint8_t>(bytes_[pos_]);
        pos_ += 1;
        return true;
    }
    bool u16(std::uint16_t& out) {
        if (pos_ + 2 > bytes_.size()) {
            return false;
        }
        out = static_cast<std::uint16_t>(
            std::to_integer<unsigned>(bytes_[pos_]) |
            (static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes_[pos_ + 1]))
             << 8));
        pos_ += 2;
        return true;
    }
    bool u32(std::uint32_t& out) {
        if (pos_ + 4 > bytes_.size()) {
            return false;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(
                     std::to_integer<unsigned>(bytes_[pos_ + i]))
                 << (8 * i);
        }
        out = v;
        pos_ += 4;
        return true;
    }
    bool u64(std::uint64_t& out) {
        if (pos_ + 8 > bytes_.size()) {
            return false;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned>(bytes_[pos_ + i]))
                 << (8 * i);
        }
        out = v;
        pos_ += 8;
        return true;
    }
    bool str(std::string& out) {
        std::uint32_t len = 0;
        if (!u32(len)) {
            return false;
        }
        if (len > bytes_.size() - pos_) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(bytes_.data()) + pos_, len);
        pos_ += len;
        return true;
    }
    [[nodiscard]] bool at_end() const { return pos_ == bytes_.size(); }

private:
    const std::vector<std::byte>& bytes_;
    std::size_t pos_ = 0;
};

bool decode_payload(const std::vector<std::byte>& bytes,
                    cl::service_instance& out) {
    payload_reader r(bytes);
    std::uint8_t version = 0;
    std::uint8_t state = 0;
    if (!r.u8(version) || version != kVersion) {
        return false;
    }
    if (!r.u8(state) || state > static_cast<std::uint8_t>(cl::instance_state::deregistered)) {
        return false;
    }
    out.state = static_cast<cl::instance_state>(state);
    if (!r.str(out.service) || !r.str(out.node_id) || !r.str(out.address.host)) {
        return false;
    }
    if (!r.u16(out.address.port) || !r.u32(out.weight) || !r.u64(out.incarnation)) {
        return false;
    }
    std::uint32_t meta_count = 0;
    if (!r.u32(meta_count) || meta_count > 100000u) {
        return false;
    }
    for (std::uint32_t i = 0; i < meta_count; ++i) {
        std::string key;
        std::string value;
        if (!r.str(key) || !r.str(value)) {
            return false;
        }
        out.meta.set(std::move(key), std::move(value));
    }
    return r.at_end();
}

// A log that carries the consensus layer's kinds alongside registry events.
class mixed_log final : public cl::registry_log {
public:
    crpc::result<u64> append(const cl::log_record& record) override {
        records_.push_back(record);
        return record.index;
    }
    crpc::result<std::vector<cl::log_record>> replay(u64 from_index) const override {
        std::vector<cl::log_record> out;
        for (const auto& record : records_) {
            if (record.index >= from_index) {
                out.push_back(record);
            }
        }
        return out;
    }
    crpc::status sync() override { return crpc::status(); }
    [[nodiscard]] u64 last_index() const noexcept override {
        return records_.empty() ? 0 : records_.back().index;
    }

    // The consensus layer writes a record with no registry-event codec; the
    // payload shape is opaque to the catalog and deliberately not decodable.
    void append_consensus(cl::log_record_kind kind, u64 index, u64 term,
                          const std::vector<std::byte>& payload) {
        cl::log_record record;
        record.kind = kind;
        record.term = term;
        record.index = index;
        record.payload = payload;
        records_.push_back(std::move(record));
    }

private:
    std::vector<cl::log_record> records_;
};


// --- Stream building ---------------------------------------------------------

struct stream_entry {
    u64 index = 0;
    u64 fence = 0;
    u64 incarnation = 0;
    cl::service_instance instance;
};

cl::log_record make_record(u64 index, u64 fence, u64 incarnation,
                           const cl::service_instance& instance) {
    cl::log_record record;
    record.kind = cl::log_record_kind::registry_event;
    record.term = 0;
    record.index = index;
    const std::vector<std::byte> body = encode_payload(instance);
    record.payload.resize(16 + body.size());
    const std::uint64_t fence_le = fence;
    const std::uint64_t incarnation_le = incarnation;
    for (int i = 0; i < 8; ++i) {
        record.payload[static_cast<std::size_t>(i)] =
            static_cast<std::byte>((fence_le >> (8 * i)) & 0xFFull);
        record.payload[8 + static_cast<std::size_t>(i)] =
            static_cast<std::byte>((incarnation_le >> (8 * i)) & 0xFFull);
    }
    std::copy(body.begin(), body.end(), record.payload.begin() + 16);
    return record;
}

bool payload_fence(const std::vector<std::byte>& payload, u64& fence,
                   u64& incarnation) {
    if (payload.size() < 16) {
        return false;
    }
    fence = 0;
    incarnation = 0;
    for (int i = 0; i < 8; ++i) {
        fence |= static_cast<u64>(std::to_integer<unsigned>(payload[static_cast<std::size_t>(i)]))
                 << (8 * i);
        incarnation |= static_cast<u64>(
                           std::to_integer<unsigned>(payload[8 + static_cast<std::size_t>(i)]))
                       << (8 * i);
    }
    return true;
}

// A backend that replays an exact, caller-chosen stream. This is what makes
// D1/D5 non-vacuous: the production file backend folds duplicates itself
// (registry_log.cpp C2), so a real log can never produce the stream under test.
class stream_log final : public cl::registry_log {
public:
    crpc::result<u64> append(const cl::log_record& record) override {
        records_.push_back(record);
        return record.index;
    }
    crpc::result<std::vector<cl::log_record>> replay(u64 from_index) const override {
        std::vector<cl::log_record> out;
        for (const auto& record : records_) {
            if (record.index >= from_index) {
                out.push_back(record);
            }
        }
        return out;
    }
    crpc::status sync() override { return crpc::status(); }
    [[nodiscard]] u64 last_index() const noexcept override {
        return records_.empty() ? 0 : records_.back().index;
    }

private:
    std::vector<cl::log_record> records_;
};

cl::registry_config legacy_config() {
    cl::registry_config config;
    config.require_issued_fence = false;
    return config;
}

cl::service_instance make_instance(const std::string& service,
                                   const std::string& node, crpc::u16 port,
                                   u64 incarnation,
                                   const std::string& host = "127.0.0.1") {
    cl::service_instance instance;
    instance.service = service;
    instance.node_id = node;
    instance.address.host = host;
    instance.address.port = port;
    instance.weight = 1;
    instance.incarnation = incarnation;
    instance.state = cl::instance_state::passing;
    return instance;
}

cl::write_context make_ctx(const std::string& node, u64 fence = 0,
                           u64 incarnation = 0) {
    cl::write_context context;
    context.node_id = node;
    context.fence = fence;
    context.incarnation = incarnation;
    context.writer_epoch = 0;  // external writer: not epoch-tracked
    return context;
}

std::string reason_of(const crpc::rpc_error& error) {
    const std::string* reason = error.metadata.find("registry.reason");
    return reason != nullptr ? *reason : std::string{};
}

template <typename T>
std::string rejection_reason(const crpc::result<T>& outcome) {
    return outcome.ok() ? std::string("<accepted>") : reason_of(outcome.error());
}

std::vector<stream_entry> simple_stream() {
    std::vector<stream_entry> entries;
    entries.push_back({1, 1, 1, make_instance("svc", "n1", 8001, 1)});
    entries.push_back({2, 2, 1, make_instance("svc", "n2", 8002, 1)});
    return entries;
}

stream_log make_log(const std::vector<stream_entry>& entries) {
    stream_log log;
    for (const auto& entry : entries) {
        (void)log.append(make_record(entry.index, entry.fence, entry.incarnation,
                                     entry.instance));
    }
    return log;
}

std::vector<stream_entry> query_all(cl::registry& reg) {
    cl::query_options options;
    options.include_deregistered = true;
    const auto result = reg.query(options);
    std::vector<stream_entry> out;
    if (!result.ok()) {
        return out;
    }
    for (const auto& instance : result.value().instances) {
        stream_entry entry;
        entry.instance = instance;
        entry.index = instance.revision;
        entry.incarnation = instance.incarnation;
        out.push_back(std::move(entry));
    }
    return out;
}

bool find_instance(cl::registry& reg, const std::string& service,
                   const std::string& node, cl::service_instance& out) {
    cl::query_options options;
    options.include_deregistered = true;
    const auto result = reg.query(options);
    if (!result.ok()) {
        return false;
    }
    for (const auto& instance : result.value().instances) {
        if (instance.service == service && instance.node_id == node) {
            out = instance;
            return true;
        }
    }
    return false;
}

// --- CRC-corrupt file backend -----------------------------------------------

bool truncate_file_to(const std::string& path, u64 bytes) {
    auto read = fs::read_all_bytes(fs::path(path));
    if (!read.ok()) {
        return false;
    }
    const fs::byte_buffer& all = read.value();
    if (all.size() < bytes) {
        return false;
    }
    return fs::write_all_bytes(
               fs::path(path),
               crpc::span<const std::byte>(all.data(), static_cast<std::size_t>(bytes)))
        .ok();
}

struct log_dir {
    fs::temp_dir dir;

    explicit log_dir(const char* prefix) {
        auto created = fs::temp_dir::create(fs::path(), prefix);
        if (created.ok()) {
            dir = std::move(created.value());
        }
    }

    bool valid() const { return dir.valid(); }

    std::string file(const char* name) const {
        return (dir.native() / fs::path(name)).str();
    }
};

} // namespace

// --- D1-D4: last-wins folding, dense stream accepted ------------------------

CRPC_TEST(restore_folds_duplicate_index_last_wins) {
    cl::registry reg(legacy_config());

    std::vector<stream_entry> entries;
    entries.push_back({1, 1, 1, make_instance("svc", "n1", 8001, 1)});
    entries.push_back({2, 2, 1, make_instance("svc", "n2", 8002, 1)});
    // Same index as the second record, a different node: last-wins must keep
    // this one and drop the n2 row entirely.
    entries.push_back({2, 5, 3, make_instance("svc", "n3", 8003, 3)});
    stream_log log = make_log(entries);

    const auto st = cl::restore_from_log(reg, log);

    // D2: accepted (kills "validate before folding" -- a stream whose raw
    // indexes are 1,2,2 has no gap, and neither does its folded 1,2).
    CRPC_CHECK(st.ok());

    // D1: the surviving row at index 2 is n3, not n2.
    cl::service_instance winner;
    CRPC_CHECK(find_instance(reg, "svc", "n3", winner));
    CRPC_CHECK_EQ(std::string("n3"), winner.node_id);
    CRPC_CHECK_EQ(u64{3}, winner.incarnation);
    cl::service_instance dropped;
    CRPC_CHECK_FALSE(find_instance(reg, "svc", "n2", dropped));

    // D3: the index is the folded maximum (2), not the record count (3).
    CRPC_CHECK_EQ(u64{2}, reg.index());

    // D4: exactly two rows exist; a non-folding apply would leave three.
    const auto all = query_all(reg);
    CRPC_CHECK_EQ(std::size_t{2}, all.size());
}

// --- D5: a true gap after folding is rejected -------------------------------

CRPC_TEST(restore_rejects_true_gap_after_fold) {
    cl::registry reg(legacy_config());

    std::vector<stream_entry> entries;
    entries.push_back({1, 1, 1, make_instance("svc", "n1", 8001, 1)});
    entries.push_back({3, 9, 3, make_instance("svc", "n3", 8003, 3)});
    stream_log log = make_log(entries);

    const auto st = cl::restore_from_log(reg, log);
    CRPC_CHECK_FALSE(st.ok());
    if (!st.ok()) {
        // The message must name the gap: a generic rejection would let an
        // earlier check (authority, fence) masquerade as the gap rule.
        CRPC_CHECK(st.error().message.find("index gap") != std::string::npos);
    }
    // R9: the failed restore must not zero the index.
    CRPC_CHECK_EQ(u64{0}, reg.index());
}

// --- G19 step3 PR-1: a gap is only legal when a consensus record fills it ----
//
// The same shape as the case above (registry events at 1 and 3), but index 2
// exists and belongs to the consensus layer. The full index sequence is dense
// (1,2,3), so the restore must succeed: the missing index is accounted for, not
// lost. This is the case that separates "check the full sequence" (correct)
// from "check only the registry-event subsequence" (would reject a legal log)
// and from "check nothing" (would accept the truncation above).
CRPC_TEST(restore_accepts_a_raft_record_filling_the_gap) {
    cl::registry reg(legacy_config());
    mixed_log log;
    CRPC_REQUIRE(log.append(make_record(1, 1, 1,
                                        make_instance("svc", "n1", 8001, 1))).ok());
    log.append_consensus(cl::log_record_kind::raft_term, 2, 3,
                         {std::byte{0x07}});
    CRPC_REQUIRE(log.append(make_record(3, 3, 1,
                                        make_instance("svc", "n3", 8003, 1))).ok());

    const auto st = cl::restore_from_log(reg, log);
    if (!st.ok()) {
        std::fprintf(stderr, "[diag] gap-filled reason=%s msg=%s\n",
                     rejection_reason(st).c_str(), st.error().message.c_str());
    }
    CRPC_CHECK(st.ok());
    CRPC_CHECK_EQ(std::size_t{2}, query_all(reg).size());
    CRPC_CHECK_EQ(u64{3}, reg.index());
}

// --- G3: a stale log (stream behind the target) is rejected -----------------

CRPC_TEST(restore_rejects_stale_log) {
    cl::registry reg(legacy_config());

    // Bring the target to index 5 with three accepted writes.
    for (int i = 1; i <= 3; ++i) {
        const auto write = reg.register_instance(
            make_instance("svc", "n" + std::to_string(i),
                          static_cast<crpc::u16>(8000 + i), 1),
            make_ctx("n" + std::to_string(i), static_cast<u64>(i), 1));
        CRPC_REQUIRE(write.ok());
    }
    CRPC_REQUIRE(reg.index() == u64{3});

    // The stream's maximum index (2) is below the target's (3). The stream
    // must not trip any earlier rule (conflict, fence), so the records carry
    // identities the target does not hold and a fence above the target's
    // water-mark: the only thing that can reject this stream is the stale-log
    // rule itself.
    std::vector<stream_entry> entries;
    entries.push_back({1, 100, 1, make_instance("other", "x1", 9001, 1)});
    entries.push_back({2, 200, 1, make_instance("other", "x2", 9002, 1)});
    stream_log log = make_log(entries);

    const auto st = cl::restore_from_log(reg, log);
    CRPC_CHECK_FALSE(st.ok());
    if (!st.ok()) {
        CRPC_CHECK_EQ(rpc_code::failed_precondition, st.code());
    }
    CRPC_CHECK_EQ(u64{3}, reg.index());
}

// --- R4: a same-incarnation payload conflict is rejected --------------------

CRPC_TEST(restore_rejects_conflicting_replay) {
    cl::registry reg(legacy_config());

    std::vector<stream_entry> entries;
    entries.push_back({1, 1, 1, make_instance("svc", "n1", 8001, 1)});
    entries.push_back({2, 2, 1, make_instance("svc", "n1", 8002, 1)});
    stream_log log = make_log(entries);

    const auto st = cl::restore_from_log(reg, log);
    CRPC_CHECK_FALSE(st.ok());
    if (!st.ok()) {
        // Precise reason: `conflict`, not a fence or authority rejection.
        CRPC_CHECK_EQ(std::string("conflict"), rejection_reason(st));
    }
    CRPC_CHECK_EQ(u64{0}, reg.index());
}

// --- R5 dimension: the water-mark is per key, not global --------------------
//
// R5 compares the log's fence water-mark against the target's fence state. The
// comparison must be per key: `last_global_fence_` is moved by *any* write or
// token issuance for *any* key (registry.cpp:305-315, :980-981), so comparing
// the log's global maximum against it rejects a legal restore of an unrelated
// key. These two cases pin the dimension.

// P1: an unrelated key's high-water must not reject the restore. The target's
// global fence is 51 (50 allocations for "other/zzz" plus one), while the log
// carries only "api/n1" at fences 1..2 and no record for "other/zzz".
CRPC_TEST(restore_allows_unrelated_key_high_water) {
    cl::registry reg(legacy_config());

    // "other/zzz" is a key the log never mentions. Issuing tokens for it moves
    // only last_global_fence_; it must not become a water-mark for "api/n1".
    u64 other_floor = 0;
    for (int i = 0; i < 50; ++i) {
        const auto issued = reg.next_fence("other", "zzz");
        CRPC_REQUIRE(issued.ok());
        other_floor = issued.value();
    }
    CRPC_REQUIRE(other_floor >= u64{50});
    // fence() is the published water-mark, which next_fence() also moves.
    CRPC_REQUIRE(reg.fence() >= u64{50});

    // The log is ahead of the target's index (0) and carries "api/n1" only.
    std::vector<stream_entry> entries;
    entries.push_back({1, 1, 1, make_instance("api", "n1", 8001, 1)});
    entries.push_back({2, 2, 1, make_instance("api", "n1", 8001, 1)});
    stream_log log = make_log(entries);

    const auto st = cl::restore_from_log(reg, log);
    if (!st.ok()) {
        // Diagnostics, not assertions: the reason is reported so the failure
        // names the rule that rejected the stream.
        std::fprintf(stderr, "[diag] P1 reason=%s index=%llu\n",
                     rejection_reason(st).c_str(),
                     static_cast<unsigned long long>(reg.index()));
    }
    CRPC_CHECK(st.ok());
    if (!st.ok()) {
        // The unrelated key's water-mark is the only thing that could reject
        // this stream, so the reason must be the fence rule -- not stale_log,
        // not a gap, not a conflict.
        CRPC_CHECK_EQ(std::string("fence_regression"), rejection_reason(st));
    }
    CRPC_CHECK_EQ(u64{2}, reg.index());
}

// P1b: the same, but the unrelated key was actually *written* (not merely
// issued tokens). A real write moves target.index_ as well, so the stream must
// stay ahead of it: target.index_ == 1 and the log's maximum index == 2. The
// stream is dense (1,2) because registry_log::append rejects an index gap.
CRPC_TEST(restore_allows_unrelated_key_high_water_after_writes) {
    cl::registry reg(legacy_config());

    // One accepted write of "other/zzz": target.index_ becomes 1 and the
    // global fence becomes 1.
    const auto token = reg.next_fence("other", "zzz");
    CRPC_REQUIRE(token.ok());
    const auto other = reg.register_instance(
        make_instance("other", "zzz", 9001, 1),
        make_ctx("zzz", token.value(), 1));
    CRPC_REQUIRE(other.ok());
    CRPC_REQUIRE(reg.index() == u64{1});
    CRPC_REQUIRE(reg.fence() >= u64{1});

    // Push the global fence to 50 WITHOUT writing: next_fence() moves the global
    // allocator but not index_. This is what gives the case discriminating
    // power: a global-comparison R5 (the pre-fix defect) sees
    // stream_max_fence(2) < last_global_fence_(50) and must reject, while the
    // per-key R5 sees no record for "api/n1" and must accept. Without this
    // push the global water-mark would stay at 1, the defective rule would also
    // accept (2 < 1 is false), and the case would be vacuously green (A's P0).
    for (int i = 0; i < 49; ++i) {
        CRPC_REQUIRE(reg.next_fence("other", "zzz").ok());
    }
    CRPC_REQUIRE(reg.index() == u64{1});
    CRPC_REQUIRE(reg.fence() >= u64{50});

    // The log's maximum index (2) is above target.index_ (1), so G3 passes;
    // only the R5 key dimension can reject it. The log carries only "api/n1"
    // at fences 1..2, and "api/n1" has no record on the target.
    std::vector<stream_entry> entries;
    entries.push_back({1, 1, 1, make_instance("api", "n1", 8001, 1)});
    entries.push_back({2, 2, 1, make_instance("api", "n1", 8001, 1)});
    stream_log log = make_log(entries);
    const u64 target_index = reg.index();
    const u64 stream_max_index = log.last_index();
    CRPC_REQUIRE(stream_max_index > target_index);

    const auto st = cl::restore_from_log(reg, log);
    CRPC_CHECK(st.ok());
    if (!st.ok()) {
        // Explicitly rule out the stale-log rule: a rejection here would be
        // G3 (stream behind the target), not the per-key fence dimension.
        CRPC_CHECK(std::string("stale_log") != rejection_reason(st));
        CRPC_CHECK_EQ(std::string("fence_regression"), rejection_reason(st));
    }
    CRPC_CHECK_EQ(u64{2}, reg.index());
}

// --- R5: a fence regression is rejected -------------------------------------

CRPC_TEST(restore_rejects_fence_regression) {
    cl::registry reg(legacy_config());

    // The target has already accepted fence 100.
    const auto issued = reg.next_fence("svc", "n1");
    CRPC_REQUIRE(issued.ok());
    const u64 high = issued.value();
    CRPC_REQUIRE(high >= u64{1});
    // Push the target's fence water-mark to 100 explicitly.
    for (u64 i = high; i < 100; ++i) {
        const auto more = reg.next_fence("svc", "n1");
        CRPC_REQUIRE(more.ok());
    }
    CRPC_REQUIRE(reg.fence() >= u64{100});
    // A fence_barrier publishes the token without touching the catalog.
    const auto barrier = reg.fence_barrier("svc", "n1", 100, make_ctx("n1", 0));
    CRPC_REQUIRE(barrier.ok());
    CRPC_CHECK(reg.fence() >= u64{100});

    // The stream's maximum fence (50) is below the target's water-mark.
    std::vector<stream_entry> entries;
    entries.push_back({1, 50, 1, make_instance("svc", "n1", 8001, 1)});
    stream_log log = make_log(entries);

    const auto st = cl::restore_from_log(reg, log);
    CRPC_CHECK_FALSE(st.ok());
    if (!st.ok()) {
        CRPC_CHECK_EQ(rpc_code::failed_precondition, st.code());
    }
    CRPC_CHECK_EQ(u64{0}, reg.index());
}

// P3: the per-key fold must take the *maximum* fence for a key, not the first
// record's. The target's "api/n1" high-water is 30; the stream carries the same
// key at fence 20 (index 1) and fence 10 (index 2). The maximum (20) is below
// 30, so the restore must be rejected -- a first-wins fold would see 20 at
// index 1 and, if the key's record were absent, would miss the regression.
CRPC_TEST(restore_rejects_fence_regression_non_monotonic) {
    cl::registry reg(legacy_config());

    for (int i = 0; i < 30; ++i) {
        const auto issued = reg.next_fence("api", "n1");
        CRPC_REQUIRE(issued.ok());
    }
    CRPC_REQUIRE(reg.fence() >= u64{30});

    std::vector<stream_entry> entries;
    entries.push_back({1, 20, 1, make_instance("api", "n1", 8001, 1)});
    entries.push_back({2, 10, 1, make_instance("api", "n1", 8001, 1)});
    stream_log log = make_log(entries);

    const auto st = cl::restore_from_log(reg, log);
    CRPC_CHECK_FALSE(st.ok());
    if (!st.ok()) {
        CRPC_CHECK_EQ(std::string("fence_regression"), rejection_reason(st));
    }
    CRPC_CHECK_EQ(u64{0}, reg.index());
}

// P4: the per-key fold must keep the *maximum* fence of the key's records.
// Target "api/n1" is at 30; the stream has the same key at fence 20 (index 1)
// and fence 40 (index 2). The maximum (40) is above 30, so the restore is
// accepted. A first-wins fold would take 20 (the first record) and reject the
// stream as a fence regression -- a false rejection. This is the M40 killer.
CRPC_TEST(restore_allows_key_max_above_water_after_lower_first) {
    cl::registry reg(legacy_config());

    for (int i = 0; i < 30; ++i) {
        const auto issued = reg.next_fence("api", "n1");
        CRPC_REQUIRE(issued.ok());
    }
    CRPC_REQUIRE(reg.fence() >= u64{30});

    std::vector<stream_entry> entries;
    entries.push_back({1, 20, 1, make_instance("api", "n1", 8001, 1)});
    entries.push_back({2, 40, 1, make_instance("api", "n1", 8001, 1)});
    stream_log log = make_log(entries);

    const auto st = cl::restore_from_log(reg, log);
    CRPC_CHECK(st.ok());
    CRPC_CHECK_EQ(u64{2}, reg.index());
}

// --- R8: a CRC-corrupt frame surfaces replay()'s error verbatim ------------

CRPC_TEST(restore_rejects_crc_mismatch) {
    log_dir dir("crpc-sb2r-");
    CRPC_REQUIRE(dir.valid());
    const std::string path = dir.file("corrupt.log");

    u64 durable = 0;
    {
        auto opened = cl::file_registry_log::open(path);
        CRPC_REQUIRE(opened.ok());
        auto log = std::move(opened.value());
        for (u64 index = 1; index <= 2; ++index) {
            cl::service_instance instance =
                make_instance("svc", "n" + std::to_string(index),
                              static_cast<crpc::u16>(8000 + index), 1);
            const cl::log_record record = make_record(index, index, 1, instance);
            CRPC_REQUIRE(log->append(record).ok());
        }
        CRPC_REQUIRE(log->sync().ok());
        durable = log->durable_size();
    }
    CRPC_REQUIRE(durable > 40);

    // Flip one payload byte of the first record: the length prefix still
    // parses, so only the CRC can catch it.
    auto read = fs::read_all_bytes(fs::path(path));
    CRPC_REQUIRE(read.ok());
    const fs::byte_buffer& all = read.value();
    std::vector<std::byte> bytes(all.data(), all.data() + all.size());
    bytes[32] = static_cast<std::byte>(std::to_integer<unsigned>(bytes[32]) ^ 0xFFu);
    CRPC_REQUIRE(
        fs::write_all_bytes(fs::path(path),
                            crpc::span<const std::byte>(bytes.data(), bytes.size()))
            .ok());

    auto opened = cl::file_registry_log::open(path);
    CRPC_CHECK_FALSE(opened.ok());

    // An openable log whose replay fails must propagate the error unchanged.
    cl::registry reg(legacy_config());
    if (opened.ok()) {
        auto log = std::move(opened.value());
        const auto replayed = log->replay(0);
        CRPC_CHECK_FALSE(replayed.ok());
        const auto st = cl::restore_from_log(reg, *log);
        CRPC_CHECK_FALSE(st.ok());
        CRPC_CHECK_EQ(replayed.code(), st.code());
    }
    CRPC_CHECK_EQ(u64{0}, reg.index());
}

// --- R1: a torn tail is accepted as its intact prefix -----------------------

CRPC_TEST(restore_accepts_torn_tail_prefix) {
    log_dir dir("crpc-sb2r-");
    CRPC_REQUIRE(dir.valid());
    const std::string path = dir.file("torn.log");

    u64 durable = 0;
    {
        auto opened = cl::file_registry_log::open(path);
        CRPC_REQUIRE(opened.ok());
        auto log = std::move(opened.value());
        for (u64 index = 1; index <= 3; ++index) {
            cl::service_instance instance =
                make_instance("svc", "n" + std::to_string(index),
                              static_cast<crpc::u16>(8000 + index), 1);
            const cl::log_record record = make_record(index, index, 1, instance);
            CRPC_REQUIRE(log->append(record).ok());
        }
        CRPC_REQUIRE(log->sync().ok());
        durable = log->durable_size();
    }
    CRPC_REQUIRE(durable > 40);
    CRPC_REQUIRE(truncate_file_to(path, durable - 5));

    auto opened = cl::file_registry_log::open(path);
    CRPC_REQUIRE(opened.ok());
    auto log = std::move(opened.value());

    cl::registry reg(legacy_config());
    const auto st = cl::restore_from_log(reg, *log);
    CRPC_CHECK(st.ok());
    CRPC_CHECK_EQ(u64{2}, reg.index());
    cl::service_instance probe1; CRPC_CHECK(find_instance(reg, "svc", "n1", probe1));
    cl::service_instance probe2; CRPC_CHECK(find_instance(reg, "svc", "n2", probe2));
    cl::service_instance probe3; CRPC_CHECK_FALSE(find_instance(reg, "svc", "n3", probe3));
}

// --- R9: a failed restore never zeroes the index ---------------------------

CRPC_TEST(restore_failure_does_not_zero_index) {
    cl::registry reg(legacy_config());

    // Establish a non-zero index with one accepted write.
    const auto first = reg.register_instance(
        make_instance("svc", "n1", 8001, 1), make_ctx("n1", 1, 1));
    CRPC_REQUIRE(first.ok());
    const u64 before = reg.index();
    CRPC_REQUIRE(before > 0);

    // Any failing stream: a true gap.
    std::vector<stream_entry> entries;
    entries.push_back({1, 1, 1, make_instance("svc", "n1", 8001, 1)});
    entries.push_back({9, 9, 9, make_instance("svc", "n9", 8009, 9)});
    stream_log log = make_log(entries);

    const auto st = cl::restore_from_log(reg, log);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK_EQ(before, reg.index());
}

// --- G5: an empty stream is a successful no-op -----------------------------

CRPC_TEST(restore_empty_log_is_noop) {
    cl::registry reg(legacy_config());
    stream_log log;

    const auto st = cl::restore_from_log(reg, log);
    CRPC_CHECK(st.ok());
    CRPC_CHECK_EQ(u64{0}, reg.index());
}

// --- G6: at least three fence-increasing records all apply -----------------

CRPC_TEST(restore_applies_at_least_three_records) {
    cl::registry reg(legacy_config());

    std::vector<stream_entry> entries;
    entries.push_back({1, 1, 1, make_instance("svc", "n1", 8001, 1)});
    entries.push_back({2, 2, 2, make_instance("svc", "n2", 8002, 2)});
    entries.push_back({3, 3, 3, make_instance("svc", "n3", 8003, 3)});
    stream_log log = make_log(entries);

    const auto st = cl::restore_from_log(reg, log);
    CRPC_CHECK(st.ok());
    CRPC_CHECK_EQ(u64{3}, reg.index());
    cl::service_instance probe1; CRPC_CHECK(find_instance(reg, "svc", "n1", probe1));
    cl::service_instance probe2; CRPC_CHECK(find_instance(reg, "svc", "n2", probe2));
    cl::service_instance probe3; CRPC_CHECK(find_instance(reg, "svc", "n3", probe3));
    CRPC_CHECK_EQ(std::size_t{3}, query_all(reg).size());
}

// --- A8: the restore must not consume a future issuance --------------------

CRPC_TEST(restore_does_not_consume_future_issuance) {
    cl::registry reg(legacy_config());

    std::vector<stream_entry> entries;
    entries.push_back({1, 1, 1, make_instance("svc", "n1", 8001, 1)});
    entries.push_back({2, 2, 1, make_instance("svc", "n2", 8002, 1)});
    entries.push_back({3, 3, 1, make_instance("svc", "n3", 8003, 1)});
    stream_log log = make_log(entries);

    const auto st = cl::restore_from_log(reg, log);
    CRPC_REQUIRE(st.ok());
    const u64 stream_max_fence = 3;

    const auto next = reg.next_fence("svc", "n1");
    CRPC_REQUIRE(next.ok());
    CRPC_CHECK(next.value() > stream_max_fence);
    CRPC_CHECK(reg.fence() >= next.value());
}

// --- G19 step1: the recorded epoch floor survives a restore ------------------

CRPC_TEST(epoch_floor_survives_restore) {
    // A key whose log record carries fence N must come back with an epoch floor
    // of N, not 0. With 0 the pre-restart writer (epoch <= N) could overwrite
    // the restored value, which is exactly the bug M37 describes.
    //
    // simple_stream(): n1 has fence 1, n2 has fence 2. So n1's floor is 1 and
    // n2's floor is 2. A writer at an epoch strictly below the floor is stale;
    // the same epoch is still accepted (only a strictly older epoch yields).
    cl::registry reg;
    stream_log log = make_log(simple_stream());
    const auto restored = cl::restore_from_log(reg, log);
    CRPC_REQUIRE(restored.ok());

    // n2 carries the higher fence (2), so it is the key that discriminates:
    // epoch 1 is strictly below 2 and must be refused.
    cl::write_context stale = make_ctx("n2", 0, 1);
    stale.fence = reg.next_fence("svc", "n2").value();
    stale.writer_epoch = 1;
    CRPC_CHECK_EQ(std::string("stale_writer"),
                  rejection_reason(reg.set_state("svc", "n2",
                                                 cl::instance_state::critical,
                                                 stale)));

    // n1's floor is exactly its own fence (1): the same epoch may still write.
    cl::write_context same = make_ctx("n1", 0, 1);
    same.fence = reg.next_fence("svc", "n1").value();
    same.writer_epoch = 1;
    CRPC_CHECK(reg.set_state("svc", "n1", cl::instance_state::critical, same).ok());

    // ...while a strictly newer epoch is accepted on either key.
    cl::write_context newer = make_ctx("n2", 0, 1);
    newer.fence = reg.next_fence("svc", "n2").value();
    newer.writer_epoch = reg.next_writer_epoch().value();
    CRPC_CHECK(reg.set_state("svc", "n2", cl::instance_state::critical, newer).ok());
}

// --- G19 step3 PR-1: the consensus layer's records live in the same log -----
//
// `registry_log.hpp:64-73` designs four record kinds (registry_event, raft_term,
// raft_vote, raft_snapshot, raft_entry) and says the consensus layer persists
// its own kinds in the same log "so that a node has one durability boundary
// instead of two". `restore_from_log` therefore reads only `registry_event`
// records: the raft records carry no catalog mutation, and their payloads are
// not in the registry event codec, so treating them as events is a hard failure
// (S-1). These cases pin both halves of that contract -- the skip and the fact
// that the skip is by kind, not a blanket "ignore decode errors".

// T1: a stream of registry events interleaved with every raft kind restores
// successfully, and only the registry events reach the catalog.
CRPC_TEST(restore_skips_non_registry_kinds) {
    cl::registry reg(legacy_config());
    mixed_log log;
    // index 1: a legal registry event.
    CRPC_REQUIRE(log.append(make_record(1, 1, 1,
                                        make_instance("svc", "n1", 8001, 1))).ok());
    // index 2: a raft term record (opaque payload).
    log.append_consensus(cl::log_record_kind::raft_term, 2, 7,
                         {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}});
    // index 3: a raft vote record.
    log.append_consensus(cl::log_record_kind::raft_vote, 3, 7,
                         {std::byte{0xAA}, std::byte{0xBB}});
    // index 4: a legal registry event.
    CRPC_REQUIRE(log.append(make_record(4, 4, 1,
                                        make_instance("svc", "n2", 8002, 1))).ok());
    // index 5: a raft snapshot record.
    log.append_consensus(cl::log_record_kind::raft_snapshot, 5, 9,
                         {std::byte{0x00}, std::byte{0xFF}, std::byte{0x10}});
    // index 6: a raft entry record.
    log.append_consensus(cl::log_record_kind::raft_entry, 6, 9,
                         {std::byte{0xDE}, std::byte{0xAD}});

    const auto st = cl::restore_from_log(reg, log);
    if (!st.ok()) {
        std::fprintf(stderr, "[diag] T1 reason=%s msg=%s\n",
                     rejection_reason(st).c_str(), st.error().message.c_str());
    }
    CRPC_CHECK(st.ok());
    // Only the two registry events are applied.
    cl::service_instance probe1;
    cl::service_instance probe2;
    CRPC_CHECK(find_instance(reg, "svc", "n1", probe1));
    CRPC_CHECK(find_instance(reg, "svc", "n2", probe2));
    CRPC_CHECK_EQ(std::size_t{2}, query_all(reg).size());
    // The catalog index is the log's high-water (6), not the count of applied
    // registry events (2): the raft records still occupy index space, so a
    // later append must land after them rather than overwrite one.
    CRPC_CHECK_EQ(u64{6}, reg.index());
}

// T2: the skip is by kind. A record that *claims* to be a registry event but
// carries an undecodable payload must still be rejected -- otherwise "skip the
// raft records" would have become "skip every record that fails to decode",
// which is a fail-open hole: a corrupted event would be silently dropped.
CRPC_TEST(restore_still_rejects_a_malformed_registry_event) {
    cl::registry reg(legacy_config());
    mixed_log log;
    // A registry_event whose payload is not the event codec. The body's version
    // byte is 0x63 (not kVersion=1), so the decoder rejects it; the header is a
    // well-formed fence/incarnation pair so only the version gate can fire.
    std::vector<std::byte> garbage(32, std::byte{0x00});
    garbage[16] = std::byte{0x63};  // version
    garbage[17] = std::byte{0x00};  // state = passing
    log.append_consensus(cl::log_record_kind::registry_event, 1, 0, garbage);

    const auto st = cl::restore_from_log(reg, log);
    if (st.ok()) {
        std::fprintf(stderr, "[diag] T2 accepted a malformed registry_event\n");
    }
    CRPC_CHECK_FALSE(st.ok());
    if (!st.ok()) {
        // Precise reason: the decode rule, not a gap, a fence or a conflict.
        CRPC_CHECK_EQ(rpc_code::invalid_argument, st.code());
        CRPC_CHECK_EQ(std::string("invalid"), rejection_reason(st));
    }
    // R9: the failed restore must not zero or move the index.
    CRPC_CHECK_EQ(u64{0}, reg.index());
}


// T4: the log's high-water index is the last record of ANY kind. If a trailing
// raft record set the index, a restore that used the folded maximum would leave
// the registry behind the log, and the next append would land on the raft
// record's index and truncate it -- silent data loss in the durability boundary.
CRPC_TEST(restore_keeps_the_log_high_water_past_a_trailing_raft_record) {
    cl::registry reg(legacy_config());
    mixed_log log;
    CRPC_REQUIRE(log.append(make_record(1, 1, 1,
                                        make_instance("svc", "n1", 8001, 1))).ok());
    // The trailing record is a raft entry: the registry-event subsequence ends
    // at index 1, but the log's high-water is 2.
    log.append_consensus(cl::log_record_kind::raft_entry, 2, 5,
                         {std::byte{0x42}, std::byte{0x43}});

    const auto st = cl::restore_from_log(reg, log);
    if (!st.ok()) {
        std::fprintf(stderr, "[diag] T4 reason=%s msg=%s\n",
                     rejection_reason(st).c_str(), st.error().message.c_str());
    }
    CRPC_REQUIRE(st.ok());
    // The catalog index must be the log high-water (2), not the folded
    // registry-event maximum (1).
    CRPC_CHECK_EQ(u64{2}, reg.index());

    // The consequence that matters: a later registry append at index 3 must be
    // accepted, and it must not be reported as a conflict overwrite of the
    // raft record (which would be the outcome if index_ had stayed at 1).
    const auto next = reg.next_fence("svc", "n2");
    CRPC_REQUIRE(next.ok());
    const auto written = reg.register_instance(
        make_instance("svc", "n2", 8002, 1), make_ctx("n2", next.value(), 1));
    if (!written.ok()) {
        std::fprintf(stderr, "[diag] T4 append reason=%s\n",
                     rejection_reason(written).c_str());
    }
    CRPC_REQUIRE(written.ok());
    CRPC_CHECK_EQ(u64{3}, written.value().index);
    // Both rows are present: the append neither overwrote nor displaced n1.
    cl::service_instance probe1;
    cl::service_instance probe2;
    CRPC_CHECK(find_instance(reg, "svc", "n1", probe1));
    CRPC_CHECK(find_instance(reg, "svc", "n2", probe2));
}

// T5: a raft-only stream is not an error, and the registry-event subsequence is
// dense from its own first record. Here the log holds exactly one registry
// event at index 3 (the two records before it are the consensus layer's). G1
// must accept a single-event subsequence regardless of where it starts, because
// the log's index space is shared.
CRPC_TEST(restore_accepts_a_single_registry_event_after_leading_raft_records) {
    cl::registry reg(legacy_config());
    mixed_log log;
    log.append_consensus(cl::log_record_kind::raft_term, 1, 1,
                         {std::byte{0x01}});
    log.append_consensus(cl::log_record_kind::raft_vote, 2, 2,
                         {std::byte{0x02}, std::byte{0x03}});
    CRPC_REQUIRE(log.append(make_record(3, 3, 1,
                                        make_instance("svc", "n1", 8001, 1))).ok());

    const auto st = cl::restore_from_log(reg, log);
    if (!st.ok()) {
        std::fprintf(stderr, "[diag] T5 reason=%s msg=%s\n",
                     rejection_reason(st).c_str(), st.error().message.c_str());
    }
    CRPC_CHECK(st.ok());
    CRPC_CHECK_EQ(std::size_t{1}, query_all(reg).size());
    // The catalog index is the log high-water (3), not the folded count (1).
    CRPC_CHECK_EQ(u64{3}, reg.index());
}

CRPC_TEST_MAIN()
