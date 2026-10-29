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

#include <crpc/io/iocp_backend.hpp>

using tcp_backend = crpc::iocp_backend;

#else

#include <crpc/io/epoll_backend.hpp>

class tcp_backend final : public crpc::backend_factory {
public:
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

#endif

#endif // CRPC_TEST_TCP_BACKEND_HPP
