// CRPC -- cursor-based Redis iteration.
#include <crpc/redis/scan.hpp>

#include <utility>

namespace crpc {
namespace redis {
namespace {

[[nodiscard]] const char* command_name(scan_kind kind) noexcept {
    switch (kind) {
    case scan_kind::keys:
        return "SCAN";
    case scan_kind::hash:
        return "HSCAN";
    case scan_kind::set:
        return "SSCAN";
    case scan_kind::sorted_set:
        return "ZSCAN";
    }
    return "SCAN";
}

// True when the kind returns pairs (field/value, member/score) rather than bare
// members. The reply is a flat array either way, so the difference is how it is
// grouped.
[[nodiscard]] bool is_paired(scan_kind kind) noexcept {
    return kind == scan_kind::hash || kind == scan_kind::sorted_set;
}

// SCAN's cursor is a bulk string; a server is free to send it as an integer-like
// string, and both spellings mean the same thing. "0" is the only value that ends
// a scan.
[[nodiscard]] bool is_terminator(std::string_view cursor) noexcept {
    return cursor == "0";
}

[[nodiscard]] std::string cursor_text(const resp_value& value) {
    if (value.is_bulk_string()) {
        return value.as_bulk_string();
    }
    if (value.is_simple_string()) {
        return value.as_simple_string();
    }
    if (value.is_integer()) {
        return std::to_string(value.as_integer());
    }
    return {};
}

// Parses one SCAN-family reply: [cursor, [items...]].
[[nodiscard]] status parse_reply(scan_kind kind, const resp_value& reply, std::string& cursor,
                                 scan_batch& batch) {
    if (reply.is_error()) {
        return status{rpc_code::application, reply.as_error()};
    }
    if (!reply.is_array() || reply.as_array().size() != 2) {
        return status{rpc_code::protocol_error, "a scan reply must be [cursor, items]"};
    }
    const resp_array& parts = reply.as_array();
    cursor = cursor_text(parts[0]);
    if (cursor.empty()) {
        return status{rpc_code::protocol_error, "a scan reply has no cursor"};
    }
    if (!parts[1].is_array()) {
        return status{rpc_code::protocol_error, "a scan reply has no item array"};
    }

    const resp_array& items = parts[1].as_array();
    const bool paired = is_paired(kind);
    if (paired && (items.size() % 2) != 0) {
        return status{rpc_code::protocol_error, "a paired scan returned an odd item count"};
    }

    batch.entries.clear();
    if (paired) {
        batch.entries.reserve(items.size() / 2);
        for (std::size_t i = 0; i + 1 < items.size(); i += 2) {
            scan_batch::entry entry;
            entry.name = cursor_text(items[i]);
            entry.value = cursor_text(items[i + 1]);
            batch.entries.push_back(std::move(entry));
        }
    } else {
        batch.entries.reserve(items.size());
        for (const resp_value& item : items) {
            scan_batch::entry entry;
            entry.name = cursor_text(item);
            batch.entries.push_back(std::move(entry));
        }
    }
    batch.done = is_terminator(cursor);
    return status{};
}

[[nodiscard]] command build_command(std::string_view name, std::string_view key,
                                  std::string_view cursor, const scan_options& options) {
    command cmd;
    cmd.reserve(8);
    cmd.emplace_back(name);
    if (!key.empty()) {
        cmd.emplace_back(key);
    }
    cmd.emplace_back(cursor);
    if (!options.match.empty()) {
        cmd.emplace_back("MATCH");
        cmd.emplace_back(options.match);
    }
    if (options.count != 0) {
        cmd.emplace_back("COUNT");
        cmd.emplace_back(std::to_string(options.count));
    }
    if (!options.type.empty()) {
        // TYPE narrows what is returned, not the work the server does; it is still
        // worth sending because it shrinks the reply.
        cmd.emplace_back("TYPE");
        cmd.emplace_back(options.type);
    }
    return cmd;
}

} // namespace

// --- scan_cursor -------------------------------------------------------------

scan_cursor::scan_cursor(command_executor& executor, scan_kind kind, std::string key,
                         scan_options options)
    : executor_(&executor), kind_(kind), key_(std::move(key)), options_(std::move(options)) {}

result<scan_batch> scan_cursor::next() {
    if (finished_) {
        // A caller that keeps calling after the end gets an empty, done batch
        // instead of an error: the loop condition is the batch, not an exception.
        scan_batch empty;
        empty.done = true;
        return empty;
    }

    // The first call must send "0"; the server's reply then carries the next
    // cursor. Sending the stored cursor on the first call would start the scan at
    // the terminator and return nothing.
    const std::string request_cursor = started_ ? cursor_ : std::string("0");
    const std::string_view key =
        kind_ == scan_kind::keys ? std::string_view() : std::string_view(key_);
    command cmd = build_command(command_name(kind_), key, request_cursor, options_);

    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }

