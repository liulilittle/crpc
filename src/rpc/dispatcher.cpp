// CRPC -- service registry and server-side dispatch implementation.
#include <crpc/rpc/dispatcher.hpp>

#include <string>

#include <crpc/resilience/admission.hpp>
#include <crpc/wire.hpp>

namespace crpc::rpc {

// The claim type is nested in the table; this is the spelling dispatch uses.
using dedup_lease = dedup_table::lease;

namespace {

void append_u16(buffer& out, u16 value) {
    std::byte tmp[2];
    endian::store_le16(tmp, value);
    (void)out.append(tmp, sizeof(tmp));
}

void append_varint(buffer& out, u64 value) {
    while (value >= 0x80u) {
        (void)out.push_back(static_cast<std::byte>((value & 0x7Fu) | 0x80u));
        value >>= 7;
    }
    (void)out.push_back(static_cast<std::byte>(value));
}

u16 clamp_u16(std::size_t value) {
    return value > 0xFFFFu ? static_cast<u16>(0xFFFFu) : static_cast<u16>(value);
}

// Produces an error response unless the request was a fire-and-forget ONE_WAY.
// The whole `rpc_error` is preserved, not just its code and message: a handler
// may attach metadata, a retry hint or a retryable flag, and dropping them here
// made every server-side marker (other than the one `send_error` builds itself)
// unreachable.
status respond_error(buffer& out, bool& has_response, bool wants_response,
                     const rpc_error& error) {
    if (!wants_response) {
        return status{};
    }
    has_response = true;
    return encode_response(error.code, error, span<const std::byte>(), out);
}

// Rebuilds the per-call context a handler sees from the decoded envelope.
// `now` is the dispatcher's reference instant; the wire carries the time left,
// so the local deadline is `now` plus that duration.
call_context build_context(const call_envelope& envelope,
                           std::chrono::steady_clock::time_point now) {
    call_context ctx;
    const trace_extension& ext = envelope.extensions;
    ctx.has_deadline = ext.has(trace_extension::has_deadline);
    if (ext.has(trace_extension::has_deadline)) {
        ctx.deadline = now + std::chrono::nanoseconds(
                                 static_cast<std::chrono::nanoseconds::rep>(ext.deadline_ns));
    }
    if (ext.has(trace_extension::has_trace_id)) {
        ctx.trace.trace.bytes = ext.trace_id;
    }
    if (ext.has(trace_extension::has_span_id)) {
        ctx.trace.span.bytes = ext.span_id;
    }
    if (ext.has(trace_extension::has_parent_span_id)) {
        ctx.trace.parent.bytes = ext.parent_span_id;
    }
    if (ext.has(trace_extension::has_idempotency_key)) {
        ctx.has_idempotency_key = true;
        ctx.idempotency = call_id::decode(ext.idempotency_key);
    }
    return ctx;
}

// Absolute local deadline the wire deadline resolves to, or the dispatch instant
// when the frame carries none. Shared by the dedup wait bound and the context.
std::chrono::steady_clock::time_point wire_deadline_of(
    const call_envelope& envelope, std::chrono::steady_clock::time_point now) {
    if (envelope.extensions.has(trace_extension::has_deadline)) {
        return now + std::chrono::nanoseconds(
                         static_cast<std::chrono::nanoseconds::rep>(
                             envelope.extensions.deadline_ns));
    }
    return now;
}

// Completes a leader claim at scope exit with whatever the response buffer
// holds. Exception-neutral: during unwinding the entry is completed without a
// payload (`canceled`), because the buffer state is undefined; a completion that
// itself fails to allocate falls back to the allocation-free path. A duplicate
// suspended against this leader is therefore never stranded.
struct leader_completion {
    leader_completion(dedup_lease& lease_ref, buffer& out, bool& has_resp, bool active) noexcept
        : lease(lease_ref), response_out(out), has_response(has_resp), active(active) {}

    ~leader_completion() {
        if (!active || !lease.is_leader()) {
            return;
        }
        if (std::uncaught_exceptions() != armed) {
            lease.complete_failed();
            return;
        }
        try {
            lease.complete(has_response ? response_out.readable() : span<const std::byte>(),
                           ok, code);
        } catch (...) {
            lease.complete_failed();
        }
    }

