// CRPC -- end-to-end PUSH/PUSH_ACK tests for the single-shard in-process path.
//
// The suite covers the minimal slice: the PUSH payload envelope, subscription
// control over the ordinary REQUEST path, the per-connection push pump with its
// egress bound, PUSH_ACK -> hub::ack, reconnect replay and seq dedup. HTTP SSE,
// stream_transport and cross-shard routing are intentionally out of scope.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/endian.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/push/wire.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/server.hpp>
#include <crpc/span.hpp>
#include <crpc/wire.hpp>

using namespace crpc;
using namespace std::chrono_literals;

namespace {

std::atomic<unsigned> fixture_counter{0};

constexpr crpc::push::delivery_mode reliable = crpc::push::delivery_mode::reliable;
constexpr crpc::push::delivery_mode unreliable = crpc::push::delivery_mode::unreliable;

std::string unique_name(const char* prefix) {
    return std::string(prefix) + "-" +
           std::to_string(fixture_counter.fetch_add(1, std::memory_order_relaxed));
}

struct push_fixture {
    inproc_backend backend;
    std::unique_ptr<server> srv;
    std::string name;

    push_fixture(std::size_t egress, push::subscribe_options subscribe_options,
                 inproc_options transport = {})
        : backend(transport) {
        name = unique_name("push-e2e");
        server::builder builder;
        builder.add_listener(endpoint::inproc(name), backend);
        builder.push_egress_max_bytes(egress);
        builder.push_subscribe_options(subscribe_options);
        srv = builder.build();
        if (srv) {
            CRPC_CHECK(srv->start().ok());
        }
    }

    ~push_fixture() {
        if (srv) {
            srv->stop();
        }
    }

    [[nodiscard]] endpoint ep() const { return endpoint::inproc(name); }
};

std::unique_ptr<rpc::client> make_client(push_fixture& fixture) {
    std::unique_ptr<connection> conn = fixture.backend.connect(fixture.ep(), {});
    if (!conn) {
        return nullptr;
    }
    rpc::client_options options;
    options.slot_capacity = 16;
    return std::make_unique<rpc::client>(std::move(conn), options);
}

struct sink_state {
    std::mutex mutex;
    std::vector<std::string> bodies;
    std::vector<u64> seqs;

    [[nodiscard]] std::size_t size() {
        std::lock_guard<std::mutex> lk(mutex);
        return bodies.size();
    }

    [[nodiscard]] std::string body(std::size_t index) {
        std::lock_guard<std::mutex> lk(mutex);
        return index < bodies.size() ? bodies[index] : std::string();
    }

    [[nodiscard]] std::vector<u64> sequences() {
        std::lock_guard<std::mutex> lk(mutex);
        return seqs;
    }
};

void record_push(void* ctx, const push::push_message_view& message) noexcept {
    auto* state = static_cast<sink_state*>(ctx);
    std::lock_guard<std::mutex> lk(state->mutex);
    state->bodies.emplace_back(reinterpret_cast<const char*>(message.body.data()),
                               message.body.size());
    state->seqs.push_back(message.seq);
}

// One outgoing raw frame. The buffer must outlive the sink completion because
// the in-process backend may defer the tail.
struct raw_out final : io::completion {
    std::unique_ptr<std::byte[]> bytes;
    std::size_t size = 0;

    void complete(io::io_result) noexcept override { delete this; }
};

// A hand-driven peer: it speaks the wire format directly so a test can stop
// reading (to exercise backpressure) and withhold acknowledgements (to
// exercise replay). It is also the client side of the RED/GREEN probe.
class raw_client final : public io::source {
public:
    struct push_record {
        u32 stream_id = 0;
        u16 topic_id = 0;
        u64 seq = 0;
        std::string body;
    };

    explicit raw_client(inproc_backend& backend, const endpoint& ep) {
        conn_ = backend.connect(ep, {});
        if (conn_) {
            conn_->bind_source(this);
        }
    }

