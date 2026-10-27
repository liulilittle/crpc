// CRPC -- the byte channel a MySQL connection runs on.
//
// The channel itself lives in `crpc::db` because TDS uses the same three
// operations; these names keep the MySQL-facing spelling that was already part of
// the public API.
#ifndef CRPC_DB_MYSQL_CHANNEL_HPP
#define CRPC_DB_MYSQL_CHANNEL_HPP

#include <crpc/db/channel.hpp>

namespace crpc {
namespace db {
namespace mysql {

using db::channel;
using db::socket_channel;

} // namespace mysql
} // namespace db
} // namespace crpc

#endif // CRPC_DB_MYSQL_CHANNEL_HPP