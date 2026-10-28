// CRPC -- WebSocket transport implementation (see transport.hpp).
#include "transport.hpp"

#include <algorithm>
#include <cstring>

#include <crpc/error.hpp>

namespace crpc {
namespace ws {
namespace detail {
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
    return state->cv.wait_for(lk, timeout, [&] { return state->done; });
}

} // namespace

transport_channel::transport_channel(std::unique_ptr<crpc::connection> link,
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

transport_channel::transport_channel(std::unique_ptr<crpc::connection> link,
                                     io::tls_context& context,
                                     std::string hostname,
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

transport_channel::~transport_channel() { close(); }

std::size_t transport_channel::on_data(span<const std::byte> data) noexcept {
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

void transport_channel::on_eof() noexcept {
    {
        std::lock_guard<std::mutex> lk(mu_);
        eof_ = true;
    }
    cv_.notify_all();
}

void transport_channel::on_error(std::error_code ec) noexcept {
    {
        std::lock_guard<std::mutex> lk(mu_);
        err_ = ec;
        eof_ = true;
    }
    cv_.notify_all();
}

void transport_channel::resume_reading() noexcept {
    if (tls_) {
        tls_->resume_read();
    }
    if (link_) {
        link_->resume_read();
    }
}

rpc_code transport_channel::map_error(std::error_code ec) const noexcept {
    if (!ec) {
        return rpc_code::ok;
    }
    if (ec.category() == rpc_error_category()) {
        return static_cast<rpc_code>(ec.value());
    }
    if (ec == crpc::io::make_error_code(crpc::io::tls_errc::ssl_closed)) {
        return rpc_code::eof;
    }
    // A fine-grained winsock/errno mapping is not worth replicating here: every
    // transport failure that is not a clean close is reported the way the rest
    // of the library reports a dead link.
    return rpc_code::connection_reset;
}

rpc_error transport_channel::fail_read() const {
    if (err_) {
        return rpc_error{map_error(err_), "the WebSocket transport failed"};
    }
    return rpc_error{rpc_code::eof, "the peer closed the WebSocket transport"};
}

bool transport_channel::wait_for_bytes(std::unique_lock<std::mutex>& lk,
                                       std::size_t& available) {
    const bool got = cv_.wait_for(lk, timeout_, [&] {
        return buf_.size() - pos_ != 0 || eof_ || closed_;
    });
    available = buf_.size() - pos_;
    return got;
}

result<std::size_t> transport_channel::read_some(span<std::byte> buffer) {
    if (closed_) {
        return rpc_error{rpc_code::shutting_down, "the WebSocket transport is closed"};
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
                             "a WebSocket read timed out"};
        }
        if (eof_ && available == 0) {
            return fail_read();
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

result<std::vector<std::byte>> transport_channel::read_exact(std::size_t count) {
    if (closed_) {
        return rpc_error{rpc_code::shutting_down, "the WebSocket transport is closed"};
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

status transport_channel::write(span<const std::byte> data) {
    if (closed_) {
        return status{rpc_code::shutting_down, "the WebSocket transport is closed"};
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
            const rpc_code mapped = map_error(state->result.ec);
            if (mapped == rpc_code::eof || mapped == rpc_code::shutting_down) {
                return status{mapped, "the WebSocket transport closed while writing"};
            }
            return status{mapped, state->result.ec.message()};
        }
        return status{};
    }
    // The write never completed within budget. Aborting the link cancels the
    // in-flight push, whose completion then wakes the wait below; the caller is
    // never left with a future write racing a half-written frame.
    if (link_) {
        link_->close();
    }
    (void)wait_on(state, std::chrono::milliseconds(1000));
    return status{rpc_code::deadline_exceeded,
                  "a WebSocket write timed out; the link was aborted"};
}

void transport_channel::close() noexcept {
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

bool transport_channel::is_open() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return !closed_ && !eof_ && link_ != nullptr && link_->is_open();
}

} // namespace detail
} // namespace ws
} // namespace crpc