    scan_batch batch;
    const status parsed = parse_reply(kind_, reply.value(), cursor_, batch);
    if (!parsed) {
        return parsed.error();
    }
    started_ = true;
    ++steps_;
    if (batch.done) {
        finished_ = true;
    }
    return batch;
}

void scan_cursor::reset() noexcept {
    cursor_ = "0";
    started_ = false;
    finished_ = false;
    steps_ = 0;
}

// --- cluster_scan ------------------------------------------------------------

cluster_scan::cluster_scan(std::vector<command_executor*> executors, scan_kind kind,
                           scan_options options)
    : executors_(std::move(executors)),
      kind_(kind),
      options_(std::move(options)),
      cursors_(executors_.size(), std::string("0")),
      node_done_(executors_.size(), false),
      active_(0),
      done_(executors_.empty()) {
    for (std::size_t i = 0; i < executors_.size(); ++i) {
        // A null entry is a configuration bug; mark the node done so the scan
        // still makes progress over the rest instead of dereferencing it.
        node_done_[i] = executors_[i] == nullptr;
    }
}

result<scan_batch> cluster_scan::next() {
    scan_batch batch;
    if (done_) {
        batch.done = true;
        return batch;
    }
    // HSCAN/SSCAN/ZSCAN are key-scoped: a hash lives on exactly one node, so
    // "cluster-wide hash scan" has no meaning. Only the keyspace can be scanned
    // across nodes.
    if (kind_ != scan_kind::keys) {
        return rpc_error{rpc_code::invalid_argument,
                         "cluster_scan only scans the keyspace; a keyed collection lives "
                         "on one node"};
    }

    // Advance one node per call, in turn, so a cluster with many masters does not
    // starve any of them.
    for (std::size_t attempt = 0; attempt < executors_.size(); ++attempt) {
        const std::size_t index = active_ % executors_.size();
        ++active_;

        if (node_done_[index]) {
            continue;
        }
        command_executor* executor = executors_[index];
        if (executor == nullptr) {
            node_done_[index] = true;
            continue;
        }

        command cmd = build_command("SCAN", std::string_view(), cursors_[index], options_);
        result<resp_value> reply = executor->execute(cmd);
        if (!reply) {
            return reply.error();
        }
        scan_batch part;
        const status parsed = parse_reply(kind_, reply.value(), cursors_[index], part);
        if (!parsed) {
            return parsed.error();
        }
        if (part.done) {
            node_done_[index] = true;
            bool all_done = true;
            for (const bool finished : node_done_) {
                if (!finished) {
                    all_done = false;
                    break;
                }
            }
            done_ = all_done;
        }

        // The caller must not see a key twice even when a resharding handed it to
        // two nodes.
        for (scan_batch::entry& entry : part.entries) {
            if (seen_.insert(entry.name).second) {
                batch.entries.push_back(std::move(entry));
            }
        }
        batch.done = done_;
        if (!batch.entries.empty() || done_) {
            return batch;
        }
        // This node returned only duplicates; advance to the next one.
    }

    // Every node was visited in this call and none produced a new key. That is not
    // the end (a node may simply have had nothing new), so return a non-done empty
    // batch and let the caller continue.
    batch.done = done_;
    return batch;
}

void cluster_scan::reset() noexcept {
    for (std::string& cursor : cursors_) {
        cursor = "0";
    }
    for (std::size_t i = 0; i < node_done_.size(); ++i) {
        node_done_[i] = executors_[i] == nullptr;
    }
    seen_.clear();
    active_ = 0;
    done_ = executors_.empty();
}

} // namespace redis
} // namespace crpc
