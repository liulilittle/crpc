// CRPC -- C ABI implementation.
//
// Thin wrappers over the C++ facade. The handles are opaque to callers; this
// translation unit owns the C++ objects behind them.
//
// Backend selection
// -----------------
// "host:port" endpoints use the platform TCP backend (IOCP on Windows, epoll on
// Linux). A bare "name" uses a process-wide in-process backend shared by every
// C handle, so two C-created endpoints with the same name can find each other.
//
// Ownership and lifetime
// ----------------------
//   * A handle owns its backend unless it uses the shared in-process backend.
//   * `crpc_client_release_call` is idempotent; releasing an already released
//     (or unknown) call is a no-op. Destroying the client releases every call
//     the caller did not release.
//   * `crpc_last_error_message` is thread-local and valid until the next CRPC
//     call on the same thread.
// No exception crosses the ABI: every entry point catches and maps failure to
// a status.
#include <crpc/c_api.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/server.hpp>
#include <crpc/span.hpp>

#if defined(CRPC_OS_WINDOWS)
#include <crpc/io/iocp_backend.hpp>
#elif defined(CRPC_OS_LINUX)
#include <crpc/io/epoll_backend.hpp>
#endif

namespace {

thread_local std::string g_last_error;

void set_error(std::string message) { g_last_error = std::move(message); }

// Every status-returning entry point clears the thread-local error first, so a
// successful call always leaves `crpc_last_error_message()` empty (R4a). Fail
// paths re-populate it through `fail()`/`set_error()`.
void clear_error() { g_last_error.clear(); }

crpc_status fail(crpc_status status, std::string message) {
    set_error(std::move(message));
    return status;
}

crpc_status from_rpc(crpc::rpc_code code) {
    switch (code) {
    case crpc::rpc_code::ok:
        return CRPC_STATUS_OK;
    case crpc::rpc_code::canceled:
        return CRPC_STATUS_CANCELED;
    case crpc::rpc_code::deadline_exceeded:
        return CRPC_STATUS_DEADLINE_EXCEEDED;
    case crpc::rpc_code::unavailable:
    case crpc::rpc_code::connection_refused:
    case crpc::rpc_code::connection_reset:
    case crpc::rpc_code::broken_pipe:
    case crpc::rpc_code::eof:
        return CRPC_STATUS_UNAVAILABLE;
    case crpc::rpc_code::not_found:
    case crpc::rpc_code::no_such_method:
        return CRPC_STATUS_NOT_FOUND;
    case crpc::rpc_code::invalid_argument:
        return CRPC_STATUS_INVALID_ARGUMENT;
    case crpc::rpc_code::failed_precondition:
        return CRPC_STATUS_INVALID_ARGUMENT;
    case crpc::rpc_code::resource_exhausted:
    case crpc::rpc_code::overload:
        return CRPC_STATUS_RESOURCE_EXHAUSTED;
    case crpc::rpc_code::protocol_error:
    case crpc::rpc_code::decode_error:
    case crpc::rpc_code::encode_error:
        return CRPC_STATUS_PROTOCOL_ERROR;
    case crpc::rpc_code::shutting_down:
        return CRPC_STATUS_SHUTTING_DOWN;
    default:
        return CRPC_STATUS_INTERNAL;
    }
}

bool parse_endpoint(const char* text, size_t length, bool& inproc, std::string& host,
                    std::uint16_t& port) {
    inproc = true;
    host.clear();
    port = 0;
    if (text == nullptr && length != 0) {
        return false;
    }
    const std::string value(text == nullptr ? "" : text, length);
    if (value.empty()) {
        return false;
    }
    const std::size_t colon = value.rfind(':');
    if (colon == std::string::npos) {
        host = value;
        return true;
    }
    const std::string host_part = value.substr(0, colon);
    const std::string port_part = value.substr(colon + 1);
    if (host_part.empty() || port_part.empty()) {
        host = value;
        return true;
    }
    unsigned long parsed = 0;
    for (const char ch : port_part) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        parsed = parsed * 10ul + static_cast<unsigned long>(ch - '0');
        if (parsed > 65535ul) {
            return false;
        }
    }
    if (parsed == 0ul) {
        return false;
    }
    host = host_part;
    port = static_cast<std::uint16_t>(parsed);
    inproc = false;
    return true;
}

std::unique_ptr<crpc::backend_factory> make_tcp_backend() {
#if defined(CRPC_OS_WINDOWS)
    return std::unique_ptr<crpc::backend_factory>(new (std::nothrow) crpc::iocp_backend());
#elif defined(CRPC_OS_LINUX)
    crpc::result<std::unique_ptr<crpc::io::epoll_backend>> created =
        crpc::io::epoll_backend::create();
    if (!created.ok()) {
        return nullptr;
    }
    return std::unique_ptr<crpc::backend_factory>(std::move(created).value());
#else
    return nullptr;
#endif
}

