// CRPC -- platform loopback transport for tests that need a real socket.
//
// The db/mq/redis/http fixtures hold the backend as a value member and hand a
// `backend_factory&` to their factories. On Windows that is `iocp_backend`;
// on Linux it is `epoll_backend`, which is only constructible through its
// fallible `create()` factory, so the alias there wraps the created instance.
// Both spell it `tcp_backend` so the fixture code stays platform-free.
#ifndef CRPC_TEST_TCP_BACKEND_HPP
#define CRPC_TEST_TCP_BACKEND_HPP

#if defined(_WIN32)

#include <memory>

#include <crpc/error.hpp>
#include <crpc/io/iocp_backend.hpp>

using tcp_backend = crpc::iocp_backend;

// The Linux class below has a static `create()` because the epoll backend is
// only constructible through its fallible factory. A type alias cannot carry a
// static member, so on Windows this free function provides the same spelling:
// shared test code calls `tcp_backend_create()` and gets an owning pointer (a
// `result`, since the epoll side can fail) on both platforms.
[[nodiscard]] inline crpc::result<std::unique_ptr<tcp_backend>> tcp_backend_create() {
    return std::make_unique<tcp_backend>();
}

#else

#include <memory>

#include <crpc/error.hpp>
#include <crpc/io/epoll_backend.hpp>

class tcp_backend final : public crpc::backend_factory {
public:
    // `create()` returns an owning pointer because the epoll backend's own
    // construction is fallible: a caller that cannot tolerate a null backend
    // (the registry tests) gets the failure instead of a silent no-op.
    [[nodiscard]] static crpc::result<std::unique_ptr<tcp_backend>> create() {
        crpc::result<std::unique_ptr<crpc::io::epoll_backend>> created =
            crpc::io::epoll_backend::create();
        if (!created.ok()) {
            return created.error();
        }
        auto wrapped = std::unique_ptr<tcp_backend>(new tcp_backend());
        wrapped->impl_ = std::move(created).value();
        return wrapped;
    }

    tcp_backend() {
        crpc::result<std::unique_ptr<crpc::io::epoll_backend>> created =
            crpc::io::epoll_backend::create();
        if (created.ok()) {
            impl_ = std::move(created).value();
        }
    }

    std::unique_ptr<crpc::connection> connect(const crpc::endpoint& ep,
                                              const crpc::connect_options& opt) override {
        return impl_ != nullptr ? impl_->connect(ep, opt) : nullptr;
    }

    std::unique_ptr<crpc::listener> listen(const crpc::endpoint& ep,
                                           const crpc::listen_options& opt) override {
        return impl_ != nullptr ? impl_->listen(ep, opt) : nullptr;
    }

private:
    std::unique_ptr<crpc::io::epoll_backend> impl_;
};

// Same portable spelling as the Windows helper above: unqualified
// `tcp_backend_create()` works from global scope on both platforms.
[[nodiscard]] inline crpc::result<std::unique_ptr<tcp_backend>> tcp_backend_create() {
    return tcp_backend::create();
}

#endif

#endif // CRPC_TEST_TCP_BACKEND_HPP
