// CRPC -- memcached client implementation.
#include <crpc/memcached/client.hpp>

#include <string>
#include <utility>
#include <vector>

namespace crpc {
namespace memcached {
namespace {

[[nodiscard]] const char* store_command(store_kind kind) noexcept {
    switch (kind) {
    case store_kind::set:
        return "set";
    case store_kind::add:
        return "add";
    case store_kind::replace:
        return "replace";
    case store_kind::append:
        return "append";
    case store_kind::prepend:
        return "prepend";
    case store_kind::cas_cmd:
        return "cas";
    }
    return "set";
}

// A CLIENT_ERROR means the server rejected the request as malformed, which is a
// caller bug and therefore `invalid_argument`. A SERVER_ERROR means the server
// itself failed (out of memory, a full cache), which is not the caller's fault
// and is worth retrying, so it maps to `internal` with the retryable flag left to
// the caller's policy. A bare ERROR is a framing violation.
[[nodiscard]] rpc_code error_code_for(error_kind kind) noexcept {
    switch (kind) {
    case error_kind::client_error:
        return rpc_code::invalid_argument;
    case error_kind::server_error:
        return rpc_code::internal;
    case error_kind::error:
        break;
    }
    return rpc_code::protocol_error;
}

} // namespace

status reply_error(const text_reply& reply) {
    if (!reply.is_error()) {
        return status{};
    }
    return status{error_code_for(reply.error), reply.text};
}

// --- connection_transport ----------------------------------------------------

struct connection_transport::impl {
    connection_transport_config config;
    std::unique_ptr<net::connection_pool> pool;

    impl(connection_transport_config cfg, crpc::backend_factory& backend, runtime* shard)
        : config(std::move(cfg)),
          pool(new net::connection_pool(config.pool, backend, shard)) {}
};

connection_transport::connection_transport(connection_transport_config config,
                                           crpc::backend_factory& backend, runtime* shard)
    : impl_(new impl(std::move(config), backend, shard)) {}

connection_transport::~connection_transport() { (void)close(); }

result<text_reply> connection_transport::exchange(span<const std::byte> request) {
    result<std::shared_ptr<net::connection>> acquired = impl_->pool->acquire();
    if (!acquired) {
        return acquired.error();
    }
    std::shared_ptr<net::connection> link = std::move(acquired.value());

    // Any failure from here on leaves the stream in an unknown state, so the link
    // is closed and dropped rather than returned to the pool. Returning it could
    // hand half a reply to the next caller.
    const auto discard = [this, &link](rpc_error error) -> result<text_reply> {
        link->close();
        impl_->pool->release(std::move(link));
        return error;
    };

    const status written = link->write(request);
    if (!written) {
        return discard(written.error());
    }

    text_parser parser(impl_->config.max_value_size, impl_->config.max_line_size);    for (;;) {
        text_reply reply;
        const text_parser::result step = parser.next(reply);
        if (step == text_parser::result::reply_ready) {
            // A reply is one request's answer. Bytes left over mean the stream
            // and the parser disagree, which must not be silently discarded.
            if (parser.buffered() != 0 || link->buffered() != 0) {
                return discard(rpc_error{rpc_code::protocol_error,
                                         "memcached reply left unread bytes on the stream"});
            }
            impl_->pool->release(std::move(link));
            return reply;
        }
        if (step == text_parser::result::error) {
            return discard(rpc_error{parser.error(), "memcached reply decoding failed"});
        }

        // The parser needs more bytes. Read one byte, which parks until at least
        // one arrives, then drain whatever else is already buffered: a large
        // value block is consumed in buffer-sized chunks and the parser
        // reassembles it.
        result<std::vector<std::byte>> first = link->read_exact(1);
        if (!first) {
            return discard(first.error());
        }
        parser.feed(span<const std::byte>(first.value().data(), first.value().size()));

        std::vector<std::byte> rest = link->read_available();
        if (!rest.empty()) {
            parser.feed(span<const std::byte>(rest.data(), rest.size()));
        }
    }
}

status connection_transport::close() noexcept {
    if (!impl_ || !impl_->pool) {
        return status{};
    }
    impl_->pool->close();
    return status{};
}

net::connection_pool& connection_transport::pool() noexcept { return *impl_->pool; }

// --- client ------------------------------------------------------------------

struct client::impl {
    options opts;
    std::shared_ptr<transport> endpoint;
    std::unique_ptr<connection_transport> owned;

