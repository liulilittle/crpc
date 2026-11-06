// CRPC -- asynchronous logger implementation.
#include <crpc/observability/log.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <system_error>
#include <type_traits>

#if defined(CRPC_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace crpc {
namespace obs {
namespace {

thread_local log_ring* g_thread_ring = nullptr;

std::size_t round_up_power_of_two(std::size_t value) noexcept {
    if (value < 2) {
        return 2;
    }
    std::size_t result = 2;
    while (result < value) {
        if (result > std::numeric_limits<std::size_t>::max() / 2) {
            return result;
        }
        result <<= 1;
    }
    return result;
}

std::uint64_t wall_clock_ns() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

namespace {

// pthread_t is a pointer on glibc and macOS but an integer (`long`) on
// bionic, so a blanket reinterpret_cast is ill-formed on Android. A template
// is required, not a plain `if constexpr`: in a non-template context both
// branches are still type-checked, and the pointer cast on an integer type is
// a hard error. Inside a template the discarded branch is never instantiated.
template <typename ThreadId>
[[nodiscard]] std::uint64_t pthread_id_as_u64(ThreadId self) noexcept {
    if constexpr (std::is_pointer<ThreadId>::value) {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(self));
    } else {
        return static_cast<std::uint64_t>(self);
    }
}

} // namespace

std::uint64_t current_thread_id() noexcept {
#if defined(CRPC_OS_WINDOWS)
    return static_cast<std::uint64_t>(GetCurrentThreadId());
#else
    return pthread_id_as_u64(pthread_self());
#endif
}

void append_hex(log_buffer& out, const std::byte* bytes, std::size_t count) noexcept {
    static constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < count; ++i) {
        const u8 value = static_cast<u8>(bytes[i]);
        out.append(digits[value >> 4]);
        out.append(digits[value & 0x0F]);
    }
}

std::size_t format_record_line(const log_record& record, char* out,
                               std::size_t capacity) noexcept {
    log_buffer line(out, capacity);
    line.append_integer(static_cast<std::int64_t>(record.timestamp_ns));
    line.append(' ');
    line.append(to_string(record.level));
    line.append(" tid=");
    line.append_integer(static_cast<std::uint64_t>(record.thread_id));

    if (record.trace.valid()) {
        line.append(" trace=");
        append_hex(line, record.trace.bytes.data(), record.trace.bytes.size());
    }
    if (record.span.valid()) {
        line.append(" span=");
        append_hex(line, record.span.bytes.data(), record.span.bytes.size());
    }

    line.append(' ');
    line.append(std::string_view(record.message, record.message_len));
    for (std::size_t i = 0; i < record.field_count && i < log_record::max_fields; ++i) {
        const log_field& field = record.fields[i];
        line.append(' ');
        line.append(std::string_view(field.key, field.key_len));
        line.append('=');
        line.append(std::string_view(field.value, field.value_len));
        if ((field.truncated & log_record::flag_truncated) != 0) {
            line.append('~');
        }
    }
    if ((record.flags & log_record::flag_truncated) != 0) {
        line.append(" [truncated]");
    }
    line.append('\n');
    return line.size();
}

} // namespace

// --- Ring --------------------------------------------------------------------

// Single-producer/single-consumer ring. `head_` is written only by the
// producer, `tail_` only by the consumer; the atomic size is the synchronisation
// point. Unlike a head/tail-only ring this uses every slot, so the configured
// capacity is the number of records that fit before the overflow policy fires.
class log_ring {
public:
    explicit log_ring(std::size_t capacity) noexcept
        : slots_(new (std::nothrow) log_record[round_up_power_of_two(capacity)]),
          capacity_(round_up_power_of_two(capacity)),
          mask_(capacity_ - 1) {}

    [[nodiscard]] bool valid() const noexcept { return slots_ != nullptr; }

