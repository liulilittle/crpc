#define _CRT_SECURE_NO_WARNINGS 1
// CRPC -- FTP integration test against a real, pre-existing server.
//
// Skipped unless CRPC_FTP_IT=1.  Target defaults to 127.0.0.1:21 and is
// overridden by CRPC_FTP_HOST / CRPC_FTP_PORT / CRPC_FTP_USER /
// CRPC_FTP_PASSWORD / CRPC_FTP_ROOT.  The case never writes outside the
// server's own root and only creates a uniquely named file it also removes.
#include <crpc/ftp/client.hpp>

#include "test_framework.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

[[nodiscard]] std::string env_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::string(value) : fallback;
}

[[nodiscard]] bool enabled() {
    const char* flag = std::getenv("CRPC_FTP_IT");
    return flag != nullptr && std::string(flag) == "1";
}

} // namespace

CRPC_TEST(ftp_it_live_server_round_trip) {
    if (!enabled()) {
        crpc_test::skip("set CRPC_FTP_IT=1 to run against a live FTP server");
    }

    crpc::ftp::client_options options;
    options.host = env_or("CRPC_FTP_HOST", "127.0.0.1");
    const std::string port = env_or("CRPC_FTP_PORT", "21");
    options.port = static_cast<std::uint16_t>(std::stoi(port));
    options.user = env_or("CRPC_FTP_USER", "anonymous");
    options.password = env_or("CRPC_FTP_PASSWORD", "anonymous@");
    options.timeout = std::chrono::seconds(30);
    // A live server behind NAT may need its advertised PASV address honoured.
    options.use_pasv_address =
        env_or("CRPC_FTP_USE_PASV_ADDRESS", "0") == "1";

    auto connected = crpc::ftp::client::connect(options);
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<crpc::ftp::client> session = std::move(connected).value();

    auto pwd = session->pwd();
    CRPC_REQUIRE(pwd.ok());

    auto type = session->type_binary();
    CRPC_REQUIRE(type.ok());

    auto listing = session->list();
    CRPC_CHECK(listing.ok());

    const std::string name =
        "crpc-ftp-it-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        ".bin";
    const std::string payload = "crpc ftp integration payload\n";
    auto stored = session->stor(name, payload);
    CRPC_CHECK(stored.ok());
    if (stored.ok()) {
        auto fetched = session->retr(name);
        CRPC_CHECK(fetched.ok());
        if (fetched.ok()) {
            CRPC_CHECK_EQ(payload, fetched.value());
        }
        auto removed = session->remove(name);
        CRPC_CHECK(removed.ok());
    }

    auto quit = session->quit();
    CRPC_CHECK(quit.ok());
}

CRPC_TEST_MAIN()
