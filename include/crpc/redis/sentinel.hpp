// CRPC -- Redis Sentinel support: master discovery, failure detection,
// leader election and automatic failover.
//
// The sentinel never owns a socket. All remote work goes through
// `sentinel_command_executor`, whose implementation is expected to use the
// CRPC `io::source` / `io::sink` transport and to suspend the calling corocc
// stackful coroutine while a command is in flight. That keeps the state
// machine transport-agnostic and lets a connection pool (or a test double)
// sit behind the same interface.
//
// The monitor itself runs as a corocc coroutine when `start()` is used; the
// deterministic entry point `poll_once_at()` performs one full pass on the
// calling thread and is what the test suite drives.
#ifndef CRPC_REDIS_SENTINEL_HPP
#define CRPC_REDIS_SENTINEL_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/redis/connection.hpp>

namespace crpc {
class runtime;
class backend_factory;
namespace redis {

// Aliases for the shared byte transport: an alias cannot be forward-declared,
// so the transport header is included above.
using connection = crpc::net::connection;
using connection_config = crpc::net::connection_config;

// A host/port pair used for masters, replicas and sentinels alike.
#if defined(CRPC_COMPILER_MSVC)
// Private STL members are an implementation detail; the exported ABI is the
// method set. C4251 would otherwise reject the class in a shared build.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
struct CRPC_API sentinel_address {
    std::string host;
    std::uint16_t port = 0;

    sentinel_address() = default;
    sentinel_address(std::string h, std::uint16_t p)
        : host(std::move(h)), port(p) {}

    [[nodiscard]] bool valid() const noexcept {
        return !host.empty() && port != 0;
    }
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const sentinel_address& a,
                           const sentinel_address& b) noexcept {
        return a.port == b.port && a.host == b.host;
    }
    friend bool operator!=(const sentinel_address& a,
                           const sentinel_address& b) noexcept {
        return !(a == b);
    }
    friend bool operator<(const sentinel_address& a,
                           const sentinel_address& b) noexcept {
        return a.host < b.host || (a.host == b.host && a.port < b.port);
    }
};

#if defined(CRPC_COMPILER_MSVC)
#pragma warning(pop)
#endif

// Minimal command-reply model. It mirrors the RESP shapes Sentinel needs
// (status/bulk/integer/array/error) without making this header depend on the
// full RESP implementation.
enum class sentinel_reply_type : std::uint8_t {
    null_value,
    status,
    integer,
    bulk,
    array,
    error
};

#if defined(CRPC_COMPILER_MSVC)
// Private STL members are an implementation detail; the exported ABI is the
// method set. C4251 would otherwise reject the class in a shared build.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
struct CRPC_API sentinel_reply {
    sentinel_reply_type type = sentinel_reply_type::null_value;
    std::int64_t integer = 0;
    std::string text;
    std::vector<sentinel_reply> elements;

    // True for a transport failure (connect/write/read/timeout), as opposed
    // to a server error such as `-ERR unknown command`.
    bool transport_failure = false;

    [[nodiscard]] bool ok() const noexcept {
        return type != sentinel_reply_type::error &&
               type != sentinel_reply_type::null_value;
    }
    [[nodiscard]] const sentinel_reply& at(std::size_t index) const;
    [[nodiscard]] std::string as_string() const;
};

#if defined(CRPC_COMPILER_MSVC)
#pragma warning(pop)
#endif

// Transport boundary. `execute()` runs one command against one node and
// returns its reply. Implementations may suspend the calling coroutine while
// waiting for bytes; they must not throw for ordinary transport failures --
// return an error reply with `transport_failure` set instead.
class CRPC_API sentinel_command_executor {
public:
    virtual ~sentinel_command_executor() = default;

    virtual sentinel_reply execute(const sentinel_address& target,
                                   const std::vector<std::string>& command) = 0;

