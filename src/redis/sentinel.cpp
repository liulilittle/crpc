// CRPC -- Redis Sentinel implementation.
#include <crpc/redis/sentinel.hpp>

#include <crpc/redis/connection.hpp>
#include <crpc/redis/resp.hpp>
#include <crpc/runtime.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace crpc {
namespace redis {

std::string sentinel_address::to_string() const {
    return host + ":" + std::to_string(port);
}

const sentinel_reply& sentinel_reply::at(std::size_t index) const {
    if (index >= elements.size()) {
        throw std::out_of_range("sentinel_reply::at");
    }
    return elements[index];
}

std::string sentinel_reply::as_string() const {
    if (type == sentinel_reply_type::integer) {
        return std::to_string(integer);
    }
    return text;
}

namespace {

using steady_clock = std::chrono::steady_clock;
using milliseconds = std::chrono::milliseconds;
using nanoseconds = std::chrono::nanoseconds;

constexpr milliseconds kMinimumPollInterval{1};
constexpr milliseconds kMinimumFailoverTimeout{1};
constexpr nanoseconds kSleepSlice{25};

bool parse_u64(const std::string& text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool parse_u16(const std::string& text, std::uint16_t& out) {
    std::uint64_t value = 0;
    if (!parse_u64(text, value) || value == 0 || value > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

bool parse_count(const std::string& text, std::size_t& out) {
    std::uint64_t value = 0;
    if (!parse_u64(text, value)) {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

bool parse_i64(const std::string& text, std::int64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        index = 1;
    }
    if (index >= text.size()) {
        return false;
    }
    std::uint64_t value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    const std::uint64_t limit =
        negative ? static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max()) +
                       1u
                 : static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max());
    if (value > limit) {
        return false;
    }
    if (negative) {
        // INT64_MIN is the one value whose magnitude exceeds INT64_MAX.
        if (value == limit) {
            out = std::numeric_limits<std::int64_t>::min();
        } else {
            out = -static_cast<std::int64_t>(value);
        }
    } else {
        out = static_cast<std::int64_t>(value);
    }
    return true;
}

// A `SENTINEL master`/`SENTINEL replicas` map is a flat array of alternating
// key/value bulk strings.
std::string field_value(const sentinel_reply& map, const std::string& key) {
    if (map.type != sentinel_reply_type::array) {
        return {};
    }
    for (std::size_t i = 0; i + 1 < map.elements.size(); i += 2) {
        if (map.elements[i].as_string() == key) {
            return map.elements[i + 1].as_string();
        }
    }
    return {};
}

sentinel_address parse_master_addr(const sentinel_reply& reply) {
    sentinel_address address;
    if (reply.type != sentinel_reply_type::array || reply.elements.size() < 2) {
        return address;
    }
    address.host = reply.elements[0].as_string();
    (void)parse_u16(reply.elements[1].as_string(), address.port);
    if (!address.valid()) {
        return sentinel_address{};
    }
    return address;
}

sentinel_address parse_master_config(const sentinel_reply& reply,
                                     std::uint64_t& config_epoch) {
    sentinel_address address;
    address.host = field_value(reply, "ip");
    (void)parse_u16(field_value(reply, "port"), address.port);
    std::uint64_t epoch = 0;
    if (parse_u64(field_value(reply, "config-epoch"), epoch)) {
        config_epoch = epoch;
    }
    if (!address.valid()) {
        return sentinel_address{};
    }
    return address;
}

struct replica_view {
    sentinel_address address;
    std::string flags;
    int priority = 100;
    std::int64_t offset = 0;
};

bool replica_is_selectable(const replica_view& replica) {
    if (!replica.address.valid() || replica.priority <= 0) {
        return false;
    }
    if (replica.flags.find("s_down") != std::string::npos ||
        replica.flags.find("o_down") != std::string::npos ||
        replica.flags.find("disconnected") != std::string::npos ||
        replica.flags.find("master") != std::string::npos) {
        return false;
    }
    return true;
}

bool replica_is_better(const replica_view& a, const replica_view& b) {
    if (a.priority != b.priority) {
        return a.priority < b.priority;
    }
    if (a.offset != b.offset) {
        return a.offset > b.offset;
    }
    return a.address < b.address;
}

sentinel_address select_candidate(const std::vector<replica_view>& replicas) {
    const replica_view* best = nullptr;
    for (const replica_view& replica : replicas) {
        if (!replica_is_selectable(replica)) {
            continue;
        }
        if (best == nullptr || replica_is_better(replica, *best)) {
            best = &replica;
        }
    }
    return best != nullptr ? best->address : sentinel_address{};
}

std::vector<replica_view> parse_replicas(const sentinel_reply& reply) {
    std::vector<replica_view> out;
    if (reply.type != sentinel_reply_type::array) {
        return out;
    }
    out.reserve(reply.elements.size());
    for (const sentinel_reply& element : reply.elements) {
        replica_view view;
        view.address.host = field_value(element, "ip");
        (void)parse_u16(field_value(element, "port"), view.address.port);
        view.flags = field_value(element, "flags");
        std::string priority = field_value(element, "slave-priority");
        if (priority.empty()) {
            priority = field_value(element, "replica-priority");
        }
        std::int64_t parsed_priority = 0;
        if (parse_i64(priority, parsed_priority)) {
            view.priority = static_cast<int>(parsed_priority);
        }
        std::string offset = field_value(element, "slave-repl-offset");
        if (offset.empty()) {
            offset = field_value(element, "replica-repl-offset");
        }
        (void)parse_i64(offset, view.offset);
        if (view.address.valid()) {
            out.push_back(std::move(view));
        }
    }
    return out;
}

std::vector<sentinel_address> parse_sentinels(const sentinel_reply& reply) {
    std::vector<sentinel_address> out;
    if (reply.type != sentinel_reply_type::array) {
        return out;
    }
    out.reserve(reply.elements.size());
    for (const sentinel_reply& element : reply.elements) {
        sentinel_address address;
        address.host = field_value(element, "ip");
        (void)parse_u16(field_value(element, "port"), address.port);
        if (address.valid()) {
            out.push_back(std::move(address));
        }
    }
    return out;
}

// `ROLE` on a master reports `[ "master", offset, [ [ip, port, offset], ... ] ]`.
std::vector<replica_view> parse_role_replicas(const sentinel_reply& reply) {
    std::vector<replica_view> out;
    if (reply.type != sentinel_reply_type::array || reply.elements.size() < 3) {
        return out;
    }
    const sentinel_reply& list = reply.elements[2];
    if (list.type != sentinel_reply_type::array) {
        return out;
    }
    out.reserve(list.elements.size());
    for (const sentinel_reply& entry : list.elements) {
        if (entry.type != sentinel_reply_type::array || entry.elements.size() < 2) {
            continue;
        }
        replica_view view;
        view.address.host = entry.elements[0].as_string();
        (void)parse_u16(entry.elements[1].as_string(), view.address.port);
        view.flags = "slave";
        if (entry.elements.size() >= 3) {
            (void)parse_i64(entry.elements[2].as_string(), view.offset);
        }
        if (view.address.valid()) {
            out.push_back(std::move(view));
        }
    }
    return out;
}

bool reply_is_pong(const sentinel_reply& reply) {
    if (reply.type == sentinel_reply_type::status ||
        reply.type == sentinel_reply_type::bulk) {
        return reply.text == "PONG" || reply.text == "pong";
    }
    return false;
}

bool role_reports_master(const sentinel_reply& reply) {
    return reply.ok() && reply.type == sentinel_reply_type::array &&
           !reply.elements.empty() &&
           reply.elements[0].as_string() == "master";
}

sentinel_reply transport_error_reply(std::string message) {
    sentinel_reply reply;
    reply.type = sentinel_reply_type::error;
    reply.text = std::move(message);
    reply.transport_failure = true;
    return reply;
}

sentinel_reply convert_resp(const resp_value& value) {
    sentinel_reply reply;
    switch (value.type()) {
    case resp_type::null_value:
        return reply;
    case resp_type::simple_string:
        reply.type = sentinel_reply_type::status;
        reply.text = value.as_simple_string();
        return reply;
    case resp_type::error:
        reply.type = sentinel_reply_type::error;
        reply.text = value.as_error();
        return reply;
    case resp_type::integer:
        reply.type = sentinel_reply_type::integer;
        reply.integer = value.as_integer();
        return reply;
    case resp_type::bulk_string:
        reply.type = sentinel_reply_type::bulk;
        reply.text = value.as_bulk_string();
        return reply;
    case resp_type::array:
        reply.type = sentinel_reply_type::array;
        for (const resp_value& item : value.as_array()) {
            reply.elements.push_back(convert_resp(item));
        }
        return reply;
    case resp_type::boolean:
        reply.type = sentinel_reply_type::integer;
        reply.integer = value.as_boolean() ? 1 : 0;
        return reply;
    case resp_type::double_value:
        reply.type = sentinel_reply_type::bulk;
        reply.text = std::to_string(value.as_double());
        return reply;
    case resp_type::big_number:
        reply.type = sentinel_reply_type::bulk;
        reply.text = value.as_big_number();
        return reply;
    case resp_type::blob_error:
        reply.type = sentinel_reply_type::error;
        reply.text = value.as_blob_error();
        return reply;
    case resp_type::verbatim_string:
        reply.type = sentinel_reply_type::bulk;
        reply.text = value.as_verbatim_string();
        return reply;
    case resp_type::map_value:
        // RESP3 maps are flattened to alternating key/value entries so the
        // `field_value` helper works for both protocol versions.
        reply.type = sentinel_reply_type::array;
        for (const auto& entry : value.as_map()) {
            reply.elements.push_back(convert_resp(entry.first));
            reply.elements.push_back(convert_resp(entry.second));
        }
        return reply;
    case resp_type::set_value:
        reply.type = sentinel_reply_type::array;
        for (const resp_value& item : value.as_set()) {
            reply.elements.push_back(convert_resp(item));
        }
        return reply;
    case resp_type::push:
        reply.type = sentinel_reply_type::array;
        for (const resp_value& item : value.as_push()) {
            reply.elements.push_back(convert_resp(item));
        }
        return reply;
    }
    return reply;
}

std::string generate_run_id(const void* seed) {
    const auto ticks = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto address = static_cast<unsigned long long>(
        reinterpret_cast<std::uintptr_t>(seed));
    char buffer[41] = {};
    (void)std::snprintf(buffer, sizeof(buffer), "%016llx%016llx%08llx", ticks,
                        address, (ticks ^ address) & 0xffffffffull);
    return std::string(buffer);
}

sentinel_event make_event(sentinel_event_type type, const std::string& name,
                          const sentinel_address& master,
                          const sentinel_address& node,
                          std::string detail) {
    sentinel_event event;
    event.type = type;
    event.master_name = name;
    event.master = master;
    event.node = node;
    event.detail = std::move(detail);
    return event;
}

enum class failover_phase : std::uint8_t { none, promoting };

struct poll_snapshot {
    sentinel_address master;
    std::vector<sentinel_address> peers;
    sentinel_health health = sentinel_health::unknown;
    failover_phase phase = failover_phase::none;
    sentinel_address candidate;
    sentinel_address failover_old_master;
    steady_clock::time_point failover_start{};
    std::uint64_t config_epoch = 0;
};

// The best master configuration observed across every reachable peer. Peers
// are compared by config epoch first, then by address as a deterministic
// tie-break, so all sentinels converge on the same configuration even when
// they have different peer sets.
struct master_config {
    sentinel_address address;
    std::uint64_t config_epoch = 0;
    std::size_t num_other_sentinels = 0;
    bool valid = false;
};

// True when `candidate` should replace `best` as the winning configuration.
bool config_is_better(const master_config& candidate,
                      const master_config& best) {
    if (!best.valid) {
        return true;
    }
    if (candidate.config_epoch != best.config_epoch) {
        return candidate.config_epoch > best.config_epoch;
    }
    return candidate.address < best.address;
}

} // namespace

struct sentinel::impl : std::enable_shared_from_this<impl> {
    impl(sentinel_options options, sentinel_command_executor& command_executor)
        : opt(std::move(options)), exec(command_executor) {
        if (opt.master_name.empty()) {
            opt.master_name = "mymaster";
        }
        if (opt.quorum == 0) {
            opt.quorum = 1;
        }
        if (opt.down_after.count() < 0) {
            opt.down_after = milliseconds{0};
        }
        if (opt.failover_timeout < kMinimumFailoverTimeout) {
            opt.failover_timeout = kMinimumFailoverTimeout;
        }
        if (opt.poll_interval < kMinimumPollInterval) {
            opt.poll_interval = kMinimumPollInterval;
        }
        master = opt.master;
        for (const sentinel_address& address : opt.sentinels) {
            if (address.valid() && address != opt.self &&
                std::find(peers.begin(), peers.end(), address) == peers.end()) {
                peers.push_back(address);
            }
        }
        run_id = opt.run_id.empty() ? generate_run_id(this) : opt.run_id;
    }

    ~impl() = default;

    sentinel_reply execute(const sentinel_address& target,
                           std::vector<std::string> command) {
        return exec.execute(target, command);
    }

    void emit(const sentinel_event& event) {
        event_handler callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = handler;
        }
        if (callback) {
            callback(event);
        }
    }

    poll_snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        poll_snapshot state;
        state.master = master;
        state.peers = peers;
        state.health = health;
        state.phase = phase;
        state.candidate = candidate;
        state.failover_old_master = failover_old_master;
        state.failover_start = failover_start;
        state.config_epoch = config_epoch;
        return state;
    }

