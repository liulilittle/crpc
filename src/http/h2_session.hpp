// CRPC -- internal HTTP/2 session shared by the h1/h2 client and the
// multiplexing h2_client. Not installed; not part of the public ABI.
//
// One blocking HTTP/2 session over one `db::channel`, in the client role.
// Single-threaded: every method must be called from the session's owning
// thread, exactly like `http2::connection` (which owns the protocol state).
//
// The shape of the API exists because a blocking channel cannot serve two
// readers: `begin` puts a whole request on the wire (HEADERS + DATA, honouring
// the send windows by pumping inbound frames while the windows are short) and
// returns without waiting for a response; `finish` pumps frames until *its*
// stream's response is complete and parks everything that belongs to other
// streams. That is what makes concurrent `begin`s observable: responses may
// be collected in any order.
//
// Errors: every failure is a `rpc_code`. A stream error (RST_STREAM, malformed
// response, content-length mismatch, header-list overflow) fails only that
// stream; a connection error (GOAWAY beyond the last stream id, a broken
// frame, a compression error) fails every stream that has not completed and
// marks the session dead.
#ifndef CRPC_HTTP_H2_SESSION_HPP
#define CRPC_HTTP_H2_SESSION_HPP

#include <cstddef>
#include <map>
#include <string>

#include <crpc/config.hpp>
#include <crpc/db/channel.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/http2/connection.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace http {
namespace detail {

namespace h2 = crpc::http2;

struct h2_session_config {
    // The `:authority` pseudo-header value and the `:scheme` value.
    std::string authority;
    std::string scheme;
    // Sent with every request before the per-request fields (already
    // filtered: connection-specific fields and Host are dropped by the
    // session as well, so a caller cannot bypass the h2 rules).
    header_list default_headers;
    // Largest response header list accepted (advertised as
    // SETTINGS_MAX_HEADER_LIST_SIZE) and request-side bound.
    std::size_t max_header_bytes = 64u * 1024u;
    std::size_t max_body_bytes = 64u * 1024u * 1024u;
    // Our advertised settings.
    u32 initial_window_size = 65535;
    u32 max_frame_size = 16384;
    // Bytes parked per stream while a send window is zero.
    std::size_t max_send_queue_bytes = 1u << 20;
};

class h2_session final {
public:
    // Does not take ownership of the channel; the owner outlives the session.
    h2_session(db::channel& link, h2_session_config config);

    h2_session(const h2_session&) = delete;
    h2_session& operator=(const h2_session&) = delete;

    // Sends the connection preface and our SETTINGS frame. Idempotent.
    [[nodiscard]] status start();

    // Opens one stream and sends the whole request. Returns the stream id.
    [[nodiscard]] result<u32> begin(method verb, std::string_view target,
                                    span<const std::byte> body,
                                    span<const header> extra_headers);

    // Pumps frames until this stream's response is complete (or fails) and
    // returns it. Safe to call for a stream whose response was already parked
    // by another stream's `finish`.
    [[nodiscard]] result<response> finish(u32 stream_id);

    [[nodiscard]] bool goaway_received() const noexcept { return h2_.goaway_received(); }
    [[nodiscard]] bool started() const noexcept { return started_; }

    // Closes the channel. Idempotent.
    void close() noexcept;

private:
    enum class stage : u8 { headers, body, failed };
    struct assembly {
        response response;
        stage stage = stage::headers;
        bool have_length = false;
        std::size_t declared = 0;
        std::size_t received = 0;
    };

    [[nodiscard]] status write_all(span<const std::byte> bytes);
    [[nodiscard]] status send_frame(h2::frame_type type, u8 flags, u32 stream_id,
                                    span<const std::byte> payload);
    [[nodiscard]] status send_header_block(u32 stream_id, const std::string& block,
                                           bool end_stream);
    // Reads once from the channel and processes every complete frame.
    [[nodiscard]] status pump_read();
    [[nodiscard]] status process_buffer();
    [[nodiscard]] status process_frame(const h2::frame_header& header,
                                       span<const std::byte> payload);
    [[nodiscard]] status handle_response_headers(u32 stream_id,
                                                 const http::header_list& fields,
                                                 bool end_stream);
    [[nodiscard]] status handle_response_trailers(u32 stream_id,
                                                  const http::header_list& fields);
    [[nodiscard]] status handle_response_data(u32 stream_id,
                                              span<const std::byte> data, u32 charged,
                                              bool end_stream);
    void complete_stream(u32 stream_id);
    void fail_stream(u32 stream_id, rpc_code code, std::string message);
    void fail_everything(rpc_code code, std::string message);
    // Releases receive windows after a DATA frame and advertises the deltas.
    [[nodiscard]] status advertise_window(u32 stream_id, u32 charged);

    db::channel* link_;
    h2_session_config config_;
    h2::connection h2_;
    bool started_ = false;

    std::string read_buf_;
    std::map<u32, assembly> inflight_;
    std::map<u32, response> completed_;
    std::map<u32, rpc_error> failures_;
    rpc_error dead_error_{};
    bool connection_dead_ = false;
    u32 next_stream_id_ = 1;
};

} // namespace detail
} // namespace http
} // namespace crpc

#endif // CRPC_HTTP_H2_SESSION_HPP
