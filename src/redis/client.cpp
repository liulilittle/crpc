// CRPC Redis -- asynchronous client facade.
#include <crpc/redis/client.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "reply_util.hpp"

namespace crpc {
namespace redis {
namespace {

// Builds a RESP command: a name followed by bulk-string arguments. It is a
// tiny local helper so the typed methods read like the Redis command they send.
class command_builder {
public:
    explicit command_builder(std::string name) {
        command_.push_back(std::move(name));
    }

    command_builder& arg(std::string value) {
        command_.push_back(std::move(value));
        return *this;
    }

    command_builder& arg(std::int64_t value) { return arg(std::to_string(value)); }

    command_builder& arg(double value) { return arg(std::to_string(value)); }

    command_builder& arg(std::chrono::seconds value) {
        return arg(static_cast<std::int64_t>(value.count()));
    }

    resp_command build() { return std::move(command_); }

private:
    resp_command command_;
};

// Returns a pooled connection to its pool when the enclosing scope ends, so an
// early return cannot leak a checked-out connection. The release thunk is a
// function pointer into `client::impl`, which is private; the guard itself
// therefore never names the type.
struct pooled_connection {
    std::shared_ptr<connection> conn;
    void (*release_fn)(void* ctx, std::shared_ptr<connection>) = nullptr;
    void* ctx = nullptr;
    ~pooled_connection() {
        if (conn != nullptr && release_fn != nullptr) {
            release_fn(ctx, std::move(conn));
        }
    }
};

// True for the transport-level failures that mean "the current master endpoint
// did not answer": connection refused/reset, broken pipe, EOF, deadline and
// `unavailable`. They are the codes mapped to `unavailable` while the pool is
// mid-switch. A Redis error reply is a successful `result`, so it never
// reaches this predicate.
bool is_master_unreachable(rpc_code code) noexcept {
    switch (code) {
    case rpc_code::connection_refused:
    case rpc_code::connection_reset:
    case rpc_code::broken_pipe:
    case rpc_code::eof:
    case rpc_code::deadline_exceeded:
    case rpc_code::unavailable:
        return true;
    default:
        return false;
    }
}

// Turns a command reply into a health status: a transport failure or a Redis
// error reply is a failure, a well-formed reply is success.
status classify(const result<resp_value>& reply) {
    if (!reply) {
        return status{reply.code()};
    }
    if (reply.value().is_error()) {
        return status{rpc_error{rpc_code::application, reply.value().as_error()}};
    }
    return status{};
}

// Reads a text field out of a HELLO map entry, folding the string
// alternatives. Returns nullptr for a non-text value (the integer `proto` or
// the array `modules`).
const std::string* map_text_value(const resp_value& value) {
    return detail::is_text(value) ? &detail::as_text(value) : nullptr;
}

// Sends `HELLO 3` and turns the reply into `hello_info`. A transport or
// framing failure fails the result; a server error reply (Redis < 6 has no
// HELLO command) fails with a clear "RESP3 not supported" error; a reply that
// is not a map, or that negotiates a proto other than 3, is a protocol error.
// A failure never downgrades the session silently.
result<hello_info> negotiate_resp3(connection& conn) {
    command_channel channel(conn);
    channel.set_protocol(resp_protocol::resp3);
    result<resp_value> reply = channel.execute(resp_command{"HELLO", "3"});
    if (!reply) {
        return reply.error();
    }
    const resp_value& value = reply.value();
    if (detail::is_error_reply(value)) {
        return rpc_error{
            rpc_code::failed_precondition,
            "RESP3 negotiation failed: server answered HELLO 3 with an error (\""
                + detail::error_message(value)
                + "\"); RESP3 requires Redis 6 or newer"};
    }
    if (!value.is_map()) {
        return rpc_error{rpc_code::protocol_error,
                         "RESP3 negotiation failed: HELLO 3 did not return a map"};
    }
    hello_info info;
    bool saw_proto = false;
    for (const auto& entry : value.as_map()) {
        const std::string* key = map_text_value(entry.first);
        if (key == nullptr) {
            continue;
        }
        const std::string* field = map_text_value(entry.second);
        if (*key == "server") {
            if (field != nullptr) {
                info.server = *field;
            }
        } else if (*key == "version") {
            if (field != nullptr) {
                info.version = *field;
            }
        } else if (*key == "proto") {
            if (entry.second.is_integer()) {
                info.proto = entry.second.as_integer();
                saw_proto = true;
            }
        } else if (*key == "mode") {
            if (field != nullptr) {
                info.mode = *field;
            }
        } else if (*key == "role") {
            if (field != nullptr) {
                info.role = *field;
            }
        }
    }
    if (!saw_proto || info.proto != 3) {
        return rpc_error{rpc_code::protocol_error,
                         "RESP3 negotiation failed: server advertised proto "
                             + std::to_string(info.proto) + " instead of 3"};
    }
    return info;
}

} // namespace

struct client::impl {
    // One pool generation: the connections aimed at one master endpoint. A
    // generation is immutable once created; a master switch swaps in a fresh one
    // and retires the old (its pool is closed, so further acquires are refused
    // and a later release drops the connection instead of re-lending it).
    struct pool_state {
        connection_pool pool;
        endpoint ep;
        // Set when this generation was replaced by a newer one.
        std::atomic<bool> retired{false};
        // False from creation (a redirect) until the master answers one command,
        // i.e. while the pool is "switching" and the new master is unconfirmed.
        std::atomic<bool> ready{true};