    ~raw_client() override {
        if (conn_) {
            conn_->close();
            conn_->bind_source(nullptr);
        }
    }

    raw_client(const raw_client&) = delete;
    raw_client& operator=(const raw_client&) = delete;

    [[nodiscard]] bool valid() const noexcept { return conn_ != nullptr; }
    void set_auto_ack(bool value) noexcept {
        auto_ack_.store(value, std::memory_order_release);
    }
    void pause() noexcept { paused_.store(true, std::memory_order_release); }
    void resume() noexcept {
        paused_.store(false, std::memory_order_release);
        if (conn_) {
            conn_->resume_read();
        }
    }

    [[nodiscard]] bool subscribe(std::string_view topic, u64 subscriber_id,
                                 push::delivery_mode mode, bool ordered) {
        push::subscribe_request request;
        request.subscriber_id = subscriber_id;
        request.mode = mode;
        request.ordered = ordered;
        request.topic.assign(topic.data(), topic.size());

        std::vector<std::byte> body(push::encoded_subscribe_request_size(request));
        std::size_t body_written = 0;
        if (!push::encode_subscribe_request(
                 span<std::byte>(body.data(), body.size()), request, body_written)
                 .ok()) {
            return false;
        }

        call_envelope envelope;
        envelope.service_id = push::push_control_service_id;
        envelope.method_id = push::push_control_method_subscribe;
        envelope.codec = codec_id::crpc_binary;
        envelope.body = span<const std::byte>(body.data(), body_written);

        std::vector<std::byte> payload(encoded_call_envelope_size(envelope));
        std::size_t payload_written = 0;
        if (!encode_call_envelope(span<std::byte>(payload.data(), payload.size()),
                                  envelope, payload_written)
                 .ok()) {
            return false;
        }

        const u32 stream_id = next_stream_id();
        const int before = responses_.load(std::memory_order_acquire);
        frame request_frame;
        request_frame.header.type = frame_type::request;
        request_frame.header.stream_id = stream_id;
        request_frame.payload =
            span<const std::byte>(payload.data(), payload_written);
        if (!send_frame(request_frame)) {
            return false;
        }
        return crpc_test::wait_for(
            [this, before] {
                return responses_.load(std::memory_order_acquire) > before;
            },
            2000);
    }

    [[nodiscard]] std::size_t push_count() const noexcept {
        return pushes_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::vector<push_record> pushes() {
        std::lock_guard<std::mutex> lk(mutex_);
        return push_records_;
    }

    std::size_t on_data(span<const std::byte> data) noexcept override {
        if (paused_.load(std::memory_order_acquire)) {
            return 0;
        }
        std::vector<u64> acks;
        u32 ack_stream = 0;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            decoder_.feed(data);
            for (;;) {
                frame value;
                const frame_decoder::result result = decoder_.next(value);
                if (result == frame_decoder::result::need_more) {
                    break;
                }
                if (result == frame_decoder::result::error) {
                    break;
                }
                if (value.header.type == frame_type::response) {
                    rpc_code code = rpc_code::ok;
                    rpc_error error;
                    span<const std::byte> body;
                    if (rpc::decode_response(value.payload, code, error, body).ok() &&
                        code == rpc_code::ok && !body.empty()) {
                        last_reply_ = push::push_subscription{};
                        (void)push::decode_push_subscription(body, last_reply_);
                    }
                    responses_.fetch_add(1, std::memory_order_acq_rel);
                } else if (value.header.type == frame_type::push) {
                    push::push_envelope envelope;
                    if (!push::decode_push_envelope(value.payload, envelope).ok()) {
                        ++decode_failures_;
                        continue;
                    }
                    push_record record;
                    record.stream_id = value.header.stream_id;
                    record.topic_id = envelope.topic_id;
                    record.seq = envelope.seq;
                    record.body.assign(
                        reinterpret_cast<const char*>(envelope.body.data()),
                        envelope.body.size());
                    push_records_.push_back(record);
                    pushes_.fetch_add(1, std::memory_order_acq_rel);
                    if (auto_ack_.load(std::memory_order_acquire) &&
                        envelope.mode == push::delivery_mode::reliable) {
                        acks.push_back(envelope.seq);
                        ack_stream = value.header.stream_id;
                    }
                }
            }
        }
        for (u64 seq : acks) {
            send_push_ack(ack_stream, seq);
        }
        return data.size();
    }

    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

