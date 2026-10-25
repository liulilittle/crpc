// CRPC -- declarative RPC service definition: `CRPC_RPC_METHOD` + `add_controller`.
//
// The declarative path declares a controller's methods with `CRPC_RPC_METHOD`
// and hands the controller to `builder::add_controller`. The builder then walks
// the static registrar list the macro populates and registers every declared
// method, so the user never writes a service id, a method id or a name by hand.
//
// The checks below cover:
//
//   * the registrar list carries one descriptor per declared method, with the
//     controller's service id, the deterministic method id and the wire name;
//   * two methods in one controller produce two descriptors;
//   * distinct controllers produce distinct service ids;
//   * `add_controller` registers the service itself (id + name) and every
//     declared method end to end -- a real server built from the builder answers
//     a real client call, with no separate `add_service` call.
#include <atomic>
#include <cstddef>
#include <string>

#include <crpc/client_facade.hpp>
#include <crpc/rpc/method_macro.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

using namespace crpc;
using namespace crpc::rpc;

// A controller that declares one method. The method is declared before the
// macro: the macro names `&Ctl::Method`, so the member must exist.
struct echo_controller {
    std::atomic<int> calls{0};

    int echo(const int& value) {
        ++calls;
        return value + 1;
    }

    CRPC_RPC_SERVICE(echo_controller, "echo")
    CRPC_RPC_METHOD(echo_controller, echo, "echo.inc")
};

// Two methods in one controller: the registrar list must hold two descriptors,
// all sharing the controller's service id.
struct pair_controller {
    std::atomic<int> ping_calls{0};
    std::atomic<int> pong_calls{0};

    int ping(const int& value) {
        ++ping_calls;
        return value;
    }
    int pong(const int& value) {
        ++pong_calls;
        return value * 2;
    }

    CRPC_RPC_SERVICE(pair_controller, "pair")
    CRPC_RPC_METHOD(pair_controller, ping, "pair.ping")
    CRPC_RPC_METHOD(pair_controller, pong, "pair.pong")
};

// No `CRPC_RPC_SERVICE`: `add_controller` must still register the service, using
// the type-name fallback.
struct fallback_controller {
    std::atomic<int> calls{0};

    int twice(const int& value) {
        ++calls;
        return value * 2;
    }

    CRPC_RPC_METHOD(fallback_controller, twice, "fallback.twice")
};

} // namespace

// The macro appends one descriptor per declared method to the controller's
// registrar list, carrying the derived service id, the derived method id and
// the declared wire name.
CRPC_TEST(registrar_lists_one_descriptor_per_declared_method) {
    const std::vector<service_registrar<echo_controller>::method_descriptor>& methods =
        service_registrar<echo_controller>::methods();
    CRPC_CHECK_EQ(1u, methods.size());
    if (methods.size() == 1) {
        const auto& descriptor = methods[0];
        CRPC_CHECK_EQ(descriptor.service_id, method_id_from<echo_controller>());
        CRPC_CHECK_EQ(descriptor.method_id,
                      method_id_from_name<echo_controller>("echo.inc"));
        CRPC_CHECK(descriptor.name != nullptr);
        CRPC_CHECK_EQ(std::string(descriptor.name), std::string("echo.inc"));
        CRPC_CHECK(descriptor.register_fn != nullptr);
    }
}

// A controller with two declared methods yields two descriptors, each with its
// own method id and wire name but the same service id.
CRPC_TEST(registrar_supports_multiple_methods_in_one_controller) {
    const std::vector<service_registrar<pair_controller>::method_descriptor>& methods =
        service_registrar<pair_controller>::methods();
    CRPC_CHECK_EQ(2u, methods.size());
    if (methods.size() == 2) {
        CRPC_CHECK_EQ(methods[0].service_id, method_id_from<pair_controller>());
        CRPC_CHECK_EQ(methods[1].service_id, method_id_from<pair_controller>());
        CRPC_CHECK_EQ(methods[0].method_id,
                      method_id_from_name<pair_controller>("pair.ping"));
        CRPC_CHECK_EQ(methods[1].method_id,
                      method_id_from_name<pair_controller>("pair.pong"));
        CRPC_CHECK_EQ(std::string(methods[0].name), std::string("pair.ping"));
        CRPC_CHECK_EQ(std::string(methods[1].name), std::string("pair.pong"));
    }
}

// Distinct controllers derive distinct service ids, so their registrar lists
// never collide.
CRPC_TEST(distinct_controllers_have_distinct_service_ids) {
    CRPC_CHECK(method_id_from<echo_controller>() != method_id_from<pair_controller>());
}

