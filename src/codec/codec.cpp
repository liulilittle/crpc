// CRPC -- codec registry implementation and built-in codec registration.
//
// The registry is a small immutable snapshot after freeze(): lookups load one
// atomic pointer and never take a lock on the hot path. Before freeze() a
// shared mutex serializes readers against registration.
#include <crpc/codec.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace crpc {
namespace {

// Content types are ASCII case-insensitive and parameters after ';' are
// ignored. Trimming is allocation-free; the canonical stored form is compared
// with the same rule.
bool ascii_iequal(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        char a = lhs[i];
        char b = rhs[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::string_view trim_content_type(std::string_view value) noexcept {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    const std::size_t semicolon = value.find(';');
    if (semicolon != std::string_view::npos) {
        value = value.substr(0, semicolon);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool initialize_default(codec_registry& registry) {
    (void)registry.add(std::make_unique<binary_codec>());
#if defined(CRPC_HAVE_RTTI_JSON)
    (void)registry.add(std::make_unique<json_codec>());
#endif
#if defined(CRPC_HAVE_RTTI_XML)
    (void)registry.add(std::make_unique<xml_codec>());
#endif
#if defined(CRPC_HAVE_RTTI_INI)
    (void)registry.add(std::make_unique<ini_codec>());
#endif
#if defined(CRPC_HAVE_RTTI_TOML)
    (void)registry.add(std::make_unique<toml_codec>());
#endif
#if defined(CRPC_HAVE_RTTI_PROTOBUF)
    (void)registry.add(std::make_unique<protobuf_codec>());
#endif
#if defined(CRPC_HAVE_RTTI_FLATBUFFERS)
    (void)registry.add(std::make_unique<flatbuffers_codec>());
#endif
    (void)registry.freeze();
    return true;
}

} // namespace

struct codec_registry::snapshot {
    std::unordered_map<u16, const codec*> by_id;
    std::vector<std::pair<std::string, const codec*>> by_content_type;
};

codec_registry::~codec_registry() { delete snapshot_owned_; }

status codec_registry::add(std::unique_ptr<codec> entry) {
    if (!entry) {
        return status{rpc_code::invalid_argument};
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (frozen_) {
        return status{rpc_code::failed_precondition};
    }
    const u16 key = static_cast<u16>(entry->id());
    if (key == 0) {
        return status{rpc_code::invalid_argument};
    }
    if (by_id_.find(key) != by_id_.end()) {
        return status{rpc_code::failed_precondition};
    }
    const std::string_view content_type =
        trim_content_type(entry->content_type());
    if (content_type.empty()) {
        return status{rpc_code::invalid_argument};
    }
    for (const auto& existing : by_content_type_) {
        if (ascii_iequal(existing.first, content_type)) {
            return status{rpc_code::failed_precondition};
        }
    }

    const codec* raw = entry.get();
    by_id_.emplace(key, raw);
    by_content_type_.emplace_back(std::string(content_type), raw);
    owned_.push_back(std::move(entry));
    return status{};
}

const codec* codec_registry::find(codec_id id) const noexcept {
    const u16 key = static_cast<u16>(id);
    const snapshot* frozen = snapshot_.load(std::memory_order_acquire);
    if (frozen != nullptr) {
        const auto it = frozen->by_id.find(key);
        return it == frozen->by_id.end() ? nullptr : it->second;
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = by_id_.find(key);
    return it == by_id_.end() ? nullptr : it->second;
}

const codec* codec_registry::find(std::string_view content_type) const noexcept {
    const std::string_view wanted = trim_content_type(content_type);
    if (wanted.empty()) {
        return nullptr;
    }
    const snapshot* frozen = snapshot_.load(std::memory_order_acquire);
    if (frozen != nullptr) {
        for (const auto& entry : frozen->by_content_type) {
            if (ascii_iequal(entry.first, wanted)) {
                return entry.second;
            }
        }
        return nullptr;
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& entry : by_content_type_) {
        if (ascii_iequal(entry.first, wanted)) {
            return entry.second;
        }
    }
    return nullptr;
}

status codec_registry::freeze() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (frozen_) {
        return status{};
    }
    snapshot* next = new (std::nothrow) snapshot();
    if (next == nullptr) {
        return status{rpc_code::resource_exhausted};
    }
    next->by_id = by_id_;
    next->by_content_type = by_content_type_;
    snapshot_.store(next, std::memory_order_release);
    snapshot_owned_ = next;
    frozen_ = true;
    return status{};
}

bool codec_registry::frozen() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return frozen_;
}

std::size_t codec_registry::size() const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return owned_.size();
}

codec_registry& default_codec_registry() {
    static codec_registry registry;
    static const bool initialized = initialize_default(registry);
    (void)initialized;
    return registry;
}

} // namespace crpc
