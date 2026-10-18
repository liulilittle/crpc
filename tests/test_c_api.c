/* CRPC -- C ABI smoke test.
 *
 * This translation unit is compiled as C, not C++, and linked against the
 * library. It proves three things the audit asked for:
 *   1. the header is valid C and the export macro does not force dllimport on a
 *      static consumer;
 *   2. every declared function links;
 *   3. a real request/response round trip works through the opaque handles.
 *
 * A TCP loopback port is tried until one binds. The handler echoes the request
 * body; the client checks the echo and the call bookkeeping.
 */
#include <crpc/c_api.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static crpc_status echo_handler(void* user, uint64_t service_id, uint64_t method_id,
                                crpc_bytes request, crpc_mutable_bytes* response) {
    (void)user;
    (void)service_id;
    (void)method_id;
    if (response->size < request.size) {
        response->size = request.size;
        return CRPC_STATUS_RESOURCE_EXHAUSTED;
    }
    if (request.size != 0) {
        memcpy(response->data, request.data, request.size);
    }
    response->size = request.size;
    return CRPC_STATUS_OK;
}

int main(void) {
    if (crpc_version_string() == NULL) {
        fprintf(stderr, "version string is null\n");
        return 1;
    }
    if (crpc_last_error_message() == NULL) {
        fprintf(stderr, "last error string is null\n");
        return 1;
    }

    crpc_server* server = NULL;
    uint16_t chosen_port = 0;
    for (uint16_t port = 45500; port < 45540; ++port) {
        crpc_server* candidate = crpc_server_create();
        if (candidate == NULL) {
            continue;
        }
        if (crpc_server_listen_tcp(candidate, "127.0.0.1", 9, port) != CRPC_STATUS_OK) {
            crpc_server_destroy(candidate);
            continue;
        }
        if (crpc_server_register(candidate, 100, 1, &echo_handler, NULL) !=
            CRPC_STATUS_OK) {
            crpc_server_destroy(candidate);
            continue;
        }
        if (crpc_server_start(candidate) != CRPC_STATUS_OK) {
            crpc_server_destroy(candidate);
            continue;
        }
        server = candidate;
        chosen_port = port;
        break;
    }
    if (server == NULL) {
        fprintf(stderr, "could not start a server: %s\n", crpc_last_error_message());
        return 2;
    }

    char endpoint[64];
    const int written = snprintf(endpoint, sizeof(endpoint), "127.0.0.1:%u",
                                 (unsigned)chosen_port);
    if (written <= 0) {
        crpc_server_destroy(server);
        return 3;
    }

    crpc_client* client = crpc_client_create(endpoint, (size_t)written);
    if (client == NULL) {
        fprintf(stderr, "could not create client: %s\n", crpc_last_error_message());
        crpc_server_destroy(server);
        return 4;
    }

    if (crpc_client_set_deadline_ms(client, 2000) != CRPC_STATUS_OK ||
        crpc_client_set_max_in_flight(client, 4) != CRPC_STATUS_OK) {
        fprintf(stderr, "could not configure client\n");
        crpc_client_destroy(client);
        crpc_server_destroy(server);
        return 5;
    }

    const uint8_t payload[] = {1, 2, 3, 4, 5};
    uint8_t buffer[32];
    memset(buffer, 0, sizeof(buffer));
    crpc_mutable_bytes response;
    response.data = buffer;
    response.size = sizeof(buffer);

    uint64_t call_id = 0;
    crpc_status status = crpc_client_call(
        client, 100, 1, (crpc_bytes){payload, sizeof(payload)}, &response, 0, &call_id);
    if (status != CRPC_STATUS_OK) {
        fprintf(stderr, "call failed: %s\n", crpc_last_error_message());
        crpc_client_destroy(client);
        crpc_server_destroy(server);
        return 6;
    }

    status = crpc_client_wait(client, call_id, &response);
    if (status != CRPC_STATUS_OK) {
        fprintf(stderr, "wait failed: %s\n", crpc_last_error_message());
        crpc_client_destroy(client);
        crpc_server_destroy(server);
        return 7;
    }
    if (response.size != sizeof(payload) ||
        memcmp(buffer, payload, sizeof(payload)) != 0) {
        fprintf(stderr, "echo mismatch: size=%u\n", (unsigned)response.size);
        crpc_client_destroy(client);
        crpc_server_destroy(server);
        return 8;
    }

    /* Cancelling a completed call is OK, and release is idempotent. */
    if (crpc_client_cancel(client, call_id) != CRPC_STATUS_OK) {
        fprintf(stderr, "cancel failed\n");
        crpc_client_destroy(client);
        crpc_server_destroy(server);
        return 9;
    }
    crpc_client_release_call(client, call_id);
    crpc_client_release_call(client, call_id);

    /* A second call reuses the same opaque client. */
    response.size = sizeof(buffer);
    call_id = 0;
    status = crpc_client_call(
        client, 100, 1, (crpc_bytes){payload, sizeof(payload)}, &response, 1000, &call_id);
    if (status != CRPC_STATUS_OK) {
        fprintf(stderr, "second call failed: %s\n", crpc_last_error_message());
        crpc_client_destroy(client);
        crpc_server_destroy(server);
        return 10;
    }
    status = crpc_client_wait(client, call_id, &response);
    if (status != CRPC_STATUS_OK || response.size != sizeof(payload) ||
        memcmp(buffer, payload, sizeof(payload)) != 0) {
        fprintf(stderr, "second wait failed\n");
        crpc_client_destroy(client);
        crpc_server_destroy(server);
        return 11;
    }
    crpc_client_release_call(client, call_id);

    crpc_client_destroy(client);
    if (crpc_server_stop(server, 1000) != CRPC_STATUS_OK) {
        fprintf(stderr, "stop failed\n");
        crpc_server_destroy(server);
        return 12;
    }
    crpc_server_destroy(server);

    /* NULL handles are documented no-ops. */
    crpc_client_destroy(NULL);
    crpc_server_destroy(NULL);

    printf("C ABI smoke test passed on port %u\n", (unsigned)chosen_port);
    return 0;
}