crpc::inproc_backend& shared_inproc() {
    static crpc::inproc_backend backend;
    return backend;
}

struct c_method {
    crpc_handler_fn fn = nullptr;
    void* user = nullptr;
};

crpc::status method_trampoline(void* self, const crpc::call_envelope& envelope,
                               crpc::span<const std::byte> body, crpc::buffer& response,
                               crpc::rpc::call_context&) {
    auto* method = static_cast<c_method*>(self);
    if (method == nullptr || method->fn == nullptr) {
        return crpc::status{crpc::rpc_code::internal};
    }
    std::vector<std::uint8_t> scratch(64 * 1024);
    for (;;) {
        crpc_mutable_bytes out;
        out.data = scratch.data();
        out.size = scratch.size();
        crpc_bytes in;
        in.data = reinterpret_cast<const std::uint8_t*>(body.data());
        in.size = body.size();
        const crpc_status status =
            method->fn(method->user, envelope.service_id, envelope.method_id, in, &out);
        if (status == CRPC_STATUS_RESOURCE_EXHAUSTED && out.size > scratch.size()) {
            scratch.resize(out.size);
            continue;
        }
        if (status != CRPC_STATUS_OK) {
            return crpc::status{
                crpc::rpc_error(crpc::rpc_code::internal, "C handler returned an error")};
        }
        if (out.size > scratch.size()) {
            return crpc::status{crpc::rpc_code::protocol_error};
        }
        if (out.size != 0 && !response.append(scratch.data(), out.size)) {
            return crpc::status{crpc::rpc_code::resource_exhausted};
        }
        return crpc::status{};
    }
}

struct client_call {
    crpc::client::call<std::vector<std::byte>> handle;
    bool waited = false;
    crpc_status status = CRPC_STATUS_OK;
    std::string message;
    std::vector<std::uint8_t> body;
};

} // namespace

struct crpc_client {
    std::unique_ptr<crpc::backend_factory> owned_backend;
    std::unique_ptr<crpc::client> facade;
    std::uint32_t default_deadline_ms = 0;
    std::uint32_t max_in_flight = 0;
    std::uint64_t next_call_id = 1;
    std::unordered_map<std::uint64_t, client_call> calls;
};

struct crpc_server {
    std::unique_ptr<crpc::server::builder> builder;
    std::unique_ptr<crpc::backend_factory> owned_backend;
    std::unique_ptr<crpc::server> facade;
    std::vector<std::unique_ptr<c_method>> methods;
    std::set<std::pair<std::uint64_t, std::uint64_t>> registered;
    std::set<std::uint64_t> services;
    bool listening = false;
    // `facade` is built before `start()` is attempted, so its presence alone
    // must not mean "running". Track the real outcome and replay a stored
    // failure on a retry (R4b).
    bool started = false;
    bool start_attempted = false;
    crpc_status start_status = CRPC_STATUS_OK;
    std::string start_message;
};

extern "C" {

const char* crpc_version_string(void) { return "2.0.0"; }

const char* crpc_last_error_message(void) { return g_last_error.c_str(); }

crpc_client* crpc_client_create(const char* endpoint, size_t endpoint_len) {
    clear_error();
    try {
        bool inproc = false;
        std::string host;
        std::uint16_t port = 0;
        if (!parse_endpoint(endpoint, endpoint_len, inproc, host, port)) {
            set_error("invalid endpoint");
            return nullptr;
        }

        std::unique_ptr<crpc_client> handle(new (std::nothrow) crpc_client());
        if (!handle) {
            set_error("out of memory");
            return nullptr;
        }

        crpc::endpoint ep;
        if (inproc) {
            ep = crpc::endpoint::inproc(host);
            handle->owned_backend = nullptr;
        } else {
            ep = crpc::endpoint::tcp(host, port);
            handle->owned_backend = make_tcp_backend();
            if (!handle->owned_backend) {
                set_error("could not create the TCP backend");
                return nullptr;
            }
        }
        crpc::backend_factory* backend =
            inproc ? static_cast<crpc::backend_factory*>(&shared_inproc())
                   : handle->owned_backend.get();

        crpc::client::builder builder;
        builder.add_endpoint(ep, *backend);
        builder.pool_size(1);
        // "0 means no deadline" for the C ABI; the facade's own 1 s default is
        // therefore disabled and applied per call instead.
        builder.default_deadline(std::chrono::nanoseconds{0});
        handle->facade = builder.build();
        if (!handle->facade) {
            set_error("could not build the client");
            return nullptr;
        }
        return handle.release();
    } catch (...) {
        set_error("internal error");
        return nullptr;
    }
}

crpc_status crpc_client_set_deadline_ms(crpc_client* client, uint32_t deadline_ms) {
    clear_error();
    if (client == nullptr || !client->facade) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "invalid client");
    }
    client->default_deadline_ms = deadline_ms;
    return CRPC_STATUS_OK;
}

