// CRPC -- G19 step2: registry client implementation.
//
// Every calling thread keeps its own anchor. The anchor is `thread_local`
// because a shared one would let one thread's commit invalidate another
// thread's in-flight observation (VERDICT section 5.2's two-order two-state
// defect). The transport-level dedup key is per-call and stable across a retry,
// so a replayed request is recognised by the server even if the client's own
// anchor moved.
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include <crpc/cluster/registry_client.hpp>

#include <crpc/cluster/registry_wire.hpp>
#include <crpc/rpc/call.hpp>

namespace crpc {
namespace cluster {
namespace {

// Builds a request frame: u16 version, u32 total_len, then tags in ascending
// id order so two encodings of one message are byte-identical.
[[nodiscard]] status build_frame(const std::vector<std::byte>& tags,
                                 std::vector<std::byte>& out) {
    out.clear();
    const auto put16 = [&out](u16 value) {
        const std::size_t at = out.size();
        out.resize(at + 2);
        endian::store_le16(out.data() + at, value);
    };
    const auto put32 = [&out](u32 value) {
        const std::size_t at = out.size();
        out.resize(at + 4);
        endian::store_le32(out.data() + at, value);
    };
    put16(registry_dto_version);
    put32(static_cast<u32>(tags.size()));
    out.insert(out.end(), tags.begin(), tags.end());
    return status{};
}

void append_u8(std::vector<std::byte>& out, u8 value) {
    out.push_back(static_cast<std::byte>(value));
}

void append_u16(std::vector<std::byte>& out, u16 value) {
    const std::size_t at = out.size();
    out.resize(at + 2);
    endian::store_le16(out.data() + at, value);
}

void append_u32(std::vector<std::byte>& out, u32 value) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    endian::store_le32(out.data() + at, value);
}

void append_u64(std::vector<std::byte>& out, u64 value) {
    const std::size_t at = out.size();
    out.resize(at + 8);
    endian::store_le64(out.data() + at, value);
}

// Appends a tag: one byte id, a u32 length, then the value bytes.
void append_raw_tag(std::vector<std::byte>& tags, u8 id,
                    const std::vector<std::byte>& value) {
    tags.push_back(static_cast<std::byte>(id));
    append_u32(tags, static_cast<u32>(value.size()));
    tags.insert(tags.end(), value.begin(), value.end());
}

void append_str_tag(std::vector<std::byte>& tags, u8 id, const std::string& value) {
    std::vector<std::byte> bytes;
    append_u32(bytes, static_cast<u32>(value.size()));
    bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(value.data()),
                 reinterpret_cast<const std::byte*>(value.data()) + value.size());
    append_raw_tag(tags, id, bytes);
}

void append_u64_tag(std::vector<std::byte>& tags, u8 id, u64 value) {
    std::vector<std::byte> bytes;
    append_u64(bytes, value);
    append_raw_tag(tags, id, bytes);
}

void append_u32_tag(std::vector<std::byte>& tags, u8 id, u32 value) {
    std::vector<std::byte> bytes;
    append_u32(bytes, value);
    append_raw_tag(tags, id, bytes);
}

void append_u16_tag(std::vector<std::byte>& tags, u8 id, u16 value) {
    std::vector<std::byte> bytes;
    append_u16(bytes, value);
    append_raw_tag(tags, id, bytes);
}

void append_u8_tag(std::vector<std::byte>& tags, u8 id, u8 value) {
    std::vector<std::byte> bytes;
    append_u8(bytes, value);
    append_raw_tag(tags, id, bytes);
}

void append_meta_tag(std::vector<std::byte>& tags, u8 id, const metadata_list& meta) {
    std::vector<std::byte> encoded;
    const status s = encode_meta_map(meta, encoded);
    (void)s;
    append_raw_tag(tags, id, encoded);
}

// The idempotency key is derived from the request content, so a retry carries
// the same key and the service layer can recognise it. It is not a secret.
[[nodiscard]] std::string request_key(const std::string& service,
                                      const std::string& node_id, u64 incarnation,
                                      u64 fence, u64 writer_epoch) {
    const std::string raw = service + '\x1f' + node_id + '\x1f' +
                            std::to_string(incarnation) + '\x1f' +
                            std::to_string(fence) + '\x1f' +
                            std::to_string(writer_epoch);
    u64 hash = 1469598103934665603ull;
    for (const char c : raw) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    char buf[32];
    (void)std::snprintf(buf, sizeof(buf), "%016llx",
                        static_cast<unsigned long long>(hash));
    return std::string(buf);
}

// The stable request id for one logical write. Deliberately excludes the fence
// and the epoch: both change on every attempt, so including them would make a
// retry unrecognisable. The payload identity is what makes a retry the same
// request.
[[nodiscard]] std::string request_key(const std::string& service,
                                      const std::string& node_id, u64 incarnation) {
    return request_key(service, node_id, incarnation, 0, 0);
}

} // namespace