    [[nodiscard]] int decode_failures() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return decode_failures_;
    }

private:
    [[nodiscard]] u32 next_stream_id() noexcept {
        const u32 id = next_stream_id_;
        ++next_stream_id_;
        if (next_stream_id_ == 0) {
            next_stream_id_ = 1;
        }
        return id;
    }

    [[nodiscard]] bool send_frame(const frame& value) {
        if (!conn_) {
            return false;
        }
        raw_out* op = new (std::nothrow) raw_out();
        if (op == nullptr) {
            return false;
        }
        const std::size_t size = encoded_frame_size(value);
        op->bytes.reset(new (std::nothrow) std::byte[size]);
        if (!op->bytes) {
            delete op;
            return false;
        }
        std::size_t written = 0;
        if (!encode_frame(span<std::byte>(op->bytes.get(), size), value, written).ok()) {
            delete op;
            return false;
        }
        op->size = written;
        conn_->sink().push(span<const std::byte>(op->bytes.get(), op->size), op);
        return true;
    }

    void send_push_ack(u32 stream_id, u64 seq) {
        std::byte payload[push::push_ack_size];
        endian::store_le64(payload, seq);
        frame ack;
        ack.header.type = frame_type::push_ack;
        ack.header.stream_id = stream_id;
        ack.payload = span<const std::byte>(payload, sizeof(payload));
        (void)send_frame(ack);
    }

    std::unique_ptr<connection> conn_;
    frame_decoder decoder_{max_frame_default};
    mutable std::mutex mutex_;
    std::vector<push_record> push_records_;
    push::push_subscription last_reply_;
    std::atomic<std::size_t> pushes_{0};
    std::atomic<int> responses_{0};
    std::atomic<bool> auto_ack_{true};
    std::atomic<bool> paused_{false};
    int decode_failures_ = 0;
    u32 next_stream_id_ = 1;
};

// --- wire codec --------------------------------------------------------------