    void push(const log_record& record, overflow_policy policy) noexcept {
        for (;;) {
            const std::size_t size = size_.load(std::memory_order_acquire);
            if (size < capacity_) {
                slots_[head_] = record;
                head_ = (head_ + 1) & mask_;
                size_.fetch_add(1, std::memory_order_release);
                return;
            }

            if (policy == overflow_policy::block) {
                std::this_thread::yield();
                continue;
            }

            if (policy == overflow_policy::throttle) {
                bool made_room = false;
                for (int attempt = 0; attempt < 64; ++attempt) {
                    std::this_thread::yield();
                    if (size_.load(std::memory_order_acquire) < capacity_) {
                        made_room = true;
                        break;
                    }
                }
                if (made_room) {
                    continue;
                }
            }

            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    [[nodiscard]] bool pop(log_record& out) noexcept {
        if (size_.load(std::memory_order_acquire) == 0) {
            return false;
        }
        out = slots_[tail_];
        tail_ = (tail_ + 1) & mask_;
        size_.fetch_sub(1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    std::unique_ptr<log_record[]> slots_;
    std::size_t capacity_;
    std::size_t mask_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::atomic<std::size_t> size_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

// --- Record helpers ----------------------------------------------------------

log_record make_record(log_level level, std::string_view message,
                       const trace_context& trace) noexcept {
    log_record record;
    record.timestamp_ns = wall_clock_ns();
    record.thread_id = current_thread_id();
    record.trace = trace.trace;
    record.span = trace.span;
    record.level = level;
    record.field_count = 0;
    record.flags = 0;

    const std::size_t count =
        message.size() < log_record::max_message ? message.size() : log_record::max_message;
    if (count != 0) {
        std::memcpy(record.message, message.data(), count);
    }
    record.message_len = static_cast<u16>(count);
    if (count != message.size()) {
        record.flags = static_cast<u8>(record.flags | log_record::flag_truncated);
    }
    return record;
}

bool record_add_field(log_record& record, std::string_view key, std::string_view value) noexcept {
    if (record.field_count >= log_record::max_fields) {
        return false;
    }

    log_field& field = record.fields[record.field_count];
    const std::size_t key_count =
        key.size() < log_field::max_key ? key.size() : log_field::max_key;
    const std::size_t value_count =
        value.size() < log_field::max_value ? value.size() : log_field::max_value;

    if (key_count != 0) {
        std::memcpy(field.key, key.data(), key_count);
    }
    if (value_count != 0) {
        std::memcpy(field.value, value.data(), value_count);
    }
    field.key_len = static_cast<u8>(key_count);
    field.value_len = static_cast<u8>(value_count);
    field.truncated = (key_count != key.size() || value_count != value.size())
                          ? log_record::flag_truncated
                          : 0;

    ++record.field_count;
    return true;
}

// --- Logger ------------------------------------------------------------------

logger::logger() noexcept {
    file_opened_ = std::chrono::steady_clock::now();
}

logger::~logger() { shutdown(); }

void logger::configure(const logger_config& config) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    config_ = config;
    if (config_.ring_capacity < 2) {
        config_.ring_capacity = 2;
    }
    ring_capacity_.store(config_.ring_capacity, std::memory_order_relaxed);
    overflow_.store(static_cast<int>(config_.overflow), std::memory_order_relaxed);
}

void logger::start() {
    std::lock_guard<std::mutex> lock(config_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    ensure_drain_buffer();
    batch_.clear();
    compressed_.clear();

    if (config_.enable_file) {
        (void)open_file();
    }

    running_.store(true, std::memory_order_release);
    try {
        sink_ = std::thread(&logger::sink_loop, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        close_file();
    }
}

void logger::shutdown() noexcept {
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (was_running) {
        wait_cv_.notify_all();
        if (sink_.joinable()) {
            sink_.join();
        }
    } else {
        // No sink was ever started (or it already stopped). Drain synchronously
        // so records accepted before shutdown are not silently lost.
        ensure_drain_buffer();
        (void)open_file();
        try {
            (void)drain_all();
        } catch (...) {
            batch_.clear();
        }
    }
    close_file();
}

void logger::reset() {
    shutdown();

    {
        std::lock_guard<std::mutex> lock(rings_mutex_);
        rings_.clear();
    }
    // Only the calling thread's cache is refreshed. A thread that is inside
    // submit() at this moment, or that cached its ring before the reset, keeps
    // a dangling pointer that never self-heals (see the contract in log.hpp).
    g_thread_ring = nullptr;

    allocation_drops_.store(0, std::memory_order_relaxed);
    written_.store(0, std::memory_order_relaxed);
    level_.store(static_cast<int>(log_level::trace), std::memory_order_relaxed);
    redactor_.store(nullptr, std::memory_order_relaxed);
    redactor_user_.store(nullptr, std::memory_order_relaxed);
    compressor_.store(nullptr, std::memory_order_relaxed);
    compressor_user_.store(nullptr, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = logger_config{};
    ring_capacity_.store(config_.ring_capacity, std::memory_order_relaxed);
    overflow_.store(static_cast<int>(config_.overflow), std::memory_order_relaxed);
    drain_buffer_.reset();
    drain_capacity_ = 0;
    batch_.clear();
    compressed_.clear();
    file_size_ = 0;
}

log_ring* logger::current_ring() noexcept {
    if (g_thread_ring != nullptr) {
        return g_thread_ring;
    }
    return register_ring();
}

log_ring* logger::register_ring() noexcept {
    const std::size_t capacity = ring_capacity_.load(std::memory_order_relaxed);
    std::unique_ptr<log_ring> ring(new (std::nothrow) log_ring(capacity));
    if (!ring || !ring->valid()) {
        allocation_drops_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    log_ring* raw = ring.get();
    try {
        std::lock_guard<std::mutex> lock(rings_mutex_);
        rings_.push_back(std::move(ring));
    } catch (...) {
        allocation_drops_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    g_thread_ring = raw;
    return raw;
}

void logger::submit(const log_record& record) noexcept {
    log_ring* ring = current_ring();
    if (ring == nullptr) {
        return;
    }
    // `push` honours the configured overflow policy. With `block` this call may
    // spin forever if no sink is draining (before start(), after shutdown()):
    // see the overflow_policy contract in log.hpp before choosing it.
    ring->push(record, static_cast<overflow_policy>(overflow_.load(std::memory_order_relaxed)));
}

bool logger::add_field(log_record& record, std::string_view key,
                       std::string_view value) noexcept {
    char buffer[log_field::max_value];
    std::size_t length = value.size() < sizeof(buffer) ? value.size() : sizeof(buffer);
    if (length != 0) {
        std::memcpy(buffer, value.data(), length);
    }

    const redact_fn function = redactor_.load(std::memory_order_relaxed);
    if (function != nullptr) {
        function(redactor_user_.load(std::memory_order_relaxed), key.data(), key.size(), buffer,
                 sizeof(buffer), &length);
        if (length > sizeof(buffer)) {
            length = sizeof(buffer);
        }
    }
    return record_add_field(record, key, std::string_view(buffer, length));
}

std::uint64_t logger::dropped_total() const {
    std::uint64_t total = allocation_drops_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(rings_mutex_);
    for (const auto& ring : rings_) {
        total += ring->dropped();
    }
    return total;
}

void logger::ensure_drain_buffer() noexcept {
    if (drain_buffer_ != nullptr && drain_capacity_ != 0) {
        return;
    }
    drain_capacity_ = 256;
    drain_buffer_.reset(new (std::nothrow) log_record[drain_capacity_]);
    if (!drain_buffer_) {
        drain_capacity_ = 0;
        return;
    }
    if (batch_.capacity() < 64u * 1024u) {
        try {
            batch_.reserve(64u * 1024u);
        } catch (...) {
            // The sink can still operate with a smaller buffer.
        }
    }
}

void logger::sink_loop() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        bool drained = false;
        try {
            drained = drain_all();
        } catch (...) {
            batch_.clear();
        }
        if (!drained) {
            std::unique_lock<std::mutex> lock(wait_mutex_);
            wait_cv_.wait_for(lock, std::chrono::milliseconds(1));
        }
    }

    try {
        (void)drain_all();
    } catch (...) {
        batch_.clear();
    }
    close_file();
}

bool logger::drain_all() {
    if (drain_buffer_ == nullptr || drain_capacity_ == 0) {
        return false;
    }

    bool drained = false;
    for (;;) {
        std::size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(rings_mutex_);
            for (auto& ring : rings_) {
                log_record record;
                while (count < drain_capacity_ && ring->pop(record)) {
                    drain_buffer_[count] = record;
                    ++count;
                    written_.fetch_add(1, std::memory_order_relaxed);
                }
                if (count == drain_capacity_) {
                    break;
                }
            }
        }

        if (count == 0) {
            break;
        }
        drained = true;

        for (std::size_t i = 0; i < count; ++i) {
            char line[1024];
            const std::size_t length = format_record_line(drain_buffer_[i], line, sizeof(line));
            if (config_.max_file_size != 0 && !batch_.empty() &&
                file_size_ + batch_.size() + length > config_.max_file_size) {
                write_batch(batch_.data(), batch_.size());
                batch_.clear();
            }
            batch_.append(line, length);
        }

        if (count == drain_capacity_ || batch_.size() >= 256u * 1024u) {
            write_batch(batch_.data(), batch_.size());
            batch_.clear();
        }
    }

    if (!batch_.empty()) {
        write_batch(batch_.data(), batch_.size());
        batch_.clear();
    }
    return drained;
}

bool logger::open_file() noexcept {
    if (!config_.enable_file || config_.file_path.empty()) {
        return false;
    }
#if defined(CRPC_OS_WINDOWS)
    if (file_handle_ != nullptr) {
        return true;
    }
    HANDLE handle = CreateFileA(config_.file_path.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    file_handle_ = handle;
    LARGE_INTEGER size{};
    file_size_ = GetFileSizeEx(handle, &size) ? static_cast<std::uint64_t>(size.QuadPart) : 0;
#else
    if (file_fd_ >= 0) {
        return true;
    }
    const int fd = ::open(config_.file_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return false;
    }
    file_fd_ = fd;
    struct stat status {};
    file_size_ = ::fstat(fd, &status) == 0 ? static_cast<std::uint64_t>(status.st_size) : 0;
#endif
    file_opened_ = std::chrono::steady_clock::now();
    return true;
}

void logger::close_file() noexcept {
#if defined(CRPC_OS_WINDOWS)
    if (file_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = nullptr;
    }
#else
    if (file_fd_ >= 0) {
        ::close(file_fd_);
        file_fd_ = -1;
    }
#endif
}

void logger::rotate_file() noexcept {
    close_file();
    if (!config_.file_path.empty()) {
        const std::string rotated = config_.file_path + ".1";
#if defined(CRPC_OS_WINDOWS)
        (void)MoveFileExA(config_.file_path.c_str(), rotated.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
        (void)::rename(config_.file_path.c_str(), rotated.c_str());
#endif
    }
    file_size_ = 0;
    (void)open_file();
}

void logger::write_bytes(const char* data, std::size_t size) noexcept {
    if (data == nullptr || size == 0) {
        return;
    }
#if defined(CRPC_OS_WINDOWS)
    if (file_handle_ == nullptr) {
        return;
    }
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t remaining = size - offset;
        const DWORD chunk = static_cast<DWORD>(remaining > 1024u * 1024u ? 1024u * 1024u : remaining);
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(file_handle_), data + offset, chunk, &written, nullptr)) {
            return;
        }
        if (written == 0) {
            return;
        }
        offset += written;
    }
#else
    if (file_fd_ < 0) {
        return;
    }
    std::size_t offset = 0;
    while (offset < size) {
        struct iovec vector;
        vector.iov_base = const_cast<char*>(data + offset);
        vector.iov_len = size - offset;
        const ssize_t written = ::writev(file_fd_, &vector, 1);
        if (written <= 0) {
            return;
        }
        offset += static_cast<std::size_t>(written);
    }
#endif
}

void logger::write_batch(const char* data, std::size_t size) {
    if (size == 0 || !config_.enable_file) {
        return;
    }

    const char* output = data;
    std::size_t output_size = size;

    const compress_fn compressor = compressor_.load(std::memory_order_relaxed);
    if (compressor != nullptr) {
        compressed_.resize(size + 1024);
        const std::size_t compressed_size =
            compressor(compressor_user_.load(std::memory_order_relaxed), data, size,
                       compressed_.data(), compressed_.size());
        if (compressed_size > 0 && compressed_size <= compressed_.size()) {
            output = compressed_.data();
            output_size = compressed_size;
        }
    }

    const bool rotate_by_size = config_.max_file_size != 0 && file_size_ != 0 &&
                                file_size_ + output_size > config_.max_file_size;
    const bool rotate_by_time =
        config_.rotate_interval.count() > 0 &&
        std::chrono::steady_clock::now() - file_opened_ >= config_.rotate_interval;
    if (rotate_by_size || rotate_by_time) {
        rotate_file();
    }

    write_bytes(output, output_size);
    file_size_ += output_size;
}

// --- Event builder -----------------------------------------------------------

log_event::log_event(log_level level, std::string_view message) noexcept : submit_(false) {
    if constexpr (detail::compile_max_level() < static_cast<int>(log_level::off)) {
        if (static_cast<int>(level) >= detail::compile_max_level()) {
            logger& sink = logger::instance();
            if (sink.enabled(level)) {
                record_ = make_record(level, message);
                submit_ = true;
            }
        }
    } else {
        (void)level;
        (void)message;
    }
}

log_event::~log_event() {
    if (submit_) {
        logger::instance().submit(record_);
    }
}

log_event& log_event::field(std::string_view key, std::string_view value) noexcept {
    if (submit_) {
        (void)logger::instance().add_field(record_, key, value);
    }
    return *this;
}

log_event& log_event::field(std::string_view key, std::int64_t value) noexcept {
    if (!submit_) {
        return *this;
    }
    char buffer[32];
    log_buffer out(buffer, sizeof(buffer));
    out.append_integer(value);
    return field(key, std::string_view(buffer, out.size()));
}

log_event& log_event::field(std::string_view key, std::uint64_t value) noexcept {
    if (!submit_) {
        return *this;
    }
    char buffer[32];
    log_buffer out(buffer, sizeof(buffer));
    out.append_integer(value);
    return field(key, std::string_view(buffer, out.size()));
}

} // namespace obs
} // namespace crpc
