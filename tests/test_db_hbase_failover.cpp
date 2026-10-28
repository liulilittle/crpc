// CRPC -- HBase REST gateway failover tests.
//
// These run without a server. A scripted `db::channel` answers each HTTP
// exchange, and the failover state machine is the thing under test: endpoint
// selection, health ejection, cooldown probing and, above all, the
// idempotency rule that a non-idempotent write whose request may have been
// executed is never silently retried on another gateway.
//
// A scripted endpoint answers one HTTP exchange per scripted entry. The
// adapter opens a fresh `http::client` (over a fresh scripted channel) for
// every call and every probe, so scripting one exchange per routed attempt is
// exact. `exchange_script::write_code` models a transport that refused the
// request before any byte could leave ("definitely not executed");
// `exchange_script::read_code` models a connection that broke after the
// request was handed over, optionally after a partial reply ("side effect
// unknown").
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/channel.hpp>
#include <crpc/db/hbase/rest.hpp>
#include <crpc/error.hpp>
#include <crpc/http/client.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::hbase::client;
using crpc::db::hbase::client_options;
using crpc::db::hbase::endpoint;
using crpc::db::hbase::endpoint_condition;

// One HTTP exchange as seen by one scripted channel.
struct exchange_script {
    // Every `write` returns this code. `crpc::rpc_code::ok` lets the bytes
    // through.
    crpc::rpc_code write_code = crpc::rpc_code::ok;
    // Bytes served to the client in place of a real reply.
    std::string reply;
    // When `reply` is exhausted (or empty), every `read_some` fails with this.
    crpc::rpc_code read_code = crpc::rpc_code::ok;
};

// The shared per-endpoint script. The adapter opens one channel per HTTP
// exchange; each channel takes the next scripted entry and commits the bytes
// it wrote to `requests` when it is closed, which is how a test asserts which
// endpoint received what.
class endpoint_script {
public:
    std::deque<exchange_script> exchanges;
    std::string name;
    std::vector<std::string> requests;

    void commit_out(std::vector<std::string>&& written) {
        for (std::string& item : written) {
            requests.push_back(std::move(item));
        }
        written.clear();
    }
};

class scripted_http_channel final : public crpc::db::channel {
public:
    explicit scripted_http_channel(std::shared_ptr<endpoint_script> script)
        : script_(std::move(script)), exchange_() {
        if (script_->exchanges.empty()) {
            exchange_.read_code = crpc::rpc_code::internal;
        } else {
            exchange_ = std::move(script_->exchanges.front());
            script_->exchanges.pop_front();
        }
    }

    ~scripted_http_channel() override { close(); }

    [[nodiscard]] crpc::result<std::vector<std::byte>> read_exact(std::size_t count) override {
        std::vector<std::byte> out;
        out.reserve(count);
        std::byte chunk[256];
        while (out.size() < count) {
            const std::size_t want =
                count - out.size() < sizeof(chunk) ? count - out.size() : sizeof(chunk);
            crpc::result<std::size_t> got =
                read_some(crpc::span<std::byte>(chunk, want));
            if (!got) {
                return got.error();
            }
            out.insert(out.end(), chunk, chunk + got.value());
        }
        return out;
    }

    [[nodiscard]] crpc::result<std::size_t> read_some(crpc::span<std::byte> buffer) override {
        // Serve the queued reply first: a scripted "connection broke mid-reply"
        // is a reply followed by a read failure, not a failure from the first
        // byte.
        if (!exchange_.reply.empty()) {
            const std::size_t take =
                buffer.size() < exchange_.reply.size() ? buffer.size() : exchange_.reply.size();
            std::memcpy(buffer.data(), exchange_.reply.data(), take);
            exchange_.reply.erase(0, take);
            return take;
        }
        if (exchange_.read_code != crpc::rpc_code::ok) {
            return crpc::rpc_error{exchange_.read_code, "scripted read failure"};
        }
        return crpc::rpc_error{crpc::rpc_code::eof, "scripted channel ran dry"};
    }

