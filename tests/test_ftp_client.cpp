// CRPC -- FTP client tests against a real loopback server.
//
// Every case starts a `ftp::server` on an ephemeral port (bind :0), connects a
// real `ftp::client` over TCP, and drives the complete command set.  Nothing is
// mocked: the bytes go through the same `net::listener` / `net::connect` /
// `net::tcp_stream` stack a production deployment uses.
#include <crpc/ftp/client.hpp>
#include <crpc/ftp/server.hpp>

#include "test_framework.hpp"
#include "tls_test_certificate.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace crpc;
using namespace crpc::ftp;

// Scratch tree under the current directory -- never the system temp directory,
// so a test run leaves its bytes inside the repository checkout.
class scratch_dir {
public:
    explicit scratch_dir(const std::string& tag) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = std::filesystem::current_path() /
               ("ftp-scratch-" + tag + "-" + std::to_string(now));
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
    }
    ~scratch_dir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    scratch_dir(const scratch_dir&) = delete;
    scratch_dir& operator=(const scratch_dir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return dir_; }
    [[nodiscard]] std::filesystem::path operator/(const std::string& name) const {
        return dir_ / name;
    }

private:
    std::filesystem::path dir_;
};

void write_file(const std::filesystem::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string deterministic_bytes(std::size_t size) {
    std::string data(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>((i * 37u + 11u) & 0xffu);
    }
    return data;
}

struct server_fixture {
    std::unique_ptr<server> instance;
    server_options options;
};

[[nodiscard]] bool start_server(server_fixture& fixture,
                                const std::filesystem::path& root) {
    fixture.options.address = "127.0.0.1";
    fixture.options.port = 0;
    fixture.options.root_directory = root.string();
    fixture.options.user = "tester";
    fixture.options.password = "secret";
    fixture.options.require_auth = true;
    auto started = server::start(fixture.options);
    if (!started) {
        std::fprintf(stderr, "server start failed: %s\n",
                     started.error().message.c_str());
        return false;
    }
    fixture.instance = std::move(started).value();
    return true;
}

[[nodiscard]] client_options client_for(const server& srv,
                                        const std::string& user = "tester",
                                        const std::string& password = "secret") {
    client_options options;
    options.host = "127.0.0.1";
    options.port = srv.port();
    options.user = user;
    options.password = password;
    options.timeout = std::chrono::seconds(20);
    return options;
}

} // namespace

// --- control connection ------------------------------------------------------

CRPC_TEST(ftp_client_greeting_and_quit) {
    scratch_dir scratch("greeting");
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto connected = client::connect(client_for(*fixture.instance));
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();

    CRPC_CHECK_EQ(220, session->greeting().code);
    CRPC_CHECK(session->is_open());
    CRPC_CHECK_FALSE(session->is_secure());

    auto noop = session->command("NOOP");
    CRPC_REQUIRE(noop.ok());
    CRPC_CHECK_EQ(200, noop.value().code);

    auto quit = session->quit();
    CRPC_REQUIRE(quit.ok());
    CRPC_CHECK_EQ(221, quit.value().code);
    CRPC_CHECK_FALSE(session->is_open());
}

CRPC_TEST(ftp_client_login_failure_is_reported) {
    scratch_dir scratch("authfail");
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto wrong = client::connect(client_for(*fixture.instance, "tester", "wrong"));
    CRPC_CHECK_FALSE(wrong.ok());
    if (!wrong.ok()) {
        CRPC_CHECK(wrong.code() == rpc_code::application ||
                   wrong.code() == rpc_code::permission_denied);
    }

    auto wrong_user = client::connect(client_for(*fixture.instance, "nobody", "secret"));
    CRPC_CHECK_FALSE(wrong_user.ok());
}

