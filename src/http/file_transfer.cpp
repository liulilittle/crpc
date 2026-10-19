#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <crpc/http/file_transfer.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace crpc::http {
namespace {

// --- SHA-256 ----------------------------------------------------------------

class sha256 {
public:
    sha256() noexcept
        : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu,
                 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

    void update(const void* data, std::size_t length) noexcept {
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
        total_ += length;
        while (length != 0) {
            const std::size_t take =
                length < (64u - block_len_) ? length : (64u - block_len_);
            std::memcpy(block_.data() + block_len_, bytes, take);
            block_len_ += take;
            bytes += take;
            length -= take;
            if (block_len_ == 64u) {
                compress(block_.data());
                block_len_ = 0;
            }
        }
    }

    [[nodiscard]] std::string hex() const {
        sha256 copy = *this;
        return copy.hex_final();
    }

private:
    static std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
        return (value >> count) | (value << (32u - count));
    }

    void compress(const std::uint8_t* block) noexcept {
        static const std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
            0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
            0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
            0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
            0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
            0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
            0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
            0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
            0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const std::size_t o = static_cast<std::size_t>(i) * 4u;
            w[i] = (static_cast<std::uint32_t>(block[o]) << 24) |
                   (static_cast<std::uint32_t>(block[o + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[o + 2]) << 8) |
                   static_cast<std::uint32_t>(block[o + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^
                                     (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^
                                     (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];

        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 =
                rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 =
                rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    [[nodiscard]] std::string hex_final() noexcept {
        const std::uint64_t bit_length = total_ * 8u;
        std::array<std::uint8_t, 64> tail{};
        std::size_t tail_len = block_len_;
        std::memcpy(tail.data(), block_.data(), block_len_);
        tail[tail_len++] = 0x80u;
        if (tail_len > 56u) {
            while (tail_len < 64u) {
                tail[tail_len++] = 0;
            }
            compress(tail.data());
            tail_len = 0;
        }
        while (tail_len < 56u) {
            tail[tail_len++] = 0;
        }
        for (int i = 7; i >= 0; --i) {
            tail[tail_len++] = static_cast<std::uint8_t>(
                (bit_length >> (static_cast<unsigned>(i) * 8u)) & 0xffu);
        }
        compress(tail.data());

        static const char digits[] = "0123456789abcdef";
        std::string out;
        out.resize(64);
        for (std::size_t i = 0; i < 8; ++i) {
            const std::uint32_t word = state_[i];
            for (std::size_t j = 0; j < 4; ++j) {
                const std::uint8_t byte = static_cast<std::uint8_t>(
                    (word >> ((3u - j) * 8u)) & 0xffu);
                out[i * 8u + j * 2u] = digits[byte >> 4];
                out[i * 8u + j * 2u + 1u] = digits[byte & 0x0fu];
            }
        }
        return out;
    }

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_len_ = 0;
    std::uint64_t total_ = 0;
};

// --- Range parsing ----------------------------------------------------------

std::string_view trim_ows(std::string_view text) noexcept {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

bool parse_u64(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t result = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
            return false;
        }
        result = result * 10u + digit;
    }
    value = result;
    return true;
}

// --- Temporary files --------------------------------------------------------

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

void fill_suffix(std::uint64_t seed, char* out, std::size_t out_size) noexcept {
    static const char digits[] = "0123456789abcdef";
    std::size_t written = 0;
    for (int half = 0; half < 2; ++half) {
        const std::uint64_t value = splitmix64(seed);
        for (int nibble = 15; nibble >= 0; --nibble) {
            if (written + 1 < out_size) {
                out[written++] = digits[(value >> (nibble * 4)) & 0x0full];
            }
        }
    }
    out[written] = '\0';
}

std::FILE* open_exclusive(const std::filesystem::path& path, int& error) noexcept {
    error = 0;
#if defined(_WIN32)
    const int fd = ::_wopen(path.c_str(),
                            _O_BINARY | _O_CREAT | _O_EXCL | _O_WRONLY,
                            _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        error = errno;
        return nullptr;
    }
    std::FILE* file = ::_wfdopen(fd, L"wb");
    if (file == nullptr) {
        error = errno;
        (void)::_close(fd);
    }
    return file;
#else
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) {
        error = errno;
        return nullptr;
    }
    std::FILE* file = ::fdopen(fd, "wb");
    if (file == nullptr) {
        error = errno;
        (void)::close(fd);
    }
    return file;
#endif
}

std::FILE* open_read(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    return ::_wfopen(path.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

} // namespace

struct upload_sink::hasher final : sha256 {};

// --- file_part --------------------------------------------------------------

file_part::file_part(file_part&& other) noexcept
    : name(std::move(other.name)),
      filename(std::move(other.filename)),
      content_type(std::move(other.content_type)),
      sha256(std::move(other.sha256)),
      size(other.size),
      data(std::move(other.data)),
      temp_path(std::move(other.temp_path)),
      kept_(other.kept_) {
    other.temp_path.clear();
    other.kept_ = true;
    other.size = 0;
}

file_part& file_part::operator=(file_part&& other) noexcept {
    if (this != &other) {
        if (!kept_) {
            drop_temp();
        }
        name = std::move(other.name);
        filename = std::move(other.filename);
        content_type = std::move(other.content_type);
        sha256 = std::move(other.sha256);
        size = other.size;
        data = std::move(other.data);
        temp_path = std::move(other.temp_path);
        kept_ = other.kept_;
        other.temp_path.clear();
        other.kept_ = true;
        other.size = 0;
    }
    return *this;
}

file_part::~file_part() {
    if (!kept_) {
        drop_temp();
    }
}

void file_part::drop_temp() noexcept {
    if (!temp_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(temp_path, ec);
        temp_path.clear();
    }
}

void file_part::remove() noexcept {
    drop_temp();
    data.clear();
}

bool file_part::exists() const {
    if (temp_path.empty()) {
        return true;
    }
    std::error_code ec;
    return std::filesystem::exists(temp_path, ec);
}

// --- upload_sink ------------------------------------------------------------

upload_sink::upload_sink(std::string name, std::string filename,
                         std::string content_type, upload_options options)
    : name_(std::move(name)),
      filename_(std::move(filename)),
      content_type_(std::move(content_type)),
      options_(std::move(options)) {
    if (options_.compute_sha256) {
        hasher_ = new (std::nothrow) hasher();
        if (hasher_ == nullptr) {
            options_.compute_sha256 = false;
        }
    }
}

upload_sink::~upload_sink() {
    if (!finalized_) {
        cancel();
    } else {
        close_file();
    }
    delete hasher_;
    hasher_ = nullptr;
}

status upload_sink::fail(rpc_code code, std::string message) {
    failed_ = true;
    error_ = rpc_error(code, std::move(message));
    return status{error_};
}

void upload_sink::close_file() noexcept {
    if (file_ != nullptr) {
        (void)std::fclose(file_);
        file_ = nullptr;
    }
}

status upload_sink::ensure_spill() {
    if (file_ != nullptr) {
        return status{};
    }
    std::filesystem::path directory = options_.temp_directory;
    std::error_code ec;
    if (directory.empty()) {
        directory = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return fail(rpc_code::internal,
                        "cannot resolve temporary directory: " + ec.message());
        }
    }
    if (!std::filesystem::exists(directory, ec)) {
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            return fail(rpc_code::internal,
                        "cannot create temporary directory: " + ec.message());
        }
    }

    std::uint64_t seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    seed ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this));
#if defined(_WIN32)
    seed ^= static_cast<std::uint64_t>(::_getpid()) << 32;
#else
    seed ^= static_cast<std::uint64_t>(::getpid()) << 32;
#endif