    // Binds the shard the monitor runs on. A concrete executor that suspends the
    // calling coroutine (the CRPC connection adapter) parks through this runtime;
    // without it the same call would fall back to a bounded plain-thread wait and
    // stall the shard. The default is a no-op so a purely synchronous double is
    // unaffected.
    virtual void bind_runtime(runtime* shard) { (void)shard; }
};

// Concrete executor over the CRPC Redis connection layer. One byte-level
// connection is created lazily per target and reused; RESP encoding/decoding
// sits on top of `connection::write()` and `connection::read_*()`, which use
// the CRPC `io::source`/`io::sink` transport and park the calling coroutine.
class CRPC_API sentinel_connection_executor final
    : public sentinel_command_executor {
public:
    explicit sentinel_connection_executor(crpc::backend_factory& backend,
                                          runtime* shard = nullptr);
    sentinel_connection_executor(crpc::backend_factory& backend,
                                 const connection_config& base,
                                 runtime* shard = nullptr);
    ~sentinel_connection_executor() override;

    sentinel_connection_executor(const sentinel_connection_executor&) = delete;
    sentinel_connection_executor& operator=(const sentinel_connection_executor&) =
        delete;

    sentinel_reply execute(const sentinel_address& target,
                           const std::vector<std::string>& command) override;

    // Rebinds the shard used by lazily created connections. Existing
    // connections are dropped so the next command rebuilds them on the new
    // shard; otherwise a connection created before `start()` would keep the
    // old (or null) shard and block the runtime thread.
    void bind_runtime(runtime* shard) override;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

struct sentinel_options {
    std::string master_name = "mymaster";

    // Sentinel nodes used for discovery and voting. The node running this
    // monitor may be listed too; it is filtered out with `self`.
    std::vector<sentinel_address> sentinels;

    // Initial master. When empty, the address is discovered from the
    // configured sentinels.
    sentinel_address master;

    // Address of this sentinel, used to avoid querying/voting for itself.
    sentinel_address self;

    // Optional stable identity used in leader election. Generated when empty.
    std::string run_id;

    std::chrono::milliseconds down_after{30000};
    std::chrono::milliseconds failover_timeout{180000};
    std::chrono::milliseconds poll_interval{1000};

    // Number of sentinels that must agree before the master is objectively
    // down. The local sentinel counts as one vote.
    std::size_t quorum = 2;

    bool auto_failover = true;
};

enum class sentinel_event_type : std::uint8_t {
    sdown,
    odown,
    failover_started,
    promoted,
    replica_reconfigured,
    master_switch,
    master_demoted,
    failover_aborted
};

struct sentinel_event {
    sentinel_event_type type = sentinel_event_type::sdown;
    std::string master_name;
    sentinel_address master;           // current/new master in this context
    sentinel_address node;             // affected node
    sentinel_address previous_master;  // only for master_switch
    std::string detail;
};

enum class sentinel_health : std::uint8_t {
    unknown,
    up,
    subjectively_down,
    objectively_down
};

struct sentinel_status {
    sentinel_health health = sentinel_health::unknown;
    sentinel_address master;
    std::size_t replicas = 0;    // known replicas of the current master
    std::size_t sentinels = 0;   // known peer sentinels (excluding self)
    std::uint64_t epoch = 0;
    std::uint64_t config_epoch = 0;
    bool failover_in_progress = false;
    bool leader = false;
};

// Monitors one master and performs automatic failover. The object is
// thread-safe for status/handler access; `poll_once*()` itself must not be
// called concurrently with the background loop.
//
// Lifetime: the background loop keeps its own reference to the monitor state,
// so destroying the `sentinel` while a command is still in flight is safe --
// the state is released when the coroutine finally exits. The executor must
// still outlive that coroutine (it is borrowed, never owned). With an external
// runtime, `stop()` returns once the loop has observed the stop flag or a
// bounded wait elapses; the caller should still stop its runtime so a parked
// command can unwind.
class CRPC_API sentinel {
public:
    using event_handler = std::function<void(const sentinel_event&)>;

    sentinel(sentinel_options options, sentinel_command_executor& executor);
    ~sentinel();

    sentinel(const sentinel&) = delete;
    sentinel& operator=(const sentinel&) = delete;
    sentinel(sentinel&&) = delete;
    sentinel& operator=(sentinel&&) = delete;

    void set_event_handler(event_handler handler);

    // One full monitoring pass at the current time.
    void poll_once();
    // Same, with an explicit monotonic timestamp (deterministic tests).
    void poll_once_at(std::chrono::steady_clock::time_point now);

    // Starts the background monitor as a corocc coroutine. When `rt` is null
    // the sentinel owns a single-threaded runtime. Returns false when the
    // coroutine could not be spawned or the monitor is already running.
    bool start(runtime* rt = nullptr);
    void stop();
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] sentinel_status status() const;
    [[nodiscard]] sentinel_address master() const;
    [[nodiscard]] std::uint64_t epoch() const;
    [[nodiscard]] const std::string& run_id() const noexcept;

private:
    struct impl;
    // Shared, not unique: the background coroutine holds its own reference so a
    // `stop()` that returns while a command is still in flight cannot leave the
    // loop dereferencing a destroyed object. The monitor's last reference (the
    // loop) releases the state on exit.
    std::shared_ptr<impl> impl_;
};

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_SENTINEL_HPP