    // Queries every peer and keeps the best configuration, so a stale peer
    // that happens to be listed first cannot mask a newer config epoch from an
    // authoritative peer (the "first valid peer wins" bug).
    master_config refresh_master(
        const std::vector<sentinel_address>& peers_snapshot) {
        master_config best;
        for (const sentinel_address& peer : peers_snapshot) {
            if (!peer.valid() || peer == opt.self) {
                continue;
            }
            const sentinel_reply config =
                execute(peer, {"SENTINEL", "master", opt.master_name});
            if (config.ok() && config.type == sentinel_reply_type::array) {
                master_config observed;
                observed.address =
                    parse_master_config(config, observed.config_epoch);
                (void)parse_count(field_value(config, "num-other-sentinels"),
                                  observed.num_other_sentinels);
                if (observed.address.valid()) {
                    observed.valid = true;
                    if (config_is_better(observed, best)) {
                        best = std::move(observed);
                    }
                    continue;
                }
            }
            const sentinel_reply legacy = execute(
                peer, {"SENTINEL", "get-master-addr-by-name", opt.master_name});
            if (legacy.ok()) {
                master_config observed;
                observed.address = parse_master_addr(legacy);
                if (observed.address.valid()) {
                    observed.valid = true;
                    if (config_is_better(observed, best)) {
                        best = std::move(observed);
                    }
                }
            }
        }
        return best;
    }