    for (int attempt = 0; attempt < 64; ++attempt) {
        char suffix[40];
        fill_suffix(seed + static_cast<std::uint64_t>(attempt), suffix,
                    sizeof(suffix));
        std::filesystem::path candidate = directory / ("crpc-upload-" +
                                                        std::string(suffix) +
                                                        ".tmp");
        int open_error = 0;
        std::FILE* file = open_exclusive(candidate, open_error);
        if (file != nullptr) {
            file_ = file;
            temp_path_ = std::move(candidate);
            if (!memory_.empty()) {
                const status written = write_file(span<const std::byte>(
                    reinterpret_cast<const std::byte*>(memory_.data()),
                    memory_.size()));
                if (!written.ok()) {
                    return written;
                }
                std::string().swap(memory_);
            }
            return status{};
        }
        if (open_error != EEXIST) {
            return fail(rpc_code::internal,
                        "cannot create temporary file (errno " +
                            std::to_string(open_error) + ")");
        }
    }
    return fail(rpc_code::resource_exhausted,
                "cannot allocate a unique temporary file name");
}

status upload_sink::write_file(span<const std::byte> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t written =
            std::fwrite(data.data() + offset, 1, data.size() - offset, file_);
        if (written == 0) {
            return fail(rpc_code::internal, "temporary file write failed");
        }
        offset += written;
    }
    return status{};
}