CRPC_TEST(ftp_client_manual_login) {
    scratch_dir scratch("manual");
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    client_options options = client_for(*fixture.instance);
    options.auto_login = false;
    auto connected = client::connect(options);
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();

    // Before login every command that needs a session is refused.
    auto early = session->pwd();
    CRPC_CHECK_FALSE(early.ok());

    auto logged_in = session->login("tester", "secret");
    CRPC_REQUIRE(logged_in.ok());
    CRPC_CHECK_EQ(230, logged_in.value().code);

    auto pwd = session->pwd();
    CRPC_REQUIRE(pwd.ok());
    CRPC_CHECK_EQ(std::string("/"), pwd.value());
}

CRPC_TEST(ftp_client_command_rejects_injection) {
    scratch_dir scratch("inject");
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto connected = client::connect(client_for(*fixture.instance));
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();

    auto injected = session->command("NOOP\r\nDELE victim");
    CRPC_CHECK_FALSE(injected.ok());
    if (!injected.ok()) {
        CRPC_CHECK(injected.code() == rpc_code::invalid_argument);
    }
    // The connection must still be usable: the rejected command was never sent.
    auto noop = session->command("NOOP");
    CRPC_REQUIRE(noop.ok());
    CRPC_CHECK_EQ(200, noop.value().code);
}

// --- directories and metadata ------------------------------------------------

CRPC_TEST(ftp_client_directory_lifecycle) {
    scratch_dir scratch("dirs");
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto connected = client::connect(client_for(*fixture.instance));
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();

    auto root = session->pwd();
    CRPC_REQUIRE(root.ok());
    CRPC_CHECK_EQ(std::string("/"), root.value());

    auto made = session->mkdir("alpha");
    CRPC_REQUIRE(made.ok());
    CRPC_CHECK_EQ(257, made.value().code);

    auto entered = session->cwd("alpha");
    CRPC_REQUIRE(entered.ok());
    auto inside = session->pwd();
    CRPC_REQUIRE(inside.ok());
    CRPC_CHECK_EQ(std::string("/alpha"), inside.value());

    auto up = session->cdup();
    CRPC_REQUIRE(up.ok());
    auto back = session->pwd();
    CRPC_REQUIRE(back.ok());
    CRPC_CHECK_EQ(std::string("/"), back.value());

    auto missing = session->cwd("does-not-exist");
    CRPC_CHECK_FALSE(missing.ok());

    auto removed = session->rmdir("alpha");
    CRPC_REQUIRE(removed.ok());
    auto listed = session->list();
    CRPC_REQUIRE(listed.ok());
    for (const std::string& line : listed.value()) {
        CRPC_CHECK(line.find("alpha") == std::string::npos);
    }
}

CRPC_TEST(ftp_client_metadata_size_mdtm_rename_remove) {
    scratch_dir scratch("meta");
    write_file(scratch / "data.bin", "0123456789");
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto connected = client::connect(client_for(*fixture.instance));
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();

    auto size = session->size("data.bin");
    CRPC_REQUIRE(size.ok());
    CRPC_CHECK_EQ(std::uint64_t{10}, size.value());

    auto mdtm = session->mdtm("data.bin");
    CRPC_REQUIRE(mdtm.ok());
    // MDTM answer: exactly `YYYYMMDDHHMMSS` digits followed by optional `.sss`.
    CRPC_CHECK(mdtm.value().size() >= 14);
    if (mdtm.value().size() >= 14) {
        for (std::size_t i = 0; i < 14; ++i) {
            CRPC_CHECK(mdtm.value()[i] >= '0' && mdtm.value()[i] <= '9');
        }
        CRPC_CHECK_EQ(std::string("202"), mdtm.value().substr(0, 3));
    }

    auto renamed = session->rename("data.bin", "renamed.bin");
    CRPC_REQUIRE(renamed.ok());
    CRPC_CHECK_EQ(250, renamed.value().code);

    auto missing_size = session->size("data.bin");
    CRPC_CHECK_FALSE(missing_size.ok());

    auto new_size = session->size("renamed.bin");
    CRPC_REQUIRE(new_size.ok());
    CRPC_CHECK_EQ(std::uint64_t{10}, new_size.value());

    auto remove_missing = session->remove("nope.bin");
    CRPC_CHECK_FALSE(remove_missing.ok());

    auto removed = session->remove("renamed.bin");
    CRPC_REQUIRE(removed.ok());
    auto gone = session->size("renamed.bin");
    CRPC_CHECK_FALSE(gone.ok());
}