struct registry_client::impl {
    std::shared_ptr<rpc::client> transport;
    registry_client_options options;
    // Per-thread anchors, keyed by (thread id, catalog key). A shared anchor
    // would let one thread's commit invalidate another thread's in-flight
    // observation, which is the two-order two-state defect VERDICT section 5.2
    // measured. `thread_local` storage would be simpler but makes the anchor
    // unobservable from a test; a thread-id key keeps it inspectable.
    mutable std::mutex mutex;
    std::map<std::string, registry_anchor> anchors;
    // The request identity of the write currently in flight on this thread. A
    // stale-anchor retry reuses it so the service's idempotency map recognises
    // the retry as the same logical request.
    std::string request_identity;
};

registry_client::registry_client(std::unique_ptr<rpc::client> transport,
                                 registry_client_options options)
    : impl_(new impl()) {
    impl_->transport = std::shared_ptr<rpc::client>(std::move(transport));
    impl_->options = std::move(options);
}

registry_client::registry_client(std::shared_ptr<rpc::client> transport,
                                 registry_client_options options)
    : impl_(new impl()) {
    impl_->transport = std::move(transport);
    impl_->options = std::move(options);
}

registry_client::~registry_client() = default;

namespace {

// Appends the token's issue time to every request. All seven methods carry it,
// because the service applies the window check to all six handlers; a method
// that omits it would be denied for a reason that has nothing to do with the
// caller's authority. The value is system_clock milliseconds.
void append_token_window(std::vector<std::byte>& tags,
                         const registry_client_options& options) {
    if (options.token_issued_at.count() != 0) {
        append_u64_tag(tags, registry_tag_token_issued_at,
                       static_cast<crpc::u64>(options.token_issued_at.count()));
    }
}

// The storage key for one (thread, catalog key) pair.
[[nodiscard]] std::string anchor_slot(const std::string& key) {
    std::uint64_t hash = 1469598103934665603ull;
    const std::thread::id id = std::this_thread::get_id();
    const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
    for (std::size_t i = 0; i < sizeof(id); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    for (const char c : key) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    char buf[32];
    (void)std::snprintf(buf, sizeof(buf), "%016llx",
                        static_cast<unsigned long long>(hash));
    return std::string(buf);
}

} // namespace

void registry_client::set_request_identity(const std::string& identity) {
    impl_->request_identity = identity;
}

void registry_client::set_stale_retries(int retries) noexcept {
    impl_->options.stale_retries = retries;
}

void registry_client::set_token_issued_at(std::chrono::milliseconds issued_at) noexcept {
    impl_->options.token_issued_at = issued_at;
}

registry_anchor registry_client::local_anchor() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->anchors.find(anchor_slot(""));
    return found != impl_->anchors.end() ? found->second : registry_anchor{};
}

registry_anchor registry_client::local_anchor_for(const std::string& key) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->anchors.find(anchor_slot(key));
    return found != impl_->anchors.end() ? found->second : registry_anchor{};
}

namespace {

// Extracts `registry.index` from an error's metadata.
[[nodiscard]] u64 error_index(const rpc_error& error) {
    const std::string* found = error.metadata.find("registry.index");
    if (found == nullptr) {
        return 0;
    }
    u64 value = 0;
    try {
        value = std::stoull(*found);
    } catch (...) {
        return 0;
    }
    return value;
}

} // namespace

result<registry_write_response> registry_client::register_instance(
    const service_instance& instance, u64 incarnation, u64 writer_epoch) {
    return register_instance(instance, incarnation, writer_epoch,
                             impl_->options.stale_retries);
}

