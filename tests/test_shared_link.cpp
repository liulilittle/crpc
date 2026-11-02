// CRPC -- shared-build link gate.
//
// This translation unit is a *consumer*: it includes only public headers and
// calls the public API the way the README and the other tests do. With
// CRPC_BUILD_SHARED=ON it is linked against the import library, so any public
// symbol that the shared build fails to export becomes an unresolved external
// here -- a build failure instead of a downstream report.
//
// Two cases exist specifically because they cannot be caught by a hand-written
// list of "the API a consumer calls":
//
//   1. `wake_functor<Resp>` is a header-inline template. Every consumer that
//      instantiates `client::call<Resp>::get()` compiles `core->signal()` into
//      its own object file, so `call_core::signal` must be exported even though
//      no consumer ever names `call_core`.
//   2. `mvc::bind_request<Req>` is a header-inline template whose body calls
//      `route_params::find`, `header_list::find` and `header_list::add`. A
//      consumer that only registers a controller still emits those calls.
//
// Both are exercised directly below. Removing the export decoration from either
// `call_core` or the MVC helpers makes this test fail to link.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <crpc/mvc/attributes.hpp>

// RTTI's per-field attribute tables are dynamic initializers on static inline
// data members; test_framework.hpp's Debug-only `.CRT$XCU` pragma makes those
// inert. Undefine _DEBUG around the harness include, exactly as test_mvc.cpp
// and test_codec.cpp do, so the STL headers above fix the iterator level first.
#undef _DEBUG
#include "test_framework.hpp"

#include <crpc/client_facade.hpp>
#include <crpc/codec.hpp>
#include <crpc/flow/estimator.hpp>
#include <crpc/frame.hpp>
#include <crpc/http/content_coding.hpp>
#include <crpc/http/file_transfer.hpp>
#include <crpc/http/message.hpp>
#include <crpc/http/multipart.hpp>
#include <crpc/http/server.hpp>
#include <crpc/http2/connection.hpp>
#include <crpc/io.hpp>
#include <crpc/io/tls.hpp>
#include <crpc/mvc/binding.hpp>
#include <crpc/mvc/router.hpp>
#include <crpc/observability/context.hpp>
#include <crpc/observability/log.hpp>
#include <crpc/observability/tracer.hpp>
#include <crpc/push/hub.hpp>
#include <crpc/push/stream.hpp>
#include <crpc/resilience/bulkhead.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/load_balancer.hpp>
#include <crpc/resilience/rate_limiter.hpp>
#include <crpc/resilience/retry.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>
#include <crpc/span.hpp>

using namespace std::chrono_literals;

namespace {

constexpr crpc::u64 kService = 7;
constexpr crpc::u64 kEcho = 1;

struct echo_service {
    int echo(const int& value) { return value + 1; }
};

struct empty_backend final : crpc::backend_factory {
    std::unique_ptr<crpc::connection> connect(const crpc::endpoint&,
                                              const crpc::connect_options&) override {
        return nullptr;
    }
    std::unique_ptr<crpc::listener> listen(const crpc::endpoint&,
                                          const crpc::listen_options&) override {
        return nullptr;
    }
};

struct one_instance_set final : crpc::resilience::instance_set {
    crpc::resilience::instance value{};
    [[nodiscard]] std::size_t size() const noexcept override { return 1; }
    [[nodiscard]] crpc::resilience::instance at(std::size_t) const noexcept override {
        return value;
    }
    [[nodiscard]] crpc::u64 version() const noexcept override { return 1; }
};

struct null_multipart_handler final : crpc::http::multipart_handler {
    crpc::status on_part_begin(const crpc::http::multipart_part_info&) override {
        return {};
    }
    crpc::status on_part_data(crpc::span<const std::byte>) override { return {}; }
    crpc::status on_part_end() override { return {}; }
};

} // namespace

// A reflected DTO whose fields come from every source bind_fields consults:
// a route parameter, a query parameter, a header and a form field. It lives at
// namespace scope (not in the anonymous namespace above) because RTTI's
// registration macros emit namespace-scope definitions.
//
// Guarded, because `RTTI_DEFINE_TYPE` needs the reflection dependency. The rest
// of this file must build without it: this target is the only runtime check
// that a public symbol really is in the export table, so losing it in a
// no-RTTI shared build would leave that build with no export gate at all.
#if CRPC_HAVE_RTTI
struct link_req {
    std::int64_t id = 0;
    std::int64_t page = 0;
    std::string token;
    std::string title;
};
RTTI_DEFINE_TYPE(link_req, id, page, token, title)
RTTI_DEFINE_FIELD_ATTRS(link_req,
                        RTTI_FIELD_ATTR_ENTRY(id, CRPC_FROM_ROUTE("id")),
                        RTTI_FIELD_ATTR_ENTRY(page, CRPC_FROM_QUERY("page")),
                        RTTI_FIELD_ATTR_ENTRY(token, CRPC_FROM_HEADER("x-token")),
                        RTTI_FIELD_ATTR_ENTRY(title, CRPC_FROM_FORM("title")))