    crpc::status write(crpc::span<const std::byte> data) override {
        // Record the attempt before failing, so a test sees exactly which
        // exchange reached the endpoint even when the transport refused it.
        written_.emplace_back(reinterpret_cast<const char*>(data.data()), data.size());
        if (exchange_.write_code != crpc::rpc_code::ok) {
            return crpc::status{exchange_.write_code, "scripted write failure"};
        }
        return crpc::status{};
    }

    void close() noexcept override {
        if (closed_) {
            return;
        }
        closed_ = true;
        script_->commit_out(std::move(written_));
    }

private:
    std::shared_ptr<endpoint_script> script_;
    exchange_script exchange_;
    std::vector<std::string> written_;
    bool closed_ = false;
};

// Builds a raw HTTP/1.1 response with an exact Content-Length, so the scripted
// channel never has to simulate a close-delimited body.
[[nodiscard]] std::string http_reply(int status, std::string_view reason,
                                     std::string_view body = {},
                                     std::vector<std::pair<std::string, std::string>> headers =
                                         {}) {
    std::string out = "HTTP/1.1 ";
    out += std::to_string(status);
    out.push_back(' ');
    out += reason;
    out += "\r\n";
    for (const auto& field : headers) {
        out += field.first;
        out += ": ";
        out += field.second;
        out += "\r\n";
    }
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n\r\n";
    out += body;
    return out;
}

[[nodiscard]] std::string version_ok() {
    return http_reply(200, "OK", "gateway-1\n");
}

[[nodiscard]] std::string put_ok() {
    return http_reply(200, "OK");
}

[[nodiscard]] std::string scanner_created(std::string_view id) {
    return http_reply(201, "Created", {}, {{"Location", "/t/scanner/" + std::string(id)}});
}

// A response that announces a body and then dies: the request reached the
// server and was answered, but the reply never completed. Retrying such a
// write on another endpoint must be refused.
[[nodiscard]] exchange_script lost_response() {
    exchange_script script;
    script.reply = "HTTP/1.1 200 OK\r\nContent-Length: 128\r\n\r\n";
    script.read_code = crpc::rpc_code::eof;
    return script;
}

struct rig {
    std::vector<std::shared_ptr<endpoint_script>> endpoints;
    std::unique_ptr<client> instance;

    rig(std::size_t count, std::uint32_t threshold, std::chrono::milliseconds cooldown,
        bool retry_on_uncertain, bool use_zero_arg_opener) {
        client_options options;
        options.failure_threshold = threshold;
        options.retry_cooldown = cooldown;
        options.retry_on_uncertain = retry_on_uncertain;

        const bool multi = count > 1 && !use_zero_arg_opener;
        for (std::size_t i = 0; i < count; ++i) {
            auto script = std::make_shared<endpoint_script>();
            script->name = "gw-" + std::to_string(i);
            endpoints.push_back(script);
            options.endpoints.push_back(endpoint{script->name, 8080});
        }
        const std::vector<std::shared_ptr<endpoint_script>> copies = endpoints;

        if (use_zero_arg_opener) {
            options.endpoints.clear();
            options.host = endpoints.empty() ? std::string() : endpoints[0]->name;
            options.port = 8080;
            const std::string host = options.host;
            const std::uint16_t port = options.port;
            instance.reset(new crpc::db::hbase::client(std::move(options), [copies, host, port] {
                crpc::http::client_options http;
                http.host = host;
                http.port = port;
                return std::unique_ptr<crpc::http::client>(
                    new crpc::http::client(std::move(http),
                                           std::unique_ptr<crpc::db::channel>(
                                               new scripted_http_channel(copies[0]))));
            }));
            return;
        }

        instance.reset(new crpc::db::hbase::client(
            std::move(options), [copies, multi](const endpoint& chosen) {
                crpc::http::client_options http;
                http.host = chosen.host;
                http.port = chosen.port;
                std::shared_ptr<endpoint_script> script = nullptr;
                for (const auto& candidate : copies) {
                    if (candidate->name == chosen.host) {
                        script = candidate;
                        break;
                    }
                }
                if (script == nullptr) {
                    script = std::make_shared<endpoint_script>();
                    script->name = chosen.host;
                    script->exchanges.push_back(
                        exchange_script{crpc::rpc_code::connection_refused, {}, crpc::rpc_code::ok});
                }
                (void)multi;
                return std::unique_ptr<crpc::http::client>(
                    new crpc::http::client(std::move(http),
                                           std::unique_ptr<crpc::db::channel>(
                                               new scripted_http_channel(script))));
            }));
    }