crpc_status crpc_client_set_max_in_flight(crpc_client* client, uint32_t max_in_flight) {
    clear_error();
    if (client == nullptr || !client->facade) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "invalid client");
    }
    client->max_in_flight = max_in_flight;
    return CRPC_STATUS_OK;
}

crpc_status crpc_client_call(crpc_client* client, uint64_t service_id, uint64_t method_id,
                             crpc_bytes request, crpc_mutable_bytes* response,
                             uint32_t deadline_ms, uint64_t* call_id) {
    (void)response; // filled by crpc_client_wait, per the header contract
    clear_error();
    if (client == nullptr || !client->facade) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "invalid client");
    }
    if (call_id == nullptr) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "call_id is null");
    }
    if (request.data == nullptr && request.size != 0) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "request data is null");
    }
    try {
        if (client->max_in_flight != 0 && client->calls.size() >= client->max_in_flight) {
            return fail(CRPC_STATUS_RESOURCE_EXHAUSTED, "maximum in-flight calls reached");
        }
        crpc::rpc::call_context context;
        const std::uint32_t effective =
            deadline_ms != 0 ? deadline_ms : client->default_deadline_ms;
        if (effective != 0) {
            context.has_deadline = true;
            context.deadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(effective);
        }
        crpc::span<const std::byte> body;
        if (request.size != 0) {
            body = crpc::span<const std::byte>(
                reinterpret_cast<const std::byte*>(request.data), request.size);
        }
        auto started = client->facade->template start_encoded<std::vector<std::byte>>(
            service_id, method_id, body, std::move(context));
        if (!started.ok()) {
            return fail(from_rpc(started.code()), started.error().message);
        }
        client_call record;
        record.handle = std::move(started).value();
        const std::uint64_t id = client->next_call_id;
        client->next_call_id += 1;
        client->calls.emplace(id, std::move(record));
        *call_id = id;
        return CRPC_STATUS_OK;
    } catch (...) {
        return fail(CRPC_STATUS_INTERNAL, "internal error");
    }
}

crpc_status crpc_client_wait(crpc_client* client, uint64_t call_id,
                             crpc_mutable_bytes* response) {
    clear_error();
    if (client == nullptr || !client->facade) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "invalid client");
    }
    const auto it = client->calls.find(call_id);
    if (it == client->calls.end()) {
        return fail(CRPC_STATUS_NOT_FOUND, "unknown call");
    }
    try {
        client_call& record = it->second;
        if (!record.waited) {
            auto result = record.handle.get();
            record.waited = true;
            if (!result.ok()) {
                record.status = from_rpc(result.code());
                record.message = result.error().message;
            } else {
                record.status = CRPC_STATUS_OK;
                const std::vector<std::byte>& bytes = result.value();
                record.body.clear();
                if (!bytes.empty()) {
                    record.body.assign(
                        reinterpret_cast<const std::uint8_t*>(bytes.data()),
                        reinterpret_cast<const std::uint8_t*>(bytes.data()) + bytes.size());
                }
            }
        }
        if (record.status != CRPC_STATUS_OK) {
            return fail(record.status, record.message);
        }
        if (response != nullptr) {
            const std::size_t size = record.body.size();
            if (response->size < size) {
                response->size = size;
                return fail(CRPC_STATUS_RESOURCE_EXHAUSTED, "response buffer too small");
            }
            if (size != 0) {
                if (response->data == nullptr) {
                    return fail(CRPC_STATUS_INVALID_ARGUMENT, "response data is null");
                }
                std::memcpy(response->data, record.body.data(), size);
            }
            response->size = size;
        }
        return CRPC_STATUS_OK;
    } catch (...) {
        return fail(CRPC_STATUS_INTERNAL, "internal error");
    }
}

crpc_status crpc_client_cancel(crpc_client* client, uint64_t call_id) {
    clear_error();
    if (client == nullptr || !client->facade) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "invalid client");
    }
    const auto it = client->calls.find(call_id);
    if (it == client->calls.end()) {
        return fail(CRPC_STATUS_NOT_FOUND, "unknown call");
    }
    it->second.handle.cancel();
    return CRPC_STATUS_OK;
}

void crpc_client_release_call(crpc_client* client, uint64_t call_id) {
    if (client == nullptr) {
        return;
    }
    // Idempotent: erasing destroys the handle, whose destructor abandons any
    // still-active attempt (the same path `abandon_attempt` implements).
    (void)client->calls.erase(call_id);
}

void crpc_client_destroy(crpc_client* client) {
    if (client == nullptr) {
        return;
    }
    try {
        client->calls.clear();
        client->facade.reset();
        client->owned_backend.reset();
    } catch (...) {
    }
    delete client;
}