#endif // CRPC_HAVE_RTTI

// Case 1: the whole documented builder/transport/resilience surface, called
// directly (never via sizeof or an address-of, which a vtable could satisfy).
CRPC_TEST(shared_link_public_surface) {
    empty_backend backend;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("link"), backend);
    sb.add_service(kService, "svc");
    sb.shards(1);
    sb.resilience_enabled(true);
    std::unique_ptr<crpc::server> srv = sb.build();
    CRPC_CHECK(srv != nullptr);

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("link"), backend);
    cb.pool_size(1);
    cb.default_deadline(std::chrono::seconds(1));
    std::unique_ptr<crpc::client> cli = cb.build();
    CRPC_CHECK(cli != nullptr);

    crpc::http::http_server::builder hb;
    hb.add_listener(crpc::endpoint::inproc("http"), backend);
    hb.shards(1);
    std::unique_ptr<crpc::http::http_server> hsrv = hb.build();
    CRPC_CHECK(hsrv != nullptr);

    const crpc::endpoint tcp = crpc::endpoint::tcp("127.0.0.1", 9000);
    const crpc::endpoint ip = crpc::endpoint::inproc("link");
    CRPC_CHECK(tcp.valid());
    CRPC_CHECK(ip.valid());

    crpc::inproc_backend ib;
    CRPC_CHECK(ib.connect(ip, crpc::connect_options{}) == nullptr);
    // `inproc_backend::listen` binds the name and returns a live listener.
    auto listener = ib.listen(ip, crpc::listen_options{});
    CRPC_CHECK(listener != nullptr);
    auto pair = crpc::inproc_pair::make({});
    CRPC_CHECK(pair.first != nullptr);
    CRPC_CHECK(pair.second != nullptr);

    crpc::resilience::rate_limit_config rcfg;
    rcfg.rate_per_second = 1000.0;
    rcfg.burst = 10.0;
    crpc::resilience::rate_limiter limiter(rcfg);
    CRPC_CHECK(limiter.try_acquire(1).ok());
    CRPC_CHECK(limiter.acquire_for(1, std::chrono::nanoseconds(1000000)).ok());
    CRPC_CHECK(limiter.tokens() >= 0.0);

    crpc::resilience::bulkhead_config bcfg;
    bcfg.capacity = 1;
    crpc::resilience::bulkhead bh(bcfg);
    {
        crpc::resilience::bulkhead_permit permit = bh.try_enter();
        CRPC_CHECK(permit.allowed());
    }
    bh.leave();

    crpc::resilience::breaker_config brcfg;
    crpc::resilience::circuit_breaker breaker(brcfg);
    {
        crpc::resilience::breaker_permit permit = breaker.allow();
        CRPC_CHECK(permit.allowed());
        permit.success();
    }
    CRPC_CHECK(breaker.state() == crpc::resilience::breaker_state::closed);
    CRPC_CHECK(breaker.stats().success == 1);

    one_instance_set instances;
    crpc::resilience::load_balancer lb;
    auto picked = lb.select(instances);
    CRPC_CHECK(picked.ok());

    crpc::resilience::retry_budget_config rbc;
    crpc::resilience::retry_budget budget(rbc);
    budget.on_request();
    // The default ratio is 0.1 token per logical call, so one deposit cannot
    // fund a retry; deposit enough calls first.
    for (int i = 0; i < 20; ++i) {
        budget.on_request();
    }
    CRPC_CHECK(budget.try_acquire());
    CRPC_CHECK(budget.tokens() >= 0.0);

    crpc::resilience::retry_policy rpolicy;
    crpc::resilience::retry_sequence sequence(rpolicy, &budget);
    std::chrono::nanoseconds delay{0};
    CRPC_CHECK(sequence.next(crpc::rpc_code::unavailable, true, delay));

    crpc::push::hub hub;
    CRPC_CHECK(hub.owns_topic("topic"));
    (void)hub.stats();
    crpc::push::stream_state ss(1, crpc::push::stream_shape::bidirectional);
    CRPC_CHECK(ss.can_read());
    CRPC_CHECK(ss.can_write());

    crpc::mvc::router router;
    CRPC_CHECK(router.freeze().ok());
    crpc::mvc::route_params params;
    params.entries.emplace_back("k", "v");
    CRPC_CHECK(params.find("k") != nullptr);

    // Free functions of the MVC surface. `default_router` and `parse_query`
    // are README-level entry points, and both are defined out of line, so a
    // shared build must export them.
    CRPC_CHECK(crpc::mvc::default_router().frozen());
    crpc::mvc::route_params parsed_query;
    CRPC_CHECK(crpc::mvc::parse_query("a=1", parsed_query, 8).ok());
    CRPC_CHECK(parsed_query.find("a") != nullptr);
    CRPC_CHECK_EQ(std::string_view("text/plain"),
                  crpc::mvc::media_type("text/plain; charset=utf-8"));
    crpc::mvc::route_params parsed_form;
    CRPC_CHECK(crpc::mvc::parse_form("b=2", parsed_form, 8).ok());
    CRPC_CHECK(parsed_form.find("b") != nullptr);

    crpc::obs::baggage bag;
    CRPC_CHECK(bag.set("k", "v"));
    CRPC_CHECK(bag.get("k") != nullptr);
    bag.clear();
    CRPC_CHECK(bag.empty());
    CRPC_CHECK(crpc::obs::logger::instance().dropped_total() == 0);
    crpc::obs::tracer& tracer = crpc::obs::tracer::instance();
    tracer.configure(crpc::obs::tracer_config{});
    CRPC_CHECK(crpc::obs::tracer::sample_trace(crpc::obs::trace_id{}, 1.0));
    {
        crpc::obs::log_event event(crpc::obs::log_level::info, "link gate");
        event.field("k", "v");
    }
    // Free functions of the observability surface.
    crpc::obs::log_record record = crpc::obs::make_record(
        crpc::obs::log_level::info, "record");
    CRPC_CHECK(crpc::obs::record_add_field(record, "k", "v"));
    CRPC_CHECK_EQ(std::string_view("ok"),
                  std::string_view(crpc::obs::to_string(
                      crpc::obs::traceparent_status::ok)));

    crpc::http::header_list headers;
    headers.add("k", "v");
    CRPC_CHECK_EQ(std::size_t{1}, headers.count("k"));
    CRPC_CHECK(headers.find("k") != nullptr);

    // Free functions of the HTTP/1.1 serializer surface.
    crpc::http::request serialized_request;
    serialized_request.method = crpc::http::method::get;
    serialized_request.target = "/";
    serialized_request.headers.add("host", "example");
    std::string wire;
    CRPC_CHECK(crpc::http::serialize(serialized_request, wire).ok());
    CRPC_CHECK(wire.find("GET /") == 0);
    crpc::http::response serialized_response;
    serialized_response.status_code = 204;
    serialized_response.reason = "No Content";
    wire.clear();
    CRPC_CHECK(crpc::http::serialize(serialized_response, wire).ok());
    // The status line, not the digits anywhere in the message: a 204 carries no
    // body, but the serializer could still print "1204" somewhere and satisfy a
    // bare substring test.
    CRPC_CHECK(wire.find("HTTP/1.1 204") != std::string::npos);

    std::string out;
    crpc::http::chunked_encoder encoder;
    CRPC_CHECK(encoder.finish(out).ok());
    crpc::http::chunked_decoder decoder;
    decoder.reset();
    crpc::http::http_parser parser(crpc::http::http_parser::kind::request,
                                   crpc::http::parser_limits{});
    CRPC_CHECK(parser.current_status() == crpc::http::parse_status::need_more);
    null_multipart_handler handler;
    crpc::http::multipart_parser multipart(std::string("boundary"), handler,
                                           crpc::http::multipart_limits{});
    CRPC_CHECK(!multipart.finish().ok());

    const crpc::http::negotiation negotiated =
        crpc::http::negotiate_response("gzip");
    CRPC_CHECK(negotiated.compressed);
    CRPC_CHECK(crpc::http::should_compress(negotiated, 4096));
    CRPC_CHECK(crpc::http::plan_request_decoding("gzip").ok);
    CRPC_CHECK(crpc::http::is_token("GET"));
    CRPC_CHECK_EQ(std::string("GET"),
                  std::string(crpc::http::to_token(crpc::http::method::get)));
    CRPC_CHECK(crpc::http::percent_decode("%20", out).ok());
    CRPC_CHECK_EQ(std::string(" "), out);
    crpc::http::content_decoder cdec;
    out.clear();
    CRPC_CHECK(cdec.finish(out).ok());
    crpc::http::content_encoder cenc;
    out.clear();
    CRPC_CHECK(cenc.start(out).ok());
    CRPC_CHECK(cenc.finish(out).ok());
    crpc::http::file_part part;
    // An in-memory part has no temp file, so `exists()` reports true.
    CRPC_CHECK(part.exists());
    crpc::http::download_source source;
    CRPC_CHECK(source.size() == 0);
    crpc::http::upload_sink sink("k", "f", "text/plain");
    CRPC_CHECK(sink.size() == 0);

    crpc::http2::connection h2(crpc::http2::connection_options{});
    CRPC_CHECK_EQ(std::size_t{0}, h2.active_stream_count());

    std::error_code tls_ec;
    CRPC_CHECK(crpc::io::tls_context::create(crpc::io::tls_config{}, tls_ec) ==
               nullptr);

    crpc::flow::rtt_estimator rtt;
    CRPC_CHECK(rtt.baseline_ns() > 0);
    crpc::flow::pacer pacer;
    pacer.reset(0);
    // reset() refills one burst of credit at the configured initial rate.
    CRPC_CHECK(pacer.available() > 0);
    crpc::flow::congestion_window window;
    window.on_congestion();
    CRPC_CHECK(window.bytes() > 0);

    CRPC_CHECK(crpc::default_codec_registry().size() > 0);

    crpc::frame_decoder fd(65536);
    CRPC_CHECK_EQ(std::size_t{0}, fd.buffered());

    crpc::runtime rt(crpc::runtime_options{});
    CRPC_CHECK(!rt.running());

    crpc::rpc::service_registry registry;
    CRPC_CHECK(registry.freeze().ok());
    crpc::rpc::call_slot_pool pool(4);
    CRPC_CHECK_EQ(crpc::u32{4}, pool.capacity());

    crpc::span<const std::byte> view;
    CRPC_CHECK(view.empty());
}