        pool_state(const pool_config& cfg, crpc::backend_factory& backend,
                   runtime* rt, endpoint target)
            : pool(pool_config_for(cfg, target), backend, rt),
              ep(std::move(target)) {}
    };

    // A checked-out connection plus the generation that lent it. Release routes
    // the connection through its own generation, so a switch can never make an
    // old-master link available again.
    struct acquire_result {
        std::shared_ptr<connection> conn;
        std::shared_ptr<pool_state> state;
    };

    options opts;
    crpc::backend_factory& backend;
    runtime* shard;

    // Guards `active`, the ownership map and the switch counters.
    mutable std::mutex pool_mutex;
    std::shared_ptr<pool_state> active;
    // Lent connection -> owning generation. Only touched under `pool_mutex`.
    std::unordered_map<const connection*, std::shared_ptr<pool_state>> owners;

    std::uint64_t switch_count = 0;
    std::uint64_t rejected_during_switch = 0;
    std::chrono::steady_clock::time_point last_switch{};
    std::atomic<bool> closed{false};

    // RESP3 negotiation state. `negotiated` maps a live connection to the link
    // generation at which `HELLO 3` was last sent; the weak_ptr guard tells a
    // recycled heap address from the same connection object, so a stale entry
    // never lets a fresh connection skip its handshake. `last_hello` is the
    // most recent successful negotiation. Both are guarded by `negotiate_mutex`.
    mutable std::mutex negotiate_mutex;
    std::unordered_map<const connection*,
                       std::pair<std::weak_ptr<connection>, std::uint64_t>>
        negotiated;
    hello_info last_hello;

    impl(options o, crpc::backend_factory& b, runtime* rt)
        : opts(std::move(o)), backend(b), shard(rt),
          active(std::make_shared<pool_state>(opts.pool, backend, shard,
                                              opts.pool.connection.ep)) {}

    static pool_config pool_config_for(const pool_config& base,
                                       const endpoint& target) {
        pool_config cfg = base;
        cfg.connection.ep = target;
        return cfg;
    }

    static sentinel_address address_of(const endpoint& ep) {
        if (ep.type() == endpoint::kind::tcp) {
            return sentinel_address{ep.host(), ep.port()};
        }
        if (ep.type() == endpoint::kind::inproc) {
            return sentinel_address{ep.name(), 0};
        }
        return sentinel_address{};
    }

    // Lends a connection from the active generation. When the generation this
    // call read was retired before (or while) it acquired -- a request that
    // arrived inside the switch window -- the acquire retries once on the newest
    // generation, so the request is served from the new master or fails, never
    // from the old master.
    result<acquire_result> acquire_pooled();

