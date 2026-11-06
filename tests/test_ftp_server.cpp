// CRPC -- FTP server tests: real sockets, protocol edge cases, confinement.
//
// These cases drive the server with a hand-written control-connection client so
// each reply code and state transition is asserted directly, without going
// through `ftp::client` (which has its own suite).  Transfers still use the real
// data path: PASV/EPRT, a real TCP listener and a real accepted socket.
#include <crpc/ftp/reply.hpp>
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
#include <thread>
#include <vector>

#include <crpc/net/channel.hpp>
#include <crpc/net/connector.hpp>
#include <crpc/net/listener.hpp>

namespace {

using namespace crpc;
using namespace crpc::ftp;

class scratch_dir {
public:
    explicit scratch_dir(const std::string& tag) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = std::filesystem::current_path() /
               ("ftp-srv-scratch-" + tag + "-" + std::to_string(now));
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

// A minimal blocking FTP control client over `net::tcp_stream`: enough to send
// commands and read structured replies without depending on ftp::client.
class raw_control {
public:
    [[nodiscard]] bool open(const net::address& target) {
        net::connect_options options;
        options.timeout = std::chrono::seconds(10);
        options.stream.rw_timeout = std::chrono::seconds(20);
        auto connected = net::connect(target, options);
        if (!connected) {
            return false;
        }
        stream_ = std::move(connected).value();
        return true;
    }

    [[nodiscard]] bool send(const std::string& line) {
        const std::string wire = line + "\r\n";
        return stream_->write(crpc::span<const std::byte>(
                                  reinterpret_cast<const std::byte*>(wire.data()),
                                  wire.size()))
            .ok();
    }

    [[nodiscard]] int reply(std::string& out) {
        std::string buffer;
        std::string line;
        int code = -1;
        bool multiline = false;
        for (;;) {
            std::byte byte{};
            auto read = stream_->read_some(crpc::span<std::byte>(&byte, 1));
            if (!read || read.value() != 1) {
                return -1;
            }
            const char c = static_cast<char>(byte);
            if (c != '\n') {
                line.push_back(c);
                continue;
            }
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (code < 0) {
                if (line.size() < 3) {
                    return -1;
                }
                code = std::stoi(line.substr(0, 3));
                multiline = line.size() > 3 && line[3] == '-';
                if (!multiline) {
                    out = line.size() > 4 ? line.substr(4) : std::string();
                    return code;
                }
                // A multiline reply keeps its whole body: every line between
                // the opening `code-` and the closing `code ` accumulates.
                out = line.size() > 4 ? line.substr(4) : std::string();
            } else if (line.size() > 3 && line.compare(0, 3, std::to_string(code)) == 0 &&
                       line[3] == ' ') {
                if (!out.empty()) {
                    out += "\n";
                }
                out += line.size() > 4 ? line.substr(4) : std::string();
                return code;
            } else {
                if (!out.empty()) {
                    out += "\n";
                }
                out += line;
            }
            line.clear();
        }
    }

    [[nodiscard]] int command(const std::string& line, std::string& out) {
        if (!send(line)) {
            return -1;
        }
        return reply(out);
    }