// Case 2: `wake_functor<Resp>` instantiated in this consumer translation unit.
// Its body calls `call_core::signal()`; the symbol must therefore be exported
// even though this file never names `call_core` as an API it uses.
CRPC_TEST(shared_link_header_template_calls_core) {
    crpc::client_detail::call_core core;
    crpc::client_detail::wake_functor<int> wake{&core};
    crpc::rpc::call<int> handle;
    wake(handle);
    CRPC_CHECK(core.done.load(std::memory_order_acquire));
    CRPC_CHECK(!handle.valid());
}

// Case 3: `mvc::bind_request<Req>` instantiated here. The template body calls
// `route_params::find`, `header_list::find` and (through the response path)
// `header_list::add`, so those symbols must be exported too.
CRPC_TEST(shared_link_header_template_binds_request) {
#if !CRPC_HAVE_RTTI
    // `bind_request` needs the reflection dependency for the DTO. Case 1 has
    // already exercised `route_params::find`, `header_list::find` and
    // `header_list::add` directly, so the export check for those symbols is
    // not lost; only this reflection-driven path is.
    ::crpc_test::skip("bind_request requires CRPC_HAVE_RTTI");
#else
    crpc::mvc::route_params route;
    route.entries.emplace_back("id", "42");

    crpc::http::request request;
    request.query = "page=3";
    request.body = "title=hello";
    request.headers.add("content-type", "application/x-www-form-urlencoded");
    request.headers.add("x-token", "secret");

    link_req out;
    const crpc::status bound = crpc::mvc::bind_request<link_req>(
        request, route, crpc::default_codec_registry(),
        crpc::mvc::default_binding_limits(), out);
    CRPC_CHECK(bound.ok());
    CRPC_CHECK_EQ(std::int64_t{42}, out.id);
    CRPC_CHECK_EQ(std::int64_t{3}, out.page);
    CRPC_CHECK_EQ(std::string("secret"), out.token);
    CRPC_CHECK_EQ(std::string("hello"), out.title);
#endif
}

// Case 4: a full call through the typed facade. `stub::call` -> `issue` ->
// `then(wake_functor<Resp>)` runs the same header-inline path as case 2, but
// this time with a live server, which is what a real consumer does.
CRPC_TEST(shared_link_end_to_end_call) {
    echo_service service;
    crpc::inproc_backend backend;
    const std::string name = "crpc-shared-link";

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc(name), backend);
    sb.add_service(kService, "svc");
    sb.add_method<echo_service, &echo_service::echo>(kService, kEcho, "echo",
                                                     service);
    sb.shards(1);
    std::unique_ptr<crpc::server> srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc(name), backend);
    cb.pool_size(1);
    cb.default_deadline(std::chrono::seconds(5));
    std::unique_ptr<crpc::client> cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        srv->stop();
        return;
    }

    auto stub = cli->service<int, int>(kService, kEcho);
    auto started = stub.call(41);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        auto handle = std::move(started).value();
        const crpc::rpc::rpc_result<int> result = handle.get();
        CRPC_CHECK(result.ok());
        if (result.ok()) {
            CRPC_CHECK_EQ(42, result.value());
        }
    }

    cli->close();
    srv->stop();
}

CRPC_TEST_MAIN()