    void discover_topology(const std::vector<sentinel_address>& peers_snapshot,
                           const sentinel_address& master_snapshot) {
        std::vector<sentinel_address> found_sentinels;
        std::vector<replica_view> found_replicas;
        bool replicas_received = false;

        for (const sentinel_address& peer : peers_snapshot) {
            if (!peer.valid() || peer == opt.self) {
                continue;
            }
            if (!replicas_received) {
                sentinel_reply reply =
                    execute(peer, {"SENTINEL", "replicas", opt.master_name});
                if (!reply.ok() || reply.type != sentinel_reply_type::array) {
                    reply = execute(peer, {"SENTINEL", "slaves", opt.master_name});
                }
                if (reply.ok() && reply.type == sentinel_reply_type::array) {
                    found_replicas = parse_replicas(reply);
                    replicas_received = true;
                }
            }
            if (found_sentinels.empty()) {
                const sentinel_reply reply =
                    execute(peer, {"SENTINEL", "sentinels", opt.master_name});
                if (reply.ok() && reply.type == sentinel_reply_type::array) {
                    found_sentinels = parse_sentinels(reply);
                }
            }
            if (replicas_received && !found_sentinels.empty()) {
                break;
            }
        }

        if (!replicas_received && master_snapshot.valid()) {
            const sentinel_reply role = execute(master_snapshot, {"ROLE"});
            if (role_reports_master(role)) {
                found_replicas = parse_role_replicas(role);
                replicas_received = true;
            }
        }

        std::lock_guard<std::mutex> lock(mutex);
        for (const sentinel_address& address : found_sentinels) {
            if (!address.valid() || address == opt.self) {
                continue;
            }
            const bool known =
                std::find(peers.begin(), peers.end(), address) != peers.end() ||
                std::find(peers_snapshot.begin(), peers_snapshot.end(), address) !=
                    peers_snapshot.end();
            if (!known) {
                peers.push_back(address);
            }
        }
        // A peer that answers with an empty array (stale, or still loading its
        // view) must not wipe a replica list another peer already provided.
        if (replicas_received && (!found_replicas.empty() || replicas.empty())) {
            replicas = std::move(found_replicas);
        }
    }