CRPC_TEST(ftp_client_list_and_nlst) {
    scratch_dir scratch("listing");
    write_file(scratch / "one.txt", "1");
    write_file(scratch / "two.txt", "22");
    std::error_code ec;
    std::filesystem::create_directories(scratch / "sub", ec);
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto connected = client::connect(client_for(*fixture.instance));
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();

    auto listing = session->list();
    CRPC_REQUIRE(listing.ok());
    CRPC_CHECK_EQ(std::size_t{3}, listing.value().size());
    bool saw_file = false;
    bool saw_dir = false;
    for (const std::string& line : listing.value()) {
        if (line.find("one.txt") != std::string::npos) {
            saw_file = true;
            CRPC_CHECK(line.size() >= 2);
            CRPC_CHECK(line[0] == '-');
        }
        if (line.find("sub") != std::string::npos) {
            saw_dir = true;
            CRPC_CHECK(line[0] == 'd');
        }
    }
    CRPC_CHECK(saw_file);
    CRPC_CHECK(saw_dir);

    auto names = session->nlst();
    CRPC_REQUIRE(names.ok());
    CRPC_CHECK_EQ(std::size_t{3}, names.value().size());

    auto one_list = session->list("one.txt");
    CRPC_REQUIRE(one_list.ok());
    CRPC_REQUIRE(one_list.value().size() == 1);
    CRPC_CHECK(one_list.value()[0].find("one.txt") != std::string::npos);

    auto names_of_sub = session->nlst("sub");
    CRPC_REQUIRE(names_of_sub.ok());
    CRPC_CHECK(names_of_sub.value().empty());
}

// --- transfers ---------------------------------------------------------------

CRPC_TEST(ftp_client_stor_retr_round_trip) {
    scratch_dir scratch("roundtrip");
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto connected = client::connect(client_for(*fixture.instance));
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();

    CRPC_REQUIRE(session->type_binary().ok());
    const std::string payload = "hello\r\nworld\r\n\x01\x02\x03";
    auto stored = session->stor("upload.bin", payload);
    CRPC_REQUIRE(stored.ok());
    CRPC_CHECK_EQ(226, stored.value().code);
    CRPC_CHECK_EQ(payload, read_file(scratch / "upload.bin"));

    auto fetched = session->retr("upload.bin");
    CRPC_REQUIRE(fetched.ok());
    CRPC_CHECK_EQ(payload, fetched.value());

    auto stored_size = session->size("upload.bin");
    CRPC_REQUIRE(stored_size.ok());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(payload.size()), stored_size.value());
}

CRPC_TEST(ftp_client_append_and_ascii_type) {
    scratch_dir scratch("append");
    write_file(scratch / "log.txt", "start");
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto connected = client::connect(client_for(*fixture.instance));
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();

    CRPC_REQUIRE(session->type_ascii().ok());
    auto appended = session->appe("log.txt", "-more");
    CRPC_REQUIRE(appended.ok());
    CRPC_CHECK_EQ(std::string("start-more"), read_file(scratch / "log.txt"));

    CRPC_REQUIRE(session->type_binary().ok());
    auto appended_bin = session->appe("log.txt", std::string_view("\x00\x01", 2));
    CRPC_REQUIRE(appended_bin.ok());
    CRPC_CHECK_EQ(std::string("start-more\x00\x01", 12), read_file(scratch / "log.txt"));
}

