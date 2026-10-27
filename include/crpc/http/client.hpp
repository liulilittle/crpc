// CRPC -- a small HTTP/1.1 client.
//
// The library had a server and a message parser but no client, and the REST adapters (HBase
// first) need one. This is deliberately the smallest client that is still correct:
//
//   * one request per connection (`Connection: close`), so there is no keep-alive state
//     machine to get wrong and no risk of reading a previous response's tail;
//   * `Content-Length` and `Transfer-Encoding: chunked` are both handled, because a server
//     may answer either way and a client that handles only one truncates or hangs;
//   * the response body is bounded, so a hostile or broken server cannot exhaust memory.
//
// It does not follow redirects: an adapter that silently followed one could send a request
// (and its credentials) to a host the caller never named.
#ifndef CRPC_HTTP_CLIENT_HPP
#define CRPC_HTTP_CLIENT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/channel.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace http {

struct client_options {
    // Sent as `Host`. The port is appended when it is not 80, which is what makes a
    // virtual host work behind a non-default port.
    std::string host;
    std::uint16_t port = 80;

    // Extra fields sent with every request (an authorization header, for instance).
    header_list default_headers;

    // Ceilings. Exceeding either fails with `resource_exhausted` rather than growing until
    // the process dies.
    std::size_t max_header_bytes = 64u * 1024u;
    std::size_t max_body_bytes = 64u * 1024u * 1024u;

    // How long a whole request may take, in seconds. 0 leaves it to the transport.
    std::uint32_t timeout_seconds = 30;
};

// One request/response exchange. Constructing it opens the link; `send` may be called once.
class CRPC_API client final {
public:
    // Takes ownership of an already-open channel. Separated from the transport so a test
    // can script a server without a socket.
    client(client_options options, std::unique_ptr<db::channel> link);
    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;

    // Sends one request and reads the whole response. The connection is closed afterwards,
    // because the request asked for `Connection: close`.
    [[nodiscard]] result<response> send(method verb, std::string_view target,
                                        span<const std::byte> body,
                                        span<const header> extra_headers = {});

    // Convenience for the common case: a text body with a content type.
    [[nodiscard]] result<response> send_text(method verb, std::string_view target,
                                             std::string_view body,
                                             std::string_view content_type);

    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// Builds a client over TCP.
[[nodiscard]] CRPC_API std::unique_ptr<client> make_client(client_options options,
                                                          crpc::backend_factory& backend,
                                                          crpc::endpoint endpoint,
                                                          crpc::runtime* shard = nullptr);

} // namespace http
} // namespace crpc

#endif // CRPC_HTTP_CLIENT_HPP