CRPC_TEST(push_wire_round_trip) {
    const std::string body = "payload-bytes";
    push::push_envelope original;
    original.mode = reliable;
    original.topic_id = 17;
    original.publisher_id = 3;
    original.epoch = 9;
    original.seq = 11;
    original.topic_seq = 5;
    original.body = span<const std::byte>(
        reinterpret_cast<const std::byte*>(body.data()), body.size());

    const std::size_t size = push::encoded_push_envelope_size(original);
    std::vector<std::byte> encoded(size);
    std::size_t written = 0;
    CRPC_CHECK(push::encode_push_envelope(
                   span<std::byte>(encoded.data(), encoded.size()), original, written)
                   .ok());
    CRPC_CHECK_EQ(written, size);

    push::push_envelope decoded;
    CRPC_CHECK(push::decode_push_envelope(
                   span<const std::byte>(encoded.data(), written), decoded)
                   .ok());
    CRPC_CHECK_EQ(decoded.version, push::push_wire_version);
    CRPC_CHECK_EQ(static_cast<u8>(decoded.mode), static_cast<u8>(reliable));
    CRPC_CHECK_EQ(decoded.topic_id, static_cast<u16>(17));
    CRPC_CHECK_EQ(decoded.publisher_id, static_cast<u64>(3));
    CRPC_CHECK_EQ(decoded.epoch, static_cast<u64>(9));
    CRPC_CHECK_EQ(decoded.seq, static_cast<u64>(11));
    CRPC_CHECK_EQ(decoded.topic_seq, static_cast<u64>(5));
    CRPC_CHECK_EQ(decoded.body.size(), body.size());
    CRPC_CHECK(decoded.body.size() == body.size() &&
               std::memcmp(decoded.body.data(), body.data(), body.size()) == 0);

    push::subscribe_request request;
    request.subscriber_id = 42;
    request.mode = reliable;
    request.ordered = true;
    request.topic = "orders";
    std::vector<std::byte> request_bytes(push::encoded_subscribe_request_size(request));
    written = 0;
    CRPC_CHECK(push::encode_subscribe_request(
                   span<std::byte>(request_bytes.data(), request_bytes.size()),
                   request, written)
                   .ok());
    push::subscribe_request decoded_request;
    CRPC_CHECK(push::decode_subscribe_request(
                   span<const std::byte>(request_bytes.data(), written),
                   decoded_request)
                   .ok());
    CRPC_CHECK_EQ(decoded_request.subscriber_id, static_cast<u64>(42));
    CRPC_CHECK_EQ(decoded_request.topic, std::string("orders"));
    CRPC_CHECK(decoded_request.ordered);

    push::push_subscription reply;
    reply.stream_id = 7;
    reply.topic_id = 17;
    reply.mode = reliable;
    reply.ordered = true;
    reply.topic = "orders";
    std::vector<std::byte> reply_bytes(push::encoded_push_subscription_size(reply));
    written = 0;
    CRPC_CHECK(push::encode_push_subscription(
                   span<std::byte>(reply_bytes.data(), reply_bytes.size()), reply,
                   written)
                   .ok());
    push::push_subscription decoded_reply;
    CRPC_CHECK(push::decode_push_subscription(
                   span<const std::byte>(reply_bytes.data(), written), decoded_reply)
                   .ok());
    CRPC_CHECK_EQ(decoded_reply.stream_id, static_cast<u32>(7));
    CRPC_CHECK_EQ(decoded_reply.topic_id, static_cast<u16>(17));
    CRPC_CHECK_EQ(decoded_reply.topic, std::string("orders"));
}