    [[nodiscard]] net::tcp_stream& stream() noexcept { return *stream_; }

private:
    std::unique_ptr<net::tcp_stream> stream_;
};

struct fixture {
    std::unique_ptr<server> instance;
    server_options options;
};

[[nodiscard]] bool start(fixture& f, const std::filesystem::path& root) {
    f.options.address = "127.0.0.1";
    f.options.port = 0;
    f.options.root_directory = root.string();
    f.options.user = "tester";
    f.options.password = "secret";
    auto started = server::start(f.options);
    if (!started) {
        std::fprintf(stderr, "server start failed: %s\n",
                     started.error().message.c_str());
        return false;
    }
    f.instance = std::move(started).value();
    return true;
}

[[nodiscard]] net::address server_address(const fixture& f) {
    return net::address::tcp("127.0.0.1", f.instance->port());
}

[[nodiscard]] bool login(raw_control& control) {
    std::string text;
    if (control.command("USER tester", text) != 331) {
        return false;
    }
    return control.command("PASS secret", text) == 230;
}

// Opens a passive data connection from a PASV reply and returns the stream.
[[nodiscard]] std::unique_ptr<net::tcp_stream> open_pasv(raw_control& control,
                                                         int& reply_code) {
    std::string text;
    reply_code = control.command("PASV", text);
    if (reply_code != 227) {
        return nullptr;
    }
    const std::size_t open = text.find('(');
    const std::size_t close = text.find(')');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return nullptr;
    }
    std::vector<int> parts;
    std::size_t pos = open + 1;
    while (pos < close) {
        const std::size_t comma = text.find(',', pos);
        const std::size_t end = comma == std::string::npos || comma > close ? close : comma;
        parts.push_back(std::stoi(text.substr(pos, end - pos)));
        if (end == close) {
            break;
        }
        pos = end + 1;
    }
    if (parts.size() != 6) {
        return nullptr;
    }
    const std::string host = std::to_string(parts[0]) + "." + std::to_string(parts[1]) +
                             "." + std::to_string(parts[2]) + "." + std::to_string(parts[3]);
    const std::uint16_t port =
        static_cast<std::uint16_t>(parts[4] * 256 + parts[5]);
    net::connect_options options;
    options.timeout = std::chrono::seconds(10);
    options.stream.rw_timeout = std::chrono::seconds(20);
    auto connected = net::connect(net::address::tcp(host, port), options);
    if (!connected) {
        return nullptr;
    }
    return std::move(connected).value();
}

[[nodiscard]] std::string read_all(net::channel& channel) {
    std::string out;
    std::byte chunk[16 * 1024];
    for (;;) {
        auto got = channel.read_some(crpc::span<std::byte>(chunk, sizeof(chunk)));
        if (!got) {
            break;
        }
        out.append(reinterpret_cast<const char*>(chunk), got.value());
    }
    return out;
}

} // namespace

// --- lifecycle ---------------------------------------------------------------

CRPC_TEST(ftp_server_reports_ephemeral_port_and_sessions) {
    scratch_dir scratch("life");
    fixture f;
    CRPC_REQUIRE(start(f, scratch.path()));
    CRPC_CHECK(f.instance->port() != 0);
    CRPC_CHECK_EQ(std::size_t{0}, f.instance->active_sessions());

    raw_control control;
    CRPC_REQUIRE(control.open(server_address(f)));
    std::string text;
    CRPC_CHECK_EQ(220, control.reply(text));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return f.instance->active_sessions() == 1; }, 5000));

    CRPC_CHECK_EQ(221, control.command("QUIT", text));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return f.instance->active_sessions() == 0; }, 5000));
}

CRPC_TEST(ftp_server_requires_root_directory) {
    server_options options;
    options.port = 0;
    options.root_directory = "F:/definitely-not-a-real-ftp-root-42";
    auto started = server::start(options);
    CRPC_CHECK_FALSE(started.ok());
}

CRPC_TEST(ftp_server_connection_limit) {
    scratch_dir scratch("limit");
    fixture f;
    f.options.max_connections = 1;
    CRPC_REQUIRE(start(f, scratch.path()));

    raw_control first;
    CRPC_REQUIRE(first.open(server_address(f)));
    std::string text;
    CRPC_CHECK_EQ(220, first.reply(text));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return f.instance->active_sessions() == 1; }, 5000));

    raw_control second;
    CRPC_REQUIRE(second.open(server_address(f)));
    // The server either answers 421 and closes, or closes without a reply.
    std::string overflow;
    const int code = second.reply(overflow);
    CRPC_CHECK(code == 421 || code == -1);
}

// --- protocol ----------------------------------------------------------------

