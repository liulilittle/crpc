#include <crpc/push/stream.hpp>

#include <cstring>

namespace crpc::push {

stream_state::stream_state(u32 stream_id, stream_shape shape) noexcept
    : id_(stream_id), shape_(shape) {}

bool stream_state::can_read() const noexcept {
    if (canceled_) {
        return false;
    }
    if (shape_ == stream_shape::client_streaming && !local_closed_) {
        return false;
    }
    return true;
}

bool stream_state::can_write() const noexcept {
    return !canceled_ && !local_closed_;
}

void stream_state::mark_local_close() noexcept {
    local_closed_ = true;
}

void stream_state::mark_remote_eof() noexcept {
    remote_closed_ = true;
}

void stream_state::mark_reset(rpc_code code) noexcept {
    canceled_ = true;
    terminal_ = code == rpc_code::ok ? rpc_code::canceled : code;
}

stream_reader::stream_reader(stream_transport& transport,
                             stream_state& state) noexcept
    : transport_(&transport), state_(&state) {}

result<std::size_t> stream_reader::read_some(span<std::byte> dst) noexcept {
    if (dst.empty()) {
        return std::size_t{0};
    }
    if (state_->canceled()) {
        return rpc_error(state_->terminal_code());
    }
    if (!state_->can_read()) {
        return rpc_error(rpc_code::failed_precondition);
    }

    if (pending_.empty()) {
        span<const std::byte> payload;
        rpc_code error = rpc_code::ok;
        const stream_transport::pull_result result =
            transport_->pull(state_->id(), payload, error);
        switch (result) {
        case stream_transport::pull_result::data:
            pending_ = payload;
            offset_ = 0;
            if (pending_.empty()) {
                return std::size_t{0};
            }
            break;
        case stream_transport::pull_result::would_block:
            return rpc_error(rpc_code::unavailable);
        case stream_transport::pull_result::eof:
            state_->mark_remote_eof();
            return rpc_error(rpc_code::eof);
        case stream_transport::pull_result::canceled:
            state_->mark_reset(error == rpc_code::ok ? rpc_code::canceled : error);
            return rpc_error(state_->terminal_code());
        case stream_transport::pull_result::error:
        default:
            return rpc_error(error == rpc_code::ok ? rpc_code::internal : error);
        }
    }

    const std::size_t available = pending_.size() - offset_;
    const std::size_t count = dst.size() < available ? dst.size() : available;
    std::memcpy(dst.data(), pending_.data() + offset_, count);
    offset_ += count;
    if (offset_ == pending_.size()) {
        pending_ = span<const std::byte>{};
        offset_ = 0;
    }
    transport_->release_read_credit(state_->id(), count);
    return std::size_t{count};
}

status stream_reader::read_exact(span<std::byte> dst) noexcept {
    std::size_t done = 0;
    while (done < dst.size()) {
        result<std::size_t> result = read_some(dst.subspan(done));
        if (!result.ok()) {
            return status(result.error().code);
        }
        const std::size_t got = result.value();
        if (got == 0) {
            return status(rpc_code::unavailable);
        }
        done += got;
    }
    return status{};
}

stream_writer::stream_writer(stream_transport& transport,
                             stream_state& state) noexcept
    : transport_(&transport), state_(&state) {}

result<std::size_t> stream_writer::write(span<const std::byte> src) noexcept {
    if (state_->canceled()) {
        return rpc_error(state_->terminal_code());
    }
    if (!state_->can_write()) {
        return rpc_error(rpc_code::failed_precondition);
    }
    if (src.empty()) {
        return std::size_t{0};
    }
    const std::size_t credit = transport_->send_credit(state_->id());
    if (credit == 0) {
        return std::size_t{0};
    }
    const std::size_t want = src.size() < credit ? src.size() : credit;
    const std::size_t accepted = transport_->send(state_->id(), src.first(want));
    return std::size_t{accepted};
}

status stream_writer::close() noexcept {
    if (state_->canceled() || state_->local_closed()) {
        return status{};
    }
    if (!transport_->send_end(state_->id())) {
        return status(rpc_code::resource_exhausted);
    }
    state_->mark_local_close();
    return status{};
}

status stream_writer::reset(rpc_code code) noexcept {
    if (state_->canceled()) {
        return status{};
    }
    transport_->send_reset(state_->id(), code);
    state_->mark_reset(code);
    return status{};
}

} // namespace crpc::push