    [[nodiscard]] endpoint_script& at(std::size_t index) { return *endpoints[index]; }

    [[nodiscard]] int requests_to(std::size_t index) const {
        return static_cast<int>(endpoints[index]->requests.size());
    }
};

[[nodiscard]] crpc::db::hbase::row make_row(std::string_view key) {
    crpc::db::hbase::row value;
    value.key.reserve(key.size());
    for (const char c : key) {
        value.key.push_back(static_cast<std::byte>(c));
    }
    crpc::db::hbase::cell cell;
    cell.family = "cf";
    cell.qualifier = "n";
    cell.value.assign(1, std::byte{'v'});
    value.cells.push_back(cell);
    return value;
}

} // namespace

CRPC_TEST(hbase_failover_skips_a_refused_endpoint_to_the_second) {
    rig r(2, 3, std::chrono::seconds(60), false, false);
    r.at(0).exchanges.push_back(
        exchange_script{crpc::rpc_code::connection_refused, {}, crpc::rpc_code::ok});
    r.at(1).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, version_ok(), crpc::rpc_code::ok});

    const auto version = r.instance->version();
    CRPC_CHECK(static_cast<bool>(version));
    if (version) {
        CRPC_CHECK(!version.value().empty());
    }
    // The refused endpoint was tried exactly once and the healthy one served.
    CRPC_CHECK_EQ(1, r.requests_to(0));
    CRPC_CHECK_EQ(1, r.requests_to(1));
    CRPC_CHECK(r.at(1).requests[0].find("GET /version") != std::string::npos);

    const auto health = r.instance->health();
    CRPC_CHECK_EQ(std::size_t{2}, health.size());
    CRPC_CHECK_EQ(static_cast<int>(endpoint_condition::alive),
                  static_cast<int>(health[0].condition));
    CRPC_CHECK_EQ(std::uint32_t{1}, health[0].consecutive_failures);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_refused),
                  static_cast<int>(health[0].last_error));
    CRPC_CHECK_EQ(static_cast<int>(endpoint_condition::alive),
                  static_cast<int>(health[1].condition));
    CRPC_CHECK(health[1].last_success_at.time_since_epoch().count() > 0);
}

CRPC_TEST(hbase_failover_remembers_the_last_successful_endpoint) {
    rig r(2, 3, std::chrono::seconds(60), false, false);
    r.at(0).exchanges.push_back(
        exchange_script{crpc::rpc_code::connection_refused, {}, crpc::rpc_code::ok});
    r.at(1).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, version_ok(), crpc::rpc_code::ok});
    r.at(1).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, version_ok(), crpc::rpc_code::ok});

    CRPC_CHECK(static_cast<bool>(r.instance->version()));
    // The second call starts from the endpoint that just worked: endpoint 0,
    // though by now healthy, is not asked again.
    CRPC_CHECK(static_cast<bool>(r.instance->version()));
    CRPC_CHECK_EQ(1, r.requests_to(0));
    CRPC_CHECK_EQ(2, r.requests_to(1));
}