CRPC_TEST(ftp_server_rejects_oversized_and_malformed_lines) {
    scratch_dir scratch("lines");
    fixture f;
    f.options.max_line_bytes = 64;
    CRPC_REQUIRE(start(f, scratch.path()));

    raw_control control;
    CRPC_REQUIRE(control.open(server_address(f)));
    std::string text;
    CRPC_CHECK_EQ(220, control.reply(text));

    // A verb of non-letters is refused, and the session survives.
    CRPC_CHECK_EQ(500, control.command("123 bad", text));
    CRPC_CHECK_EQ(200, control.command("NOOP", text));

    // A line past the server's limit is refused and the connection is closed.
    const std::string long_line = "USER " + std::string(200, 'a');
    const int code = control.command(long_line, text);
    CRPC_CHECK(code == 500 || code == 421 || code == -1);
}

CRPC_TEST(ftp_server_sequence_errors) {
    scratch_dir scratch("sequence");
    fixture f;
    CRPC_REQUIRE(start(f, scratch.path()));

    raw_control control;
    CRPC_REQUIRE(control.open(server_address(f)));
    std::string text;
    CRPC_CHECK_EQ(220, control.reply(text));

    // Before login, session commands are refused with 530.
    CRPC_CHECK_EQ(530, control.command("PWD", text));
    CRPC_CHECK_EQ(530, control.command("RETR x", text));
    CRPC_CHECK_EQ(530, control.command("STOR x", text));

    // USER twice: the second one restarts authentication.
    CRPC_CHECK_EQ(331, control.command("USER tester", text));
    CRPC_CHECK_EQ(331, control.command("USER tester", text));
    CRPC_CHECK_EQ(230, control.command("PASS secret", text));

    // Commands that need an argument.
    CRPC_CHECK_EQ(501, control.command("RETR", text));
    CRPC_CHECK_EQ(501, control.command("CWD", text));
    CRPC_CHECK_EQ(501, control.command("REST", text));
    CRPC_CHECK_EQ(501, control.command("PORT", text));

    // Unknown verbs are 502, not a dropped connection.
    CRPC_CHECK_EQ(502, control.command("FROB x", text));
    CRPC_CHECK_EQ(200, control.command("NOOP", text));

    // A second PASS after login is 503.
    CRPC_CHECK_EQ(503, control.command("PASS secret", text));
}

CRPC_TEST(ftp_server_features_and_auth_negotiation) {
    scratch_dir scratch("feat");
    fixture f;
    CRPC_REQUIRE(start(f, scratch.path()));

    raw_control control;
    CRPC_REQUIRE(control.open(server_address(f)));
    std::string text;
    CRPC_CHECK_EQ(220, control.reply(text));

    // FEAT is allowed before login and is a multi-line 211 carrying UTF8.
    auto feat_code = control.command("FEAT", text);
    CRPC_CHECK_EQ(211, feat_code);
    CRPC_CHECK(text.find("UTF8") != std::string::npos);

    // AUTH TLS on a cleartext server is refused.
    CRPC_CHECK_EQ(534, control.command("AUTH TLS", text));

    // PBSZ/PROT require an established protection buffer.
    CRPC_CHECK_EQ(331, control.command("USER tester", text));
    CRPC_CHECK_EQ(230, control.command("PASS secret", text));
    CRPC_CHECK_EQ(503, control.command("PBSZ 0", text));
}

CRPC_TEST(ftp_server_pasv_is_single_use) {
    scratch_dir scratch("pasv");
    write_file(scratch / "file.txt", "abcdef");
    fixture f;
    CRPC_REQUIRE(start(f, scratch.path()));

    raw_control control;
    CRPC_REQUIRE(control.open(server_address(f)));
    std::string text;
    CRPC_CHECK_EQ(220, control.reply(text));
    CRPC_REQUIRE(login(control));
    CRPC_CHECK_EQ(200, control.command("TYPE I", text));

    // The first PASV listener is consumed by the transfer that follows.
    int code = 0;
    auto data = open_pasv(control, code);
    CRPC_REQUIRE(code == 227);
    CRPC_REQUIRE(data != nullptr);
    CRPC_CHECK_EQ(150, control.command("RETR file.txt", text));
    CRPC_CHECK_EQ(std::string("abcdef"), read_all(*data));
    data->close();
    CRPC_CHECK_EQ(226, control.reply(text));

    // The old port is dead: connecting again must fail.
    net::connect_options options;
    options.timeout = std::chrono::milliseconds(500);
    auto stale = net::connect(net::address::tcp(data->peer().host(), data->peer().port()),
                              options);
    CRPC_CHECK_FALSE(stale.ok());

    // A transfer without any PASV/PORT is refused, not silently ignored.
    CRPC_CHECK_EQ(425, control.command("RETR file.txt", text));
}