result<registry_write_response> registry_client::register_instance(
    const service_instance& instance, u64 incarnation, u64 writer_epoch,
    int stale_retries_left) {
    const std::string key = instance.service + '\x1f' + instance.node_id;
    // Establish the request identity once per logical write. A stale-anchor
    // retry inside this call reuses it; a later, unrelated write gets a fresh
    // one.
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->request_identity.empty()) {
            impl_->request_identity =
                request_key(instance.service, instance.node_id, incarnation);
        }
    }
    // 1. Allocate a fence for this key from the authority.
    const result<u64> fence = next_fence(instance.service, instance.node_id);
    if (!fence.ok()) {
        return fence.error();
    }

    // 2. Build the request. The client anchor travels in `client_request_id`
    //    metadata so the service layer can recognise an exact replay.
    const registry_anchor anchor = [&] {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->anchors.find(anchor_slot(key));
        return found != impl_->anchors.end() ? found->second : registry_anchor{};
    }();

    std::vector<std::byte> tags;
    append_str_tag(tags, registry_tag_token, impl_->options.token);
    append_token_window(tags, impl_->options);
    append_str_tag(tags, registry_tag_service, instance.service);
    append_str_tag(tags, registry_tag_node_id, instance.node_id);
    append_str_tag(tags, registry_tag_host, instance.address.host);
    append_u16_tag(tags, registry_tag_port, instance.address.port);
    append_u64_tag(tags, registry_tag_incarnation, incarnation);
    append_u64_tag(tags, registry_tag_fence, fence.value());
    append_u64_tag(tags, registry_tag_writer_epoch, writer_epoch);
    // A stable request id for this logical write. It is derived from the
    // payload and the incarnation, so a retry of the same write carries the
    // same id and the service's idempotency map recognises it (the fence is
    // re-allocated on every call and cannot serve as the key).
    if (!impl_->request_identity.empty()) {
        append_str_tag(tags, registry_tag_client_request_id, impl_->request_identity);
    } else {
        append_str_tag(tags, registry_tag_client_request_id,
                       request_key(instance.service, instance.node_id, incarnation));
    }
    // The client's anchor rides in the reserved index tag: 0 means "no anchor".
    // In step2 the authority epoch is constant, so the index alone carries the
    // dictionary order; step3 adds the epoch without changing this tag.
    append_u64_tag(tags, registry_tag_index, anchor.index);
    append_u32_tag(tags, registry_tag_weight, instance.weight);
    append_u8_tag(tags, registry_tag_state, static_cast<u8>(instance.state));
    if (!instance.meta.empty()) {
        append_meta_tag(tags, registry_tag_meta, instance.meta);
    }

    std::vector<std::byte> body;
    const status framed = build_frame(tags, body);
    if (!framed.ok()) {
        return framed.error();
    }

    // 3. Send. `call_context::metadata` is not used for the credential: the
    //    token is in the DTO body (VERDICT section 1.4).
    rpc::call_context ctx;
    ctx.set_timeout(std::chrono::duration_cast<std::chrono::nanoseconds>(
        impl_->options.default_timeout));
    ctx.set_idempotent(true);

    auto started = impl_->transport->start_call<std::vector<std::byte>>(
        registry_service_id, registry_method_register,
        span<const std::byte>(body.data(), body.size()), ctx);
    if (!started.ok()) {
        return started.error();
    }
    auto finished = std::move(started).value().get();
    if (!finished.ok()) {
        const rpc_error& error = finished.error();
        const std::string* reason = error.metadata.find("registry.reason");
        if (reason != nullptr && *reason == registry_reason_stale_anchor) {
            // Re-anchor from the authority's index in the error metadata, then
            // retry ONCE. The retry is the third layer of the three-layer
            // retry; a second stale answer is returned to the caller verbatim
            // (with its metadata), never rebuilt -- rebuilding would drop
            // `registry.reason` and `registry.index`, which is exactly what
            // the caller needs in order to re-anchor.
            const registry_anchor observed{0, error_index(error)};
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->anchors[anchor_slot(key)] = observed;
            }
            if (stale_retries_left > 0) {
                // The retry carries the DECREMENTED budget. Without the
                // decrement the recursion never terminates: every level sees
                // another `stale_anchor` (the authority stays ahead of a
                // client that keeps presenting its old observation) and the
                // stack overflows. A second stale answer is returned verbatim
                // with its metadata, never rebuilt.
                return register_instance(instance, incarnation, writer_epoch,
                                         stale_retries_left - 1);
            }
            return error;
        }
        return error;
    }
    {
        registry_write_response out;
        const status decoded = decode_write_response(
            span<const std::byte>(finished.value().data(), finished.value().size()),
            out);
        if (!decoded.ok()) {
            return decoded.error();
        }
        // Record the anchor the service just accepted so the next write on this
        // thread presents an up-to-date observation instead of re-using a stale
        // one (which the service would reject as stale_anchor).
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->anchors[anchor_slot(key)] = registry_anchor{out.authority_epoch,
                                                               out.index};
            impl_->request_identity.clear();
        }
        return out;
    }
}