status upload_sink::write(span<const std::byte> data) {
    if (failed_) {
        return status{error_};
    }
    if (canceled_ || finalized_) {
        return status{rpc_error(rpc_code::failed_precondition,
                                "upload sink is closed")};
    }
    const std::uint64_t add = static_cast<std::uint64_t>(data.size());
    if (add > options_.max_part_bytes ||
        size_ > options_.max_part_bytes - add) {
        return fail(rpc_code::resource_exhausted,
                    "upload part exceeds max_part_bytes");
    }
    if (file_ == nullptr &&
        size_ + add <= static_cast<std::uint64_t>(options_.memory_threshold)) {
        if (add != 0) {
            memory_.append(reinterpret_cast<const char*>(data.data()),
                           data.size());
        }
    } else {
        status result = ensure_spill();
        if (!result.ok()) {
            return result;
        }
        result = write_file(data);
        if (!result.ok()) {
            return result;
        }
    }
    size_ += add;
    if (hasher_ != nullptr && add != 0) {
        hasher_->update(data.data(), data.size());
    }
    return status{};
}

result<file_part> upload_sink::finalize() {
    if (failed_) {
        return error_;
    }
    if (canceled_ || finalized_) {
        return rpc_error(rpc_code::failed_precondition,
                         "upload sink is closed");
    }
    if (file_ != nullptr) {
        if (std::fflush(file_) != 0) {
            return fail(rpc_code::internal, "temporary file flush failed")
                .error();
        }
        close_file();
    }
    finalized_ = true;

    file_part part;
    part.name = std::move(name_);
    part.filename = std::move(filename_);
    part.content_type = std::move(content_type_);
    part.size = size_;
    if (file_ != nullptr && !temp_path_.empty()) {
        part.temp_path = std::move(temp_path_);
    } else if (!temp_path_.empty()) {
        part.temp_path = std::move(temp_path_);
    } else {
        part.data = std::move(memory_);
    }
    if (hasher_ != nullptr) {
        part.sha256 = hasher_->hex();
    }
    return part;
}

void upload_sink::cancel() noexcept {
    if (finalized_) {
        return;
    }
    canceled_ = true;
    close_file();
    if (!temp_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(temp_path_, ec);
        temp_path_.clear();
    }
    std::string().swap(memory_);
}

// --- Range and download -----------------------------------------------------

