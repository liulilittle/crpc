// CRPC -- TiDB optimistic-transaction retry support.
#include <crpc/db/tidb/retry.hpp>

#include <cstdlib>

namespace crpc {
namespace db {
namespace tidb {

bool is_transaction_conflict(const rpc_error& error) noexcept {
    // The MySQL layer stamps every server ERR packet with the raw provider
    // code in metadata["sql_code"], so the discriminator is exact instead of
    // a string match on the human-readable message.
    static constexpr std::uint16_t conflict_codes[] = {9007, 8027, 1205, 1213};
    const std::string* raw = error.metadata.find("sql_code");
    if (raw == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(raw->c_str(), &end, 10);
    if (end == raw->c_str() || *end != '\0') {
        return false;
    }
    for (const std::uint16_t code : conflict_codes) {
        if (parsed == static_cast<unsigned long>(code)) {
            return true;
        }
    }
    return false;
}

} // namespace tidb
} // namespace db
} // namespace crpc