CRPC_TEST(push_wire_rejects_malformed_input) {
    const std::string body = "xyz";
    push::push_envelope valid;
    valid.mode = reliable;
    valid.topic_id = 1;
    valid.seq = 1;
    valid.topic_seq = 1;
    valid.body = span<const std::byte>(
        reinterpret_cast<const std::byte*>(body.data()), body.size());

    std::vector<std::byte> encoded(push::encoded_push_envelope_size(valid));
    std::size_t written = 0;
    CRPC_CHECK(push::encode_push_envelope(
                   span<std::byte>(encoded.data(), encoded.size()), valid, written)
                   .ok());

    auto decode = [](const std::vector<std::byte>& bytes) {
        push::push_envelope value;
        return push::decode_push_envelope(
                   span<const std::byte>(bytes.data(), bytes.size()), value)
            .ok();
    };

    // Version.
    {
        std::vector<std::byte> bad = encoded;
        bad[0] = std::byte{2};
        CRPC_CHECK_FALSE(decode(bad));
    }
    // Unknown mode.
    {
        std::vector<std::byte> bad = encoded;
        bad[1] = std::byte{2};
        CRPC_CHECK_FALSE(decode(bad));
    }
    // Topic id zero.
    {
        std::vector<std::byte> bad = encoded;
        endian::store_le16(bad.data() + 2, static_cast<u16>(0));
        CRPC_CHECK_FALSE(decode(bad));
    }
    // Reliable sequence zero.
    {
        std::vector<std::byte> bad = encoded;
        endian::store_le64(bad.data() + 20, static_cast<u64>(0));
        CRPC_CHECK_FALSE(decode(bad));
    }
    // Unreliable envelope with a non-zero sequence.
    {
        std::vector<std::byte> bad = encoded;
        bad[1] = std::byte{1};
        CRPC_CHECK_FALSE(decode(bad));
    }
    // Body length longer than the remaining payload.
    {
        std::vector<std::byte> bad = encoded;
        endian::store_le32(bad.data() + 36, static_cast<u32>(body.size() + 1));
        CRPC_CHECK_FALSE(decode(bad));
    }
    // Body length shorter than the remaining payload (trailing bytes).
    {
        std::vector<std::byte> bad = encoded;
        endian::store_le32(bad.data() + 36, static_cast<u32>(body.size() - 1));
        CRPC_CHECK_FALSE(decode(bad));
    }
    // Truncated fixed field.
    {
        std::vector<std::byte> bad(encoded.begin(), encoded.begin() + 39);
        CRPC_CHECK_FALSE(decode(bad));
    }
    // Empty input.
    {
        std::vector<std::byte> bad;
        CRPC_CHECK_FALSE(decode(bad));
    }

    push::subscribe_request request;
    request.subscriber_id = 1;
    request.topic = "t";
    std::vector<std::byte> request_bytes(push::encoded_subscribe_request_size(request));
    written = 0;
    CRPC_CHECK(push::encode_subscribe_request(
                   span<std::byte>(request_bytes.data(), request_bytes.size()),
                   request, written)
                   .ok());
    auto decode_request = [](const std::vector<std::byte>& bytes) {
        push::subscribe_request value;
        return push::decode_subscribe_request(
                   span<const std::byte>(bytes.data(), bytes.size()), value)
            .ok();
    };
    // Zero subscriber id.
    {
        std::vector<std::byte> bad = request_bytes;
        endian::store_le64(bad.data(), static_cast<u64>(0));
        CRPC_CHECK_FALSE(decode_request(bad));
    }
    // Empty topic length.
    {
        std::vector<std::byte> bad = request_bytes;
        endian::store_le16(bad.data() + 10, static_cast<u16>(0));
        CRPC_CHECK_FALSE(decode_request(bad));
    }
    // Topic length larger than the remaining bytes.
    {
        std::vector<std::byte> bad = request_bytes;
        endian::store_le16(bad.data() + 10, static_cast<u16>(2));
        CRPC_CHECK_FALSE(decode_request(bad));
    }
    // Ordered flag other than 0/1.
    {
        std::vector<std::byte> bad = request_bytes;
        bad[9] = std::byte{2};
        CRPC_CHECK_FALSE(decode_request(bad));
    }
    // Unknown mode.
    {
        std::vector<std::byte> bad = request_bytes;
        bad[8] = std::byte{3};
        CRPC_CHECK_FALSE(decode_request(bad));
    }
    // Encode-time rejection: subscriber id zero and empty topic.
    {
        push::subscribe_request invalid = request;
        invalid.subscriber_id = 0;
        std::vector<std::byte> out(push::encoded_subscribe_request_size(invalid));
        CRPC_CHECK_FALSE(
            push::encode_subscribe_request(span<std::byte>(out.data(), out.size()),
                                           invalid, written)
                .ok());
        invalid = request;
        invalid.topic.clear();
        out.resize(push::encoded_subscribe_request_size(invalid));
        CRPC_CHECK_FALSE(
            push::encode_subscribe_request(span<std::byte>(out.data(), out.size()),
                                           invalid, written)
                .ok());
    }
}

// --- end to end --------------------------------------------------------------