    dedup_lease& lease;
    buffer& response_out;
    bool& has_response;
    bool active = false;
    bool ok = false;
    rpc_code code = rpc_code::canceled;
    int armed = std::uncaught_exceptions();
};

} // namespace

// --- Response payload codec --------------------------------------------------

status encode_response(rpc_code code, const rpc_error& error, span<const std::byte> body,
                       buffer& out) noexcept {
    out.clear();
    if (code == rpc_code::ok) {
        if (!out.push_back(std::byte{0})) {
            return status{rpc_code::encode_error};
        }
        if (!body.empty() && !out.append(body)) {
            return status{rpc_code::encode_error};
        }
        return status{};
    }

    if (!out.push_back(std::byte{1})) {
        return status{rpc_code::encode_error};
    }
    append_varint(out, static_cast<u64>(code));
    if (!out.push_back(error.retryable ? std::byte{1} : std::byte{0})) {
        return status{rpc_code::encode_error};
    }
    append_varint(out, static_cast<u64>(error.retry_after.count()));

    const u16 message_length = clamp_u16(error.message.size());
    append_u16(out, message_length);
    if (message_length != 0 && !out.append(error.message.data(), message_length)) {
        return status{rpc_code::encode_error};
    }

    const u16 meta_count = clamp_u16(error.metadata.size());
    append_u16(out, meta_count);
    std::size_t written = 0;
    for (const auto& entry : error.metadata) {
        if (written >= meta_count) {
            break;
        }
        ++written;
        const u16 key_length = clamp_u16(entry.first.size());
        append_u16(out, key_length);
        if (key_length != 0 && !out.append(entry.first.data(), key_length)) {
            return status{rpc_code::encode_error};
        }
        const u16 value_length = clamp_u16(entry.second.size());
        append_u16(out, value_length);
        if (value_length != 0 && !out.append(entry.second.data(), value_length)) {
            return status{rpc_code::encode_error};
        }
    }
    return status{};
}

status decode_response(span<const std::byte> in, rpc_code& code, rpc_error& error,
                       span<const std::byte>& body) noexcept {
    // Decoding a response allocates (the message string and every metadata
    // entry), and this function is `noexcept` -- the low-level client's reader
    // calls it. An allocation failure would reach `std::terminate`. Report it
    // as a decode error instead; `error` and `body` are left unchanged because
    // `decoded` is only published at the end.
    try {
        byte_reader reader(in);
        u8 status_byte = 0;
        if (!reader.get_u8(status_byte)) {
            return status{rpc_code::decode_error};
        }
        if (status_byte == 0) {
            code = rpc_code::ok;
            error = rpc_error{};
            body = reader.remaining_bytes();
            return status{};
        }
        if (status_byte != 1) {
            return status{rpc_code::protocol_error};
        }

        u64 raw_code = 0;
        if (!reader.get_varint(raw_code)) {
            return status{rpc_code::decode_error};
        }
        rpc_error decoded;
        if (raw_code > static_cast<u64>(rpc_code::application)) {
            decoded.metadata.set("crpc.raw_code", std::to_string(raw_code));
            decoded.code = rpc_code::internal;
        } else {
            decoded.code = static_cast<rpc_code>(raw_code);
        }

        u8 retryable = 0;
        u64 retry_after = 0;
        u16 message_length = 0;
        if (!reader.get_u8(retryable) || !reader.get_varint(retry_after) ||
            !reader.get_u16(message_length)) {
            return status{rpc_code::decode_error};
        }
        span<const std::byte> message;
        if (!reader.get_bytes(message_length, message)) {
            return status{rpc_code::decode_error};
        }
        decoded.message.assign(reinterpret_cast<const char*>(message.data()), message.size());

        u16 meta_count = 0;
        if (!reader.get_u16(meta_count)) {
            return status{rpc_code::decode_error};
        }
        for (u16 i = 0; i < meta_count; ++i) {
            u16 key_length = 0;
            u16 value_length = 0;
            span<const std::byte> key;
            span<const std::byte> value;
            if (!reader.get_u16(key_length) || !reader.get_bytes(key_length, key) ||
                !reader.get_u16(value_length) || !reader.get_bytes(value_length, value)) {
                return status{rpc_code::decode_error};
            }
            decoded.metadata.set(
                std::string(reinterpret_cast<const char*>(key.data()), key.size()),
                std::string(reinterpret_cast<const char*>(value.data()), value.size()));
        }
        decoded.retryable = retryable != 0;
        decoded.retry_after =
            std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(retry_after));

        code = decoded.code;
        error = std::move(decoded);
        body = span<const std::byte>();
        return status{};
    } catch (...) {
        // Allocation failure while decoding. Report it as a decode error; the
        // catch body performs no allocation.
        return status{rpc_code::decode_error};
    }
}