CRPC_TEST(ftp_client_large_file_and_resume) {
    scratch_dir scratch("large");
    const std::string payload = deterministic_bytes(1536 * 1024 + 17);
    write_file(scratch / "large.bin", payload);
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto connected = client::connect(client_for(*fixture.instance));
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();

    CRPC_REQUIRE(session->type_binary().ok());

    auto full = session->retr("large.bin");
    CRPC_REQUIRE(full.ok());
    CRPC_CHECK_EQ(payload.size(), full.value().size());
    CRPC_CHECK(full.value() == payload);

    // REST + offset: the server must skip exactly `offset` bytes.
    const std::uint64_t offset = 1024 * 1024 + 3;
    auto resumed = session->retr("large.bin", offset);
    CRPC_REQUIRE(resumed.ok());
    CRPC_CHECK_EQ(payload.size() - offset, resumed.value().size());
    CRPC_CHECK_EQ(payload.substr(static_cast<std::size_t>(offset)), resumed.value());

    // Half-way offset must not produce a prefix of the whole file.
    CRPC_CHECK(resumed.value() != payload.substr(0, resumed.value().size()));
}

CRPC_TEST(ftp_client_file_helpers_and_resume_to_disk) {
    scratch_dir scratch("files");
    const std::string payload = deterministic_bytes(300 * 1024);
    write_file(scratch / "source.bin", payload);
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto connected = client::connect(client_for(*fixture.instance));
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();
    CRPC_REQUIRE(session->type_binary().ok());

    const std::filesystem::path uploaded = scratch / "server-copy.bin";
    auto stor = session->stor_from_file("server-copy.bin", scratch / "source.bin");
    CRPC_REQUIRE(stor.ok());
    CRPC_CHECK_EQ(226, stor.value().code);
    CRPC_CHECK(read_file(scratch / "server-copy.bin") == payload);

    const std::filesystem::path downloaded = scratch / "client-copy.bin";
    auto retr = session->retr_to_file("server-copy.bin", downloaded);
    CRPC_REQUIRE(retr.ok());
    CRPC_CHECK_EQ(downloaded.string(), retr.value());
    CRPC_CHECK(read_file(downloaded) == payload);

    // Resume a partial download: the first 100 KiB is already on disk.
    const std::filesystem::path partial = scratch / "partial.bin";
    write_file(partial, payload.substr(0, 100 * 1024));
    auto resumed = session->retr_to_file("server-copy.bin", partial, 100 * 1024);
    CRPC_REQUIRE(resumed.ok());
    CRPC_CHECK(read_file(partial) == payload);

    // Appending upload of a second file.
    write_file(scratch / "tail.bin", "TAIL");
    auto appended = session->stor_from_file("server-copy.bin", scratch / "tail.bin", true);
    CRPC_REQUIRE(appended.ok());
    CRPC_CHECK(read_file(scratch / "server-copy.bin") == payload + "TAIL");
}

CRPC_TEST(ftp_client_missing_remote_file_is_reported) {
    scratch_dir scratch("missing");
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto connected = client::connect(client_for(*fixture.instance));
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();
    CRPC_REQUIRE(session->type_binary().ok());

    auto retr = session->retr("nothing.bin");
    CRPC_CHECK_FALSE(retr.ok());
    if (!retr.ok()) {
        CRPC_CHECK(retr.code() == rpc_code::application);
        CRPC_CHECK(retr.error().metadata.find("ftp.code") != nullptr);
    }

    // The session must stay synchronised after a 550.
    auto noop = session->command("NOOP");
    CRPC_REQUIRE(noop.ok());
    CRPC_CHECK_EQ(200, noop.value().code);
}

// --- confinement -------------------------------------------------------------