CRPC_TEST(push_subscribe_publish_ack_advances_hub) {
    push_fixture fixture(1u << 20, push::subscribe_options{});
    CRPC_CHECK(fixture.srv != nullptr);
    std::unique_ptr<rpc::client> client = make_client(fixture);
    CRPC_CHECK(client != nullptr);

    sink_state sink;
    client->set_push_sink(&record_push, &sink);

    auto subscription = client->subscribe_push("orders", 7, reliable, false);
    CRPC_CHECK(subscription.ok());
    CRPC_CHECK(subscription.ok() && subscription.value().stream_id != 0);
    CRPC_CHECK(subscription.ok() && subscription.value().topic_id != 0);

    const std::string body = "hello-push";
    CRPC_CHECK(fixture.srv->publish(
        "orders",
        span<const std::byte>(reinterpret_cast<const std::byte*>(body.data()),
                              body.size()),
        reliable));

    CRPC_CHECK(crpc_test::wait_for([&sink] { return sink.size() == 1; }, 2000));
    CRPC_CHECK_EQ(sink.body(0), body);
    const std::vector<u64> seqs = sink.sequences();
    CRPC_CHECK_EQ(seqs.size(), static_cast<std::size_t>(1));
    if (!seqs.empty()) {
        CRPC_CHECK_EQ(seqs[0], static_cast<u64>(1));
    }

    // The client acknowledged automatically; the server applies it on the shard
    // thread. Once it has been applied the message is no longer replayable.
    CRPC_CHECK(crpc_test::wait_for(
        [&fixture] { return fixture.srv->stats().push_acks >= 1; }, 2000));

    client.reset();
    CRPC_CHECK(crpc_test::wait_for(
        [&fixture] { return fixture.srv->stats().push_active_connections == 0; },
        2000));

    // Reconnect with the same subscriber id: a fully acknowledged stream
    // replays nothing.
    raw_client replay_client(fixture.backend, fixture.ep());
    CRPC_CHECK(replay_client.valid());
    CRPC_CHECK(replay_client.subscribe("orders", 7, reliable, false));
    std::this_thread::sleep_for(100ms);
    CRPC_CHECK_EQ(replay_client.push_count(), static_cast<std::size_t>(0));

    // The subscription still exists and a new publish reaches the new
    // connection (the hub subscription survived the detach).
    const std::string second = "after-reconnect";
    CRPC_CHECK(fixture.srv->publish(
        "orders",
        span<const std::byte>(reinterpret_cast<const std::byte*>(second.data()),
                              second.size()),
        reliable));
    CRPC_CHECK(crpc_test::wait_for(
        [&replay_client] { return replay_client.push_count() == 1; }, 2000));
    const std::vector<raw_client::push_record> records = replay_client.pushes();
    CRPC_CHECK_EQ(records.size(), static_cast<std::size_t>(1));
    if (!records.empty()) {
        CRPC_CHECK_EQ(records[0].body, second);
        CRPC_CHECK_EQ(records[0].seq, static_cast<u64>(2));
    }
}

CRPC_TEST(push_reconnect_replays_only_unacknowledged) {
    push_fixture fixture(1u << 20, push::subscribe_options{});
    CRPC_CHECK(fixture.srv != nullptr);

    auto first = std::make_unique<raw_client>(fixture.backend, fixture.ep());
    CRPC_CHECK(first->valid());
    first->set_auto_ack(false);
    CRPC_CHECK(first->subscribe("events", 77, reliable, false));

    for (int i = 0; i < 3; ++i) {
        const std::string body = "m" + std::to_string(i);
        CRPC_CHECK(fixture.srv->publish(
            "events",
            span<const std::byte>(reinterpret_cast<const std::byte*>(body.data()),
                                  body.size()),
            reliable));
    }
    CRPC_CHECK(crpc_test::wait_for(
        [&first] { return first->push_count() == 3; }, 2000));
    {
        const std::vector<raw_client::push_record> records = first->pushes();
        CRPC_CHECK_EQ(records.size(), static_cast<std::size_t>(3));
        for (std::size_t i = 0; i < records.size(); ++i) {
            CRPC_CHECK_EQ(records[i].seq, static_cast<u64>(i + 1));
        }
    }

    first.reset();
    CRPC_CHECK(crpc_test::wait_for(
        [&fixture] { return fixture.srv->stats().push_active_connections == 0; },
        2000));

    auto resumed = std::make_unique<raw_client>(fixture.backend, fixture.ep());
    CRPC_CHECK(resumed->valid());
    resumed->set_auto_ack(true);
    CRPC_CHECK(resumed->subscribe("events", 77, reliable, false));

    CRPC_CHECK(crpc_test::wait_for(
        [&resumed] { return resumed->push_count() == 3; }, 2000));
    {
        const std::vector<raw_client::push_record> records = resumed->pushes();
        CRPC_CHECK_EQ(records.size(), static_cast<std::size_t>(3));
        for (std::size_t i = 0; i < records.size(); ++i) {
            CRPC_CHECK_EQ(records[i].seq, static_cast<u64>(i + 1));
            CRPC_CHECK_EQ(records[i].body, std::string("m") + std::to_string(i));
        }
    }
    CRPC_CHECK(crpc_test::wait_for(
        [&fixture] { return fixture.srv->stats().push_acks >= 3; }, 2000));
}