    void poll_demotions(const sentinel_address& current_master) {
        std::vector<sentinel_address> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending = demote;
        }
        if (pending.empty() || !current_master.valid()) {
            return;
        }

        std::vector<sentinel_address> remaining;
        for (const sentinel_address& address : pending) {
            if (!address.valid() || address == current_master) {
                continue;
            }
            if (!reply_is_pong(execute(address, {"PING"}))) {
                remaining.push_back(address);
                continue;
            }
            const sentinel_reply reply =
                execute(address, {"REPLICAOF", current_master.host,
                                  std::to_string(current_master.port)});
            if (!reply.ok()) {
                remaining.push_back(address);
                continue;
            }
            emit(make_event(sentinel_event_type::master_demoted, opt.master_name,
                            current_master, address,
                            "old master reconfigured as replica"));
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            demote = std::move(remaining);
        }
    }

    void abort_failover(const std::string& reason) {
        sentinel_address current_master;
        {
            std::lock_guard<std::mutex> lock(mutex);
            current_master = master;
            phase = failover_phase::none;
            candidate = sentinel_address{};
            failover_old_master = sentinel_address{};
            failover_active = false;
            leader = false;
        }
        emit(make_event(sentinel_event_type::failover_aborted, opt.master_name,
                        current_master, sentinel_address{}, reason));
    }

    void complete_failover(const sentinel_address& old_master,
                           const sentinel_address& new_master) {
        std::vector<replica_view> replicas_snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            replicas_snapshot = replicas;
            master = new_master;
            // A failover must leave the configuration epoch at least as high
            // as the election epoch, and keep the election epoch in step, so a
            // later election cannot present an epoch a peer already voted at.
            config_epoch = std::max(config_epoch + 1, epoch);
            epoch = std::max(epoch, config_epoch);
            health = sentinel_health::up;
            down_since_set = false;
            leader = false;
            phase = failover_phase::none;
            candidate = sentinel_address{};
            failover_old_master = sentinel_address{};
            failover_active = false;
            if (old_master.valid() && old_master != new_master &&
                std::find(demote.begin(), demote.end(), old_master) ==
                    demote.end()) {
                demote.push_back(old_master);
            }
        }

        emit(make_event(sentinel_event_type::promoted, opt.master_name, new_master,
                        new_master, "replica promoted to master"));

        for (const replica_view& replica : replicas_snapshot) {
            if (!replica.address.valid() || replica.address == new_master) {
                continue;
            }
            if (!replica_is_selectable(replica)) {
                continue;
            }
            const sentinel_reply reply =
                execute(replica.address, {"REPLICAOF", new_master.host,
                                          std::to_string(new_master.port)});
            if (reply.ok()) {
                emit(make_event(sentinel_event_type::replica_reconfigured,
                                opt.master_name, new_master, replica.address,
                                "replica points at the new master"));
            }
        }

