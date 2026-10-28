// CRPC -- blocking channel over a transport with optional TLS (see the header).
#include <crpc/db/stream_channel.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

#include <crpc/error.hpp>

namespace crpc {
namespace db {
namespace {

// A sink completion that signals a waiting thread and then destroys itself.
// The wait side holds the shared state, so the self-destruct never races a
// reader that timed out and walked away.
struct write_state {
    std::mutex mu;
    std::condition_variable cv;
    io::io_result result{std::make_error_code(std::errc::operation_canceled), 0};
    bool done = false;
};

class sync_completion final : public io::completion {
public:
    explicit sync_completion(std::shared_ptr<write_state> state) noexcept
        : state_(std::move(state)) {}

    void complete(io::io_result result) noexcept override {
        {
            std::lock_guard<std::mutex> lk(state_->mu);
            state_->result = result;
            state_->done = true;
        }
        state_->cv.notify_all();
        delete this;
    }

private:
    std::shared_ptr<write_state> state_;
};

[[nodiscard]] bool wait_on(const std::shared_ptr<write_state>& state,
                           std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(state->mu);
    if (timeout.count() <= 0) {
        state->cv.wait(lk, [&] { return state->done; });
        return true;
    }
    return state->cv.wait_for(lk, timeout, [&] { return state->done; });
}

} // namespace

stream_channel::stream_channel(std::unique_ptr<crpc::connection> link,
                               std::chrono::milliseconds timeout)
    : link_(std::move(link)),
      source_(*this),
      timeout_(timeout) {
    if (link_) {
        link_->bind_source(&source_);
    } else {
        closed_ = true;
    }
}

stream_channel::stream_channel(std::unique_ptr<crpc::connection> link,
                               io::tls_context& context, std::string hostname,
                               std::chrono::milliseconds timeout)
    : link_(std::move(link)),
      source_(*this),
      timeout_(timeout) {
    if (!link_) {
        closed_ = true;
        return;
    }
    std::error_code ec;
    tls_ = io::tls_stream::create(link_->sink(), context, std::move(hostname), ec);
    if (!tls_ || ec) {
        closed_ = true;
        err_ = ec;
        return;
    }
    tls_->bind_source(&source_);
    link_->bind_source(tls_.get());
}

stream_channel::~stream_channel() { close(); }

std::size_t stream_channel::on_data(span<const std::byte> data) noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    // A terminal source (on_eof/on_error) already ran; refusing new data and
    // pausing the backend is the safe response after close.
    if (closed_ || eof_) {
        return 0;
    }
    const std::size_t room = high_water_ > buf_.size() ? high_water_ - buf_.size() : 0;
    const std::size_t take = std::min(room, data.size());
    if (take != 0) {
        buf_.append(reinterpret_cast<const char*>(data.data()), take);
    }
    cv_.notify_all();
    return take;
}

void stream_channel::on_eof() noexcept {
    {
        std::lock_guard<std::mutex> lk(mu_);
        eof_ = true;
    }
    cv_.notify_all();
}

void stream_channel::on_error(std::error_code ec) noexcept {
    {
        std::lock_guard<std::mutex> lk(mu_);
        err_ = ec;
        eof_ = true;
    }
    cv_.notify_all();
}

void stream_channel::resume_reading() noexcept {
    if (tls_) {
        tls_->resume_read();
    }
    if (link_) {
        link_->resume_read();
    }
}

rpc_error stream_channel::failure(std::error_code ec) const {
    if (!ec) {
        return rpc_error{rpc_code::eof, "the peer closed the stream channel"};
    }
    if (ec.category() == rpc_error_category()) {
        return rpc_error{static_cast<rpc_code>(ec.value()), ec.message()};
    }
    if (ec.category() == io::tls_error_category()) {
        switch (static_cast<io::tls_errc>(ec.value())) {
        case io::tls_errc::ssl_verification_failed:
            return rpc_error{rpc_code::failed_precondition,
                             "TLS certificate or host name verification failed"};
        case io::tls_errc::ssl_configuration_error:
            return rpc_error{rpc_code::failed_precondition,
                             "TLS configuration error: " + ec.message()};
        case io::tls_errc::ssl_alpn_mismatch:
            return rpc_error{rpc_code::failed_precondition,
                             "TLS ALPN negotiation failed: " + ec.message()};
        case io::tls_errc::ssl_not_available:
            return rpc_error{rpc_code::failed_precondition,
                             "TLS is not available: rebuild the library with "
                             "CRPC_WITH_OPENSSL=ON"};
        case io::tls_errc::ssl_closed:
            return rpc_error{rpc_code::eof, "the peer closed the TLS connection"};
        case io::tls_errc::ssl_handshake_failed:
            return rpc_error{rpc_code::unavailable,
                             "TLS handshake failed: " + ec.message()};
        case io::tls_errc::ssl_protocol_error:
            return rpc_error{rpc_code::protocol_error,
                             "TLS protocol error: " + ec.message()};
        case io::tls_errc::ok:
        default:
            break;
        }
    }
    // A fine-grained winsock/errno mapping is not worth replicating here: every
    // transport failure that is not a clean close is reported the way the rest
    // of the library reports a dead link.
    return rpc_error{rpc_code::connection_reset,
                     "the stream channel failed: " + ec.message()};
}

bool stream_channel::wait_for_bytes(std::unique_lock<std::mutex>& lk,
                                    std::size_t& available) {
    const auto ready = [&] { return buf_.size() - pos_ != 0 || eof_ || closed_; };
    bool got = false;
    if (timeout_.count() <= 0) {
        cv_.wait(lk, ready);
        got = true;
    } else {
        got = cv_.wait_for(lk, timeout_, ready);
    }
    available = buf_.size() - pos_;
    return got;
}

result<std::size_t> stream_channel::read_some(span<std::byte> buffer) {
    if (closed_) {
        return rpc_error{rpc_code::shutting_down, "the stream channel is closed"};
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
    std::size_t count = 0;
    bool drained = false;
    {
        std::unique_lock<std::mutex> lk(mu_);
        std::size_t available = 0;
        if (!wait_for_bytes(lk, available)) {
            return rpc_error{rpc_code::deadline_exceeded,
                             "a stream channel read timed out"};
        }
        if (eof_ && available == 0) {
            return failure(err_);
        }
        count = std::min(available, buffer.size());
        std::memcpy(buffer.data(), buf_.data() + pos_, count);
        pos_ += count;
        if (pos_ == buf_.size()) {
            buf_.clear();
            pos_ = 0;
            drained = true;
        } else if (pos_ > 0) {
            // Compact occasionally so the buffer does not march across memory
            // on a long-lived stream.
            buf_.erase(0, pos_);
            pos_ = 0;
            if (buf_.empty()) {
                drained = true;
            }
        }
    }
    if (drained) {
        resume_reading();
    }
    return count;
}

result<std::vector<std::byte>> stream_channel::read_exact(std::size_t count) {
    if (closed_) {
        return rpc_error{rpc_code::shutting_down, "the stream channel is closed"};
    }
    std::vector<std::byte> out;
    out.reserve(count);
    std::byte chunk[16 * 1024];
    while (out.size() < count) {
        const std::size_t want = std::min(count - out.size(), sizeof chunk);
        result<std::size_t> got = read_some(span<std::byte>(chunk, want));
        if (!got) {
            return got.error();
        }
        if (got.value() == 0) {
            continue;
        }
        out.insert(out.end(), chunk, chunk + got.value());
    }
    return out;
}

status stream_channel::write(span<const std::byte> data) {
    if (closed_) {
        return status{rpc_code::shutting_down, "the stream channel is closed"};
    }
    if (data.empty()) {
        return status{};
    }
    std::lock_guard<std::mutex> guard(write_mu_);

    auto state = std::make_shared<write_state>();
    auto* completion = new sync_completion(state);
    if (tls_) {
        tls_->push(data, completion);
    } else {
        link_->sink().push(data, completion);
    }
    if (wait_on(state, timeout_)) {
        if (state->result.ec) {
            rpc_error mapped = failure(state->result.ec);
            if (mapped.code == rpc_code::eof ||
                mapped.code == rpc_code::shutting_down) {
                return status{mapped.code, "the stream channel closed while writing"};
            }
            return status{std::move(mapped)};
        }
        return status{};
    }
    // The write never completed within budget. Aborting the link cancels the
    // in-flight push, whose completion then wakes the wait below; the caller is
    // never left with a future write racing a half-written record.
    if (link_) {
        link_->close();
    }
    (void)wait_on(state, std::chrono::milliseconds(1000));
    return status{rpc_code::deadline_exceeded,
                  "a stream channel write timed out; the link was aborted"};
}

void stream_channel::close() noexcept {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) {
            return;
        }
        closed_ = true;
    }
    if (tls_) {
        tls_->close();
    }
    if (link_) {
        link_->bind_source(nullptr);
        link_->close();
    }
    cv_.notify_all();
}

bool stream_channel::is_open() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return !closed_ && !eof_ && link_ != nullptr && link_->is_open();
}

std::string stream_channel::tls_server_name() const {
    return tls_ != nullptr ? tls_->server_name() : std::string();
}

std::string stream_channel::tls_alpn_protocol() const {
    return tls_ != nullptr ? tls_->alpn_protocol() : std::string();
}

bool stream_channel::tls_handshake_complete() const noexcept {
    return tls_ != nullptr && tls_->handshake_complete();
}

std::error_code stream_channel::tls_error() const noexcept {
    return tls_ != nullptr ? tls_->error()
                           : io::make_error_code(io::tls_errc::ssl_not_available);
}

} // namespace db
} // namespace crpc