CRPC_TEST(push_client_deduplicates_replayed_sequence) {
    auto pair = inproc_pair::make();
    struct ack_capture final : io::source {
        frame_decoder decoder{max_frame_default};
        std::atomic<int> acks{0};
        std::size_t on_data(span<const std::byte> data) noexcept override {
            decoder.feed(data);
            for (;;) {
                frame value;
                const frame_decoder::result result = decoder.next(value);
                if (result != frame_decoder::result::frame_ready) {
                    break;
                }
                if (value.header.type == frame_type::push_ack) {
                    acks.fetch_add(1, std::memory_order_acq_rel);
                }
            }
            return data.size();
        }
        void on_eof() noexcept override {}
        void on_error(std::error_code) noexcept override {}
    } capture;
    pair.second->bind_source(&capture);

    rpc::client_options options;
    options.slot_capacity = 8;
    rpc::client client(std::move(pair.first), options);
    sink_state sink;
    client.set_push_sink(&record_push, &sink);

    const std::string body = "once";
    push::push_envelope envelope;
    envelope.mode = reliable;
    envelope.topic_id = 3;
    envelope.publisher_id = 1;
    envelope.epoch = 1;
    envelope.seq = 4;
    envelope.topic_seq = 2;
    envelope.body = span<const std::byte>(
        reinterpret_cast<const std::byte*>(body.data()), body.size());
    std::vector<std::byte> payload(push::encoded_push_envelope_size(envelope));
    std::size_t written = 0;
    CRPC_CHECK(push::encode_push_envelope(
                   span<std::byte>(payload.data(), payload.size()), envelope, written)
                   .ok());

    struct noop final : io::completion {
        void complete(io::io_result) noexcept override {}
    } done;

    for (int i = 0; i < 2; ++i) {
        frame push;
        push.header.type = frame_type::push;
        push.header.stream_id = 9;
        push.payload = span<const std::byte>(payload.data(), written);
        const std::size_t size = encoded_frame_size(push);
        std::vector<std::byte> encoded(size);
        std::size_t frame_written = 0;
        CRPC_CHECK(encode_frame(span<std::byte>(encoded.data(), encoded.size()), push,
                                frame_written)
                       .ok());
        pair.second->sink().push(
            span<const std::byte>(encoded.data(), frame_written), &done);
    }

    CRPC_CHECK(crpc_test::wait_for(
        [&capture] { return capture.acks.load(std::memory_order_acquire) == 1; },
        2000));
    std::this_thread::sleep_for(50ms);
    CRPC_CHECK_EQ(sink.size(), static_cast<std::size_t>(1));
    CRPC_CHECK_EQ(capture.acks.load(std::memory_order_acquire), 1);
    CRPC_CHECK_EQ(sink.body(0), body);

    pair.second->bind_source(nullptr);
}

