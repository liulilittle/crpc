/* CRPC -- C ABI.
 *
 * The C++ API is the primary interface. This header exists for two consumers
 * that cannot use C++ templates: language bindings, and plugin authors who must
 * not take a dependency on the library's C++ ABI.
 *
 * Contract:
 *   - opaque handles only; no C++ type appears in a signature;
 *   - every string is a (pointer, length) pair and is not required to be
 *     NUL-terminated;
 *   - every fallible call returns crpc_status; the error detail is retrieved
 *     with crpc_last_error_message();
 *   - callbacks carry a `void* user` context and must not throw (they are C);
 *   - handles are not thread-safe unless the specific function says so;
 *   - destroying a handle twice is undefined; destroying a NULL handle is a
 *     no-op.
 *
 * Version: CRPC_C_ABI_VERSION. A change to a signature or to an enum value is a
 * major-version change; adding a function or appending an enum value is minor.
 */
#ifndef CRPC_C_API_H
#define CRPC_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRPC_C_ABI_VERSION 1u

#if defined(_WIN32)
#define CRPC_C_API __declspec(dllimport)
#else
#define CRPC_C_API
#endif

/* Status codes. Values are stable; new ones are appended. */
typedef enum crpc_status {
    CRPC_STATUS_OK = 0,
    CRPC_STATUS_INVALID_ARGUMENT = 1,
    CRPC_STATUS_NOT_FOUND = 2,
    CRPC_STATUS_RESOURCE_EXHAUSTED = 3,
    CRPC_STATUS_DEADLINE_EXCEEDED = 4,
    CRPC_STATUS_CANCELED = 5,
    CRPC_STATUS_UNAVAILABLE = 6,
    CRPC_STATUS_PROTOCOL_ERROR = 7,
    CRPC_STATUS_INTERNAL = 8,
    CRPC_STATUS_SHUTTING_DOWN = 9,
    CRPC_STATUS_NOT_IMPLEMENTED = 10
} crpc_status;

/* A borrowed byte range. The callee does not take ownership. */
typedef struct crpc_bytes {
    const uint8_t* data;
    size_t size;
} crpc_bytes;

/* A mutable byte range supplied by the caller. */
typedef struct crpc_mutable_bytes {
    uint8_t* data;
    size_t size;
} crpc_mutable_bytes;

typedef struct crpc_client crpc_client;
typedef struct crpc_server crpc_server;

/* --- Library ----------------------------------------------------------------- */

/* Returns a static, human-readable string. Never NULL. */
CRPC_C_API const char* crpc_version_string(void);

/* Returns the last error message for the calling thread. Never NULL; an empty
 * string when there is no error. The pointer stays valid until the next CRPC
 * call on the same thread. */
CRPC_C_API const char* crpc_last_error_message(void);

/* --- Client ------------------------------------------------------------------ */

/* Creates a client bound to one endpoint ("host:port" for TCP, "name" for an
 * in-process endpoint). Returns NULL on failure. */
CRPC_C_API crpc_client* crpc_client_create(const char* endpoint, size_t endpoint_len);

/* Sets the default deadline for calls started by this client, in milliseconds.
 * 0 means no deadline. */
CRPC_C_API crpc_status crpc_client_set_deadline_ms(crpc_client* client,
                                                   uint32_t deadline_ms);

/* Sets the maximum number of concurrent calls. Exceeding it makes the next call
 * fail with CRPC_STATUS_RESOURCE_EXHAUSTED rather than queueing forever. */
CRPC_C_API crpc_status crpc_client_set_max_in_flight(crpc_client* client,
                                                     uint32_t max_in_flight);

/* Starts a call. `request` and `response` are opaque encoded payloads in the
 * codec negotiated with the server (crpc-binary by default).
 *
 * `call_id` receives a handle that must be released with crpc_client_release_call.
 * The response buffer is filled by crpc_client_wait; if it is too small, the
 * call fails with CRPC_STATUS_RESOURCE_EXHAUSTED and the required size is
 * written to `response->size`. */
CRPC_C_API crpc_status crpc_client_call(crpc_client* client, uint64_t service_id,
                                        uint64_t method_id, crpc_bytes request,
                                        crpc_mutable_bytes* response,
                                        uint32_t deadline_ms, uint64_t* call_id);

/* Waits for a call started with crpc_client_call. Returns the call's terminal
 * status. `response->size` is updated with the number of bytes written. */
CRPC_C_API crpc_status crpc_client_wait(crpc_client* client, uint64_t call_id,
                                        crpc_mutable_bytes* response);

/* Cancels a call. Idempotent: cancelling a completed call returns OK. */
CRPC_C_API crpc_status crpc_client_cancel(crpc_client* client, uint64_t call_id);

/* Releases the bookkeeping for a call. Must be called exactly once per started
 * call, whether it completed or not. */
CRPC_C_API void crpc_client_release_call(crpc_client* client, uint64_t call_id);

/* Destroys the client, failing every in-flight call. NULL is a no-op. */
CRPC_C_API void crpc_client_destroy(crpc_client* client);

/* --- Server ------------------------------------------------------------------ */

/* A handler receives the request and writes the response into `response`.
 * Returning a non-zero value is treated as CRPC_STATUS_INTERNAL. It must not
 * block for longer than the request deadline. */
typedef crpc_status (*crpc_handler_fn)(void* user, uint64_t service_id,
                                       uint64_t method_id, crpc_bytes request,
                                       crpc_mutable_bytes* response);

CRPC_C_API crpc_server* crpc_server_create(void);

/* Binds a TCP listener. Returns CRPC_STATUS_OK once the socket is listening. */
CRPC_C_API crpc_status crpc_server_listen_tcp(crpc_server* server, const char* host,
                                              size_t host_len, uint16_t port);

/* Registers a handler. The same service/method pair cannot be registered twice. */
CRPC_C_API crpc_status crpc_server_register(crpc_server* server, uint64_t service_id,
                                            uint64_t method_id, crpc_handler_fn fn,
                                            void* user);

/* Starts serving. Idempotent. */
CRPC_C_API crpc_status crpc_server_start(crpc_server* server);

/* Stops accepting, waits up to `drain_ms` for in-flight calls, then cancels the
 * rest. Idempotent. */
CRPC_C_API crpc_status crpc_server_stop(crpc_server* server, uint32_t drain_ms);

/* Destroys the server. NULL is a no-op. */
CRPC_C_API void crpc_server_destroy(crpc_server* server);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CRPC_C_API_H */
