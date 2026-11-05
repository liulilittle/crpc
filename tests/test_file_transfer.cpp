// CRPC -- upload sink and download source tests.
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <crpc/http/file_transfer.hpp>

#include "test_framework.hpp"

namespace {

using crpc::http::download_source;
using crpc::http::file_part;
using crpc::http::parse_range_header;
using crpc::http::range_kind;
using crpc::http::upload_options;
using crpc::http::upload_sink;
using crpc::rpc_code;
using crpc::span;
using crpc::status;

struct temp_root {
    std::filesystem::path path;
    temp_root() {
        std::error_code ec;
        // A per-process directory: two copies of this executable (ctest -j, or a
        // parallel build tree) otherwise delete each other's fixtures and count
        // each other's files, which shows up as spurious delete/leak failures
        // under --repeat or -j. Same pattern as test_http_download.cpp.
        const auto pid = static_cast<unsigned long long>(
#if defined(_WIN32)
            ::GetCurrentProcessId()
#else
            ::getpid()
#endif
        );
        path = std::filesystem::temp_directory_path(ec) /
               ("crpc_ft_tests_" + std::to_string(pid));
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~temp_root() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

temp_root& root() {
    static temp_root instance;
    return instance;
}

span<const std::byte> as_span(const std::string& text) {
    return span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

std::size_t count_files() {
    std::size_t count = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(root().path, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        ++count;
    }
    return count;
}

std::string drain(download_source& source) {
    std::string out;
    std::byte buffer[3];
    for (;;) {
        std::size_t count = 0;
        status st = source.read(span<std::byte>(buffer, sizeof(buffer)), count);
        CRPC_CHECK(st.ok());
        if (count == 0) {
            break;
        }
        out.append(reinterpret_cast<const char*>(buffer), count);
    }
    return out;
}

upload_options memory_options(std::size_t threshold) {
    upload_options options;
    options.memory_threshold = threshold;
    options.temp_directory = root().path;
    return options;
}

} // namespace

CRPC_TEST(file_transfer_memory_path) {
    upload_sink sink("field", "a.txt", "text/plain", memory_options(1024));
    CRPC_CHECK(sink.write(as_span("hello world")).ok());
    CRPC_CHECK_FALSE(sink.spilled());
    CRPC_CHECK_EQ(std::uint64_t{11}, sink.size());
    auto finalized = sink.finalize();
    CRPC_CHECK(finalized.ok());
    file_part part = std::move(finalized.value());
    CRPC_CHECK(part.in_memory());
    CRPC_CHECK_EQ(std::uint64_t{11}, part.size);
    CRPC_CHECK_EQ(std::string("hello world"), part.data);
    CRPC_CHECK_EQ(std::string("field"), part.name);
    CRPC_CHECK_EQ(std::string("a.txt"), part.filename);
    CRPC_CHECK_EQ(std::string("text/plain"), part.content_type);
    CRPC_CHECK(part.temp_path.empty());
}

CRPC_TEST(file_transfer_spill_path) {
    upload_sink sink("f", "big.bin", "application/octet-stream",
                     memory_options(4));
    CRPC_CHECK(sink.write(as_span("0123456789")).ok());
    CRPC_CHECK(sink.spilled());
    CRPC_CHECK_EQ(std::uint64_t{10}, sink.size());
    auto finalized = sink.finalize();
    CRPC_CHECK(finalized.ok());
    file_part part = std::move(finalized.value());
    CRPC_CHECK_FALSE(part.in_memory());
    CRPC_CHECK(part.exists());
    CRPC_CHECK_EQ(std::string("0123456789"), read_file(part.temp_path));
    const std::filesystem::path path = part.temp_path;
    part.keep();
    part = file_part{};
    CRPC_CHECK(std::filesystem::exists(path));
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

CRPC_TEST(file_transfer_raii_delete_on_destroy) {
    std::filesystem::path path;
    {
        upload_sink sink("f", "x", "text/plain", memory_options(2));
        CRPC_CHECK(sink.write(as_span("abcdef")).ok());
        auto finalized = sink.finalize();
        CRPC_CHECK(finalized.ok());
        file_part part = std::move(finalized.value());
        path = part.temp_path;
        CRPC_CHECK(std::filesystem::exists(path));
    }
    CRPC_CHECK_FALSE(std::filesystem::exists(path));
}

CRPC_TEST(file_transfer_delete_on_cancel) {
    const std::size_t before = count_files();
    {
        upload_sink sink("f", "x", "text/plain", memory_options(2));
        CRPC_CHECK(sink.write(as_span("abcdef")).ok());
        CRPC_CHECK(sink.spilled());
        sink.cancel();
        CRPC_CHECK_EQ(before, count_files());
    }
    CRPC_CHECK_EQ(before, count_files());
}

CRPC_TEST(file_transfer_delete_on_connection_loss) {
    const std::size_t before = count_files();
    {
        upload_sink sink("f", "x", "text/plain", memory_options(2));
        CRPC_CHECK(sink.write(as_span("abcdef")).ok());
        CRPC_CHECK(sink.spilled());
        // No finalize: this models a connection loss. The destructor must
        // remove the temporary file.
    }
    CRPC_CHECK_EQ(before, count_files());
}

CRPC_TEST(file_transfer_owner_deletes_after_finalize) {
    upload_sink sink("f", "x", "text/plain", memory_options(2));
    CRPC_CHECK(sink.write(as_span("abcdef")).ok());
    auto finalized = sink.finalize();
    CRPC_CHECK(finalized.ok());
    file_part part = std::move(finalized.value());
    const std::filesystem::path path = part.temp_path;
    CRPC_CHECK(!path.empty());
    CRPC_CHECK(std::filesystem::exists(path));
    // Destroying the sink must not delete a file owned by the part.
    part.keep();
    CRPC_CHECK(std::filesystem::exists(path));
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

CRPC_TEST(file_transfer_destructor_removes_unfinalized) {
    const std::size_t before = count_files();
    {
        upload_sink sink("g", "y", "text/plain", memory_options(2));
        CRPC_CHECK(sink.write(as_span("ghijkl")).ok());
        CRPC_CHECK(sink.spilled());
    }
    CRPC_CHECK_EQ(before, count_files());
}

CRPC_TEST(file_transfer_limit) {
    upload_options options = memory_options(1024);
    options.max_part_bytes = 4;
    upload_sink sink("f", "x", "text/plain", options);
    status st = sink.write(as_span("12345"));
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(rpc_code::resource_exhausted == st.code());
}

CRPC_TEST(file_transfer_sha256) {
    upload_options options = memory_options(1024);
    options.compute_sha256 = true;
    upload_sink sink("f", "x", "text/plain", options);
    CRPC_CHECK(sink.write(as_span("abc")).ok());
    auto finalized = sink.finalize();
    CRPC_CHECK(finalized.ok());
    file_part part = std::move(finalized.value());
    CRPC_CHECK_EQ(
        std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        part.sha256);
}

CRPC_TEST(file_transfer_unique_temp_names) {
    upload_options options = memory_options(0);
    upload_sink first("f", "a", "text/plain", options);
    upload_sink second("f", "b", "text/plain", options);
    CRPC_CHECK(first.write(as_span("aaa")).ok());
    CRPC_CHECK(second.write(as_span("bbb")).ok());
    auto one = first.finalize();
    auto two = second.finalize();
    CRPC_CHECK(one.ok() && two.ok());
    file_part a = std::move(one.value());
    file_part b = std::move(two.value());
    CRPC_CHECK(a.temp_path != b.temp_path);
    CRPC_CHECK_EQ(std::string("aaa"), read_file(a.temp_path));
    CRPC_CHECK_EQ(std::string("bbb"), read_file(b.temp_path));
    a.keep();
    b.keep();
    std::error_code ec;
    std::filesystem::remove(a.temp_path, ec);
    std::filesystem::remove(b.temp_path, ec);
}

CRPC_TEST(file_transfer_range_parser) {
    CRPC_CHECK(range_kind::none == parse_range_header("", 10).kind);
    CRPC_CHECK(range_kind::none == parse_range_header("items=0-1", 10).kind);
    CRPC_CHECK(range_kind::satisfiable == parse_range_header("bytes=2-5", 10).kind);
    CRPC_CHECK(range_kind::satisfiable == parse_range_header("bytes=4-", 10).kind);
    CRPC_CHECK(range_kind::satisfiable == parse_range_header("bytes=-3", 10).kind);
    CRPC_CHECK(range_kind::unsatisfiable == parse_range_header("bytes=20-30", 10).kind);
    CRPC_CHECK(range_kind::unsatisfiable == parse_range_header("bytes=-0", 10).kind);
    // Syntactically invalid / multiple ranges are ignored (RFC 9110 14.1.2);
    // the caller serves 200 with the full entity.
    CRPC_CHECK(range_kind::multiple == parse_range_header("bytes=0-1,3-4", 10).kind);
    CRPC_CHECK(range_kind::invalid == parse_range_header("bytes=abc", 10).kind);
    CRPC_CHECK(range_kind::invalid == parse_range_header("bytes=5-3", 10).kind);
    CRPC_CHECK(range_kind::invalid == parse_range_header("bytes", 10).kind);
    CRPC_CHECK(range_kind::satisfiable == parse_range_header("bytes=1-", 10).kind);

    const auto full = parse_range_header("bytes=0-999", 10);
    CRPC_CHECK(range_kind::satisfiable == full.kind);
    CRPC_CHECK_EQ(std::uint64_t{0}, full.first);
    CRPC_CHECK_EQ(std::uint64_t{9}, full.last);
    CRPC_CHECK_EQ(std::uint64_t{10}, full.length);

    const auto suffix = parse_range_header("bytes=-3", 10);
    CRPC_CHECK_EQ(std::uint64_t{7}, suffix.first);
    CRPC_CHECK_EQ(std::uint64_t{9}, suffix.last);
}

CRPC_TEST(file_transfer_download_memory_streaming) {
    auto created = download_source::from_memory("0123456789");
    CRPC_CHECK(created.ok());
    download_source source = std::move(created.value());
    CRPC_CHECK_EQ(std::uint64_t{10}, source.size());
    CRPC_CHECK_EQ(200, source.status_code());
    std::string out;
    std::byte buffer[3];
    for (;;) {
        std::size_t count = 0;
        status st = source.read(span<std::byte>(buffer, sizeof(buffer)), count);
        CRPC_CHECK(st.ok());
        if (count == 0) {
            break;
        }
        out.append(reinterpret_cast<const char*>(buffer), count);
    }
    CRPC_CHECK_EQ(std::string("0123456789"), out);
}

CRPC_TEST(file_transfer_download_ranges) {
    auto created = download_source::from_memory("0123456789");
    CRPC_CHECK(created.ok());
    download_source source = std::move(created.value());

    CRPC_CHECK(source.apply_range("bytes=2-5").ok());
    CRPC_CHECK_EQ(206, source.status_code());
    CRPC_CHECK_EQ(std::string("bytes 2-5/10"), source.content_range());
    CRPC_CHECK_EQ(std::string("2345"), drain(source));

    CRPC_CHECK(source.rewind().ok());
    CRPC_CHECK_EQ(std::string("2345"), drain(source));

    CRPC_CHECK(source.apply_range("bytes=4-").ok());
    CRPC_CHECK_EQ(std::string("bytes 4-9/10"), source.content_range());
    CRPC_CHECK_EQ(std::string("456789"), drain(source));

    CRPC_CHECK(source.apply_range("bytes=-3").ok());
    CRPC_CHECK_EQ(std::string("789"), drain(source));

    CRPC_CHECK(source.apply_range("bytes=0-999").ok());
    CRPC_CHECK_EQ(std::string("0123456789"), drain(source));

    CRPC_CHECK(source.apply_range("bytes=20-30").ok());
    CRPC_CHECK_EQ(416, source.status_code());
    CRPC_CHECK_EQ(std::string("bytes */10"), source.content_range());
    CRPC_CHECK_EQ(std::uint64_t{0}, source.remaining());

    CRPC_CHECK(source.apply_range("items=0-1").ok());
    CRPC_CHECK_EQ(200, source.status_code());
    CRPC_CHECK_EQ(std::string("0123456789"), drain(source));
}

CRPC_TEST(file_transfer_download_empty) {
    auto created = download_source::from_memory("");
    CRPC_CHECK(created.ok());
    download_source source = std::move(created.value());
    CRPC_CHECK_EQ(std::uint64_t{0}, source.size());
    CRPC_CHECK(source.apply_range("bytes=0-0").ok());
    CRPC_CHECK_EQ(416, source.status_code());
    CRPC_CHECK_EQ(std::string("bytes */0"), source.content_range());
    CRPC_CHECK(source.apply_range("bytes=-1").ok());
    CRPC_CHECK_EQ(416, source.status_code());
}

CRPC_TEST(file_transfer_download_file) {
    upload_options options = memory_options(2);
    upload_sink sink("f", "data.bin", "application/octet-stream", options);
    CRPC_CHECK(sink.write(as_span("file-content-123")).ok());
    auto finalized = sink.finalize();
    CRPC_CHECK(finalized.ok());
    file_part part = std::move(finalized.value());
    CRPC_CHECK(!part.temp_path.empty());

    auto opened = download_source::open_file(part.temp_path);
    CRPC_CHECK(opened.ok());
    download_source source = std::move(opened.value());
    CRPC_CHECK_EQ(std::uint64_t{16}, source.size());
    CRPC_CHECK(source.apply_range("bytes=5-11").ok());
    CRPC_CHECK_EQ(206, source.status_code());
    CRPC_CHECK_EQ(std::string("content"), drain(source));

    auto missing = download_source::open_file(root().path / "does-not-exist.bin");
    CRPC_CHECK_FALSE(missing.ok());
    CRPC_CHECK(rpc_code::not_found == missing.error().code);

    std::error_code ec;
    std::filesystem::remove(part.temp_path, ec);
}

CRPC_TEST_MAIN()
