// CRPC Redis -- client-side cache implementation. See cache.hpp for the
// contract and the P6 plan reference.
#include <crpc/redis/cache.hpp>

#include <crpc/redis/resp.hpp>

namespace crpc {
namespace redis {

namespace {

// The invalidation push Redis sends over a tracked connection has the shape
// `> [invalidate, key1, key2, ...]`. Anything else on the channel (a Pub/Sub
// message, a PONG from a keepalive) must be left for its own consumer.
bool is_invalidation(const resp_value& push) {
    if (!push.is_push()) {
        return false;
    }
    const auto& items = push.as_push();
    if (items.empty()) {
        return false;
    }
    const resp_value& head = items.front();
    // Redis 7 sends the kind as a bulk string; be tolerant of a simple string
    // (mirrors first_push_kind_is in the IT suite).
    if (head.is_bulk_string()) {
        return head.as_bulk_string() == "invalidate";
    }
    if (head.is_simple_string()) {
        return head.as_simple_string() == "invalidate";
    }
    return false;
}

// `CLIENT TRACKING on` -- sent once when tracking starts and again on resync.
constexpr const char* kTrackingCommand[] = {"CLIENT", "TRACKING", "on"};

} // namespace

client_cache::client_cache(message_channel& channel,
                           std::size_t max_entries) noexcept
    : channel_(&channel), max_entries_(max_entries == 0 ? 1 : max_entries) {}

client_cache::~client_cache() = default;

status client_cache::start() {
    command cmd(kTrackingCommand,
                kTrackingCommand + sizeof(kTrackingCommand) /
                                      sizeof(kTrackingCommand[0]));
    const status sent = channel_->send(cmd);
    if (!sent) {
        return sent;
    }
    // Consume the `+OK` acknowledge so the caller's next `receive()` reads a
    // real value (a push or a command reply), not the bootstrap reply. The
    // same discipline Pub/Sub's subscribe uses.
    result<resp_value> ack = channel_->receive();
    if (!ack) {
        return ack.error();
    }
    if (ack.value().is_error()) {
        return status{rpc_code::application, ack.value().as_error()};
    }
    tracking_on_ = true;
    return status{};
}

status client_cache::resync() {
    entries_.clear();
    lru_.clear();
    ++version_;
    tracking_on_ = false;
    // start() sends `CLIENT TRACKING on` and consumes its +OK acknowledge, so
    // the caller's next receive() is a real value again.
    return start();
}

const resp_value* client_cache::get(std::string_view key) {
    const auto found = entries_.find(std::string(key));
    if (found == entries_.end()) {
        return nullptr;
    }
    touch(found->second.second);
    return &found->second.first;
}

void client_cache::put(std::string key, resp_value value) {
    auto slot = entries_.find(key);
    if (slot != entries_.end()) {
        slot->second.first = std::move(value);
        touch(slot->second.second);
        return;
    }
    lru_.push_front(std::string(key));
    entries_.emplace(std::move(key),
                     std::pair<resp_value, std::list<std::string>::iterator>{
                         std::move(value), lru_.begin()});
    evict();
}

void client_cache::erase(std::string_view key) {
    const auto found = entries_.find(std::string(key));
    if (found == entries_.end()) {
        return;
    }
    lru_.erase(found->second.second);
    entries_.erase(found);
}

std::uint64_t client_cache::version() const noexcept { return version_; }

std::size_t client_cache::size() const noexcept { return entries_.size(); }

status client_cache::poll_once() {
    result<resp_value> next = channel_->receive();
    if (!next) {
        const rpc_code code = next.error().code;
        // eof means "the stream is closed", which ends tracking; any other
        // read failure propagates. A caller that wants "nothing pending now"
        // uses a non-blocking channel; this channel is driven by the caller.
        if (code == rpc_code::eof) {
            tracking_on_ = false;
            return status{};
        }
        return next.error();
    }
    if (!is_invalidation(next.value())) {
        return status{};
    }
    const auto& items = next.value().as_push();
    // Redis 7 sends the invalidated keys nested: `> [invalidate, [key, ...]]`.
    // An empty `> [invalidate]` flushes the whole table. Handle both the flat
    // form (used by the unit fixtures) and the nested form (the live server).
    const auto erase_key = [this](const resp_value& item) {
        if (item.is_bulk_string()) {
            erase(item.as_bulk_string());
        } else if (item.is_simple_string()) {
            erase(item.as_simple_string());
        }
    };
    bool whole_table = true;
    for (std::size_t i = 1; i < items.size(); ++i) {
        const resp_value& item = items[i];
        if (item.is_array()) {
            whole_table = false;
            for (const resp_value& key : item.as_array()) {
                erase_key(key);
            }
        } else if (item.is_bulk_string() || item.is_simple_string()) {
            whole_table = false;
            erase_key(item);
        }
    }
    if (whole_table) {
        entries_.clear();
        lru_.clear();
    }
    ++version_;
    return status{};
}

void client_cache::touch(std::list<std::string>::iterator where) {
    lru_.splice(lru_.begin(), lru_, where);
}

void client_cache::evict() {
    while (entries_.size() > max_entries_) {
        auto back = lru_.end();
        --back;
        entries_.erase(*back);
        lru_.pop_back();
    }
}

} // namespace redis
} // namespace crpc