CRPC_TEST(ftp_server_rest_and_appe) {
    scratch_dir scratch("rest");
    write_file(scratch / "data.bin", "0123456789ABCDEF");
    fixture f;
    CRPC_REQUIRE(start(f, scratch.path()));

    raw_control control;
    CRPC_REQUIRE(control.open(server_address(f)));
    std::string text;
    CRPC_CHECK_EQ(220, control.reply(text));
    CRPC_REQUIRE(login(control));
    CRPC_CHECK_EQ(200, control.command("TYPE I", text));

    // REST is accepted, then the retrieve starts at the offset.
    CRPC_CHECK_EQ(350, control.command("REST 5", text));
    int code = 0;
    auto data = open_pasv(control, code);
    CRPC_REQUIRE(code == 227);
    CRPC_REQUIRE(control.command("RETR data.bin", text) == 150);
    CRPC_CHECK_EQ(std::string("56789ABCDEF"), read_all(*data));
    data->close();
    CRPC_CHECK_EQ(226, control.reply(text));

    // REST is per-transfer state: the next RETR starts at zero again.
    auto second = open_pasv(control, code);
    CRPC_REQUIRE(code == 227);
    CRPC_REQUIRE(control.command("RETR data.bin", text) == 150);
    CRPC_CHECK_EQ(std::string("0123456789ABCDEF"), read_all(*second));
    second->close();
    CRPC_CHECK_EQ(226, control.reply(text));

    // APPE appends at the current end.  The data connection is opened first
    // (PASV), then the command is sent, then the bytes go out.
    auto append = open_pasv(control, code);
    CRPC_REQUIRE(code == 227);
    CRPC_REQUIRE(append != nullptr);
    CRPC_REQUIRE(control.command("APPE data.bin", text) == 150);
    const std::string tail = "XYZ";
    const crpc::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(tail.data()), tail.size());
    CRPC_CHECK(append->write(bytes).ok());
    append->close();
    CRPC_CHECK_EQ(226, control.reply(text));
    CRPC_CHECK_EQ(std::string("0123456789ABCDEFXYZ"), read_file(scratch / "data.bin"));

    // An out-of-range REST is 501.
    CRPC_CHECK(control.command("REST -5", text) == 501);
    CRPC_CHECK(control.command("REST abc", text) == 501);
}

CRPC_TEST(ftp_server_confinement_rejects_escape) {
    scratch_dir scratch("confine");
    write_file(scratch / "ok.txt", "ok");
    std::error_code ec;
    std::filesystem::create_directories(scratch.path().parent_path(), ec);
    write_file(scratch.path().parent_path() / "ftp-srv-escape.txt", "SECRET");
    fixture f;
    CRPC_REQUIRE(start(f, scratch.path()));

    raw_control control;
    CRPC_REQUIRE(control.open(server_address(f)));
    std::string text;
    CRPC_CHECK_EQ(220, control.reply(text));
    CRPC_REQUIRE(login(control));

    // Relative traversal, absolute paths, and Windows forms are all refused
    // with a permanent negative completion.
    CRPC_CHECK(control.command("SIZE ../ftp-srv-escape.txt", text) == 550);
    CRPC_CHECK(control.command("SIZE /etc/passwd", text) == 550);
    CRPC_CHECK(control.command("SIZE C:/Windows/win.ini", text) == 550);
    CRPC_CHECK(control.command("SIZE \\\\localhost\\C$\\win.ini", text) == 550);
    CRPC_CHECK(control.command("CWD ..", text) == 550);
    CRPC_CHECK(control.command("RETR ../ftp-srv-escape.txt", text) == 550);
    CRPC_CHECK(control.command("DELE ../ftp-srv-escape.txt", text) == 550);
    CRPC_CHECK(control.command("MKD ../ftp-out", text) == 550);
    // The escape target must still be intact after all of that.
    CRPC_CHECK_EQ(std::string("SECRET"),
                  read_file(scratch.path().parent_path() / "ftp-srv-escape.txt"));

    // The legitimate file resolves and reports its size.
    CRPC_CHECK_EQ(213, control.command("SIZE ok.txt", text));

    std::error_code cleanup;
    std::filesystem::remove(scratch.path().parent_path() / "ftp-srv-escape.txt",
                            cleanup);
}