CRPC_TEST(hbase_failover_ejects_an_endpoint_after_the_threshold) {
    rig r(2, 1, std::chrono::seconds(60), false, false);
    for (int i = 0; i < 4; ++i) {
        r.at(0).exchanges.push_back(
            exchange_script{crpc::rpc_code::connection_refused, {}, crpc::rpc_code::ok});
        r.at(1).exchanges.push_back(
            exchange_script{crpc::rpc_code::ok, version_ok(), crpc::rpc_code::ok});
    }

    CRPC_CHECK(static_cast<bool>(r.instance->version()));
    auto health = r.instance->health();
    CRPC_CHECK_EQ(static_cast<int>(endpoint_condition::dead),
                  static_cast<int>(health[0].condition));
    CRPC_CHECK(health[0].consecutive_failures >= 1);
    CRPC_CHECK(health[0].retry_in.count() > 0);

    // A dead endpoint is skipped entirely while the cooldown runs.
    CRPC_CHECK(static_cast<bool>(r.instance->version()));
    CRPC_CHECK_EQ(1, r.requests_to(0));
    CRPC_CHECK_EQ(2, r.requests_to(1));
}

CRPC_TEST(hbase_failover_probes_a_recovered_endpoint_after_the_cooldown) {
    rig r(2, 1, std::chrono::milliseconds(0), false, false);
    // One refused exchange; the first call ejects endpoint 0.
    r.at(0).exchanges.push_back(
        exchange_script{crpc::rpc_code::connection_refused, {}, crpc::rpc_code::ok});
    for (int i = 0; i < 4; ++i) {
        r.at(1).exchanges.push_back(
            exchange_script{crpc::rpc_code::ok, version_ok(), crpc::rpc_code::ok});
    }

    CRPC_CHECK(static_cast<bool>(r.instance->version()));
    CRPC_CHECK_EQ(static_cast<int>(endpoint_condition::dead),
                  static_cast<int>(r.instance->health()[0].condition));

    // Endpoint 0 is back. The next public call runs the recovery probe
    // (`GET /version`) against it; the probe reply re-admits it.
    r.at(0).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, version_ok(), crpc::rpc_code::ok});
    CRPC_CHECK(static_cast<bool>(r.instance->version()));
    const auto health = r.instance->health();
    CRPC_CHECK_EQ(static_cast<int>(endpoint_condition::alive),
                  static_cast<int>(health[0].condition));
    CRPC_CHECK_EQ(std::uint32_t{0}, health[0].consecutive_failures);
    // The failed exchange plus the probe; the probe is `GET /version`.
    CRPC_CHECK_EQ(2, r.requests_to(0));
    CRPC_CHECK(r.at(0).requests[1].find("GET /version") != std::string::npos);
}

CRPC_TEST(hbase_failover_a_failed_probe_keeps_the_endpoint_dead) {
    rig r(2, 1, std::chrono::milliseconds(0), false, false);
    for (int i = 0; i < 3; ++i) {
        r.at(0).exchanges.push_back(
            exchange_script{crpc::rpc_code::connection_refused, {}, crpc::rpc_code::ok});
        r.at(1).exchanges.push_back(
            exchange_script{crpc::rpc_code::ok, version_ok(), crpc::rpc_code::ok});
    }
    CRPC_CHECK(static_cast<bool>(r.instance->version()));
    CRPC_CHECK_EQ(static_cast<int>(endpoint_condition::dead),
                  static_cast<int>(r.instance->health()[0].condition));

    // The probe reaches the endpoint (its write succeeds) but the reply fails
    // at the transport level: the endpoint stays dead and the cooldown
    // restarts, so it is not probed again on the next call.
    r.at(0).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, {}, crpc::rpc_code::connection_reset});
    CRPC_CHECK(static_cast<bool>(r.instance->version()));
    const auto health = r.instance->health();
    CRPC_CHECK_EQ(static_cast<int>(endpoint_condition::dead),
                  static_cast<int>(health[0].condition));
    CRPC_CHECK(health[0].consecutive_failures >= 2);
    // One refused exchange plus exactly one probe attempt.
    CRPC_CHECK_EQ(2, r.requests_to(0));
}

