// CRPC Redis -- Redis Cluster routing, topology discovery and redirects.
#include <crpc/redis/cluster.hpp>

#include <crpc/redis/connection.hpp>
#include <crpc/redis/sentinel.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crpc {
namespace redis {
namespace {

constexpr std::uint32_t no_range = 0xFFFFFFFFu;

std::uint16_t crc16_xmodem(std::string_view data) noexcept {
    std::uint16_t crc = 0;
    for (char raw : data) {
        crc = static_cast<std::uint16_t>(
            crc ^ (static_cast<std::uint16_t>(
                       static_cast<unsigned char>(raw))
                   << 8));
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021u);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const unsigned char left = static_cast<unsigned char>(lhs[i]);
        const unsigned char right = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

bool starts_with(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

bool parse_uint(std::string_view text, std::uint64_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (char raw : text) {
        if (raw < '0' || raw > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(raw - '0');
        if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) {
            return false;
        }
        value = value * 10ull + digit;
    }
    out = value;
    return true;
}

bool parse_int64(std::string_view text, std::int64_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    bool negative = false;
    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    std::uint64_t magnitude = 0;
    if (!parse_uint(text, magnitude) || magnitude > 0x7FFFFFFFFFFFFFFFull) {
        return false;
    }
    out = negative ? -static_cast<std::int64_t>(magnitude)
                   : static_cast<std::int64_t>(magnitude);
    return true;
}

const std::string* as_text(const resp_value& value) noexcept {
    switch (value.type()) {
    case resp_type::simple_string:
        return &value.as_simple_string();
    case resp_type::bulk_string:
        return &value.as_bulk_string();
    case resp_type::verbatim_string:
        return &value.as_verbatim_string();
    case resp_type::big_number:
        return &value.as_big_number();
    case resp_type::error:
        return &value.as_error();
    default:
        return nullptr;
    }
}

bool as_integer(const resp_value& value, std::int64_t& out) noexcept {
    if (value.is_integer()) {
        out = value.as_integer();
        return true;
    }
    const std::string* text = as_text(value);
    return text != nullptr && parse_int64(*text, out);
}

const resp_value* map_lookup(const resp_value& container,
                             std::string_view key) noexcept {
    if (container.is_map()) {
        for (const auto& entry : container.as_map()) {
            const std::string* name = as_text(entry.first);
            if (name != nullptr && *name == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }
    if (container.is_array()) {
        const resp_array& items = container.as_array();
        for (std::size_t i = 0; i + 1 < items.size(); i += 2) {
            const std::string* name = as_text(items[i]);
            if (name != nullptr && *name == key) {
                return &items[i + 1];
            }
        }
    }
    return nullptr;
}

struct redirect_info {
    enum class kind : std::uint8_t { none, moved, ask };

    kind type = kind::none;
    std::uint16_t slot = 0;
    cluster_address target;
};

redirect_info parse_redirect(std::string_view message) {
    redirect_info result;
    std::size_t prefix_size = 0;
    if (starts_with(message, "MOVED ")) {
        result.type = redirect_info::kind::moved;
        prefix_size = 6;
    } else if (starts_with(message, "ASK ")) {
        result.type = redirect_info::kind::ask;
        prefix_size = 4;
    } else {
        return result;
    }

    std::string_view rest = message.substr(prefix_size);
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    const std::size_t separator = rest.find(' ');
    if (separator == std::string_view::npos) {
        return redirect_info{};
    }

    std::uint64_t slot = 0;
    if (!parse_uint(rest.substr(0, separator), slot) ||
        slot >= cluster_slot_count) {
        return redirect_info{};
    }

    std::string_view address = rest.substr(separator + 1);
    while (!address.empty() &&
           (address.back() == ' ' || address.back() == '\r' ||
            address.back() == '\n')) {
        address.remove_suffix(1);
    }
    const std::size_t colon = address.rfind(':');
    if (colon == std::string_view::npos) {
        return redirect_info{};
    }

    std::string host(address.substr(0, colon));
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    std::uint64_t port = 0;
    if (!parse_uint(address.substr(colon + 1), port) || port == 0 ||
        port > 65535) {
        return redirect_info{};
    }

    result.slot = static_cast<std::uint16_t>(slot);
    result.target =
        cluster_address(std::move(host), static_cast<std::uint16_t>(port));
    if (!result.target.valid()) {
        return redirect_info{};
    }
    return result;
}

bool parse_count(const std::string& text, int& out) noexcept {
    std::uint64_t value = 0;
    if (!parse_uint(text, value) || value == 0 || value > 1000000ull) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

void append_range(std::vector<std::string>& keys, const command& cmd, int first,
                  int last, int step) {
    const int count = static_cast<int>(cmd.size());
    if (first < 1 || step <= 0 || first >= count) {
        return;
    }
    int last_index = last >= 0 ? last : count + last;
    if (last_index >= count) {
        last_index = count - 1;
    }
    if (last_index < first) {
        return;
    }
    for (int i = first; i <= last_index; i += step) {
        keys.push_back(cmd[static_cast<std::size_t>(i)]);
    }
}

void append_counted(std::vector<std::string>& keys, const command& cmd,
                    std::size_t count_index) {
    if (count_index >= cmd.size()) {
        return;
    }
    int numkeys = 0;
    if (!parse_count(cmd[count_index], numkeys)) {
        return;
    }
    const int count = static_cast<int>(cmd.size());
    const int first = static_cast<int>(count_index) + 1;
    int last = first + numkeys - 1;
    if (last >= count) {
        last = count - 1;
    }
    for (int i = first; i <= last; ++i) {
        keys.push_back(cmd[static_cast<std::size_t>(i)]);
    }
}

void append_store_key(std::vector<std::string>& keys, const command& cmd,
                      std::size_t start, bool radius) {
    for (std::size_t i = start; i + 1 < cmd.size(); ++i) {
        const bool is_store = equals_ignore_case(cmd[i], "store");
        const bool is_storedist = radius && equals_ignore_case(cmd[i], "storedist");
        if (is_store || is_storedist) {
            keys.push_back(cmd[i + 1]);
            return;
        }
    }
}

void append_stream_keys(std::vector<std::string>& keys, const command& cmd) {
    std::size_t streams = std::string::npos;
    for (std::size_t i = 1; i < cmd.size(); ++i) {
        if (equals_ignore_case(cmd[i], "streams")) {
            streams = i;
            break;
        }
    }
    if (streams == std::string::npos) {
        return;
    }
    const std::size_t remaining = cmd.size() - streams - 1;
    const std::size_t key_count = remaining / 2;
    for (std::size_t i = 0; i < key_count; ++i) {
        keys.push_back(cmd[streams + 1 + i]);
    }
}

bool is_keyless(std::string_view name) noexcept {
    static constexpr const char* const names[] = {
        "ping",        "echo",        "auth",         "hello",
        "select",      "quit",        "reset",        "info",
        "time",        "dbsize",      "flushdb",      "flushall",
        "save",        "bgsave",      "bgrewriteaof", "lastsave",
        "shutdown",    "slaveof",     "replicaof",    "failover",
        "monitor",     "role",        "readonly",     "readwrite",
        "asking",      "multi",       "exec",         "discard",
        "unwatch",     "wait",        "waitaof",      "cluster",
        "config",      "client",      "command",      "script",
        "function",    "pubsub",      "publish",      "spublish",
        "subscribe",   "unsubscribe", "psubscribe",   "punsubscribe",
        "ssubscribe",  "sunsubscribe", "acl",         "module",
        "slowlog",     "latency",     "randomkey",    "keys",
        "scan",        "swapdb",      "lolwut"};
    for (const char* candidate : names) {
        if (equals_ignore_case(name, candidate)) {
            return true;
        }
    }
    return false;
}

result<std::optional<std::uint16_t>> slot_for_keys(
    const std::vector<std::string>& keys) {
    std::optional<std::uint16_t> slot;
    for (const std::string& key : keys) {
        const std::uint16_t candidate = cluster_slot_of(key);
        if (!slot.has_value()) {
            slot = candidate;
        } else if (slot.value() != candidate) {
            return rpc_error{rpc_code::failed_precondition,
                             "CROSSSLOT Keys in request don't hash to the "
                             "same slot"};
        }
    }
    return slot;
}

bool parse_slots_node(const resp_value& node, const cluster_address& seed,
                      cluster_address& out) {
    if (!node.is_array()) {
        return false;
    }
    const resp_array& parts = node.as_array();
    if (parts.size() < 2) {
        return false;
    }
    const std::string* host = as_text(parts[0]);
    if (host == nullptr) {
        return false;
    }
    std::int64_t port = 0;
    if (!as_integer(parts[1], port) || port <= 0 || port > 65535) {
        return false;
    }
    std::string resolved = host->empty() ? seed.host : *host;
    if (resolved.empty()) {
        return false;
    }
    out = cluster_address(std::move(resolved), static_cast<std::uint16_t>(port));
    return out.valid();
}

bool parse_shards_node(const resp_value& node, const cluster_address& seed,
                       cluster_address& out) {
    const resp_value* port_value = map_lookup(node, "port");
    if (port_value == nullptr) {
        return false;
    }
    std::int64_t port = 0;
    if (!as_integer(*port_value, port) || port <= 0 || port > 65535) {
        return false;
    }

    const std::string* host = nullptr;
    const resp_value* endpoint = map_lookup(node, "endpoint");
    if (endpoint != nullptr) {
        host = as_text(*endpoint);
    }
    if (host == nullptr || host->empty()) {
        const resp_value* ip = map_lookup(node, "ip");
        if (ip != nullptr) {
            host = as_text(*ip);
        }
    }
    std::string resolved =
        (host != nullptr && !host->empty()) ? *host : seed.host;
    if (resolved.empty()) {
        return false;
    }
    out = cluster_address(std::move(resolved), static_cast<std::uint16_t>(port));
    return out.valid();
}

void add_shard_range(cluster_topology& out, const resp_value& begin_value,
                     const resp_value& end_value,
                     const cluster_address& master,
                     const std::vector<cluster_address>& replicas) {
    std::int64_t begin = 0;
    std::int64_t end = 0;
    if (!as_integer(begin_value, begin) || !as_integer(end_value, end)) {
        return;
    }
    if (begin < 0 || end < begin || end >= cluster_slot_count) {
        return;
    }
    cluster_slot_range range;
    range.begin = static_cast<std::uint16_t>(begin);
    range.end = static_cast<std::uint16_t>(end);
    range.master = master;
    range.replicas = replicas;
    out.assign(range);
}

bool parse_cluster_slots(const resp_value& reply, const cluster_address& seed,
                         cluster_topology& out) {
    if (!reply.is_array()) {
        return false;
    }
    for (const resp_value& entry : reply.as_array()) {
        if (!entry.is_array()) {
            continue;
        }
        const resp_array& parts = entry.as_array();
        if (parts.size() < 3) {
            continue;
        }
        std::int64_t begin = 0;
        std::int64_t end = 0;
        if (!as_integer(parts[0], begin) || !as_integer(parts[1], end)) {
            continue;
        }
        if (begin < 0 || end < begin || end >= cluster_slot_count) {
            continue;
        }
        cluster_address master;
        if (!parse_slots_node(parts[2], seed, master)) {
            continue;
        }
        cluster_slot_range range;
        range.begin = static_cast<std::uint16_t>(begin);
        range.end = static_cast<std::uint16_t>(end);
        range.master = std::move(master);
        for (std::size_t i = 3; i < parts.size(); ++i) {
            cluster_address replica;
            if (parse_slots_node(parts[i], seed, replica)) {
                range.replicas.push_back(std::move(replica));
            }
        }
        out.assign(range);
    }
    return !out.empty();
}

bool parse_cluster_shards(const resp_value& reply, const cluster_address& seed,
                          cluster_topology& out) {
    if (!reply.is_array()) {
        return false;
    }
    for (const resp_value& shard : reply.as_array()) {
        const resp_value* slots = map_lookup(shard, "slots");
        const resp_value* nodes = map_lookup(shard, "nodes");
        if (slots == nullptr || nodes == nullptr || !slots->is_array() ||
            !nodes->is_array()) {
            continue;
        }

        cluster_address master;
        std::vector<cluster_address> replicas;
        bool have_master = false;
        for (const resp_value& node : nodes->as_array()) {
            cluster_address address;
            if (!parse_shards_node(node, seed, address)) {
                continue;
            }
            const resp_value* role = map_lookup(node, "role");
            const std::string* role_text =
                role != nullptr ? as_text(*role) : nullptr;
            if (role_text != nullptr &&
                equals_ignore_case(*role_text, "master")) {
                master = std::move(address);
                have_master = true;
            } else {
                replicas.push_back(std::move(address));
            }
        }
        if (!have_master) {
            if (replicas.empty()) {
                continue;
            }
            master = replicas.front();
            replicas.erase(replicas.begin());
        }
        if (!master.valid()) {
            continue;
        }

        const resp_array& slot_items = slots->as_array();
        if (slot_items.empty()) {
            continue;
        }
        if (slot_items.front().is_array()) {
            for (const resp_value& pair : slot_items) {
                if (!pair.is_array()) {
                    continue;
                }
                const resp_array& bounds = pair.as_array();
                if (bounds.size() < 2) {
                    continue;
                }
                add_shard_range(out, bounds[0], bounds[1], master, replicas);
            }
        } else {
            for (std::size_t i = 0; i + 1 < slot_items.size(); i += 2) {
                add_shard_range(out, slot_items[i], slot_items[i + 1], master,
                                replicas);
            }
        }
    }
    return !out.empty();
}

// One node reached through a connection pool. Every call checks out a
// connection, runs one command stream on it and returns it to the pool.
class pooled_node_executor final : public command_executor {
public:
    pooled_node_executor(pool_config config, crpc::backend_factory& backend,
                         runtime* shard)
        : pool_(std::move(config), backend, shard) {}

    [[nodiscard]] result<resp_value> execute(const command& cmd) override {
        result<std::shared_ptr<connection>> acquired = pool_.acquire();
        if (!acquired) {
            return acquired.error();
        }
        std::shared_ptr<connection> conn = std::move(acquired).value();
        command_channel channel(*conn);
        result<resp_value> reply = channel.execute(cmd);
        if (!reply) {
            conn->close();
        }
        pool_.release(std::move(conn));
        return reply;
    }

    [[nodiscard]] result<std::vector<resp_value>> execute_batch(
        const std::vector<command>& commands) override {
        if (commands.empty()) {
            return std::vector<resp_value>{};
        }
        result<std::shared_ptr<connection>> acquired = pool_.acquire();
        if (!acquired) {
            return acquired.error();
        }
        std::shared_ptr<connection> conn = std::move(acquired).value();
        command_channel channel(*conn);
        result<std::vector<resp_value>> replies =
            channel.execute_batch(commands);
        if (!replies) {
            conn->close();
        }
        pool_.release(std::move(conn));
        return replies;
    }

private:
    connection_pool pool_;
};

} // namespace

// --- Address and topology ---------------------------------------------------

std::string cluster_address::to_string() const {
    std::string result;
    const bool ipv6 = host.find(':') != std::string::npos;
    if (ipv6) {
        result.push_back('[');
    }
    result += host;
    if (ipv6) {
        result.push_back(']');
    }
    result.push_back(':');
    result += std::to_string(port);
    return result;
}

cluster_topology::cluster_topology()
    : range_index_(cluster_slot_count, no_range) {}

cluster_topology::~cluster_topology() = default;

cluster_topology::cluster_topology(const cluster_topology&) = default;
cluster_topology& cluster_topology::operator=(const cluster_topology&) =
    default;
cluster_topology::cluster_topology(cluster_topology&&) noexcept = default;
cluster_topology& cluster_topology::operator=(cluster_topology&&) noexcept =
    default;

void cluster_topology::reset() noexcept {
    ranges_.clear();
    range_index_.assign(cluster_slot_count, no_range);
    assigned_ = 0;
    ++generation_;
}

bool cluster_topology::assign(const cluster_slot_range& range) {
    if (range.begin > range.end || range.end >= cluster_slot_count ||
        !range.master.valid()) {
        return false;
    }

    std::vector<cluster_slot_range> kept;
    kept.reserve(ranges_.size() + 2);
    for (const cluster_slot_range& existing : ranges_) {
        if (existing.end < range.begin || existing.begin > range.end) {
            kept.push_back(existing);
            continue;
        }
        if (existing.begin < range.begin) {
            cluster_slot_range left = existing;
            left.end = static_cast<std::uint16_t>(range.begin - 1u);
            kept.push_back(std::move(left));
        }
        if (existing.end > range.end) {
            cluster_slot_range right = existing;
            right.begin = static_cast<std::uint16_t>(range.end + 1u);
            kept.push_back(std::move(right));
        }
    }
    kept.push_back(range);
    ranges_ = std::move(kept);
    ++generation_;
    rebuild_index();
    return true;
}

const cluster_address* cluster_topology::master_for(
    std::uint16_t slot) const noexcept {
    if (slot >= cluster_slot_count) {
        return nullptr;
    }
    const std::uint32_t index = range_index_[slot];
    if (index == no_range) {
        return nullptr;
    }
    return &ranges_[index].master;
}

const cluster_slot_range* cluster_topology::range_for(
    std::uint16_t slot) const noexcept {
    if (slot >= cluster_slot_count) {
        return nullptr;
    }
    const std::uint32_t index = range_index_[slot];
    if (index == no_range) {
        return nullptr;
    }
    return &ranges_[index];
}

bool cluster_topology::empty() const noexcept { return ranges_.empty(); }

bool cluster_topology::complete() const noexcept {
    return assigned_ == cluster_slot_count;
}

std::size_t cluster_topology::assigned_slots() const noexcept {
    return assigned_;
}

std::uint64_t cluster_topology::generation() const noexcept {
    return generation_;
}

void cluster_topology::rebuild_index() noexcept {
    range_index_.assign(cluster_slot_count, no_range);
    assigned_ = 0;
    for (std::size_t i = 0; i < ranges_.size(); ++i) {
        const cluster_slot_range& range = ranges_[i];
        for (std::uint32_t slot = range.begin; slot <= range.end; ++slot) {
            range_index_[slot] = static_cast<std::uint32_t>(i);
            ++assigned_;
        }
    }
}

// --- Slot hashing -----------------------------------------------------------

std::uint16_t cluster_slot_of(std::string_view key) noexcept {
    const std::size_t open = key.find('{');
    if (open != std::string_view::npos) {
        const std::size_t close = key.find('}', open + 1);
        if (close != std::string_view::npos && close > open + 1) {
            key = key.substr(open + 1, close - open - 1);
        }
    }
    return static_cast<std::uint16_t>(crc16_xmodem(key) & 0x3FFFu);
}

// --- Key extraction ---------------------------------------------------------

std::vector<std::string> cluster_command_keys(const command& cmd) {
    std::vector<std::string> keys;
    if (cmd.empty()) {
        return keys;
    }
    const std::string_view name(cmd[0]);
    if (is_keyless(name)) {
        return keys;
    }

    if (equals_ignore_case(name, "mset") ||
        equals_ignore_case(name, "msetnx")) {
        append_range(keys, cmd, 1, -1, 2);
        return keys;
    }
    if (equals_ignore_case(name, "mget") ||
        equals_ignore_case(name, "del") ||
        equals_ignore_case(name, "unlink") ||
        equals_ignore_case(name, "exists") ||
        equals_ignore_case(name, "touch") ||
        equals_ignore_case(name, "watch")) {
        append_range(keys, cmd, 1, -1, 1);
        return keys;
    }
    if (equals_ignore_case(name, "rename") ||
        equals_ignore_case(name, "renamenx") ||
        equals_ignore_case(name, "copy") ||
        equals_ignore_case(name, "smove") ||
        equals_ignore_case(name, "lmove") ||
        equals_ignore_case(name, "rpoplpush") ||
        equals_ignore_case(name, "brpoplpush") ||
        equals_ignore_case(name, "blmove") ||
        equals_ignore_case(name, "lcs")) {
        append_range(keys, cmd, 1, 2, 1);
        return keys;
    }
    if (equals_ignore_case(name, "blpop") ||
        equals_ignore_case(name, "brpop") ||
        equals_ignore_case(name, "bzpopmin") ||
        equals_ignore_case(name, "bzpopmax")) {
        append_range(keys, cmd, 1, -2, 1);
        return keys;
    }
    if (equals_ignore_case(name, "eval") ||
        equals_ignore_case(name, "evalsha") ||
        equals_ignore_case(name, "eval_ro") ||
        equals_ignore_case(name, "evalsha_ro") ||
        equals_ignore_case(name, "fcall") ||
        equals_ignore_case(name, "fcall_ro")) {
        append_counted(keys, cmd, 2);
        return keys;
    }
    if (equals_ignore_case(name, "zunionstore") ||
        equals_ignore_case(name, "zinterstore") ||
        equals_ignore_case(name, "zdiffstore")) {
        if (cmd.size() > 1) {
            keys.push_back(cmd[1]);
        }
        append_counted(keys, cmd, 2);
        return keys;
    }
    if (equals_ignore_case(name, "zunion") ||
        equals_ignore_case(name, "zinter") ||
        equals_ignore_case(name, "zdiff") ||
        equals_ignore_case(name, "sintercard") ||
        equals_ignore_case(name, "zintercard") ||
        equals_ignore_case(name, "lmpop") ||
        equals_ignore_case(name, "zmpop")) {
        append_counted(keys, cmd, 1);
        return keys;
    }
    if (equals_ignore_case(name, "blmpop") ||
        equals_ignore_case(name, "bzmpop")) {
        append_counted(keys, cmd, 2);
        return keys;
    }
    if (equals_ignore_case(name, "sinterstore") ||
        equals_ignore_case(name, "sunionstore") ||
        equals_ignore_case(name, "sdiffstore")) {
        if (cmd.size() > 1) {
            keys.push_back(cmd[1]);
        }
        append_range(keys, cmd, 2, -1, 1);
        return keys;
    }
    if (equals_ignore_case(name, "bitop")) {
        if (cmd.size() > 2) {
            keys.push_back(cmd[2]);
        }
        append_range(keys, cmd, 3, -1, 1);
        return keys;
    }
    if (equals_ignore_case(name, "pfmerge")) {
        if (cmd.size() > 1) {
            keys.push_back(cmd[1]);
        }
        append_range(keys, cmd, 2, -1, 1);
        return keys;
    }
    if (equals_ignore_case(name, "pfcount")) {
        append_range(keys, cmd, 1, -1, 1);
        return keys;
    }
    if (equals_ignore_case(name, "sort") ||
        equals_ignore_case(name, "sort_ro")) {
        if (cmd.size() > 1) {
            keys.push_back(cmd[1]);
        }
        append_store_key(keys, cmd, 2, false);
        return keys;
    }
    if (equals_ignore_case(name, "georadius") ||
        equals_ignore_case(name, "georadiusbymember") ||
        equals_ignore_case(name, "georadius_ro") ||
        equals_ignore_case(name, "georadiusbymember_ro")) {
        if (cmd.size() > 1) {
            keys.push_back(cmd[1]);
        }
        append_store_key(keys, cmd, 2, true);
        return keys;
    }
    if (equals_ignore_case(name, "xread") ||
        equals_ignore_case(name, "xreadgroup")) {
        append_stream_keys(keys, cmd);
        return keys;
    }
    if (equals_ignore_case(name, "memory")) {
        if (cmd.size() > 2 && equals_ignore_case(cmd[1], "usage")) {
            keys.push_back(cmd[2]);
        }
        return keys;
    }
    if (equals_ignore_case(name, "object")) {
        if (cmd.size() > 2) {
            keys.push_back(cmd[2]);
        }
        return keys;
    }
    if (equals_ignore_case(name, "debug")) {
        if (cmd.size() > 2 && equals_ignore_case(cmd[1], "object")) {
            keys.push_back(cmd[2]);
        }
        return keys;
    }
    if (equals_ignore_case(name, "xinfo") ||
        equals_ignore_case(name, "xgroup")) {
        if (cmd.size() > 2) {
            keys.push_back(cmd[2]);
        }
        return keys;
    }

    if (cmd.size() > 1) {
        keys.push_back(cmd[1]);
    }
    return keys;
}

// --- Executor provider ------------------------------------------------------

cluster_executor_provider::~cluster_executor_provider() = default;

void cluster_executor_provider::release(command_executor* executor) noexcept {
    (void)executor;
}

struct cluster_pool_provider::impl {
    impl(crpc::backend_factory& backend_ref, pool_config config,
         runtime* shard)
        : backend(backend_ref), base(std::move(config)), runtime_ptr(shard) {}

    crpc::backend_factory& backend;
    pool_config base;
    runtime* runtime_ptr;
    std::mutex mutex;
    std::map<cluster_address, std::unique_ptr<pooled_node_executor>> nodes;
};

cluster_pool_provider::cluster_pool_provider(crpc::backend_factory& backend,
                                             runtime* shard)
    : impl_(std::make_unique<impl>(backend, pool_config{}, shard)) {}

cluster_pool_provider::cluster_pool_provider(crpc::backend_factory& backend,
                                             const pool_config& config,
                                             runtime* shard)
    : impl_(std::make_unique<impl>(backend, config, shard)) {}

cluster_pool_provider::~cluster_pool_provider() = default;

result<command_executor*> cluster_pool_provider::executor_for(
    const cluster_address& node) {
    if (!node.valid()) {
        return rpc_error{rpc_code::invalid_argument, "invalid cluster node"};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto existing = impl_->nodes.find(node);
    if (existing != impl_->nodes.end()) {
        return static_cast<command_executor*>(existing->second.get());
    }

    pool_config config = impl_->base;
    config.connection.ep = endpoint::tcp(node.host, node.port);
    auto created = std::make_unique<pooled_node_executor>(
        std::move(config), impl_->backend, impl_->runtime_ptr);
    pooled_node_executor* raw = created.get();
    impl_->nodes.emplace(node, std::move(created));
    return static_cast<command_executor*>(raw);
}

// --- Cluster client ---------------------------------------------------------

struct cluster::impl {
    impl(cluster_options opts, cluster_executor_provider& external)
        : options(std::move(opts)), provider(&external) {}

    impl(cluster_options opts, crpc::backend_factory& backend, runtime* shard)
        : options(std::move(opts)) {
        owned = std::make_unique<cluster_pool_provider>(backend, options.pool,
                                                        shard);
        provider = owned.get();
    }

    cluster_options options;
    cluster_executor_provider* provider = nullptr;
    std::unique_ptr<cluster_pool_provider> owned;
    cluster_topology topology;
};

cluster::cluster(cluster_options options, cluster_executor_provider& provider)
    : impl_(std::make_unique<impl>(std::move(options), provider)) {}

cluster::cluster(cluster_options options, crpc::backend_factory& backend,
                 runtime* shard)
    : impl_(std::make_unique<impl>(std::move(options), backend, shard)) {}

cluster::~cluster() = default;

const cluster_topology& cluster::topology() const noexcept {
    return impl_->topology;
}

const cluster_options& cluster::options() const noexcept {
    return impl_->options;
}

const cluster_address* cluster::any_node() const noexcept {
    for (std::uint32_t slot = 0; slot < cluster_slot_count; ++slot) {
        const cluster_address* master = impl_->topology.master_for(
            static_cast<std::uint16_t>(slot));
        if (master != nullptr) {
            return master;
        }
    }
    if (!impl_->options.seeds.empty()) {
        return &impl_->options.seeds.front();
    }
    return nullptr;
}

result<resp_value> cluster::execute(const command& cmd) {
    if (cmd.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "an empty Redis command cannot be routed"};
    }
    const std::vector<std::string> keys = cluster_command_keys(cmd);
    result<std::optional<std::uint16_t>> slot = slot_for_keys(keys);
    if (!slot) {
        return slot.error();
    }
    if (slot.value().has_value()) {
        return execute_on_slot(slot.value().value(), cmd);
    }
    return execute_keyless(cmd);
}

result<resp_value> cluster::execute_keyless(const command& cmd) {
    const cluster_address* node = any_node();
    if (node == nullptr) {
        const status refreshed = refresh();
        if (!refreshed) {
            return refreshed.error();
        }
        node = any_node();
    }
    if (node == nullptr) {
        return rpc_error{rpc_code::unavailable,
                         "no Redis Cluster node is known"};
    }
    return execute_on_node(*node, cmd);
}

result<std::vector<resp_value>> cluster::execute_batch(
    const std::vector<command>& commands) {
    if (commands.empty()) {
        return std::vector<resp_value>{};
    }
    std::optional<std::uint16_t> slot;
    for (const command& cmd : commands) {
        if (cmd.empty()) {
            return rpc_error{rpc_code::invalid_argument,
                             "an empty Redis command cannot be routed"};
        }
        const std::vector<std::string> keys = cluster_command_keys(cmd);
        for (const std::string& key : keys) {
            const std::uint16_t candidate = cluster_slot_of(key);
            if (!slot.has_value()) {
                slot = candidate;
            } else if (slot.value() != candidate) {
                return rpc_error{rpc_code::failed_precondition,
                                 "CROSSSLOT Keys in request don't hash to the "
                                 "same slot"};
            }
        }
    }
    if (!slot.has_value()) {
        return execute_batch_keyless(commands);
    }
    return execute_batch_on_slot(slot.value(), commands);
}

result<std::vector<resp_value>> cluster::execute_batch_keyless(
    const std::vector<command>& commands) {
    const cluster_address* node = any_node();
    if (node == nullptr) {
        const status refreshed = refresh();
        if (!refreshed) {
            return refreshed.error();
        }
        node = any_node();
    }
    if (node == nullptr) {
        return rpc_error{rpc_code::unavailable,
                         "no Redis Cluster node is known"};
    }
    result<command_executor*> executor_result =
        impl_->provider->executor_for(*node);
    if (!executor_result) {
        return executor_result.error();
    }
    command_executor* executor = executor_result.value();
    result<std::vector<resp_value>> replies =
        executor->execute_batch(commands);
    impl_->provider->release(executor);
    return replies;
}

result<resp_value> cluster::execute_on_node(const cluster_address& node,
                                            const command& cmd) {
    if (!node.valid()) {
        return rpc_error{rpc_code::invalid_argument, "invalid cluster node"};
    }
    if (cmd.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "an empty Redis command cannot be routed"};
    }
    result<command_executor*> executor_result =
        impl_->provider->executor_for(node);
    if (!executor_result) {
        return executor_result.error();
    }
    command_executor* executor = executor_result.value();
    result<resp_value> reply = executor->execute(cmd);
    impl_->provider->release(executor);
    return reply;
}

result<resp_value> cluster::execute_ask(std::uint16_t slot,
                                        const cluster_address& target,
                                        const command& cmd) {
    std::size_t redirects = 0;
    cluster_address current = target;
    bool need_asking = true;
    for (;;) {
        result<command_executor*> executor_result =
            impl_->provider->executor_for(current);
        if (!executor_result) {
            return executor_result.error();
        }
        command_executor* executor = executor_result.value();

        if (need_asking) {
            result<resp_value> asking = executor->execute(command{"ASKING"});
            if (!asking) {
                impl_->provider->release(executor);
                return asking.error();
            }
            if (asking.value().is_error()) {
                impl_->provider->release(executor);
                return asking;
            }
        }

        result<resp_value> reply = executor->execute(cmd);
        impl_->provider->release(executor);
        if (!reply) {
            return reply.error();
        }
        if (!reply.value().is_error()) {
            return reply;
        }

        const redirect_info redirect = parse_redirect(reply.value().as_error());
        if (redirect.type == redirect_info::kind::moved) {
            if (++redirects > impl_->options.max_redirects) {
                return rpc_error{rpc_code::unavailable,
                                 "too many MOVED redirects"};
            }
            cluster_slot_range update;
            update.begin = slot;
            update.end = slot;
            update.master = redirect.target;
            impl_->topology.assign(update);
            current = redirect.target;
            need_asking = false;
            continue;
        }
        if (redirect.type == redirect_info::kind::ask) {
            if (++redirects > impl_->options.max_redirects) {
                return rpc_error{rpc_code::unavailable,
                                 "too many ASK redirects"};
            }
            current = redirect.target;
            need_asking = true;
            continue;
        }
        return reply;
    }
}

result<resp_value> cluster::execute_on_slot(std::uint16_t slot,
                                            const command& cmd) {
    if (slot >= cluster_slot_count) {
        return rpc_error{rpc_code::invalid_argument,
                         "cluster slot is out of range"};
    }
    if (cmd.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "an empty Redis command cannot be routed"};
    }

    std::size_t redirects = 0;
    std::size_t refreshes = 0;
    for (;;) {
        const cluster_address* owner = impl_->topology.master_for(slot);
        if (owner == nullptr) {
            if (refreshes >= impl_->options.max_refresh_attempts) {
                return rpc_error{rpc_code::unavailable,
                                 "no node owns cluster slot " +
                                     std::to_string(slot)};
            }
            ++refreshes;
            const status refreshed = refresh();
            if (!refreshed) {
                return refreshed.error();
            }
            continue;
        }

        result<command_executor*> executor_result =
            impl_->provider->executor_for(*owner);
        if (!executor_result) {
            return executor_result.error();
        }
        command_executor* executor = executor_result.value();
        result<resp_value> reply = executor->execute(cmd);
        impl_->provider->release(executor);
        if (!reply) {
            return reply.error();
        }
        if (!reply.value().is_error()) {
            return reply;
        }

        const redirect_info redirect = parse_redirect(reply.value().as_error());
        if (redirect.type == redirect_info::kind::moved) {
            if (++redirects > impl_->options.max_redirects) {
                return rpc_error{rpc_code::unavailable,
                                 "too many MOVED redirects"};
            }
            cluster_slot_range update;
            update.begin = slot;
            update.end = slot;
            update.master = redirect.target;
            impl_->topology.assign(update);
            continue;
        }
        if (redirect.type == redirect_info::kind::ask) {
            return execute_ask(slot, redirect.target, cmd);
        }

        const std::string& message = reply.value().as_error();
        if (starts_with(message, "CLUSTERDOWN") ||
            starts_with(message, "TRYAGAIN")) {
            if (refreshes >= impl_->options.max_refresh_attempts) {
                return reply;
            }
            ++refreshes;
            const status refreshed = refresh();
            if (!refreshed) {
                return refreshed.error();
            }
            continue;
        }
        return reply;
    }
}

result<std::vector<resp_value>> cluster::execute_batch_on_slot(
    std::uint16_t slot, const std::vector<command>& commands) {
    if (slot >= cluster_slot_count) {
        return rpc_error{rpc_code::invalid_argument,
                         "cluster slot is out of range"};
    }
    if (commands.empty()) {
        return std::vector<resp_value>{};
    }
    for (const command& cmd : commands) {
        if (cmd.empty()) {
            return rpc_error{rpc_code::invalid_argument,
                             "an empty Redis command cannot be routed"};
        }
    }

    std::size_t refreshes = 0;
    const cluster_address* owner = impl_->topology.master_for(slot);
    while (owner == nullptr) {
        if (refreshes >= impl_->options.max_refresh_attempts) {
            return rpc_error{rpc_code::unavailable,
                             "no node owns cluster slot " +
                                 std::to_string(slot)};
        }
        ++refreshes;
        const status refreshed = refresh();
        if (!refreshed) {
            return refreshed.error();
        }
        owner = impl_->topology.master_for(slot);
    }

    result<command_executor*> executor_result =
        impl_->provider->executor_for(*owner);
    if (!executor_result) {
        return executor_result.error();
    }
    command_executor* executor = executor_result.value();
    result<std::vector<resp_value>> batch = executor->execute_batch(commands);
    impl_->provider->release(executor);
    if (!batch) {
        return batch.error();
    }

    std::vector<resp_value> replies = std::move(batch).value();
    if (replies.size() != commands.size()) {
        return rpc_error{rpc_code::protocol_error,
                         "batch reply count does not match the command count"};
    }

    for (std::size_t i = 0; i < replies.size(); ++i) {
        if (!replies[i].is_error()) {
            continue;
        }
        const redirect_info redirect = parse_redirect(replies[i].as_error());
        if (redirect.type == redirect_info::kind::moved) {
            cluster_slot_range update;
            update.begin = slot;
            update.end = slot;
            update.master = redirect.target;
            impl_->topology.assign(update);
            result<resp_value> retried = execute_on_slot(slot, commands[i]);
            if (!retried) {
                return retried.error();
            }
            replies[i] = std::move(retried).value();
            continue;
        }
        if (redirect.type == redirect_info::kind::ask) {
            result<resp_value> retried =
                execute_ask(slot, redirect.target, commands[i]);
            if (!retried) {
                return retried.error();
            }
            replies[i] = std::move(retried).value();
            continue;
        }
        const std::string& message = replies[i].as_error();
        if (starts_with(message, "CLUSTERDOWN") ||
            starts_with(message, "TRYAGAIN")) {
            if (refreshes >= impl_->options.max_refresh_attempts) {
                continue;
            }
            ++refreshes;
            const status refreshed = refresh();
            if (!refreshed) {
                return refreshed.error();
            }
            result<resp_value> retried = execute_on_slot(slot, commands[i]);
            if (!retried) {
                return retried.error();
            }
            replies[i] = std::move(retried).value();
        }
    }
    return replies;
}

status cluster::refresh() {
    std::vector<cluster_address> candidates;
    const auto add_candidate = [&candidates](const cluster_address& address) {
        if (!address.valid()) {
            return;
        }
        if (std::find(candidates.begin(), candidates.end(), address) ==
            candidates.end()) {
            candidates.push_back(address);
        }
    };

    for (const cluster_address& seed : impl_->options.seeds) {
        add_candidate(seed);
    }
    if (impl_->options.seed_provider) {
        const std::vector<cluster_address> dynamic =
            impl_->options.seed_provider();
        for (const cluster_address& seed : dynamic) {
            add_candidate(seed);
        }
    }
    for (const cluster_slot_range& range : impl_->topology.ranges()) {
        add_candidate(range.master);
        for (const cluster_address& replica : range.replicas) {
            add_candidate(replica);
        }
    }

    if (candidates.empty()) {
        return status{rpc_error{rpc_code::failed_precondition,
                                "no Redis Cluster seed is configured"}};
    }

    rpc_error last{rpc_code::failed_precondition,
                   "no Redis Cluster node returned a topology"};
    for (const cluster_address& candidate : candidates) {
        result<command_executor*> executor_result =
            impl_->provider->executor_for(candidate);
        if (!executor_result) {
            last = executor_result.error();
            continue;
        }
        command_executor* executor = executor_result.value();

        if (impl_->options.prefer_cluster_shards) {
            result<resp_value> shards =
                executor->execute(command{"CLUSTER", "SHARDS"});
            if (!shards) {
                last = shards.error();
                impl_->provider->release(executor);
                continue;
            }
            if (!shards.value().is_error()) {
                cluster_topology parsed;
                if (parse_cluster_shards(shards.value(), candidate, parsed) &&
                    !parsed.empty()) {
                    impl_->topology = std::move(parsed);
                    impl_->provider->release(executor);
                    return status{};
                }
            }
        }

        result<resp_value> slots =
            executor->execute(command{"CLUSTER", "SLOTS"});
        if (!slots) {
            last = slots.error();
            impl_->provider->release(executor);
            continue;
        }
        if (slots.value().is_error()) {
            last = rpc_error{rpc_code::unavailable,
                             slots.value().as_error()};
            impl_->provider->release(executor);
            continue;
        }
        cluster_topology parsed;
        if (parse_cluster_slots(slots.value(), candidate, parsed) &&
            !parsed.empty()) {
            impl_->topology = std::move(parsed);
            impl_->provider->release(executor);
            return status{};
        }
        last = rpc_error{rpc_code::protocol_error,
                         "CLUSTER SLOTS reply contained no usable slot "
                         "ranges"};
        impl_->provider->release(executor);
    }
    return status{last};
}

result<std::unique_ptr<cluster_slot_executor>> cluster::pin(
    const std::vector<std::string>& keys) {
    if (keys.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "pin() requires at least one key"};
    }
    result<std::optional<std::uint16_t>> slot = slot_for_keys(keys);
    if (!slot) {
        return slot.error();
    }
    if (!slot.value().has_value()) {
        return rpc_error{rpc_code::invalid_argument,
                         "pin() requires at least one key"};
    }
    return std::unique_ptr<cluster_slot_executor>(
        new cluster_slot_executor(*this, slot.value().value()));
}

result<std::unique_ptr<cluster_slot_executor>> cluster::pin_slot(
    std::uint16_t slot) {
    if (slot >= cluster_slot_count) {
        return rpc_error{rpc_code::invalid_argument,
                         "cluster slot is out of range"};
    }
    return std::unique_ptr<cluster_slot_executor>(
        new cluster_slot_executor(*this, slot));
}

// --- Pinned executor --------------------------------------------------------

cluster_slot_executor::cluster_slot_executor(cluster& owner,
                                             std::uint16_t slot) noexcept
    : owner_(&owner), slot_(slot) {}

cluster_slot_executor::~cluster_slot_executor() = default;

result<resp_value> cluster_slot_executor::execute(const command& cmd) {
    return owner_->execute_on_slot(slot_, cmd);
}

result<std::vector<resp_value>> cluster_slot_executor::execute_batch(
    const std::vector<command>& commands) {
    return owner_->execute_batch_on_slot(slot_, commands);
}

// --- Sentinel cooperation ---------------------------------------------------

std::vector<cluster_address> cluster_seeds_from_sentinel(
    const sentinel& monitor) {
    std::vector<cluster_address> seeds;
    const sentinel_address master = monitor.master();
    if (master.valid()) {
        seeds.emplace_back(master.host, master.port);
    }
    return seeds;
}

std::vector<cluster_address> cluster_seeds_from_sentinels(
    const std::vector<const sentinel*>& monitors) {
    std::vector<cluster_address> seeds;
    for (const sentinel* monitor : monitors) {
        if (monitor == nullptr) {
            continue;
        }
        const sentinel_address master = monitor->master();
        if (!master.valid()) {
            continue;
        }
        cluster_address address(master.host, master.port);
        if (std::find(seeds.begin(), seeds.end(), address) == seeds.end()) {
            seeds.push_back(std::move(address));
        }
    }
    return seeds;
}

} // namespace redis
} // namespace crpc