CRPC_TEST(push_slow_client_pauses_pump_and_hub_drops) {
    push::subscribe_options subscribe;
    subscribe.max_messages = 4;
    subscribe.max_bytes = 1u << 20;
    subscribe.on_overflow = push::overflow_policy::drop_newest;
    constexpr std::size_t egress_bound = 512;
    inproc_options transport;
    // A tiny transport queue makes the peer stop accepting frames quickly, so
    // the PUSH egress bound is what the pump actually runs into.
    transport.max_queued_bytes = 512;
    push_fixture fixture(egress_bound, subscribe, transport);
    CRPC_CHECK(fixture.srv != nullptr);

    raw_client slow(fixture.backend, fixture.ep());
    CRPC_CHECK(slow.valid());
    CRPC_CHECK(slow.subscribe("feed", 11, unreliable, false));
    slow.pause();

    constexpr int message_count = 32;
    std::vector<std::string> bodies;
    bodies.reserve(message_count);
    for (int i = 0; i < message_count; ++i) {
        bodies.push_back("msg-" + std::to_string(i) + "-" +
                         std::string(80, static_cast<char>('a' + (i % 26))));
    }

    const auto begin = std::chrono::steady_clock::now();
    for (const std::string& body : bodies) {
        CRPC_CHECK(fixture.srv->publish(
            "feed",
            span<const std::byte>(reinterpret_cast<const std::byte*>(body.data()),
                                  body.size()),
            unreliable));
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    // A hang guard, not a throughput claim: the subscriber above is paused, so the
    // whole point is that publishing never waits for it. If `publish` blocked, this
    // loop would not return at all; 2 s against a few microseconds of in-process work
    // is the margin that turns a hang into a failure instead of a ctest timeout
    // (plan.md D1, keep).
    CRPC_CHECK(elapsed < 2s);

    const server::statistics blocked = fixture.srv->stats();
    CRPC_CHECK(blocked.push_dropped_overflow > 0);
    CRPC_CHECK(blocked.push_egress_high_water <= egress_bound);
    CRPC_CHECK(blocked.push_queued_messages <= subscribe.max_messages);

    slow.resume();
    CRPC_CHECK(crpc_test::wait_for(
        [&fixture] {
            const server::statistics now = fixture.srv->stats();
            return now.push_queued_messages == 0 && now.push_egress_bytes == 0;
        },
        2000));
    std::this_thread::sleep_for(50ms);

    const std::vector<raw_client::push_record> records = slow.pushes();
    CRPC_CHECK(!records.empty());
    CRPC_CHECK(records.size() < static_cast<std::size_t>(message_count));
    const server::statistics final_stats = fixture.srv->stats();
    CRPC_CHECK_EQ(records.size() + final_stats.push_dropped_overflow,
                  static_cast<std::size_t>(message_count));
    for (std::size_t i = 0; i < records.size(); ++i) {
        // The retained messages are the prefix: drop_newest discards the tail,
        // and the pump delivers whatever it had already taken in order.
        CRPC_CHECK(i < bodies.size() && records[i].body == bodies[i]);
    }
    CRPC_CHECK_EQ(slow.decode_failures(), 0);
}

CRPC_TEST(push_close_detaches_before_teardown) {
    push_fixture fixture(1u << 20, push::subscribe_options{});
    CRPC_CHECK(fixture.srv != nullptr);
    std::unique_ptr<rpc::client> client = make_client(fixture);
    CRPC_CHECK(client != nullptr);

    sink_state sink;
    client->set_push_sink(&record_push, &sink);
    auto subscription = client->subscribe_push("lifecycle", 5, reliable, false);
    CRPC_CHECK(subscription.ok());
    CRPC_CHECK(crpc_test::wait_for(
        [&fixture] { return fixture.srv->stats().push_active_connections == 1; },
        2000));

    client.reset();
    CRPC_CHECK(crpc_test::wait_for(
        [&fixture] { return fixture.srv->stats().push_active_connections == 0; },
        2000));

    // The subscription itself survives (so a reconnect can resume); no push is
    // delivered to the dead connection.
    const std::string body = "nobody-home";
    CRPC_CHECK(fixture.srv->publish(
        "lifecycle",
        span<const std::byte>(reinterpret_cast<const std::byte*>(body.data()),
                              body.size()),
        reliable));
    std::this_thread::sleep_for(50ms);
    CRPC_CHECK_EQ(fixture.srv->stats().push_active_connections, static_cast<std::size_t>(0));
    CRPC_CHECK(fixture.srv->stats().push_queued_messages <= static_cast<std::size_t>(1));
}

} // namespace

CRPC_TEST_MAIN()