CRPC_TEST(ftp_client_rejects_path_escape) {
    scratch_dir scratch("escape");
    // One file immediately outside the served root, plus a nested secret.
    write_file(std::filesystem::current_path() / "ftp-outside-secret.txt", "TOP SECRET");
    write_file(scratch.path().parent_path() / "ftp-sibling-secret.txt", "SIBLING SECRET");
    write_file(scratch / "inside.txt", "inside");
    server_fixture fixture;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto connected = client::connect(client_for(*fixture.instance));
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();
    CRPC_REQUIRE(session->type_binary().ok());

    // 1) Parent traversal via a relative path.
    auto parent = session->retr("../ftp-sibling-secret.txt");
    CRPC_CHECK_FALSE(parent.ok());

    auto parent_stor = session->stor("../ftp-escape-upload.txt", "x");
    CRPC_CHECK_FALSE(parent_stor.ok());

    // 2) Absolute path.
    auto absolute = session->retr("/etc/passwd");
    CRPC_CHECK_FALSE(absolute.ok());

    auto absolute_size = session->size("/etc/passwd");
    CRPC_CHECK_FALSE(absolute_size.ok());

    // 3) Traversal hidden in the middle of a path.
    auto nested = session->retr("sub/../../ftp-sibling-secret.txt");
    CRPC_CHECK_FALSE(nested.ok());

    // 4) CWD traversal is refused, and the session keeps its directory.
    auto cwd_escape = session->cwd("..");
    CRPC_CHECK_FALSE(cwd_escape.ok());
    auto pwd = session->pwd();
    CRPC_REQUIRE(pwd.ok());
    CRPC_CHECK_EQ(std::string("/"), pwd.value());

    // 5) The legitimate file still works, so the rejection is not a general
    //    failure of the transfer path.
    auto inside = session->retr("inside.txt");
    CRPC_REQUIRE(inside.ok());
    CRPC_CHECK_EQ(std::string("inside"), inside.value());

    // 6) A symlink that points outside the root is refused even though the
    //    request itself has no `..`.  Creating a symlink needs a privilege on
    //    Windows; when that fails the check is skipped and reported.
    std::error_code symlink_ec;
    const std::filesystem::path link = scratch / "escape-link";
    std::filesystem::create_directory_symlink(scratch.path().parent_path(), link,
                                              symlink_ec);
    if (!symlink_ec) {
        auto through_link = session->retr("escape-link/ftp-sibling-secret.txt");
        CRPC_CHECK_FALSE(through_link.ok());
    } else {
        std::printf("NOTE: symlink escape check skipped (%s)\n",
                    symlink_ec.message().c_str());
    }

    std::error_code cleanup_ec;
    std::filesystem::remove(std::filesystem::current_path() / "ftp-outside-secret.txt",
                            cleanup_ec);
    std::filesystem::remove(scratch.path().parent_path() / "ftp-sibling-secret.txt",
                            cleanup_ec);
}

CRPC_TEST(ftp_client_read_only_server_refuses_writes) {
    scratch_dir scratch("readonly");
    write_file(scratch / "keep.txt", "keep");
    server_fixture fixture;
    fixture.options.read_only = true;
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    auto connected = client::connect(client_for(*fixture.instance));
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();

    auto stor = session->stor("new.txt", "data");
    CRPC_CHECK_FALSE(stor.ok());
    auto appe = session->appe("keep.txt", "more");
    CRPC_CHECK_FALSE(appe.ok());
    auto remove = session->remove("keep.txt");
    CRPC_CHECK_FALSE(remove.ok());
    auto rmdir = session->rmdir("keep.txt");
    CRPC_CHECK_FALSE(rmdir.ok());
    auto mkdir = session->mkdir("dir");
    CRPC_CHECK_FALSE(mkdir.ok());
    auto rename = session->rename("keep.txt", "other.txt");
    CRPC_CHECK_FALSE(rename.ok());

    // Reads still work.
    auto retr = session->retr("keep.txt");
    CRPC_REQUIRE(retr.ok());
    CRPC_CHECK_EQ(std::string("keep"), retr.value());
}

// --- FTPS --------------------------------------------------------------------