result<registry_write_response> registry_client::deregister(
    const std::string& service, const std::string& node_id, u64 incarnation,
    u64 writer_epoch) {
    const result<u64> fence = next_fence(service, node_id);
    if (!fence.ok()) {
        return fence.error();
    }
    std::vector<std::byte> tags;
    append_str_tag(tags, registry_tag_token, impl_->options.token);
    append_token_window(tags, impl_->options);
    append_str_tag(tags, registry_tag_service, service);
    append_str_tag(tags, registry_tag_node_id, node_id);
    append_u64_tag(tags, registry_tag_incarnation, incarnation);
    append_u64_tag(tags, registry_tag_fence, fence.value());
    append_u64_tag(tags, registry_tag_writer_epoch, writer_epoch);

    std::vector<std::byte> body;
    const status framed = build_frame(tags, body);
    if (!framed.ok()) {
        return framed.error();
    }
    rpc::call_context ctx;
    ctx.set_timeout(std::chrono::duration_cast<std::chrono::nanoseconds>(
        impl_->options.default_timeout));
    ctx.set_idempotent(true);
    auto started = impl_->transport->start_call<std::vector<std::byte>>(
        registry_service_id, registry_method_deregister,
        span<const std::byte>(body.data(), body.size()), ctx);
    if (!started.ok()) {
        return started.error();
    }
    auto finished = std::move(started).value().get();
    if (!finished.ok()) {
        return finished.error();
    }
    {
        registry_write_response out;
        const status decoded = decode_write_response(
            span<const std::byte>(finished.value().data(), finished.value().size()),
            out);
        if (!decoded.ok()) {
            return decoded.error();
        }
        return out;
    }
}

result<registry_write_response> registry_client::set_state(
    const std::string& service, const std::string& node_id, instance_state state,
    u64 incarnation, u64 writer_epoch) {
    const result<u64> fence = next_fence(service, node_id);
    if (!fence.ok()) {
        return fence.error();
    }
    std::vector<std::byte> tags;
    append_str_tag(tags, registry_tag_token, impl_->options.token);
    append_token_window(tags, impl_->options);
    append_str_tag(tags, registry_tag_service, service);
    append_str_tag(tags, registry_tag_node_id, node_id);
    append_u64_tag(tags, registry_tag_incarnation, incarnation);
    append_u64_tag(tags, registry_tag_fence, fence.value());
    append_u64_tag(tags, registry_tag_writer_epoch, writer_epoch);
    append_u8_tag(tags, registry_tag_state, static_cast<u8>(state));

    std::vector<std::byte> body;
    const status framed = build_frame(tags, body);
    if (!framed.ok()) {
        return framed.error();
    }
    rpc::call_context ctx;
    ctx.set_timeout(std::chrono::duration_cast<std::chrono::nanoseconds>(
        impl_->options.default_timeout));
    ctx.set_idempotent(true);
    auto started = impl_->transport->start_call<std::vector<std::byte>>(
        registry_service_id, registry_method_set_state,
        span<const std::byte>(body.data(), body.size()), ctx);
    if (!started.ok()) {
        return started.error();
    }
    auto finished = std::move(started).value().get();
    if (!finished.ok()) {
        return finished.error();
    }
    {
        registry_write_response out;
        const status decoded = decode_write_response(
            span<const std::byte>(finished.value().data(), finished.value().size()),
            out);
        if (!decoded.ok()) {
            return decoded.error();
        }
        return out;
    }
}