    void release_connection(std::shared_ptr<connection> conn);

    // The redirect implementation: repoints the pool. Public `client::redirect`
    // delegates here; the Sentinel-handler lambda in `follow()` calls the
    // `sentinel_address` overload directly.
    status redirect(const sentinel_address& new_master);
    status redirect(const endpoint& new_endpoint);

    static void release_thunk(void* ctx, std::shared_ptr<connection> conn) {
        static_cast<impl*>(ctx)->release_connection(std::move(conn));
    }

    // Post-command bookkeeping. A round trip proves the master answered, so the
    // generation becomes ready (the switch window closes). While a generation is
    // unconfirmed, a transport failure is a switch rejection -- counted and
    // surfaced as `unavailable`, never served from the old master.
    status settle_switch(result<resp_value>& reply,
                         const std::shared_ptr<pool_state>& state);

    result<hello_info> ensure_negotiated(const std::shared_ptr<connection>& conn,
                                         resp_protocol protocol);
    result<acquire_result> acquire_negotiated(resp_protocol protocol);
    result<resp_value> execute_over_pool(resp_protocol protocol,
                                         const resp_command& cmd);
    result<std::vector<resp_value>> execute_batch_over_pool(
        resp_protocol protocol, const std::vector<resp_command>& commands);
};

result<hello_info> client::impl::ensure_negotiated(
    const std::shared_ptr<connection>& conn, resp_protocol protocol) {
    if (protocol != resp_protocol::resp3) {
        return hello_info{};
    }
    const connection* raw = conn.get();
    const std::uint64_t generation = conn->reconnect_count();
    {
        std::lock_guard<std::mutex> lock(negotiate_mutex);
        const auto entry = negotiated.find(raw);
        if (entry != negotiated.end()) {
            const std::shared_ptr<connection> alive = entry->second.first.lock();
            if (alive != nullptr && entry->second.second == generation) {
                return last_hello;
            }
            // A recycled heap address or a newer link generation: renegotiate.
            negotiated.erase(entry);
        }
    }

    result<hello_info> info = negotiate_resp3(*conn);
    if (!info) {
        return info.error();
    }

    std::lock_guard<std::mutex> lock(negotiate_mutex);
    negotiated[raw] = {conn, generation};
    last_hello = info.value();
    return info;
}

result<client::impl::acquire_result> client::impl::acquire_pooled() {
    std::shared_ptr<pool_state> state;
    {
        std::lock_guard<std::mutex> lock(pool_mutex);
        if (closed.load(std::memory_order_acquire)) {
            return rpc_error{rpc_code::shutting_down, "client is closed"};
        }
        state = active;
    }

    result<std::shared_ptr<connection>> acquired = state->pool.acquire();
    if (!acquired && acquired.code() == rpc_code::shutting_down &&
        state->retired.load(std::memory_order_acquire)) {
        // The generation was retired while this call was in flight: the request
        // arrived inside the switch window. Re-read the active generation and
        // lend from it, so the request lands on the new master instead of
        // failing -- or reports `unavailable` when the new pool cannot help.
        std::shared_ptr<pool_state> current;
        {
            std::lock_guard<std::mutex> lock(pool_mutex);
            if (closed.load(std::memory_order_acquire)) {
                return rpc_error{rpc_code::shutting_down, "client is closed"};
            }
            current = active;
        }
        if (current == state) {
            // No newer generation exists: this is client shutdown, not a switch.
            return acquired.error();
        }
        state = current;
        acquired = state->pool.acquire();
        if (!acquired) {
            std::lock_guard<std::mutex> lock(pool_mutex);
            ++rejected_during_switch;
            return rpc_error{rpc_code::unavailable,
                             "request rejected during master switch: the new "
                                 "master pool could not lend a connection"};
        }
    }
    if (!acquired) {
        return acquired.error();
    }

    std::shared_ptr<connection> conn = std::move(acquired).value();
    std::lock_guard<std::mutex> lock(pool_mutex);
    owners[conn.get()] = state;
    return acquire_result{std::move(conn), state};
}

void client::impl::release_connection(std::shared_ptr<connection> conn) {
    if (!conn) {
        return;
    }
    std::shared_ptr<pool_state> owner;
    {
        std::lock_guard<std::mutex> lock(pool_mutex);
        const auto it = owners.find(conn.get());
        if (it != owners.end()) {
            owner = it->second;
            owners.erase(it);
        }
    }
    if (owner == nullptr) {
        // Not lent by this client (a foreign handle or an unknown link). Close
        // it rather than risk a stale connection leaking into a generation.
        conn->close();
        return;
    }
    // A retired generation's pool is closed, so `release` drops the connection
    // (closing it) instead of re-lending it; the active generation pools it.
    owner->pool.release(std::move(conn));
}

status client::impl::settle_switch(
    result<resp_value>& reply, const std::shared_ptr<pool_state>& state) {
    if (reply.ok()) {
        state->ready.store(true, std::memory_order_release);
        return status{};
    }
    if (!state->ready.load(std::memory_order_acquire) &&
        is_master_unreachable(reply.code())) {
        std::lock_guard<std::mutex> lock(pool_mutex);
        ++rejected_during_switch;
        return status{rpc_code::unavailable,
                      "request rejected during master switch: new master "
                          + state->ep.to_string() + " did not answer ("
                          + reply.error().message + ")"};
    }
    // A non-transport failure (for example a refused RESP3 handshake) proves the
    // endpoint answered, so the switch window is over.
    state->ready.store(true, std::memory_order_release);
    return status{reply.code(), reply.error().message};
}

result<client::impl::acquire_result> client::impl::acquire_negotiated(
    resp_protocol protocol) {
    result<acquire_result> acquired = acquire_pooled();
    if (!acquired) {
        return acquired.error();
    }
    result<hello_info> handshake =
        ensure_negotiated(acquired.value().conn, protocol);
    if (!handshake) {
        std::shared_ptr<connection> refused = std::move(acquired.value().conn);
        refused->close();
        release_connection(std::move(refused));
        return handshake.error();
    }
    return acquired;
}

result<resp_value> client::impl::execute_over_pool(resp_protocol protocol,
                                                   const resp_command& cmd) {
    result<acquire_result> acquired = acquire_negotiated(protocol);
    if (!acquired) {
        return acquired.error();
    }
    std::shared_ptr<pool_state> state = acquired.value().state;
    pooled_connection guard{std::move(acquired).value().conn, &client::impl::release_thunk, this};
    command_channel channel(*guard.conn);
    channel.set_protocol(protocol);
    result<resp_value> reply = channel.execute(cmd);
    const status settled = settle_switch(reply, state);
    if (!settled.ok()) {
        return rpc_error{settled.code(), settled.message()};
    }
    return reply;
}

result<std::vector<resp_value>> client::impl::execute_batch_over_pool(
    resp_protocol protocol, const std::vector<resp_command>& commands) {
    if (commands.empty()) {
        return std::vector<resp_value>{};
    }
    result<acquire_result> acquired = acquire_negotiated(protocol);
    if (!acquired) {
        return acquired.error();
    }
    std::shared_ptr<pool_state> state = acquired.value().state;
    pooled_connection guard{std::move(acquired).value().conn, &client::impl::release_thunk, this};
    command_channel channel(*guard.conn);
    channel.set_protocol(protocol);
    result<std::vector<resp_value>> replies = channel.execute_batch(commands);
    if (!replies.ok()) {
        result<resp_value> folded =
            rpc_error{replies.code(), replies.error().message};
        const status settled = settle_switch(folded, state);
        return rpc_error{settled.code(), settled.message()};
    }
    state->ready.store(true, std::memory_order_release);
    return replies;
}

client::client(options opts, crpc::backend_factory& backend, runtime* shard)
    : impl_(std::make_shared<impl>(std::move(opts), backend, shard)) {}

client::~client() = default;

status client::connect() {
    result<impl::acquire_result> acquired =
        impl_->acquire_negotiated(impl_->opts.protocol);
    if (!acquired) {
        return status{acquired.error()};
    }
    std::shared_ptr<impl::pool_state> state = acquired.value().state;
    std::shared_ptr<connection> conn = std::move(acquired).value().conn;
    command_channel channel(*conn);
    channel.set_protocol(impl_->opts.protocol);
    result<resp_value> reply = channel.execute(resp_command{"PING"});
    impl_->release_connection(std::move(conn));
    const status settled = impl_->settle_switch(reply, state);
    if (!settled.ok()) {
        return settled;
    }
    return classify(reply);
}

void client::close() noexcept {
    std::shared_ptr<impl::pool_state> current;
    {
        std::lock_guard<std::mutex> lock(impl_->pool_mutex);
        impl_->closed.store(true, std::memory_order_release);
        current = impl_->active;
    }
    if (current != nullptr) {
        current->pool.close();
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->negotiate_mutex);
        impl_->negotiated.clear();
    } catch (...) {
        // close() is noexcept; a mutex failure cannot break the shutdown
        // contract, and the stale entries self-heal through their weak_ptr.
    }
}

bool client::is_open() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->pool_mutex);
    return impl_->active != nullptr && impl_->active->pool.total() > 0;
}