        sentinel_event event = make_event(sentinel_event_type::master_switch,
                                          opt.master_name, new_master, new_master,
                                          "failover");
        event.previous_master = old_master;
        emit(event);
    }

    void begin_failover(const sentinel_address& old_master,
                        steady_clock::time_point now) {
        std::vector<replica_view> replicas_snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            replicas_snapshot = replicas;
        }
        const sentinel_address candidate_address =
            select_candidate(replicas_snapshot);

        sentinel_event started =
            make_event(sentinel_event_type::failover_started, opt.master_name,
                       old_master, candidate_address,
                       candidate_address.valid() ? candidate_address.to_string()
                                                 : "no suitable replica");
        emit(started);

        if (!candidate_address.valid()) {
            abort_failover("no suitable replica");
            return;
        }

        const sentinel_reply promotion =
            execute(candidate_address, {"REPLICAOF", "NO", "ONE"});
        if (!promotion.ok()) {
            abort_failover("promotion command failed: " + promotion.text);
            return;
        }

        if (role_reports_master(execute(candidate_address, {"ROLE"}))) {
            complete_failover(old_master, candidate_address);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            phase = failover_phase::promoting;
            candidate = candidate_address;
            failover_old_master = old_master;
            failover_start = now;
            failover_active = true;
        }
    }

    void poll_promotion(const poll_snapshot& state,
                        steady_clock::time_point now) {
        if (role_reports_master(execute(state.candidate, {"ROLE"}))) {
            complete_failover(state.failover_old_master, state.candidate);
            return;
        }
        if (now - state.failover_start >= opt.failover_timeout) {
            abort_failover("promotion timed out");
        }
    }

    void poll_at(steady_clock::time_point now) {
        const poll_snapshot state = snapshot();
        if (state.phase == failover_phase::promoting) {
            poll_promotion(state, now);
            return;
        }

        // Refresh the master view before demoting any old master: the demotion
        // must point the old master at the *new* master, not at a stale one that
        // is about to be replaced in this same pass.
        sentinel_address current_master = state.master;
        const master_config discovered = refresh_master(state.peers);
        if (discovered.valid) {
            // A higher config epoch always wins. At an equal epoch a different
            // address is adopted while the current master is unhealthy, or as a
            // deterministic tie-break (smaller address) so two sentinels that
            // see the same split-brain converge instead of each keeping a
            // healthy-looking master forever.
            const bool adopt =
                !current_master.valid() ||
                (discovered.address != current_master &&
                 (discovered.config_epoch > state.config_epoch ||
                  (discovered.config_epoch == state.config_epoch &&
                   (state.health != sentinel_health::up ||
                    discovered.address < current_master))));
            if (adopt) {
                const sentinel_address previous = current_master;
                current_master = discovered.address;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    master = discovered.address;
                    if (discovered.config_epoch > config_epoch) {
                        config_epoch = discovered.config_epoch;
                    }
                    // The election epoch must never trail a configuration this
                    // sentinel has accepted, or peers will reject its votes.
                    if (config_epoch > epoch) {
                        epoch = config_epoch;
                    }
                    if (discovered.num_other_sentinels > known_sentinels) {
                        known_sentinels = discovered.num_other_sentinels;
                    }
                    health = sentinel_health::unknown;
                    down_since_set = false;
                    leader = false;
                    if (previous.valid() && previous != discovered.address &&
                        std::find(demote.begin(), demote.end(), previous) ==
                            demote.end()) {
                        demote.push_back(previous);
                    }
                }
                if (previous.valid()) {
                    sentinel_event event = make_event(
                        sentinel_event_type::master_switch, opt.master_name,
                        discovered.address, discovered.address,
                        "sentinel configuration");
                    event.previous_master = previous;
                    emit(event);
                }
            } else if (discovered.address == current_master &&
                       discovered.config_epoch > state.config_epoch) {
                std::lock_guard<std::mutex> lock(mutex);
                if (discovered.config_epoch > config_epoch) {
                    config_epoch = discovered.config_epoch;
                }
                if (config_epoch > epoch) {
                    epoch = config_epoch;
                }
                if (discovered.num_other_sentinels > known_sentinels) {
                    known_sentinels = discovered.num_other_sentinels;
                }
            }
        }

        poll_demotions(current_master);

        if (!current_master.valid()) {
            discover_topology(state.peers, current_master);
            return;
        }

        const bool pong = reply_is_pong(execute(current_master, {"PING"}));
        bool sdown_now = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (pong) {
                health = sentinel_health::up;
                down_since_set = false;
                leader = false;
            } else {
                if (!down_since_set) {
                    down_since = now;
                    down_since_set = true;
                }
                if (now - down_since >= opt.down_after &&
                    health != sentinel_health::subjectively_down &&
                    health != sentinel_health::objectively_down) {
                    health = sentinel_health::subjectively_down;
                    sdown_now = true;
                } else if (health == sentinel_health::unknown) {
                    // Not down yet: the master keeps its last known good
                    // state until down-after elapses.
                    health = sentinel_health::up;
                }
            }
        }
        if (sdown_now) {
            emit(make_event(sentinel_event_type::sdown, opt.master_name,
                            current_master, current_master,
                            "master is subjectively down"));
        }

        discover_topology(state.peers, current_master);

        sentinel_health current_health;
        std::vector<sentinel_address> current_peers;
        std::uint64_t vote_epoch = 0;
        std::string identity;
        {
            std::lock_guard<std::mutex> lock(mutex);
            current_health = health;
            current_peers = peers;
            vote_epoch = epoch;
            identity = run_id;
        }
        if (current_health != sentinel_health::subjectively_down &&
            current_health != sentinel_health::objectively_down) {
            return;
        }

        // --- ODOWN probe: no vote is cast ---
        // Only the down/up answer matters here, so the run id is "*". Sending
        // this sentinel's identity would make every peer spend its one vote for
        // the epoch on a sentinel that may never fail over, starving a real
        // election later.
        std::size_t agree = 1;
        for (const sentinel_address& peer : current_peers) {
            if (!peer.valid() || peer == opt.self) {
                continue;
            }
            const sentinel_reply reply = execute(
                peer, {"SENTINEL", "is-master-down-by-addr", current_master.host,
                       std::to_string(current_master.port),
                       std::to_string(vote_epoch), "*"});
            if (!reply.ok() || reply.type != sentinel_reply_type::array ||
                reply.elements.size() < 3) {
                continue;
            }
            if (reply.elements[0].integer != 0 ||
                reply.elements[0].text == "1") {
                ++agree;
            }
        }

        bool odown_now = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (agree >= opt.quorum &&
                health == sentinel_health::subjectively_down) {
                health = sentinel_health::objectively_down;
                odown_now = true;
            }
        }
        if (odown_now) {
            emit(make_event(sentinel_event_type::odown, opt.master_name,
                            current_master, current_master,
                            "master is objectively down"));
        }

        sentinel_health confirmed;
        {
            std::lock_guard<std::mutex> lock(mutex);
            confirmed = health;
        }
        if (confirmed != sentinel_health::objectively_down ||
            !opt.auto_failover) {
            return;
        }

        // --- Vote request: this sentinel's identity, only now ---
        // The self vote is registered first, as a real candidate votes for
        // itself before asking the others. A peer that already voted for this
        // sentinel (or for another candidate at the same epoch) will not vote
        // again; that exclusivity is what stops two concurrent candidates from
        // both reaching a majority. The reply is not counted here -- the
        // implicit self vote below already accounts for it.
        std::size_t votes_for_self = 1;
        if (opt.self.valid()) {
            (void)execute(
                opt.self, {"SENTINEL", "is-master-down-by-addr",
                           current_master.host,
                           std::to_string(current_master.port),
                           std::to_string(vote_epoch), identity});
        }
        for (const sentinel_address& peer : current_peers) {
            if (!peer.valid() || peer == opt.self) {
                continue;
            }
            const sentinel_reply reply = execute(
                peer, {"SENTINEL", "is-master-down-by-addr", current_master.host,
                       std::to_string(current_master.port),
                       std::to_string(vote_epoch), identity});
            if (!reply.ok() || reply.type != sentinel_reply_type::array ||
                reply.elements.size() < 3) {
                continue;
            }
            std::uint64_t leader_epoch = 0;
            (void)parse_u64(reply.elements[2].as_string(), leader_epoch);
            if (reply.elements[1].as_string() == identity &&
                leader_epoch >= vote_epoch) {
                ++votes_for_self;
            }
        }

        // The majority base is the master's own sentinel count when a peer
        // reported it, so two sentinels with different local peer views compute
        // the same threshold; the local set is the fallback.
        std::size_t total = current_peers.size() + 1;
        if (known_sentinels + 1 > total) {
            total = known_sentinels + 1;
        }
        const std::size_t majority = total / 2 + 1;
        if (votes_for_self >= majority && votes_for_self >= opt.quorum) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                leader = true;
                epoch = std::max(epoch + 1, config_epoch + 1);
            }
            begin_failover(current_master, now);
        }
    }

    void loop() {
        {
            std::lock_guard<std::mutex> lock(loop_thread_mutex);
            loop_thread = std::this_thread::get_id();
        }
        while (!stop_requested.load(std::memory_order_acquire)) {
            try {
                poll_at(steady_clock::now());
            } catch (...) {
                // A command executor must report transport failures as values,
                // but a third-party implementation may still throw. The
                // monitor stays alive and retries on the next interval.
            }

            const steady_clock::time_point deadline =
                steady_clock::now() + opt.poll_interval;
            while (!stop_requested.load(std::memory_order_acquire)) {
                const steady_clock::time_point now = steady_clock::now();
                if (now >= deadline) {
                    break;
                }
                nanoseconds remaining =
                    std::chrono::duration_cast<nanoseconds>(deadline - now);
                const nanoseconds slice = std::min(remaining, kSleepSlice);
                if (slice.count() <= 0) {
                    break;
                }
                runtime* rt = active_runtime.load(std::memory_order_acquire);
                if (rt != nullptr) {
                    const std::uint64_t now_ns = rt->now_ns();
                    const auto slice_ns =
                        static_cast<std::uint64_t>(slice.count());
                    const crpc::status slept =
                        rt->sleep_until(now_ns + slice_ns);
                    if (!slept.ok()) {
                        std::this_thread::sleep_for(slice);
                    }
                } else {
                    std::this_thread::sleep_for(slice);
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(loop_thread_mutex);
            loop_thread = std::thread::id{};
        }
        loop_running.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(done_mutex);
            loop_done = true;
        }
        done_cv.notify_all();
    }

    static void loop_entry(void* context) {
        // The coroutine owns its own reference to the state, so `stop()` (and
        // the destructor) may return while a command is still in flight without
        // leaving the loop with a dangling `impl*`. The last reference -- held
        // here -- releases the state when the loop finally exits.
        std::unique_ptr<std::shared_ptr<impl>> guard(
            static_cast<std::shared_ptr<impl>*>(context));
        if (guard != nullptr && *guard != nullptr) {
            (*guard)->loop();
        }
    }

    sentinel_options opt;
    sentinel_command_executor& exec;

    mutable std::mutex mutex;
    event_handler handler;

    sentinel_address master;
    std::vector<sentinel_address> peers;
    std::vector<replica_view> replicas;
    std::vector<sentinel_address> demote;

    sentinel_health health = sentinel_health::unknown;
    bool down_since_set = false;
    steady_clock::time_point down_since{};
    std::uint64_t epoch = 0;
    std::uint64_t config_epoch = 0;
    // Largest `num-other-sentinels` reported by any peer. Used as the
    // master-agnostic majority base instead of the local peer set, so two
    // sentinels with different peer views still compute the same threshold.
    std::size_t known_sentinels = 0;
    bool leader = false;

    failover_phase phase = failover_phase::none;
    sentinel_address candidate;
    sentinel_address failover_old_master;
    steady_clock::time_point failover_start{};
    bool failover_active = false;

    std::string run_id;

    std::atomic<bool> stop_requested{false};
    std::atomic<bool> loop_running{false};
    std::unique_ptr<runtime> owned_runtime;
    std::atomic<runtime*> active_runtime{nullptr};

    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool loop_done = false;

    std::mutex loop_thread_mutex;
    std::thread::id loop_thread{};
};