result<registry_query_response> registry_client::query(const std::string& service,
                                                       const std::string& node_id,
                                                       bool passing_only) {
    std::vector<std::byte> tags;
    append_str_tag(tags, registry_tag_token, impl_->options.token);
    append_token_window(tags, impl_->options);
    if (!service.empty()) {
        append_str_tag(tags, registry_tag_service, service);
    }
    if (!node_id.empty()) {
        append_str_tag(tags, registry_tag_node_id, node_id);
    }
    if (passing_only) {
        append_u8_tag(tags, registry_tag_passing_only, 1);
    }
    std::vector<std::byte> body;
    const status framed = build_frame(tags, body);
    if (!framed.ok()) {
        return framed.error();
    }
    rpc::call_context ctx;
    ctx.set_timeout(std::chrono::duration_cast<std::chrono::nanoseconds>(
        impl_->options.default_timeout));
    auto started = impl_->transport->start_call<std::vector<std::byte>>(
        registry_service_id, registry_method_query,
        span<const std::byte>(body.data(), body.size()), ctx);
    if (!started.ok()) {
        return started.error();
    }
    auto finished = std::move(started).value().get();
    if (!finished.ok()) {
        return finished.error();
    }
    // Decode the query response from the returned body.
    registry_query_response out;
    const status decoded = decode_query_response(
        span<const std::byte>(finished.value().data(), finished.value().size()),
        out);
    if (!decoded.ok()) {
        return decoded.error();
    }
    return out;
}

result<registry_anchor> registry_client::anchor() {
    std::vector<std::byte> tags;
    append_str_tag(tags, registry_tag_token, impl_->options.token);
    append_token_window(tags, impl_->options);
    std::vector<std::byte> body;
    const status framed = build_frame(tags, body);
    if (!framed.ok()) {
        return framed.error();
    }
    rpc::call_context ctx;
    ctx.set_timeout(std::chrono::duration_cast<std::chrono::nanoseconds>(
        impl_->options.default_timeout));
    auto started = impl_->transport->start_call<std::vector<std::byte>>(
        registry_service_id, registry_method_ping,
        span<const std::byte>(body.data(), body.size()), ctx);
    if (!started.ok()) {
        return started.error();
    }
    auto finished = std::move(started).value().get();
    if (!finished.ok()) {
        return finished.error();
    }
    const auto& raw = finished.value();
    registry_anchor out;
    std::vector<registry_tag> decoded_tags;
    const status walked = decode_registry_tags(
        span<const std::byte>(raw.data(), raw.size()), decoded_tags);
    if (!walked.ok()) {
        return walked.error();
    }
    for (const registry_tag& tag : decoded_tags) {
        if (tag.id == registry_tag_incarnation && tag.value.size() == sizeof(u64)) {
            out.authority_epoch = endian::load_le64(tag.value.data());
        } else if (tag.id == registry_tag_fence && tag.value.size() == sizeof(u64)) {
            out.index = endian::load_le64(tag.value.data());
        }
    }
    return out;
}

result<u64> registry_client::next_fence(const std::string& service,
                                        const std::string& node_id) {
    std::vector<std::byte> tags;
    append_str_tag(tags, registry_tag_token, impl_->options.token);
    append_token_window(tags, impl_->options);
    append_str_tag(tags, registry_tag_service, service);
    append_str_tag(tags, registry_tag_node_id, node_id);
    std::vector<std::byte> body;
    const status framed = build_frame(tags, body);
    if (!framed.ok()) {
        return framed.error();
    }
    rpc::call_context ctx;
    ctx.set_timeout(std::chrono::duration_cast<std::chrono::nanoseconds>(
        impl_->options.default_timeout));
    auto started = impl_->transport->start_call<std::vector<std::byte>>(
        registry_service_id, registry_method_next_fence,
        span<const std::byte>(body.data(), body.size()), ctx);
    if (!started.ok()) {
        return started.error();
    }
    auto finished = std::move(started).value().get();
    if (!finished.ok()) {
        return finished.error();
    }
    const auto& raw = finished.value();
    std::vector<registry_tag> decoded_tags;
    const status walked = decode_registry_tags(
        span<const std::byte>(raw.data(), raw.size()), decoded_tags);
    if (!walked.ok()) {
        return walked.error();
    }
    for (const registry_tag& tag : decoded_tags) {
        if (tag.id == registry_tag_fence && tag.value.size() == sizeof(u64)) {
            return endian::load_le64(tag.value.data());
        }
    }
    return rpc_error(rpc_code::decode_error,
                     "registry_client: next_fence response carried no fence");
}

result<registry_anchor> registry_client::ping() { return anchor(); }

} // namespace cluster
} // namespace crpc