status client::health_check() { return classify(ping()); }

result<std::shared_ptr<connection>> client::acquire() {
    result<impl::acquire_result> acquired =
        impl_->acquire_negotiated(impl_->opts.protocol);
    if (!acquired) {
        return acquired.error();
    }
    return std::move(acquired).value().conn;
}

void client::release(std::shared_ptr<connection> conn) {
    impl_->release_connection(std::move(conn));
}

hello_info client::hello() const {
    std::lock_guard<std::mutex> lock(impl_->negotiate_mutex);
    return impl_->last_hello;
}

std::size_t client::idle() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->pool_mutex);
    return impl_->active != nullptr ? impl_->active->pool.idle() : std::size_t{0};
}

std::size_t client::in_use() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->pool_mutex);
    return impl_->active != nullptr ? impl_->active->pool.in_use()
                                    : std::size_t{0};
}

std::size_t client::total() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->pool_mutex);
    return impl_->active != nullptr ? impl_->active->pool.total() : std::size_t{0};
}

status client::impl::redirect(const sentinel_address& new_master) {
    if (!new_master.valid()) {
        return status{rpc_code::invalid_argument,
                      "redirect requires a host and a non-zero port"};
    }
    return redirect(endpoint::tcp(new_master.host, new_master.port));
}

status client::impl::redirect(const endpoint& new_endpoint) {
    if (!new_endpoint.valid()) {
        return status{rpc_code::invalid_argument,
                      "redirect requires a valid endpoint"};
    }
    std::shared_ptr<pool_state> retired_gen;
    {
        std::lock_guard<std::mutex> lock(pool_mutex);
        if (closed.load(std::memory_order_acquire)) {
            return status{rpc_code::shutting_down, "client is closed"};
        }
        // Idempotent: the address the pool already points at is not a switch.
        if (active->ep == new_endpoint) {
            return status{};
        }
        retired_gen = std::move(active);
        active = std::make_shared<pool_state>(opts.pool, backend, shard,
                                              new_endpoint);
        // The new master is unconfirmed until it answers a command: the switch
        // window is open from here on, so transport failures are surfaced as
        // `unavailable` instead of being served from the old master.
        active->ready.store(false, std::memory_order_release);
        ++switch_count;
        last_switch = std::chrono::steady_clock::now();
        retired_gen->retired.store(true, std::memory_order_release);
        // Stop lending old-master connections now: every idle link is closed and
        // a connection returned later is dropped by the closed pool.
        retired_gen->pool.close();
    }
    return status{};
}