struct sentinel_connection_executor::impl {
    impl(crpc::backend_factory& backend_factory_ref, connection_config config,
         runtime* shard)
        : backend(backend_factory_ref), base(std::move(config)),
          runtime_ptr(shard) {}

    connection* get(const sentinel_address& target) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = connections.find(target);
        if (it != connections.end()) {
            return it->second.get();
        }
        connection_config config = base;
        config.ep = endpoint::tcp(target.host, target.port);
        auto created = std::make_unique<connection>(config, backend, runtime_ptr);
        connection* raw = created.get();
        connections.emplace(target, std::move(created));
        return raw;
    }

    // Rebinds the shard and drops any connection created for the previous one;
    // a connection pins its shard at construction, so a cached one would keep
    // parking (or blocking) through the wrong runtime.
    void bind(runtime* shard) {
        std::lock_guard<std::mutex> lock(mutex);
        if (runtime_ptr == shard) {
            return;
        }
        runtime_ptr = shard;
        connections.clear();
    }

    void evict(const sentinel_address& target) {
        std::lock_guard<std::mutex> lock(mutex);
        connections.erase(target);
    }

    crpc::backend_factory& backend;
    connection_config base;
    runtime* runtime_ptr;
    std::mutex mutex;
    std::map<sentinel_address, std::unique_ptr<connection>> connections;
};