// The method id is derived from the controller type and the wire name, so two
// methods with different names in the same controller get different ids.
CRPC_TEST(method_ids_differ_for_different_names) {
    CRPC_CHECK(method_id_from_name<echo_controller>("echo.inc") !=
               method_id_from_name<echo_controller>("echo.other"));
}

// End to end: a real server built from a builder that used `add_controller`
// answers a real client call on the declared method.
CRPC_TEST(add_controller_registers_declared_methods_end_to_end) {
    inproc_backend backend;
    echo_controller controller;

    server::builder sb;
    sb.add_listener(endpoint::inproc("service-definition-echo"), backend);
    sb.add_controller(controller);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    runtime_options ro;
    ro.start_handler_pool = false;
    runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(endpoint::inproc("service-definition-echo"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(
        method_id_from<echo_controller>(),
        method_id_from_name<echo_controller>("echo.inc"));
    auto started = stub.call(41);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::client::call<int> handle = std::move(started).value();
        const rpc::rpc_result<int> result = handle.get();
        CRPC_CHECK(result.ok());
        if (result.ok()) {
            CRPC_CHECK_EQ(42, result.value());
        }
        CRPC_CHECK_EQ(1, controller.calls.load());
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// `add_controller` registers *every* declared method, not just the first: both
// methods of `pair_controller` are callable.
CRPC_TEST(add_controller_registers_all_declared_methods) {
    inproc_backend backend;
    pair_controller controller;

    server::builder sb;
    sb.add_listener(endpoint::inproc("service-definition-pair"), backend);
    sb.add_controller(controller);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    runtime_options ro;
    ro.start_handler_pool = false;
    runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(endpoint::inproc("service-definition-pair"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    {
        auto stub = cli->service<int, int>(
            method_id_from<pair_controller>(),
            method_id_from_name<pair_controller>("pair.ping"));
        auto started = stub.call(7);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            crpc::client::call<int> handle = std::move(started).value();
            const rpc::rpc_result<int> result = handle.get();
            CRPC_CHECK(result.ok());
            if (result.ok()) {
                CRPC_CHECK_EQ(7, result.value());
            }
        }
    }
    {
        auto stub = cli->service<int, int>(
            method_id_from<pair_controller>(),
            method_id_from_name<pair_controller>("pair.pong"));
        auto started = stub.call(7);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            crpc::client::call<int> handle = std::move(started).value();
            const rpc::rpc_result<int> result = handle.get();
            CRPC_CHECK(result.ok());
            if (result.ok()) {
                CRPC_CHECK_EQ(14, result.value());
            }
        }
    }

    CRPC_CHECK_EQ(1, controller.ping_calls.load());
    CRPC_CHECK_EQ(1, controller.pong_calls.load());

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// The declared service name is what `add_controller` registers.
CRPC_TEST(service_name_comes_from_the_declared_macro) {
    CRPC_CHECK_EQ(std::string(service_name_of<echo_controller>()), std::string("echo"));
    CRPC_CHECK_EQ(std::string(service_name_of<pair_controller>()), std::string("pair"));
}

// Without `CRPC_RPC_SERVICE` the name falls back to the compiler's type-name
// spelling when the translation unit can write `typeid`, and to an empty string
// otherwise (the documented no-RTTI case, e.g. GCC/Clang `-fno-rtti`).
CRPC_TEST(service_name_falls_back_to_the_type_name) {
    const std::string fallback = service_name_of<fallback_controller>();
#if CRPC_RPC_TYPEID_AVAILABLE
    CRPC_CHECK_FALSE(fallback.empty());
#else
    CRPC_CHECK(fallback.empty());
#endif
}

// A controller with no `CRPC_RPC_SERVICE` still registers its service and is
// callable.
CRPC_TEST(add_controller_registers_service_without_declared_name) {
    inproc_backend backend;
    fallback_controller controller;

    server::builder sb;
    sb.add_listener(endpoint::inproc("service-definition-fallback"), backend);
    sb.add_controller(controller);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    runtime_options ro;
    ro.start_handler_pool = false;
    runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(endpoint::inproc("service-definition-fallback"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(
        method_id_from<fallback_controller>(),
        method_id_from_name<fallback_controller>("fallback.twice"));
    auto started = stub.call(21);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::client::call<int> handle = std::move(started).value();
        const rpc::rpc_result<int> result = handle.get();
        CRPC_CHECK(result.ok());
        if (result.ok()) {
            CRPC_CHECK_EQ(42, result.value());
        }
        CRPC_CHECK_EQ(1, controller.calls.load());
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

CRPC_TEST_MAIN()