status client::redirect(const sentinel_address& new_master) {
    return impl_->redirect(new_master);
}

status client::redirect(const endpoint& new_endpoint) {
    return impl_->redirect(new_endpoint);
}

bool client::follow(sentinel& target, const std::string& master_name) {
    if (master_name.empty()) {
        return false;
    }
    const std::weak_ptr<impl> weak = impl_;
    target.set_event_handler([weak, master_name](const sentinel_event& event) {
        if (event.type != sentinel_event_type::master_switch ||
            event.master_name != master_name) {
            return;
        }
        if (const std::shared_ptr<impl> state = weak.lock()) {
            (void)state->redirect(event.master);
        }
    });
    return true;
}

switch_stats client::stats() const {
    std::lock_guard<std::mutex> lock(impl_->pool_mutex);
    switch_stats out;
    if (impl_->active != nullptr) {
        out.master = impl::address_of(impl_->active->ep);
        out.switching =
            !impl_->active->ready.load(std::memory_order_acquire);
    }
    out.switch_count = impl_->switch_count;
    out.rejected_during_switch = impl_->rejected_during_switch;
    out.last_switch = impl_->last_switch;
    return out;
}

sentinel_address client::master() const { return stats().master; }

bool client::switching() const noexcept { return stats().switching; }

result<resp_value> client::execute(const resp_command& cmd) {
    return impl_->execute_over_pool(impl_->opts.protocol, cmd);
}