CRPC_TEST(hbase_failover_no_live_endpoint_reports_unavailable) {
    rig r(2, 1, std::chrono::seconds(60), false, false);
    for (int i = 0; i < 2; ++i) {
        r.at(0).exchanges.push_back(
            exchange_script{crpc::rpc_code::connection_refused, {}, crpc::rpc_code::ok});
        r.at(1).exchanges.push_back(
            exchange_script{crpc::rpc_code::connection_refused, {}, crpc::rpc_code::ok});
    }

    const auto version = r.instance->version();
    CRPC_CHECK_FALSE(static_cast<bool>(version));
    if (!version) {
        // Both endpoints were live when the call started; each attempt reports
        // its own transport failure, so the last one is what comes back.
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_refused),
                      static_cast<int>(version.error().code));
    }
    const auto health = r.instance->health();
    CRPC_CHECK_EQ(std::size_t{2}, health.size());
    CRPC_CHECK_EQ(static_cast<int>(endpoint_condition::dead),
                  static_cast<int>(health[0].condition));
    CRPC_CHECK_EQ(static_cast<int>(endpoint_condition::dead),
                  static_cast<int>(health[1].condition));

    // With every endpoint ejected and the cooldown running there is no live
    // candidate and nothing to probe, so the call must not touch the network
    // and reports that no gateway can serve it.
    const int before = r.requests_to(0) + r.requests_to(1);
    const auto again = r.instance->version();
    CRPC_CHECK_FALSE(static_cast<bool>(again));
    if (!again) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                      static_cast<int>(again.error().code));
    }
    CRPC_CHECK_EQ(before, r.requests_to(0) + r.requests_to(1));
}

CRPC_TEST(hbase_failover_an_empty_endpoint_list_is_a_precondition) {
    rig r(0, 3, std::chrono::seconds(60), false, false);
    const auto version = r.instance->version();
    CRPC_CHECK_FALSE(static_cast<bool>(version));
    if (!version) {
        // No gateway at all is a configuration error, reported before any
        // network attempt.
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(version.error().code));
    }
}

CRPC_TEST(hbase_failover_single_endpoint_with_host_and_port_still_works) {
    rig r(1, 3, std::chrono::seconds(60), false, true);
    r.at(0).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, version_ok(), crpc::rpc_code::ok});
    const auto version = r.instance->version();
    CRPC_CHECK(static_cast<bool>(version));
    if (version) {
        CRPC_CHECK(!version.value().empty());
    }
    const auto health = r.instance->health();
    CRPC_CHECK_EQ(std::size_t{1}, health.size());
    CRPC_CHECK_EQ(static_cast<int>(endpoint_condition::alive),
                  static_cast<int>(health[0].condition));
    // The zero-argument opener is what an existing caller keeps; one endpoint
    // is served exactly like before.
    CRPC_CHECK_EQ(1, r.requests_to(0));
}

CRPC_TEST(hbase_failover_multi_endpoint_refuses_a_zero_arg_opener) {
    // A multi-endpoint list given to the zero-argument opener cannot address
    // the extra gateways: the adapter must refuse, not silently reuse one
    // client for every endpoint.
    client_options options;
    options.failure_threshold = 3;
    options.endpoints = {endpoint{"gw-0", 8080}, endpoint{"gw-1", 8080}};
    auto script = std::make_shared<endpoint_script>();
    script->name = "gw-0";
    auto cl = std::unique_ptr<crpc::db::hbase::client>(new crpc::db::hbase::client(
        std::move(options), [script] {
            return std::unique_ptr<crpc::http::client>(new crpc::http::client(
                crpc::http::client_options{},
                std::unique_ptr<crpc::db::channel>(new scripted_http_channel(script))));
        }));
    const auto version = cl->version();
    CRPC_CHECK_FALSE(static_cast<bool>(version));
    if (!version) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(version.error().code));
    }
    CRPC_CHECK_EQ(0, static_cast<int>(script->requests.size()));
}

