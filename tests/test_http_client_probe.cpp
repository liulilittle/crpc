// A focused probe kept as its own binary: does the HTTP client complete a PUT with a body
// against a real gateway? Kept separate from the HBase cases so it runs even when one of them
// hangs, which is what it exists to distinguish.
//
// Skipped unless CRPC_HBASE_IT=1.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <crpc/error.hpp>
#include <crpc/http/client.hpp>
#include <crpc/io/iocp_backend.hpp>

namespace {

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

[[nodiscard]] const char* require_gateway() {
    const char* value = std::getenv("CRPC_HBASE_IT");
    if (value == nullptr || std::strcmp(value, "1") != 0) {
        crpc_test::skip("CRPC_HBASE_IT is not 1");
    }
    return "";
}

} // namespace

CRPC_TEST(http_put_with_a_body_completes) {
    (void)require_gateway();
    crpc::iocp_backend backend;
    crpc::http::client_options options;
    options.host = env_or("CRPC_HBASE_HOST", "127.0.0.1");
    options.port = static_cast<std::uint16_t>(std::stoi(env_or("CRPC_HBASE_PORT", "18080")));
    options.timeout_seconds = 10;

    auto link = crpc::http::make_client(options, backend,
                                        crpc::endpoint::tcp(options.host, options.port));
    const std::string body =
        "<TableSchema name=\"crpc_probe2\"><ColumnSchema name=\"cf\"/></TableSchema>";
    const auto reply = link->send_text(crpc::http::method::put, "/crpc_probe2/schema", body,
                                       "text/xml");
    if (!reply) {
        std::fprintf(stderr, "  http put failed: %s (%d)\n", reply.error().message.c_str(),
                     static_cast<int>(reply.error().code));
    }
    CRPC_CHECK(static_cast<bool>(reply));
    if (reply) {
        std::fprintf(stderr, "  put status=%d body=[%s]\n", reply.value().status_code,
                     reply.value().body.c_str());
        CRPC_CHECK(reply.value().status_code == 201 || reply.value().status_code == 200);
    }
}

CRPC_TEST(http_get_completes) {
    (void)require_gateway();
    crpc::iocp_backend backend;
    crpc::http::client_options options;
    options.host = env_or("CRPC_HBASE_HOST", "127.0.0.1");
    options.port = static_cast<std::uint16_t>(std::stoi(env_or("CRPC_HBASE_PORT", "18080")));
    options.timeout_seconds = 10;
    auto link = crpc::http::make_client(options, backend,
                                        crpc::endpoint::tcp(options.host, options.port));
    const auto reply = link->send(crpc::http::method::get, "/version", {});
    CRPC_CHECK(static_cast<bool>(reply));
    if (reply) {
        std::fprintf(stderr, "  version status=%d body=[%s]\n", reply.value().status_code,
                     reply.value().body.substr(0, 40).c_str());
        CRPC_CHECK_EQ(200, reply.value().status_code);
    }
}

CRPC_TEST_MAIN()