result<std::vector<resp_value>> client::execute_batch(
    const std::vector<resp_command>& commands) {
    return impl_->execute_batch_over_pool(impl_->opts.protocol, commands);
}

result<resp_value> client::command(span<const std::string> args) {
    if (args.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "an empty Redis command cannot be sent"};
    }
    resp_command cmd;
    cmd.reserve(args.size());
    for (const std::string& arg : args) {
        cmd.push_back(arg);
    }
    return execute(cmd);
}

// --- typed convenience ---------------------------------------------------------

result<resp_value> client::ping() { return execute(resp_command{"PING"}); }

result<resp_value> client::get(std::string key) {
    return execute(command_builder("GET").arg(std::move(key)).build());
}

result<resp_value> client::set(std::string key, std::string value) {
    return execute(
        command_builder("SET").arg(std::move(key)).arg(std::move(value)).build());
}

result<resp_value> client::setex(std::string key, std::chrono::seconds seconds,
                                std::string value) {
    return execute(command_builder("SETEX")
                       .arg(std::move(key))
                       .arg(seconds)
                       .arg(std::move(value))
                       .build());
}

result<resp_value> client::del(std::vector<std::string> keys) {
    command_builder builder("DEL");
    for (std::string& key : keys) {
        builder.arg(std::move(key));
    }
    return execute(builder.build());
}

result<resp_value> client::exists(std::vector<std::string> keys) {
    command_builder builder("EXISTS");
    for (std::string& key : keys) {
        builder.arg(std::move(key));
    }
    return execute(builder.build());
}

result<resp_value> client::expire(std::string key, std::chrono::seconds seconds) {
    return execute(
        command_builder("EXPIRE").arg(std::move(key)).arg(seconds).build());
}

result<resp_value> client::ttl(std::string key) {
    return execute(command_builder("TTL").arg(std::move(key)).build());
}

result<resp_value> client::incr(std::string key) {
    return execute(command_builder("INCR").arg(std::move(key)).build());
}

result<resp_value> client::decr(std::string key) {
    return execute(command_builder("DECR").arg(std::move(key)).build());
}

result<resp_value> client::incrby(std::string key, std::int64_t amount) {
    return execute(
        command_builder("INCRBY").arg(std::move(key)).arg(amount).build());
}

result<resp_value> client::append(std::string key, std::string value) {
    return execute(
        command_builder("APPEND").arg(std::move(key)).arg(std::move(value)).build());
}

result<resp_value> client::strlen(std::string key) {
    return execute(command_builder("STRLEN").arg(std::move(key)).build());
}

result<resp_value> client::keys(std::string pattern) {
    return execute(command_builder("KEYS").arg(std::move(pattern)).build());
}