range_parse_result parse_range_header(std::string_view value,
                                      std::uint64_t entity_size) {
    range_parse_result result;
    const std::string_view trimmed = trim_ows(value);
    if (trimmed.empty()) {
        result.kind = range_kind::none;
        return result;
    }
    const std::size_t equals = trimmed.find('=');
    if (equals == std::string_view::npos) {
        // No unit at all: syntactically invalid, ignore the header (200).
        result.kind = range_kind::invalid;
        return result;
    }
    const std::string_view unit = trim_ows(trimmed.substr(0, equals));
    if (!ascii_iequals(unit, "bytes")) {
        // Unknown units are ignored (RFC 9110 14.2).
        result.kind = range_kind::none;
        return result;
    }
    const std::string_view spec = trim_ows(trimmed.substr(equals + 1));
    if (spec.empty()) {
        result.kind = range_kind::invalid;
        return result;
    }
    if (spec.find(',') != std::string_view::npos) {
        // Multiple ranges require multipart/byteranges, which is not
        // implemented; ignore the header and serve the full entity (200).
        result.kind = range_kind::multiple;
        return result;
    }
    const std::size_t dash = spec.find('-');
    if (dash == std::string_view::npos) {
        result.kind = range_kind::invalid;
        return result;
    }
    const std::string_view first_text = spec.substr(0, dash);
    const std::string_view last_text = spec.substr(dash + 1);

    if (first_text.empty()) {
        std::uint64_t suffix = 0;
        if (!parse_u64(last_text, suffix)) {
            result.kind = range_kind::invalid;
            return result;
        }
        if (suffix == 0 || entity_size == 0) {
            // A zero-length suffix or an empty entity is unsatisfiable.
            result.kind = range_kind::unsatisfiable;
            return result;
        }
        result.first = suffix >= entity_size ? 0 : entity_size - suffix;
        result.last = entity_size - 1;
        result.length = entity_size - result.first;
        result.kind = range_kind::satisfiable;
        return result;
    }

    std::uint64_t first = 0;
    if (!parse_u64(first_text, first)) {
        result.kind = range_kind::invalid;
        return result;
    }
    if (first >= entity_size) {
        // Well-formed but out of range: unsatisfiable (416).
        result.kind = range_kind::unsatisfiable;
        return result;
    }
    std::uint64_t last = entity_size - 1;
    if (!last_text.empty()) {
        if (!parse_u64(last_text, last)) {
            result.kind = range_kind::invalid;
            return result;
        }
        if (last < first) {
            // `last < first` is syntactically invalid: ignore (200).
            result.kind = range_kind::invalid;
            return result;
        }
        if (last >= entity_size) {
            last = entity_size - 1;
        }
    }
    result.first = first;
    result.last = last;
    result.length = last - first + 1;
    result.kind = range_kind::satisfiable;
    return result;
}

download_source::download_source(download_source&& other) noexcept
    : memory_(std::move(other.memory_)),
      content_range_(std::move(other.content_range_)),
      path_(std::move(other.path_)),
      file_(other.file_),
      size_(other.size_),
      first_(other.first_),
      remaining_(other.remaining_),
      range_length_(other.range_length_),
      position_(other.position_),
      status_code_(other.status_code_) {
    other.file_ = nullptr;
    other.size_ = 0;
    other.remaining_ = 0;
    other.range_length_ = 0;
}

download_source& download_source::operator=(download_source&& other) noexcept {
    if (this != &other) {
        if (file_ != nullptr) {
            (void)std::fclose(file_);
        }
        memory_ = std::move(other.memory_);
        content_range_ = std::move(other.content_range_);
        path_ = std::move(other.path_);
        file_ = other.file_;
        size_ = other.size_;
        first_ = other.first_;
        remaining_ = other.remaining_;
        range_length_ = other.range_length_;
        position_ = other.position_;
        status_code_ = other.status_code_;
        other.file_ = nullptr;
        other.size_ = 0;
        other.remaining_ = 0;
        other.range_length_ = 0;
    }
    return *this;
}