sentinel_connection_executor::sentinel_connection_executor(
    crpc::backend_factory& backend, runtime* shard)
    : impl_(std::make_unique<impl>(backend, connection_config{}, shard)) {}

sentinel_connection_executor::sentinel_connection_executor(
    crpc::backend_factory& backend, const connection_config& base,
    runtime* shard)
    : impl_(std::make_unique<impl>(backend, base, shard)) {}

sentinel_connection_executor::~sentinel_connection_executor() = default;

void sentinel_connection_executor::bind_runtime(runtime* shard) {
    impl_->bind(shard);
}

sentinel_reply sentinel_connection_executor::execute(
    const sentinel_address& target, const std::vector<std::string>& command) {
    connection* conn = impl_->get(target);
    if (conn == nullptr) {
        return transport_error_reply("no connection");
    }

    // Any transport failure or timeout leaves the connection's buffer holding a
    // reply that belongs to this command. If the connection were kept, the next
    // `read_available()` would surface that stale reply as the new command's
    // answer. So on every failure the connection is closed and dropped from the
    // cache, forcing a clean reconnect for the next command.
    const auto fail = [this, &target](connection* failed, std::string message) {
        failed->close();
        impl_->evict(target);
        return transport_error_reply(std::move(message));
    };

    resp_encoder encoder;
    const std::string payload = encoder.encode_command_to_string(command);
    const span<const std::byte> request(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    const status written = conn->write(request);
    if (!written.ok()) {
        return fail(conn, written.message());
    }

    // Sentinel replies are small (address lists and per-node field maps). A
    // tighter bound than the protocol default keeps a hostile peer from
    // forcing a large allocation.
    resp_parser parser(16u * 1024u * 1024u);
    for (;;) {
        resp_value value;
        const resp_parser::result next = parser.next(value);
        if (next == resp_parser::result::value_ready) {
            return convert_resp(value);
        }
        if (next == resp_parser::result::error) {
            return fail(conn, "RESP protocol error");
        }

        std::vector<std::byte> buffered = conn->read_available();
        if (!buffered.empty()) {
            parser.feed(span<const std::byte>(buffered.data(), buffered.size()));
            continue;
        }

        result<std::vector<std::byte>> chunk = conn->read_exact(1);
        if (!chunk.ok()) {
            return fail(conn, chunk.error().message);
        }
        const std::vector<std::byte>& received = chunk.value();
        parser.feed(span<const std::byte>(received.data(), received.size()));
    }
}

sentinel::sentinel(sentinel_options options, sentinel_command_executor& executor)
    : impl_(std::make_shared<impl>(std::move(options), executor)) {}

sentinel::~sentinel() {
    stop();
}

void sentinel::set_event_handler(event_handler handler) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->handler = std::move(handler);
}