result<resp_value> client::mget(std::vector<std::string> keys) {
    command_builder builder("MGET");
    for (std::string& key : keys) {
        builder.arg(std::move(key));
    }
    return execute(builder.build());
}

result<resp_value> client::mset(
    std::vector<std::pair<std::string, std::string>> pairs) {
    command_builder builder("MSET");
    for (auto& pair : pairs) {
        builder.arg(std::move(pair.first)).arg(std::move(pair.second));
    }
    return execute(builder.build());
}

result<resp_value> client::hset(std::string key, std::string field,
                                std::string value) {
    return execute(command_builder("HSET")
                       .arg(std::move(key))
                       .arg(std::move(field))
                       .arg(std::move(value))
                       .build());
}

result<resp_value> client::hget(std::string key, std::string field) {
    return execute(
        command_builder("HGET").arg(std::move(key)).arg(std::move(field)).build());
}

result<resp_value> client::hdel(std::string key, std::vector<std::string> fields) {
    command_builder builder("HDEL");
    builder.arg(std::move(key));
    for (std::string& field : fields) {
        builder.arg(std::move(field));
    }
    return execute(builder.build());
}

result<resp_value> client::hexists(std::string key, std::string field) {
    return execute(command_builder("HEXISTS")
                       .arg(std::move(key))
                       .arg(std::move(field))
                       .build());
}

result<resp_value> client::hgetall(std::string key) {
    return execute(command_builder("HGETALL").arg(std::move(key)).build());
}

result<resp_value> client::lpush(std::string key, std::vector<std::string> values) {
    command_builder builder("LPUSH");
    builder.arg(std::move(key));
    for (std::string& value : values) {
        builder.arg(std::move(value));
    }
    return execute(builder.build());
}

result<resp_value> client::rpush(std::string key, std::vector<std::string> values) {
    command_builder builder("RPUSH");
    builder.arg(std::move(key));
    for (std::string& value : values) {
        builder.arg(std::move(value));
    }
    return execute(builder.build());
}

result<resp_value> client::lpop(std::string key) {
    return execute(command_builder("LPOP").arg(std::move(key)).build());
}

result<resp_value> client::rpop(std::string key) {
    return execute(command_builder("RPOP").arg(std::move(key)).build());
}

result<resp_value> client::llen(std::string key) {
    return execute(command_builder("LLEN").arg(std::move(key)).build());
}

result<resp_value> client::sadd(std::string key, std::vector<std::string> members) {
    command_builder builder("SADD");
    builder.arg(std::move(key));
    for (std::string& member : members) {
        builder.arg(std::move(member));
    }
    return execute(builder.build());
}

result<resp_value> client::srem(std::string key, std::vector<std::string> members) {
    command_builder builder("SREM");
    builder.arg(std::move(key));
    for (std::string& member : members) {
        builder.arg(std::move(member));
    }
    return execute(builder.build());
}

result<resp_value> client::smembers(std::string key) {
    return execute(command_builder("SMEMBERS").arg(std::move(key)).build());
}

result<resp_value> client::sismember(std::string key, std::string member) {
    return execute(command_builder("SISMEMBER")
                       .arg(std::move(key))
                       .arg(std::move(member))
                       .build());
}

result<resp_value> client::zadd(std::string key, double score, std::string member) {
    return execute(command_builder("ZADD")
                       .arg(std::move(key))
                       .arg(score)
                       .arg(std::move(member))
                       .build());
}

result<resp_value> client::zrange(std::string key, std::int64_t start,
                                  std::int64_t stop, bool with_scores) {
    command_builder builder("ZRANGE");
    builder.arg(std::move(key)).arg(start).arg(stop);
    if (with_scores) {
        builder.arg("WITHSCORES");
    }
    return execute(builder.build());
}

result<resp_value> client::zscore(std::string key, std::string member) {
    return execute(command_builder("ZSCORE")
                       .arg(std::move(key))
                       .arg(std::move(member))
                       .build());
}

} // namespace redis
} // namespace crpc