download_source::~download_source() {
    if (file_ != nullptr) {
        (void)std::fclose(file_);
        file_ = nullptr;
    }
}

result<download_source> download_source::open_file(
    const std::filesystem::path& path) {
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return rpc_error(rpc_code::not_found,
                         "cannot stat download file: " + ec.message());
    }
    std::FILE* file = open_read(path);
    if (file == nullptr) {
        return rpc_error(rpc_code::not_found, "cannot open download file");
    }
    download_source source;
    source.file_ = file;
    source.path_ = path;
    source.size_ = size;
    source.first_ = 0;
    source.remaining_ = size;
    source.range_length_ = size;
    source.position_ = 0;
    source.status_code_ = 200;
    return source;
}

result<download_source> download_source::from_memory(std::string_view bytes) {
    download_source source;
    source.memory_.assign(bytes.data(), bytes.size());
    source.size_ = static_cast<std::uint64_t>(bytes.size());
    source.first_ = 0;
    source.remaining_ = source.size_;
    source.range_length_ = source.size_;
    source.position_ = 0;
    source.status_code_ = 200;
    return source;
}

status download_source::seek_to(std::uint64_t offset) {
    if (file_ != nullptr) {
#if defined(_WIN32)
        if (::_fseeki64(file_, static_cast<__int64>(offset), SEEK_SET) != 0) {
            return status{rpc_error(rpc_code::internal,
                                    "download file seek failed")};
        }
#else
        if (::fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) {
            return status{rpc_error(rpc_code::internal,
                                    "download file seek failed")};
        }
#endif
    }
    position_ = offset;
    return status{};
}

status download_source::apply_range(std::string_view range_header) {
    const range_parse_result parsed = parse_range_header(range_header, size_);
    if (parsed.kind == range_kind::none ||
        parsed.kind == range_kind::invalid ||
        parsed.kind == range_kind::multiple) {
        first_ = 0;
        range_length_ = size_;
        remaining_ = size_;
        status_code_ = 200;
        content_range_.clear();
    } else if (parsed.kind == range_kind::satisfiable) {
        first_ = parsed.first;
        range_length_ = parsed.length;
        remaining_ = parsed.length;
        status_code_ = 206;
        content_range_ = "bytes " + std::to_string(parsed.first) + "-" +
                         std::to_string(parsed.last) + "/" +
                         std::to_string(size_);
    } else {
        first_ = 0;
        range_length_ = 0;
        remaining_ = 0;
        status_code_ = 416;
        content_range_ = "bytes */" + std::to_string(size_);
    }
    return rewind();
}

status download_source::rewind() {
    remaining_ = range_length_;
    return seek_to(first_);
}

status download_source::read(span<std::byte> out, std::size_t& count) {
    count = 0;
    if (remaining_ == 0 || out.empty()) {
        return status{};
    }
    std::uint64_t wanted = static_cast<std::uint64_t>(out.size());
    if (wanted > remaining_) {
        wanted = remaining_;
    }
    std::size_t produced = 0;
    if (file_ != nullptr) {
        produced = std::fread(out.data(), 1, static_cast<std::size_t>(wanted),
                              file_);
        if (produced == 0) {
            if (std::ferror(file_) != 0) {
                return status{rpc_error(rpc_code::internal,
                                        "download file read failed")};
            }
            remaining_ = 0;
            return status{rpc_error(rpc_code::eof,
                                    "download file truncated")};
        }
    } else {
        const std::uint64_t offset = position_;
        if (offset > size_) {
            return status{rpc_error(rpc_code::internal,
                                    "download position out of range")};
        }
        std::uint64_t available = size_ - offset;
        if (available > wanted) {
            available = wanted;
        }
        produced = static_cast<std::size_t>(available);
        std::memcpy(out.data(), memory_.data() + offset, produced);
    }
    remaining_ -= produced;
    position_ += produced;
    count = produced;
    return status{};
}

} // namespace crpc::http