crpc_server* crpc_server_create(void) {
    clear_error();
    try {
        std::unique_ptr<crpc_server> handle(new (std::nothrow) crpc_server());
        if (!handle) {
            set_error("out of memory");
            return nullptr;
        }
        handle->builder = std::make_unique<crpc::server::builder>();
        return handle.release();
    } catch (...) {
        set_error("internal error");
        return nullptr;
    }
}

crpc_status crpc_server_listen_tcp(crpc_server* server, const char* host, size_t host_len,
                                   uint16_t port) {
    clear_error();
    if (server == nullptr || !server->builder) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "invalid server");
    }
    if (server->listening) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "server is already listening");
    }
    if (host == nullptr || host_len == 0) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "host is empty");
    }
    try {
        if (!server->owned_backend) {
            server->owned_backend = make_tcp_backend();
            if (!server->owned_backend) {
                return fail(CRPC_STATUS_INTERNAL, "could not create the TCP backend");
            }
        }
        const std::string host_text(host, host_len);
        server->builder->add_listener(crpc::endpoint::tcp(host_text, port),
                                      *server->owned_backend);
        server->listening = true;
        return CRPC_STATUS_OK;
    } catch (...) {
        return fail(CRPC_STATUS_INTERNAL, "internal error");
    }
}

crpc_status crpc_server_register(crpc_server* server, uint64_t service_id,
                                 uint64_t method_id, crpc_handler_fn fn, void* user) {
    clear_error();
    if (server == nullptr || !server->builder) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "invalid server");
    }
    if (server->facade) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "server is already started");
    }
    if (fn == nullptr) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "handler is null");
    }
    const std::pair<std::uint64_t, std::uint64_t> key(service_id, method_id);
    if (server->registered.count(key) != 0) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "method already registered");
    }
    try {
        auto method = std::make_unique<c_method>();
        method->fn = fn;
        method->user = user;
        if (server->services.count(service_id) == 0) {
            const std::string name = "service-" + std::to_string(service_id);
            server->builder->add_service(service_id, name);
            server->services.insert(service_id);
        }
        server->builder->add_raw_method(service_id, method_id, "method",
                                        &method_trampoline, method.get());
        server->registered.insert(key);
        server->methods.push_back(std::move(method));
        return CRPC_STATUS_OK;
    } catch (...) {
        return fail(CRPC_STATUS_INTERNAL, "internal error");
    }
}

crpc_status crpc_server_start(crpc_server* server) {
    clear_error();
    if (server == nullptr || !server->builder) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "invalid server");
    }
    if (server->started) {
        return CRPC_STATUS_OK; // idempotent
    }
    if (server->start_attempted && server->start_status != CRPC_STATUS_OK) {
        // A previous start failed after the facade was built. Report the same
        // failure instead of the facade's presence masking it as OK (R4b).
        return fail(server->start_status, server->start_message);
    }
    if (!server->listening) {
        return fail(CRPC_STATUS_INVALID_ARGUMENT, "no listener configured");
    }
    try {
        // The C API supplies the drain budget to stop(), so the build-time
        // drain must not add a second timeout.
        server->builder->drain_timeout(std::chrono::milliseconds(0));
        server->facade = server->builder->build();
        if (!server->facade) {
            server->start_attempted = true;
            server->start_status = CRPC_STATUS_INTERNAL;
            server->start_message = "could not build the server";
            return fail(server->start_status, server->start_message);
        }
        const crpc::status started = server->facade->start();
        if (!started.ok()) {
            server->start_attempted = true;
            server->start_status = from_rpc(started.code());
            server->start_message = started.message();
            return fail(server->start_status, server->start_message);
        }
        server->started = true;
        return CRPC_STATUS_OK;
    } catch (...) {
        server->start_attempted = true;
        server->start_status = CRPC_STATUS_INTERNAL;
        server->start_message = "internal error";
        return fail(server->start_status, server->start_message);
    }
}

crpc_status crpc_server_stop(crpc_server* server, uint32_t drain_ms) {
    clear_error();
    if (server == nullptr) {
        return CRPC_STATUS_OK;
    }
    try {
        if (server->facade) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(drain_ms);
            while (server->facade->stats().in_flight_calls != 0 &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            server->facade->stop();
        }
        return CRPC_STATUS_OK;
    } catch (...) {
        return fail(CRPC_STATUS_INTERNAL, "internal error");
    }
}

void crpc_server_destroy(crpc_server* server) {
    if (server == nullptr) {
        return;
    }
    try {
        if (server->facade) {
            server->facade->stop();
            server->facade.reset();
        }
        server->methods.clear();
        server->owned_backend.reset();
        server->builder.reset();
    } catch (...) {
    }
    delete server;
}

} // extern "C"