// --- method_slot -------------------------------------------------------------

method_slot::method_slot(u64 service_id, u64 method_id, std::string name,
                         method_handler handler, void* self, method_options options) noexcept
    : service_id_(service_id),
      method_id_(method_id),
      name_(std::move(name)),
      handler_(handler),
      self_(self),
      max_concurrency_(options.max_concurrency) {}

bool method_slot::enter() const noexcept {
    const u32 limit = max_concurrency_;
    u32 current = in_flight_.load(std::memory_order_relaxed);
    for (;;) {
        if (limit != 0 && current >= limit) {
            return false;
        }
        if (in_flight_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
            return true;
        }
    }
}

void method_slot::leave() const noexcept {
    u32 current = in_flight_.load(std::memory_order_relaxed);
    while (current != 0 &&
           !in_flight_.compare_exchange_weak(current, current - 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
    }
}

status method_slot::invoke(const call_envelope& envelope, span<const std::byte> body,
                           buffer& response, call_context& ctx) const {
    if (handler_ == nullptr) {
        return status{rpc_code::internal};
    }
    return handler_(self_, envelope, body, response, ctx);
}

// --- service_slot ------------------------------------------------------------

service_slot::service_slot(u64 id, std::string name) : id_(id), name_(std::move(name)) {}

const method_slot* service_slot::find_method(u64 method_id) const noexcept {
    const auto it = methods_.find(method_id);
    return it == methods_.end() ? nullptr : it->second.get();
}

// --- service_registry --------------------------------------------------------

service_registry::service_registry() = default;
service_registry::~service_registry() = default;

status service_registry::register_service(u64 service_id, std::string name) {
    if (frozen()) {
        return status{rpc_code::failed_precondition};
    }
    if (service_id == 0) {
        return status{rpc_code::invalid_argument};
    }
    if (services_.find(service_id) != services_.end()) {
        return status{rpc_code::failed_precondition};
    }
    auto service = std::make_unique<service_slot>(service_id, std::move(name));
    services_.emplace(service_id, std::move(service));
    return status{};
}

status service_registry::register_method(u64 service_id, u64 method_id, std::string name,
                                         method_handler handler, void* self,
                                         method_options options) {
    if (frozen()) {
        return status{rpc_code::failed_precondition};
    }
    if (method_id == 0 || handler == nullptr) {
        return status{rpc_code::invalid_argument};
    }
    const auto it = services_.find(service_id);
    if (it == services_.end()) {
        return status{rpc_code::not_found};
    }
    if (it->second->methods_.find(method_id) != it->second->methods_.end()) {
        return status{rpc_code::failed_precondition};
    }
    it->second->methods_.emplace(
        method_id, std::make_unique<method_slot>(service_id, method_id, std::move(name), handler,
                                                 self, options));
    return status{};
}

status service_registry::freeze() noexcept {
    frozen_.store(true, std::memory_order_release);
    return status{};
}

const service_slot* service_registry::find_service(u64 service_id) const noexcept {
    const auto it = services_.find(service_id);
    return it == services_.end() ? nullptr : it->second.get();
}

const method_slot* service_registry::find_method(u64 service_id, u64 method_id) const noexcept {
    const service_slot* service = find_service(service_id);
    return service == nullptr ? nullptr : service->find_method(method_id);
}

std::size_t service_registry::method_count() const noexcept {
    std::size_t total = 0;
    for (const auto& entry : services_) {
        total += entry.second->method_count();
    }
    return total;
}

// --- dispatcher --------------------------------------------------------------

status dispatcher::dispatch(const frame& request, buffer& response_out, bool& has_response,
                            std::chrono::steady_clock::time_point now) noexcept {
    bool handler_ran = false;
    return dispatch(request, response_out, has_response, now, handler_ran);
}

status dispatcher::dispatch(const frame& request, buffer& response_out, bool& has_response,
                            std::chrono::steady_clock::time_point now,
                            bool& handler_ran) noexcept {
    return dispatch(request, response_out, has_response, now, handler_ran, nullptr);
}

status dispatcher::dispatch(const frame& request, buffer& response_out, bool& has_response,
                            std::chrono::steady_clock::time_point now, bool& handler_ran,
                            const void* dedup_wait_key) noexcept {
    return dispatch(request, response_out, has_response, now, handler_ran,
                    dedup_wait_key, nullptr);
}

status dispatcher::dispatch(const frame& request, buffer& response_out, bool& has_response,
                            std::chrono::steady_clock::time_point now, bool& handler_ran,
                            const void* dedup_wait_key,
                            const metadata_list* context_metadata) noexcept {
    // Building the decode-failure message, the refusal errors and the success
    // response all allocate, and this function is `noexcept` (the server's
    // shard calls it). An allocation failure would reach `std::terminate`.
    // Report it as an internal error instead. The concurrency counters taken
    // below are released on every exit, including the catch path.
    has_response = false;
    response_out.clear();
    handler_ran = false;

    const service_slot* service = nullptr;
    const method_slot* method = nullptr;
    bool entered = false;
    try {
        const bool one_way = request.header.type == frame_type::one_way;
        if (request.header.type != frame_type::request && !one_way) {
            return status{rpc_code::protocol_error};
        }
        const bool wants_response = !one_way;

        call_envelope envelope;
        const status decoded = decode_call_envelope(request.payload, envelope);
        if (!decoded.ok()) {
            const std::string message =
                decoded.message().empty() ? "request decode failed" : decoded.message();
            return respond_error(response_out, has_response, wants_response,
                                 rpc_error(rpc_code::decode_error, message));
        }

        service = registry_->find_service(envelope.service_id);
        if (service == nullptr) {
            return respond_error(response_out, has_response, wants_response,
                                 rpc_error(rpc_code::not_found, "unknown service"));
        }
        method = service->find_method(envelope.method_id);
        if (method == nullptr) {
            return respond_error(response_out, has_response, wants_response,
                                 rpc_error(rpc_code::no_such_method, "unknown method"));
        }

        if (envelope.extensions.has(trace_extension::has_deadline)) {
            // Relative deadline (design section 8.5): refuse to start work that
            // cannot finish. The floor is shared with the server's admission
            // refusals so the two cannot drift (resilience::min_remaining_ns).
            if (envelope.extensions.deadline_ns < resilience::min_remaining_ns) {
                return respond_error(response_out, has_response, wants_response,
                                     rpc_error(rpc_code::deadline_exceeded,
                                               "deadline already passed"));
            }
        }

        call_context ctx = build_context(envelope, now);
        // Context injection from the observability interceptor chain: a
        // `before_call` hook's metadata entries become visible to the
        // application handler. Null (the empty-chain case) costs nothing.
        if (context_metadata != nullptr) {
            for (const auto& entry : *context_metadata) {
                ctx.metadata.set(entry.first, entry.second);
            }
        }

        // --- Idempotency dedup ------------------------------------------------
        //
        // A keyed request claims the table before the handler runs. Everything
        // above this point is a per-request refusal and deduplicates nothing:
        // decode failures, unknown service/method and already-expired deadlines
        // never reach the table.
        dedup_lease lease;
        bool leading = false;
        if (ctx.has_idempotency_key && !ctx.idempotency.empty()) {
            // The suspension bound is the caller's own wire deadline (a client
            // that has given up must not hold a shard thread) capped by the
            // configured `max_wait`; with `max_wait <= 0` duplicates are never
            // suspended and only the wire deadline can bind.
            const std::chrono::steady_clock::time_point wire_deadline =
                wire_deadline_of(envelope, now);
            std::chrono::steady_clock::time_point wait_deadline = wire_deadline;
            const std::chrono::milliseconds max_wait = dedup_.options().max_wait;
            if (max_wait.count() > 0) {
                const std::chrono::steady_clock::time_point capped = now + max_wait;
                if (!envelope.extensions.has(trace_extension::has_deadline) ||
                    capped < wait_deadline) {
                    wait_deadline = capped;
                }
            }
            lease = dedup_.claim(ctx.idempotency, envelope.service_id, envelope.method_id,
                                 now, wait_deadline, dedup_wait_key, wants_response);
            switch (lease.state()) {
            case dedup_lease::action::leader:
                leading = true;
                break;
            case dedup_lease::action::none:
                // Table disabled or saturated: at-least-once, exactly as before.
                break;
            case dedup_lease::action::duplicate:
                // One-way duplicate: the side effect already ran (or is
                // running); there is no response to wait for.
                return status{};
            case dedup_lease::action::replay:
            case dedup_lease::action::served: {
                const std::shared_ptr<const std::vector<std::byte>> payload =
                    lease.replay_payload();
                if (payload != nullptr) {
                    // A one-way replay has no response to deliver: the cached
                    // marker only says "already executed, drop".
                    if (wants_response) {
                        if (!response_out.append(
                                crpc::span<const std::byte>(payload->data(),
                                                            payload->size()))) {
                            return status{rpc_code::encode_error};
                        }
                        has_response = true;
                    }
                    return status{};
                }
                // A payload-less outcome can only be a failed or aborted
                // leader: answer with its code. A success that does not exist
                // is never replayed.
                return respond_error(response_out, has_response, wants_response,
                                     rpc_error(lease.replay_code(),
                                               "idempotent replay: the original attempt failed"));
            }
            case dedup_lease::action::abandoned:
                return respond_error(response_out, has_response, wants_response,
                                     rpc_error(rpc_code::canceled,
                                               "duplicate abandoned: connection closed while "
                                               "the original call was in flight"));
            case dedup_lease::action::expired: {
                // The wire deadline is the more precise reason when it is the
                // one that bound the wait; otherwise the server-side cap fired
                // and a retry after the original completes will replay.
                const bool wire_bound =
                    envelope.extensions.has(trace_extension::has_deadline) &&
                    wire_deadline <= wait_deadline;
                if (wire_bound) {
                    return respond_error(
                        response_out, has_response, wants_response,
                        rpc_error(rpc_code::deadline_exceeded,
                                  "deadline elapsed while the original call was in flight"));
                }
                rpc_error refused(rpc_code::unavailable,
                                  "duplicate wait exceeded the server's dedup wait bound");
                refused.retryable = true;
                return respond_error(response_out, has_response, wants_response, refused);
            }
            }
        }
        leader_completion completion(lease, response_out, has_response, leading);

        if (!method->enter()) {
            return respond_error(response_out, has_response, wants_response,
                                 rpc_error(rpc_code::overload,
                                           "method concurrency limit reached"));
        }
        service->enter();
        entered = true;

        buffer body_out;
        status handler_status;
        handler_ran = true;
        try {
            handler_status = method->invoke(envelope, envelope.body, body_out, ctx);
        } catch (const std::exception& ex) {
            handler_status =
                status{rpc_error(rpc_code::internal, std::string("handler threw: ") + ex.what())};
        } catch (...) {
            handler_status = status{rpc_error(rpc_code::internal, "handler threw an exception")};
        }

        service->leave();
        method->leave();
        entered = false;

        if (!handler_status.ok()) {
            completion.code = handler_status.error().code;
            return respond_error(response_out, has_response, wants_response,
                                 handler_status.error());
        }
        if (!wants_response) {
            // A one-way success is cached as an empty retained marker so a
            // replayed one-way within the window is dropped as a duplicate.
            completion.ok = true;
            return status{};
        }
        has_response = true;
        const status encoded =
            encode_response(rpc_code::ok, rpc_error{}, body_out.readable(), response_out);
        if (!encoded.ok()) {
            completion.code = rpc_code::encode_error;
            return encoded;
        }
        completion.ok = true;
        return status{};
    } catch (...) {
        // Allocation failure. Release the concurrency counters if they were
        // taken, then report an internal error. The catch body performs no
        // allocation. The `leader_completion` guard runs during unwinding and
        // completes the dedup entry without a payload, so no duplicate stays
        // suspended against a leader that died here.
        if (entered) {
            service->leave();
            method->leave();
        }
        return status{rpc_code::internal};
    }
}

} // namespace crpc::rpc

