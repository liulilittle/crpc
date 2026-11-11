// CRPC -- G19 step4 PR-7: kv_store implementation.
#include <crpc/cluster/kv_store.hpp>

#include <algorithm>
#include <cstddef>

namespace crpc {
namespace cluster {

bool kv_store::valid_key(const std::string& key) noexcept {
    if (key.empty()) {
        return false;
    }
    if (key.size() > kMaxKeyBytes) {
        return false;
    }
    for (const char c : key) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f) {
            return false;
        }
    }
    return true;
}

result<kv_entry> kv_store::get(const std::string& key) const {
    if (!valid_key(key)) {
        return result<kv_entry>(rpc_code::invalid_argument);
    }
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return result<kv_entry>(rpc_code::not_found);
    }
    return it->second;
}

kv_write_result kv_store::put(const std::string& key,
                              std::vector<std::byte> value,
                              u64 index) {
    kv_write_result out;
    if (!valid_key(key)) {
        out.kind = kv_result_kind::invalid_key;
        return out;
    }
    kv_entry entry;
    entry.key = key;
    entry.value = std::move(value);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        entry.create_index = index;
    } else {
        entry.create_index = it->second.create_index;
    }
    entry.modify_index = index;
    entry.lock_index = it != entries_.end() ? it->second.lock_index : 0;
    entry.flags = it != entries_.end() ? it->second.flags : std::vector<std::string>{};
    entries_[key] = std::move(entry);
    highest_index_ = std::max(highest_index_, index);
    out.kind = kv_result_kind::ok;
    out.index = index;
    out.modify_index = index;
    return out;
}

kv_write_result kv_store::cas(const std::string& key,
                              std::vector<std::byte> value,
                              u64 cas_index, u64 index) {
    kv_write_result out;
    if (!valid_key(key)) {
        out.kind = kv_result_kind::invalid_key;
        return out;
    }
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        if (cas_index == 0) {
            kv_entry entry;
            entry.key = key;
            entry.value = std::move(value);
            entry.create_index = index;
            entry.modify_index = index;
            entries_[key] = std::move(entry);
            highest_index_ = std::max(highest_index_, index);
            out.kind = kv_result_kind::ok;
            out.index = index;
            out.modify_index = index;
            return out;
        }
        out.kind = kv_result_kind::cas_conflict;
        return out;
    }
    const kv_entry& current = it->second;
    if (cas_index == 0) {
        out.kind = kv_result_kind::cas_conflict;
        return out;
    }
    if (current.modify_index != cas_index) {
        out.kind = kv_result_kind::cas_conflict;
        return out;
    }
    kv_entry next = current;
    next.value = std::move(value);
    next.modify_index = index;
    entries_[key] = std::move(next);
    highest_index_ = std::max(highest_index_, index);
    out.kind = kv_result_kind::ok;
    out.index = index;
    out.modify_index = index;
    return out;
}

kv_write_result kv_store::erase(const std::string& key, u64 index) {
    kv_write_result out;
    if (!valid_key(key)) {
        out.kind = kv_result_kind::invalid_key;
        return out;
    }
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        out.kind = kv_result_kind::not_found;
        return out;
    }
    entries_.erase(it);
    highest_index_ = std::max(highest_index_, index);
    out.kind = kv_result_kind::ok;
    out.index = index;
    out.modify_index = index;
    return out;
}

std::vector<std::string> kv_store::keys_with_prefix(
    const std::string& prefix) const {
    std::vector<std::string> out;
    for (const auto& pair : entries_) {
        const std::string& key = pair.first;
        if (key.size() >= prefix.size() &&
            key.compare(0, prefix.size(), prefix) == 0) {
            out.push_back(key);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

u64 kv_store::highest_index() const noexcept {
    return highest_index_;
}

}} // namespace crpc::cluster