void sentinel::poll_once() {
    impl_->poll_at(steady_clock::now());
}

void sentinel::poll_once_at(std::chrono::steady_clock::time_point now) {
    impl_->poll_at(now);
}

bool sentinel::start(runtime* external_runtime) {
    impl& state = *impl_;
    bool expected = false;
    if (!state.loop_running.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel)) {
        return false;
    }

    state.stop_requested.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state.done_mutex);
        state.loop_done = false;
    }

    runtime* active = external_runtime;
    if (active == nullptr) {
        runtime_options options;
        options.start_handler_pool = false;
        options.idle_wait = std::chrono::milliseconds(20);
        state.owned_runtime = std::make_unique<runtime>(options);
        state.owned_runtime->start_thread();
        active = state.owned_runtime.get();
    }
    state.active_runtime.store(active, std::memory_order_release);

    // The monitor runs as a coroutine on this shard, so its executor must park
    // through the same runtime. Without this the connection adapter would fall
    // back to a bounded plain-thread wait and stall the shard thread for up to
    // the operation timeout on every command.
    state.exec.bind_runtime(active);

    // The coroutine holds its own reference (see `loop_entry`).
    auto* held = new std::shared_ptr<impl>(impl_->shared_from_this());
    if (!active->spawn(&impl::loop_entry, held)) {
        delete held;
        state.stop_requested.store(true, std::memory_order_release);
        state.active_runtime.store(nullptr, std::memory_order_release);
        if (state.owned_runtime != nullptr) {
            state.owned_runtime->stop();
            state.owned_runtime->join();
            state.owned_runtime.reset();
        }
        state.loop_running.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void sentinel::stop() {
    impl& state = *impl_;
    if (!state.loop_running.load(std::memory_order_acquire) &&
        state.owned_runtime == nullptr) {
        state.active_runtime.store(nullptr, std::memory_order_release);
        return;
    }

    state.stop_requested.store(true, std::memory_order_release);

    {
        // Calling stop() from an event handler runs on the monitor coroutine's
        // thread. Joining or waiting for that coroutine from inside it would
        // deadlock; the loop observes the flag and exits on its own.
        std::lock_guard<std::mutex> lock(state.loop_thread_mutex);
        if (state.loop_thread != std::thread::id{} &&
            state.loop_thread == std::this_thread::get_id()) {
            return;
        }
    }

    if (state.owned_runtime != nullptr) {
        state.owned_runtime->stop();
        state.owned_runtime->join();
        state.owned_runtime.reset();
        state.active_runtime.store(nullptr, std::memory_order_release);
        state.loop_running.store(false, std::memory_order_release);
        return;
    }

    // External runtime: the caller drives it, so wait (bounded) for the
    // coroutine to observe the stop flag before this object can go away. Even
    // if a command is still parked and the wait times out, the loop keeps its
    // own reference to the state, so this cannot be a use-after-free; the
    // caller should still stop its runtime to let the coroutine finish.
    std::unique_lock<std::mutex> lock(state.done_mutex);
    (void)state.done_cv.wait_for(lock, std::chrono::seconds(2), [&state] {
        return state.loop_done;
    });
    state.active_runtime.store(nullptr, std::memory_order_release);
}

bool sentinel::running() const noexcept {
    return impl_->loop_running.load(std::memory_order_acquire);
}

sentinel_status sentinel::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sentinel_status out;
    out.health = impl_->health;
    out.master = impl_->master;
    out.replicas = impl_->replicas.size();
    out.sentinels = impl_->peers.size();
    out.epoch = impl_->epoch;
    out.config_epoch = impl_->config_epoch;
    out.failover_in_progress =
        impl_->phase != failover_phase::none || impl_->failover_active;
    out.leader = impl_->leader;
    return out;
}

sentinel_address sentinel::master() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->master;
}

std::uint64_t sentinel::epoch() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->epoch;
}

const std::string& sentinel::run_id() const noexcept {
    return impl_->run_id;
}

} // namespace redis
} // namespace crpc