CRPC_TEST(hbase_failover_put_reports_unknown_side_effect_when_the_reply_is_lost) {
    rig r(2, 3, std::chrono::seconds(60), false, false);
    // The write request is fully sent, the gateway answers, and the reply dies
    // mid-body: the write may have been applied. Retrying it on the second
    // endpoint could double the row, so the call must stop here with an
    // explicit "unknown side effect" and must not touch the second endpoint.
    r.at(0).exchanges.push_back(lost_response());
    r.at(1).exchanges.push_back(exchange_script{crpc::rpc_code::ok, put_ok(), crpc::rpc_code::ok});

    const crpc::status wrote = r.instance->put_row("t", make_row("r1"));
    CRPC_CHECK_FALSE(static_cast<bool>(wrote));
    if (!wrote) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unknown_side_effect),
                      static_cast<int>(wrote.error().code));
        CRPC_CHECK(wrote.error().message.find("unknown") != std::string::npos ||
                   wrote.error().message.find("may have") != std::string::npos);
    }
    // The second gateway was never consulted for the write.
    CRPC_CHECK_EQ(0, r.requests_to(1));
    const auto health = r.instance->health();
    CRPC_CHECK_EQ(std::uint32_t{1}, health[0].consecutive_failures);
    CRPC_CHECK_EQ(static_cast<int>(endpoint_condition::alive),
                  static_cast<int>(health[0].condition));
}

CRPC_TEST(hbase_failover_put_without_side_effect_retries_a_safe_write) {
    rig r(2, 3, std::chrono::seconds(60), false, false);
    // The connection was refused: no byte left the client, so the write cannot
    // have been applied anywhere. Moving it to the second endpoint is safe.
    r.at(0).exchanges.push_back(
        exchange_script{crpc::rpc_code::connection_refused, {}, crpc::rpc_code::ok});
    r.at(1).exchanges.push_back(exchange_script{crpc::rpc_code::ok, put_ok(), crpc::rpc_code::ok});

    const crpc::status wrote = r.instance->put_row("t", make_row("r1"));
    CRPC_CHECK(static_cast<bool>(wrote));
    if (wrote) {
        CRPC_CHECK_EQ(1, r.requests_to(0));
        // The PUT carries a head and a body: two write calls on the live one.
        CRPC_CHECK_EQ(2, r.requests_to(1));
    }
}

CRPC_TEST(hbase_failover_retry_on_uncertain_replays_a_lost_write) {
    rig r(2, 3, std::chrono::seconds(60), true, false);
    r.at(0).exchanges.push_back(lost_response());
    r.at(1).exchanges.push_back(exchange_script{crpc::rpc_code::ok, put_ok(), crpc::rpc_code::ok});

    // The caller opted into the at-most-once risk: the lost write is replayed
    // on the second endpoint and succeeds there. A PUT carries a head and a
    // body, so each successful exchange is two write calls.
    const crpc::status wrote = r.instance->put_row("t", make_row("r1"));
    CRPC_CHECK(static_cast<bool>(wrote));
    if (wrote) {
        CRPC_CHECK_EQ(2, r.requests_to(0));
        CRPC_CHECK_EQ(2, r.requests_to(1));
    }
}

