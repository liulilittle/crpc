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