    [[nodiscard]] std::string prefixed(std::string_view key) const {
        if (opts.key_prefix.empty()) {
            return std::string(key);
        }
        std::string full;
        full.reserve(opts.key_prefix.size() + key.size());
        full.append(opts.key_prefix);
        full.append(key.data(), key.size());
        return full;
    }

    [[nodiscard]] status check_value_size(std::string_view value) const {
        if (value.size() > opts.max_value_size) {
            return status{rpc_code::resource_exhausted,
                          "memcached value exceeds the configured maximum"};
        }
        return status{};
    }
};

client::client(options opts, crpc::backend_factory& backend, runtime* shard) : impl_(new impl) {
    impl_->opts = std::move(opts);
    connection_transport_config config;
    config.pool = impl_->opts.pool;
    config.max_value_size = impl_->opts.max_value_size;
    config.max_line_size = impl_->opts.max_line_size;
    impl_->owned.reset(new connection_transport(std::move(config), backend, shard));
    impl_->endpoint = std::shared_ptr<transport>(impl_->owned.get(), [](transport*) {});
}

client::client(options opts, std::shared_ptr<transport> endpoint) : impl_(new impl) {
    impl_->opts = std::move(opts);
    impl_->endpoint = std::move(endpoint);
}

client::~client() {
    if (impl_ && impl_->owned) {
        (void)impl_->owned->close();
    }
}

std::string client::prefixed(std::string_view key) const { return impl_->prefixed(key); }

status client::check_value_size(std::string_view value) const {
    return impl_->check_value_size(value);
}

result<text_reply> client::exchange(span<const std::byte> request) {
    if (!impl_->endpoint) {
        return rpc_error{rpc_code::failed_precondition, "memcached client has no transport"};
    }
    return impl_->endpoint->exchange(request);
}

result<text_reply> client::execute(span<const std::byte> request) { return exchange(request); }

result<std::vector<value_entry>> client::get(const std::vector<std::string>& keys) {
    return get_and_touch(keys, 0, false);
}

result<std::vector<value_entry>> client::gets(const std::vector<std::string>& keys) {
    return get_and_touch(keys, 0, true);
}

result<std::vector<value_entry>> client::get_and_touch(const std::vector<std::string>& keys,
                                                       std::uint64_t exptime, bool with_cas) {
    if (keys.empty()) {
        return rpc_error{rpc_code::invalid_argument, "memcached retrieval needs at least one key"};
    }
    const bool timed = exptime != 0;
    const char* command = with_cas ? (timed ? "gats" : "gets") : (timed ? "gat" : "get");

    std::vector<std::string> full_keys;
    full_keys.reserve(keys.size());
    for (const std::string& key : keys) {
        full_keys.push_back(impl_->prefixed(key));
    }
    std::vector<std::string_view> views;
    views.reserve(full_keys.size());
    for (const std::string& key : full_keys) {
        views.emplace_back(key);
    }

    const std::size_t size = text_encoder::retrieval_size(command, views, exptime);
    std::vector<std::byte> request(size);
    std::size_t written = 0;
    const status encoded = text_encoder{}.retrieval(
        command, views, exptime, span<std::byte>(request.data(), request.size()), written);
    if (!encoded) {
        return encoded.error();
    }
    request.resize(written);

    result<text_reply> reply = exchange(span<const std::byte>(request.data(), request.size()));
    if (!reply) {
        return reply.error();
    }
    const status failure = reply_error(reply.value());
    if (!failure) {
        return failure.error();
    }
    if (reply.value().type != text_reply::kind::values) {
        return rpc_error{rpc_code::protocol_error, "memcached retrieval reply was not a value list"};
    }
    return std::move(reply.value().entries);
}

result<store_status> client::store(store_kind kind, std::string_view key,
                                   std::string_view value, std::uint32_t flags,
                                   std::uint64_t exptime, std::uint64_t cas) {
    const status size_ok = check_value_size(value);
    if (!size_ok) {
        return size_ok.error();
    }
    // `cas` is the only command with a CAS field, and it requires one; every other
    // command rejects it. Encoding the wrong shape would make the server treat the
    // token as part of the value length.
    const std::uint64_t token = kind == store_kind::cas_cmd ? cas : 0;
    if (kind == store_kind::cas_cmd && token == 0) {
        return rpc_error{rpc_code::invalid_argument, "memcached cas needs a CAS token"};
    }

    const std::string full_key = impl_->prefixed(key);
    const char* command = store_command(kind);
    const std::size_t size = text_encoder::storage_size(command, full_key, value, token);
    std::vector<std::byte> request(size);
    std::size_t written = 0;
    const status encoded =
        text_encoder{}.storage(command, full_key, flags, exptime, value, token,
                               span<std::byte>(request.data(), request.size()), written);
    if (!encoded) {
        return encoded.error();
    }
    request.resize(written);

    result<text_reply> reply = exchange(span<const std::byte>(request.data(), request.size()));
    if (!reply) {
        return reply.error();
    }
    const status failure = reply_error(reply.value());
    if (!failure) {
        return failure.error();
    }
    if (reply.value().type != text_reply::kind::status) {
        return rpc_error{rpc_code::protocol_error, "memcached store reply was not a status"};
    }
    return reply.value().store;
}

result<store_status> client::erase(std::string_view key, std::uint64_t cas) {
    const std::string full_key = impl_->prefixed(key);
    std::vector<std::byte> request(full_key.size() + 32);
    std::size_t written = 0;
    const status encoded = text_encoder{}.remove(
        full_key, cas, span<std::byte>(request.data(), request.size()), written);
    if (!encoded) {
        return encoded.error();
    }
    request.resize(written);

    result<text_reply> reply = exchange(span<const std::byte>(request.data(), request.size()));
    if (!reply) {
        return reply.error();
    }
    const status failure = reply_error(reply.value());
    if (!failure) {
        return failure.error();
    }
    if (reply.value().type != text_reply::kind::status) {
        return rpc_error{rpc_code::protocol_error, "memcached delete reply was not a status"};
    }
    return reply.value().store;
}

result<std::uint64_t> client::increment(std::string_view key, std::uint64_t delta) {
    const std::string full_key = impl_->prefixed(key);
    std::vector<std::byte> request(full_key.size() + 40);
    std::size_t written = 0;
    const status encoded = text_encoder{}.arithmetic(
        "incr", full_key, delta, span<std::byte>(request.data(), request.size()), written);
    if (!encoded) {
        return encoded.error();
    }
    request.resize(written);

    result<text_reply> reply = exchange(span<const std::byte>(request.data(), request.size()));
    if (!reply) {
        return reply.error();
    }
    const status failure = reply_error(reply.value());
    if (!failure) {
        return failure.error();
    }
    if (reply.value().type == text_reply::kind::status &&
        reply.value().store == store_status::not_found) {
        // The counter does not exist. Reporting it as a number would be a lie.
        return rpc_error{rpc_code::not_found, "memcached counter does not exist"};
    }
    if (reply.value().type != text_reply::kind::number) {
        return rpc_error{rpc_code::protocol_error, "memcached incr reply was not a number"};
    }
    return reply.value().number;
}

result<std::uint64_t> client::decrement(std::string_view key, std::uint64_t delta) {
    const std::string full_key = impl_->prefixed(key);
    std::vector<std::byte> request(full_key.size() + 40);
    std::size_t written = 0;
    const status encoded = text_encoder{}.arithmetic(
        "decr", full_key, delta, span<std::byte>(request.data(), request.size()), written);
    if (!encoded) {
        return encoded.error();
    }
    request.resize(written);

    result<text_reply> reply = exchange(span<const std::byte>(request.data(), request.size()));
    if (!reply) {
        return reply.error();
    }
    const status failure = reply_error(reply.value());
    if (!failure) {
        return failure.error();
    }
    if (reply.value().type == text_reply::kind::status &&
        reply.value().store == store_status::not_found) {
        return rpc_error{rpc_code::not_found, "memcached counter does not exist"};
    }
    if (reply.value().type != text_reply::kind::number) {
        return rpc_error{rpc_code::protocol_error, "memcached decr reply was not a number"};
    }
    return reply.value().number;
}

result<store_status> client::touch(std::string_view key, std::uint64_t exptime) {
    const std::string full_key = impl_->prefixed(key);
    std::vector<std::byte> request(full_key.size() + 40);
    std::size_t written = 0;
    const status encoded = text_encoder{}.touch(
        full_key, exptime, span<std::byte>(request.data(), request.size()), written);
    if (!encoded) {
        return encoded.error();
    }
    request.resize(written);

    result<text_reply> reply = exchange(span<const std::byte>(request.data(), request.size()));
    if (!reply) {
        return reply.error();
    }
    const status failure = reply_error(reply.value());
    if (!failure) {
        return failure.error();
    }
    if (reply.value().type != text_reply::kind::status) {
        return rpc_error{rpc_code::protocol_error, "memcached touch reply was not a status"};
    }
    return reply.value().store;
}

result<std::string> client::version() {
    std::vector<std::byte> request(32);
    std::size_t written = 0;
    const status encoded =
        text_encoder{}.simple("version", {}, span<std::byte>(request.data(), request.size()),
                              written);
    if (!encoded) {
        return encoded.error();
    }
    request.resize(written);

    result<text_reply> reply = exchange(span<const std::byte>(request.data(), request.size()));
    if (!reply) {
        return reply.error();
    }
    const status failure = reply_error(reply.value());
    if (!failure) {
        return failure.error();
    }
    if (reply.value().type != text_reply::kind::version) {
        return rpc_error{rpc_code::protocol_error, "memcached version reply was not a version"};
    }
    return reply.value().text;
}

result<std::vector<std::pair<std::string, std::string>>> client::stats(
    std::string_view argument) {
    std::vector<std::byte> request(argument.size() + 32);
    std::size_t written = 0;
    const status encoded = text_encoder{}.simple(
        "stats", argument, span<std::byte>(request.data(), request.size()), written);
    if (!encoded) {
        return encoded.error();
    }
    request.resize(written);

    result<text_reply> reply = exchange(span<const std::byte>(request.data(), request.size()));
    if (!reply) {
        return reply.error();
    }
    const status failure = reply_error(reply.value());
    if (!failure) {
        return failure.error();
    }
    if (reply.value().type != text_reply::kind::stats) {
        return rpc_error{rpc_code::protocol_error, "memcached stats reply was not a stat list"};
    }
    return std::move(reply.value().stats_entries);
}

result<text_reply> client::meta_get(std::string_view key, const std::vector<std::string>& flags) {
    const std::string full_key = impl_->prefixed(key);
    std::vector<std::string_view> views;
    views.reserve(flags.size());
    for (const std::string& flag : flags) {
        views.emplace_back(flag);
    }
    std::vector<std::byte> request(
        text_encoder::meta_size("mg", full_key, views, {}) + 8);
    std::size_t written = 0;
    const status encoded =
        text_encoder{}.meta("mg", full_key, 0, views, {}, span<std::byte>(request.data(),
                                                                         request.size()),
                            written);
    if (!encoded) {
        return encoded.error();
    }
    request.resize(written);

    result<text_reply> reply = exchange(span<const std::byte>(request.data(), request.size()));
    if (!reply) {
        return reply.error();
    }
    const status failure = reply_error(reply.value());
    if (!failure) {
        return failure.error();
    }
    return reply;
}

result<store_status> client::meta_set(std::string_view key, std::string_view value,
                                      const std::vector<std::string>& flags) {
    const status size_ok = check_value_size(value);
    if (!size_ok) {
        return size_ok.error();
    }
    const std::string full_key = impl_->prefixed(key);
    std::vector<std::string_view> views;
    views.reserve(flags.size());
    for (const std::string& flag : flags) {
        views.emplace_back(flag);
    }
    std::vector<std::byte> request(
        text_encoder::meta_size("ms", full_key, views, value) + 24);
    std::size_t written = 0;
    const status encoded = text_encoder{}.meta("ms", full_key, value.size(), views, value,
                                               span<std::byte>(request.data(), request.size()),
                                               written);
    if (!encoded) {
        return encoded.error();
    }
    request.resize(written);

    result<text_reply> reply = exchange(span<const std::byte>(request.data(), request.size()));
    if (!reply) {
        return reply.error();
    }
    const status failure = reply_error(reply.value());
    if (!failure) {
        return failure.error();
    }
    // A meta store answers HD (stored) or NS (not stored); both are outcomes.
    if (reply.value().type == text_reply::kind::meta_status ||
        reply.value().type == text_reply::kind::meta_hit) {
        for (const std::string& token : reply.value().meta_tokens) {
            if (token == "HD") {
                return store_status::stored;
            }
            if (token == "NS") {
                return store_status::not_stored;
            }
        }
        return store_status::ok;
    }
    if (reply.value().type == text_reply::kind::status) {
        return reply.value().store;
    }
    return rpc_error{rpc_code::protocol_error, "memcached meta store reply was not a status"};
}

result<store_status> client::meta_delete(std::string_view key,
                                         const std::vector<std::string>& flags) {
    const std::string full_key = impl_->prefixed(key);
    std::vector<std::string_view> views;
    views.reserve(flags.size());
    for (const std::string& flag : flags) {
        views.emplace_back(flag);
    }
    std::vector<std::byte> request(text_encoder::meta_size("md", full_key, views, {}) + 8);
    std::size_t written = 0;
    const status encoded = text_encoder{}.meta(
        "md", full_key, 0, views, {}, span<std::byte>(request.data(), request.size()), written);
    if (!encoded) {
        return encoded.error();
    }
    request.resize(written);

    result<text_reply> reply = exchange(span<const std::byte>(request.data(), request.size()));
    if (!reply) {
        return reply.error();
    }
    const status failure = reply_error(reply.value());
    if (!failure) {
        return failure.error();
    }
    for (const std::string& token : reply.value().meta_tokens) {
        if (token == "HD") {
            return store_status::deleted;
        }
        if (token == "NF") {
            return store_status::not_found;
        }
        if (token == "EX") {
            return store_status::exists;
        }
    }
    return store_status::ok;
}

status client::meta_noop() {
    std::vector<std::byte> request(8);
    std::size_t written = 0;
    const status encoded =
        text_encoder{}.meta("mn", {}, 0, {}, {}, span<std::byte>(request.data(), request.size()),
                            written);
    if (!encoded) {
        return encoded;
    }
    request.resize(written);

    result<text_reply> reply = exchange(span<const std::byte>(request.data(), request.size()));
    if (!reply) {
        return reply.error();
    }
    const status failure = reply_error(reply.value());
    if (!failure) {
        return failure;
    }
    for (const std::string& token : reply.value().meta_tokens) {
        if (token == "MN") {
            return status{};
        }
    }
    return status{rpc_code::protocol_error, "memcached mn reply was not MN"};
}

transport& client::endpoint() noexcept { return *impl_->endpoint; }

std::size_t client::pool_idle() const noexcept {
    return impl_->owned ? impl_->owned->pool().idle() : 0;
}

std::size_t client::pool_in_use() const noexcept {
    return impl_->owned ? impl_->owned->pool().in_use() : 0;
}

} // namespace memcached
} // namespace crpc