#if defined(CRPC_HAVE_OPENSSL)

namespace {

[[nodiscard]] bool write_certificate_files(const std::filesystem::path& dir,
                                           std::filesystem::path& cert,
                                           std::filesystem::path& key) {
    cert = dir / "ftp-test-cert.pem";
    key = dir / "ftp-test-key.pem";
    write_file(cert, crpc_test_tls::k_certificate_pem);
    write_file(key, crpc_test_tls::k_private_key_pem);
    return std::filesystem::exists(cert) && std::filesystem::exists(key);
}

} // namespace

CRPC_TEST(ftp_client_explicit_tls_full_session) {
    scratch_dir scratch("tls");
    std::filesystem::path cert;
    std::filesystem::path key;
    CRPC_REQUIRE(write_certificate_files(scratch.path(), cert, key));
    write_file(scratch / "tls.txt", "protected");

    server_fixture fixture;
    fixture.options.enable_tls = true;
    fixture.options.cert_chain = cert.string();
    fixture.options.private_key = key.string();
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    client_options options = client_for(*fixture.instance);
    options.use_tls = true;
    options.verify_peer = true;
    options.ca_bundle = cert.string();
    auto connected = client::connect(options);
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();

    CRPC_CHECK(session->is_secure());
    CRPC_CHECK(session->data_is_secure());

    // Directory and metadata commands over the protected control connection.
    auto pwd = session->pwd();
    CRPC_REQUIRE(pwd.ok());
    CRPC_CHECK_EQ(std::string("/"), pwd.value());

    // A protected data connection (PROT P) for both directions.
    CRPC_REQUIRE(session->type_binary().ok());
    auto stored = session->stor("secure.bin", "ciphertext on the wire");
    CRPC_REQUIRE(stored.ok());
    CRPC_CHECK_EQ(std::string("ciphertext on the wire"), read_file(scratch / "secure.bin"));

    auto fetched = session->retr("tls.txt");
    CRPC_REQUIRE(fetched.ok());
    CRPC_CHECK_EQ(std::string("protected"), fetched.value());

    auto listing = session->list();
    CRPC_REQUIRE(listing.ok());
    CRPC_CHECK(listing.value().size() >= 2);
}

CRPC_TEST(ftp_client_tls_rejects_untrusted_certificate) {
    scratch_dir scratch("tlsbad");
    std::filesystem::path cert;
    std::filesystem::path key;
    CRPC_REQUIRE(write_certificate_files(scratch.path(), cert, key));
    write_file(scratch / "file.txt", "x");

    server_fixture fixture;
    fixture.options.enable_tls = true;
    fixture.options.cert_chain = cert.string();
    fixture.options.private_key = key.string();
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    client_options options = client_for(*fixture.instance);
    options.use_tls = true;
    options.verify_peer = true;
    // No ca_bundle and no default trust anchor for this self-signed cert.
    auto connected = client::connect(options);
    CRPC_CHECK_FALSE(connected.ok());
}

CRPC_TEST(ftp_client_implicit_tls) {
    scratch_dir scratch("impl");
    std::filesystem::path cert;
    std::filesystem::path key;
    CRPC_REQUIRE(write_certificate_files(scratch.path(), cert, key));
    write_file(scratch / "implicit.txt", "implicit");

    server_fixture fixture;
    fixture.options.enable_tls = true;
    fixture.options.allow_implicit_tls = true;
    fixture.options.cert_chain = cert.string();
    fixture.options.private_key = key.string();
    CRPC_REQUIRE(start_server(fixture, scratch.path()));

    client_options options = client_for(*fixture.instance);
    options.implicit_tls = true;
    options.ca_bundle = cert.string();
    auto connected = client::connect(options);
    CRPC_REQUIRE(connected.ok());
    std::unique_ptr<client> session = std::move(connected).value();
    CRPC_CHECK(session->is_secure());
    CRPC_CHECK_EQ(220, session->greeting().code);

    auto fetched = session->retr("implicit.txt");
    CRPC_REQUIRE(fetched.ok());
    CRPC_CHECK_EQ(std::string("implicit"), fetched.value());
}