CRPC_TEST(hbase_failover_an_http_status_reply_is_definitive_not_a_retry) {
    rig r(2, 3, std::chrono::seconds(60), false, false);
    // The gateway answers 503: that is a reply, the endpoint is reachable, and
    // another gateway reflects the same cluster. Failover cannot improve this,
    // so the second endpoint is not consulted and the endpoint is not marked
    // failing.
    r.at(0).exchanges.push_back(exchange_script{
        crpc::rpc_code::ok, http_reply(503, "Service Unavailable"), crpc::rpc_code::ok});

    const auto version = r.instance->version();
    CRPC_CHECK_FALSE(static_cast<bool>(version));
    if (!version) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                      static_cast<int>(version.error().code));
    }
    CRPC_CHECK_EQ(0, r.requests_to(1));
    const auto health = r.instance->health();
    CRPC_CHECK_EQ(std::uint32_t{0}, health[0].consecutive_failures);
    CRPC_CHECK_EQ(static_cast<int>(endpoint_condition::alive),
                  static_cast<int>(health[0].condition));
    CRPC_CHECK(health[0].last_success_at.time_since_epoch().count() > 0);
}

CRPC_TEST(hbase_failover_a_lost_scan_restarts_on_the_next_endpoint) {
    rig r(2, 3, std::chrono::seconds(60), false, false);
    // Endpoint 0: the scanner opens, but the first drain read dies at the
    // transport level. The scanner is host-local, so the only correct recovery
    // is to discard the partial rows and run a fresh scan on the next gateway.
    r.at(0).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, scanner_created("s0"), crpc::rpc_code::ok});
    r.at(0).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, {}, crpc::rpc_code::connection_reset});
    r.at(0).exchanges.push_back(exchange_script{crpc::rpc_code::ok, http_reply(200, "OK"),
                                                crpc::rpc_code::ok});

    // Endpoint 1: put scanner (head + body), two gets; the 204 completes the
    // drain so no explicit release is needed.
    const std::string rows = "{\"Row\":[{\"key\":\"azAw\",\"Cell\":[]},"
                             "{\"key\":\"azAx\",\"Cell\":[]}]}";
    r.at(1).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, scanner_created("s1"), crpc::rpc_code::ok});
    r.at(1).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, http_reply(200, "OK", rows), crpc::rpc_code::ok});
    r.at(1).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, http_reply(204, "No Content"), crpc::rpc_code::ok});

    const auto scanned = r.instance->scan("t", {}, 0);
    CRPC_CHECK(static_cast<bool>(scanned));
    if (scanned) {
        CRPC_CHECK_EQ(std::size_t{2}, scanned.value().size());
    }
    // Endpoint 0: put scanner (head + body), failed drain, best-effort release.
    CRPC_CHECK_EQ(4, r.requests_to(0));
    // Endpoint 1: put scanner (head + body), two gets.
    CRPC_CHECK_EQ(4, r.requests_to(1));
}

CRPC_TEST(hbase_failover_a_scanner_http_error_stays_put) {
    rig r(2, 3, std::chrono::seconds(60), false, false);
    // The gateway opens the scanner and then answers the drain with 404: a
    // definitive reply from this gateway, not a transport failure. The call
    // reports it and does not start a second scan on the other gateway.
    r.at(0).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, scanner_created("s0"), crpc::rpc_code::ok});
    r.at(0).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, http_reply(404, "Not Found"), crpc::rpc_code::ok});
    r.at(0).exchanges.push_back(exchange_script{crpc::rpc_code::ok, http_reply(200, "OK"),
                                                crpc::rpc_code::ok});

    r.at(1).exchanges.push_back(
        exchange_script{crpc::rpc_code::ok, version_ok(), crpc::rpc_code::ok});

    const auto scanned = r.instance->scan("t", {}, 0);
    CRPC_CHECK_FALSE(static_cast<bool>(scanned));
    if (!scanned) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                      static_cast<int>(scanned.error().code));
    }
    CRPC_CHECK_EQ(0, r.requests_to(1));
    // The 404 also proves the endpoint answered, so it is not marked failing.
    const auto health = r.instance->health();
    CRPC_CHECK_EQ(std::uint32_t{0}, health[0].consecutive_failures);
}

CRPC_TEST_MAIN()