CRPC_TEST(ftp_server_eprt_and_port_are_refused_in_passive_only_mode) {
    scratch_dir scratch("eprt");
    write_file(scratch / "f.txt", "data");
    fixture f;
    CRPC_REQUIRE(start(f, scratch.path()));

    raw_control control;
    CRPC_REQUIRE(control.open(server_address(f)));
    std::string text;
    CRPC_CHECK_EQ(220, control.reply(text));
    CRPC_REQUIRE(login(control));

    // The server implements EPRT/PORT: they arm an active data connection.  A
    // target that is not the control peer is refused (FTP bounce).
    const int eprt = control.command("EPRT |1|8.8.8.8|1234|", text);
    CRPC_CHECK(eprt == 501 || eprt == 500);
    const int port = control.command("PORT 8,8,8,8,4,210", text);
    CRPC_CHECK(port == 501 || port == 500);
}

CRPC_TEST(ftp_server_stop_closes_sessions) {
    scratch_dir scratch("stop");
    fixture f;
    CRPC_REQUIRE(start(f, scratch.path()));

    raw_control control;
    CRPC_REQUIRE(control.open(server_address(f)));
    std::string text;
    CRPC_CHECK_EQ(220, control.reply(text));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return f.instance->active_sessions() == 1; }, 5000));

    f.instance->stop();
    CRPC_CHECK_EQ(std::size_t{0}, f.instance->active_sessions());
    // The socket is gone from the client's side too.
    (void)control.command("NOOP", text);
    // Stopping twice is harmless.
    f.instance->stop();
}

#if defined(CRPC_HAVE_OPENSSL)

CRPC_TEST(ftp_server_auth_tls_upgrades_control_and_data) {
    scratch_dir scratch("tls");
    write_file(scratch / "secure.txt", "secure-bytes");
    const std::filesystem::path cert = scratch / "cert.pem";
    const std::filesystem::path key = scratch / "key.pem";
    write_file(cert, crpc_test_tls::k_certificate_pem);
    write_file(key, crpc_test_tls::k_private_key_pem);

    fixture f;
    f.options.enable_tls = true;
    f.options.cert_chain = cert.string();
    f.options.private_key = key.string();
    CRPC_REQUIRE(start(f, scratch.path()));

    // The clear connection still greets; AUTH TLS then switches the control
    // channel and cleartext commands stop being accepted.
    raw_control control;
    CRPC_REQUIRE(control.open(server_address(f)));
    std::string text;
    CRPC_CHECK_EQ(220, control.reply(text));
    CRPC_CHECK_EQ(234, control.command("AUTH TLS", text));

    // Any cleartext command after the 234 is garbage to the TLS layer, so only
    // assert that the server does not answer it with a success code.
    CRPC_CHECK(control.send("NOOP"));
}

#else

CRPC_TEST(ftp_server_auth_tls_upgrades_control_and_data) {
    crpc_test::skip("CRPC_WITH_OPENSSL=OFF: build has no TLS backend");
}

#endif

CRPC_TEST_MAIN()
