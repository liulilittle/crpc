// CRPC -- the shared database byte channel.
//
// The implementation lives in `crpc::db` so every adapter uses one socket
// wrapper; this file is the MySQL-facing name for it.
#include <crpc/db/channel.hpp>

#include <utility>

namespace crpc {
namespace db {

channel::~channel() = default;

socket_channel::socket_channel(crpc::net::connection_config config,
                               crpc::backend_factory& backend, crpc::runtime* shard)
    : link_(new crpc::net::connection(std::move(config), backend, shard)) {}

socket_channel::~socket_channel() { close(); }

result<std::vector<std::byte>> socket_channel::read_exact(std::size_t count) {
    if (!link_) {
        return rpc_error{rpc_code::shutting_down, "channel is closed"};
    }
    return link_->read_exact(count);
}

result<std::size_t> socket_channel::read_some(span<std::byte> buffer) {
    if (!link_) {
        return rpc_error{rpc_code::shutting_down, "channel is closed"};
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
    // One blocking byte proves the peer is alive, then whatever the transport has
    // already buffered is taken in the same call. That keeps a TLS record layer from
    // making one syscall per byte while still never blocking for more than one byte.
    result<std::vector<std::byte>> first = link_->read_exact(1);
    if (!first) {
        return first.error();
    }
    buffer[0] = first.value()[0];
    std::size_t total = 1;
    if (buffer.size() > 1) {
        const std::vector<std::byte> more = link_->read_available();
        const std::size_t take = more.size() < buffer.size() - 1 ? more.size() : buffer.size() - 1;
        for (std::size_t i = 0; i < take; ++i) {
            buffer[1 + i] = more[i];
        }
        total += take;
    }
    return total;
}

status socket_channel::write(span<const std::byte> data) {
    if (!link_) {
        return status{rpc_code::shutting_down, "channel is closed"};
    }
    return link_->write(data);
}

void socket_channel::close() noexcept {
    if (link_) {
        link_->close();
    }
}

bool socket_channel::is_open() const noexcept { return link_ != nullptr && link_->is_open(); }

} // namespace db
} // namespace crpc