#else

CRPC_TEST(ftp_client_explicit_tls_full_session) {
    crpc_test::skip("CRPC_WITH_OPENSSL=OFF: build has no TLS backend");
}

CRPC_TEST(ftp_client_tls_rejects_untrusted_certificate) {
    crpc_test::skip("CRPC_WITH_OPENSSL=OFF: build has no TLS backend");
}

CRPC_TEST(ftp_client_implicit_tls) {
    crpc_test::skip("CRPC_WITH_OPENSSL=OFF: build has no TLS backend");
}

#endif

// --- EPSV reply parsing (pure function) ---------------------------------------
//
// The passive-mode path falls back to PASV when EPSV yields no usable port, so
// a parser regression would be invisible in the behaviour cases above.  These
// cases pin the parser contract directly: only the exact RFC 2429 shape
// "(|||port|)" yields a port; everything else yields 0 (the caller's "fall back
// to PASV" signal).

CRPC_TEST(ftp_client_epsv_port_parsing_contract) {
    using crpc::ftp::detail::parse_epsv_port;

    // A full reply line parses; the text around the parenthesised tuple is
    // irrelevant.
    CRPC_CHECK_EQ(1, parse_epsv_port("Entering Extended Passive Mode (|||1|)"));
    CRPC_CHECK_EQ(21234,
                  parse_epsv_port("Entering Extended Passive Mode (|||21234|)"));
    CRPC_CHECK_EQ(65535, parse_epsv_port("(|||65535|)"));
    CRPC_CHECK_EQ(1, parse_epsv_port("x(|||1|)y"));

    // Boundary: 0 is syntactically well formed but returns 0 -- no caller can
    // dial port 0, and 0 doubles as the failure signal.
    CRPC_CHECK_EQ(0, parse_epsv_port("Entering Extended Passive Mode (|||0|)"));

    // A second number inside the tuple is not the RFC shape: reject.
    CRPC_CHECK_EQ(0, parse_epsv_port("Entering Extended Passive Mode (|||21|22|)"));

    // Malformed: missing delimiters, wrong delimiter count, no parens.
    CRPC_CHECK_EQ(0, parse_epsv_port("Entering Extended Passive Mode (|1|)"));
    CRPC_CHECK_EQ(0, parse_epsv_port("Entering Extended Passive Mode (||1|)"));
    CRPC_CHECK_EQ(0, parse_epsv_port("Entering Extended Passive Mode (|||||)"));
    CRPC_CHECK_EQ(0, parse_epsv_port("Entering Extended Passive Mode (|||12345"));
    CRPC_CHECK_EQ(0, parse_epsv_port("Entering Extended Passive Mode |||12345|)"));
    CRPC_CHECK_EQ(0, parse_epsv_port(""));
    CRPC_CHECK_EQ(0, parse_epsv_port("nothing here"));

    // Malformed: non-digit payloads.
    CRPC_CHECK_EQ(0, parse_epsv_port("Entering Extended Passive Mode (|||12a4|)"));
    CRPC_CHECK_EQ(0, parse_epsv_port("Entering Extended Passive Mode (||| 1234|)"));
    CRPC_CHECK_EQ(0, parse_epsv_port("Entering Extended Passive Mode (|||-123|)"));
    CRPC_CHECK_EQ(0,
                  parse_epsv_port("Entering Extended Passive Mode (|||0x1F|)"));

    // Out of range: the port must fit in 16 bits.
    CRPC_CHECK_EQ(0, parse_epsv_port("Entering Extended Passive Mode (|||65536|)"));
    CRPC_CHECK_EQ(0,
                  parse_epsv_port("Entering Extended Passive Mode (|||99999999|)"));
}

CRPC_TEST_MAIN()
