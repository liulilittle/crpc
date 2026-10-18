// CRPC -- streaming multipart parser tests.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/http/multipart.hpp>

#include "test_framework.hpp"

namespace {

using crpc::http::multipart_handler;
using crpc::http::multipart_limits;
using crpc::http::multipart_parser;
using crpc::http::multipart_part_info;
using crpc::rpc_code;
using crpc::span;
using crpc::status;
using crpc::result;

struct recorded_part {
    std::uint64_t index = 0;
    std::string name;
    std::string filename;
    std::string content_type;
    std::string data;
    bool has_filename = false;
    std::size_t data_callbacks = 0;
};

struct collector : multipart_handler {
    std::vector<recorded_part> parts;
    std::size_t ends = 0;

    status on_part_begin(const multipart_part_info& info) override {
        recorded_part part;
        part.index = info.index;
        part.name = info.name;
        part.filename = info.filename;
        part.content_type = info.content_type;
        part.has_filename = info.has_filename;
        parts.push_back(std::move(part));
        return status{};
    }

    status on_part_data(span<const std::byte> data) override {
        if (parts.empty()) {
            return status{crpc::rpc_error(rpc_code::internal, "data before begin")};
        }
        parts.back().data.append(reinterpret_cast<const char*>(data.data()),
                                 data.size());
        ++parts.back().data_callbacks;
        return status{};
    }

    status on_part_end() override {
        ++ends;
        return status{};
    }
};

span<const std::byte> as_span(const std::string& text) {
    return span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

std::string start() { return "--B\r\n"; }
std::string part_header(std::string_view disposition,
                        std::string_view content_type = {}) {
    std::string out = "Content-Disposition: ";
    out.append(disposition.data(), disposition.size());
    out.append("\r\n");
    if (!content_type.empty()) {
        out.append("Content-Type: ");
        out.append(content_type.data(), content_type.size());
        out.append("\r\n");
    }
    out.append("\r\n");
    return out;
}

std::string delimiter() { return "\r\n--B"; }
std::string closing() { return "\r\n--B--\r\n"; }

// Runs a body through a fresh parser, optionally splitting the input.
status run_body(const std::string& content_type, const std::string& body,
                collector& sink, multipart_limits limits = {},
                std::size_t split = static_cast<std::size_t>(-1)) {
    result<multipart_parser> created =
        multipart_parser::create(content_type, sink, limits);
    if (!created.ok()) {
        return status{created.error()};
    }
    multipart_parser parser = std::move(created.value());
    if (split == static_cast<std::size_t>(-1) || split >= body.size()) {
        status st = parser.feed(as_span(body));
        if (!st.ok()) {
            return st;
        }
    } else {
        std::string first = body.substr(0, split);
        std::string second = body.substr(split);
        status st = parser.feed(as_span(first));
        if (!st.ok()) {
            return st;
        }
        st = parser.feed(as_span(second));
        if (!st.ok()) {
            return st;
        }
    }
    return parser.finish();
}

std::string simple_body(const std::string& field, const std::string& value,
                        const std::string& filename = {}) {
    std::string disposition = "form-data; name=\"" + field + "\"";
    if (!filename.empty()) {
        disposition += "; filename=\"" + filename + "\"";
    }
    return start() + part_header(disposition) + value + closing();
}

} // namespace

CRPC_TEST(multipart_single_part) {
    collector sink;
    const std::string body = simple_body("field", "value");
    status st = run_body("multipart/form-data; boundary=B", body, sink);
    CRPC_CHECK(st.ok());
    CRPC_CHECK_EQ(std::size_t{1}, sink.parts.size());
    CRPC_CHECK_EQ(1u, sink.ends);
    CRPC_CHECK_EQ(std::string("field"), sink.parts[0].name);
    CRPC_CHECK_EQ(std::string("value"), sink.parts[0].data);
    CRPC_CHECK_FALSE(sink.parts[0].has_filename);
    CRPC_CHECK_EQ(std::uint64_t{0}, sink.parts[0].index);
}

CRPC_TEST(multipart_multiple_parts_with_preamble) {
    collector sink;
    std::string body = "this is a preamble\r\n";
    body += start() + part_header("form-data; name=\"a\"", "text/plain");
    body += "one";
    body += delimiter() + "\r\n";
    body += part_header("form-data; name=\"b\"");
    body += "two\r\nthree";
    body += closing();
    body += "epilogue ignored";
    status st = run_body("multipart/form-data; boundary=B", body, sink);
    CRPC_CHECK(st.ok());
    CRPC_CHECK_EQ(std::size_t{2}, sink.parts.size());
    CRPC_CHECK_EQ(std::string("one"), sink.parts[0].data);
    CRPC_CHECK_EQ(std::string("two\r\nthree"), sink.parts[1].data);
    CRPC_CHECK_EQ(std::string("text/plain"), sink.parts[0].content_type);
    CRPC_CHECK_EQ(std::uint64_t{1}, sink.parts[1].index);
}

CRPC_TEST(multipart_boundary_split_every_offset) {
    const std::string body =
        start() + part_header("form-data; name=\"f\"; filename=\"x.bin\"") +
        std::string("some payload\r\nwith crlf") + closing();
    const std::string content_type = "multipart/form-data; boundary=B";
    for (std::size_t split = 0; split <= body.size(); ++split) {
        collector sink;
        status st = run_body(content_type, body, sink, multipart_limits{}, split);
        CRPC_CHECK(st.ok());
        CRPC_CHECK_EQ(std::size_t{1}, sink.parts.size());
        CRPC_CHECK_EQ(std::string("some payload\r\nwith crlf"),
                      sink.parts[0].data);
        CRPC_CHECK_EQ(std::string("x.bin"), sink.parts[0].filename);
    }
}

CRPC_TEST(multipart_feed_one_byte_at_a_time) {
    const std::string body =
        start() + part_header("form-data; name=\"f\"") + "0123456789" + closing();
    collector sink;
    result<multipart_parser> created =
        multipart_parser::create("multipart/form-data; boundary=B", sink);
    CRPC_CHECK(created.ok());
    multipart_parser parser = std::move(created.value());
    for (char c : body) {
        status st = parser.feed(span<const std::byte>(
            reinterpret_cast<const std::byte*>(&c), 1));
        CRPC_CHECK(st.ok());
    }
    CRPC_CHECK(parser.finish().ok());
    CRPC_CHECK_EQ(std::string("0123456789"), sink.parts[0].data);
}

CRPC_TEST(multipart_boundary_like_content) {
    const std::string content = std::string("A\r\n--BX\r\n--BOU\r\n--B") +
                                "prefix\r\n--BO";
    const std::string body =
        start() + part_header("form-data; name=\"f\"") + content + closing();
    collector sink;
    status st = run_body("multipart/form-data; boundary=B", body, sink);
    CRPC_CHECK(st.ok());
    CRPC_CHECK_EQ(std::size_t{1}, sink.parts.size());
    CRPC_CHECK_EQ(content, sink.parts[0].data);
}

CRPC_TEST(multipart_boundary_like_crlf_terminator) {
    // "\r\n--BX" looks like a delimiter but is not; the CR LF must stay data.
    const std::string content = "line\r\n--BXcleanup";
    const std::string body =
        start() + part_header("form-data; name=\"f\"") + content + closing();
    collector sink;
    status st = run_body("multipart/form-data; boundary=B", body, sink);
    CRPC_CHECK(st.ok());
    CRPC_CHECK_EQ(content, sink.parts[0].data);
}

CRPC_TEST(multipart_crlf_and_truncation) {
    collector sink;
    const std::string good = simple_body("f", "value");
    status st = run_body("multipart/form-data; boundary=B",
                         good.substr(0, good.size() - 5), sink);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(rpc_code::protocol_error == st.code());

    collector sink2;
    std::string bad_header =
        "--B\r\nContent-Disposition: form-data; name=\"f\"\r\n\tfolded\r\n\r\nx"
        "\r\n--B--\r\n";
    status st2 = run_body("multipart/form-data; boundary=B", bad_header, sink2);
    CRPC_CHECK_FALSE(st2.ok());
    CRPC_CHECK(rpc_code::protocol_error == st2.code());
}

CRPC_TEST(multipart_limits) {
    {   // per-part limit
        collector sink;
        multipart_limits limits;
        limits.max_part_bytes = 4;
        status st = run_body("multipart/form-data; boundary=B",
                             simple_body("f", "0123456789"), sink, limits);
        CRPC_CHECK_FALSE(st.ok());
        CRPC_CHECK(rpc_code::resource_exhausted == st.code());
    }
    {   // total limit across parts
        collector sink;
        multipart_limits limits;
        limits.max_total_bytes = 5;
        std::string body = start() + part_header("form-data; name=\"a\"") + "abcd" +
                           delimiter() + "\r\n" +
                           part_header("form-data; name=\"b\"") + "efgh" +
                           closing();
        status st = run_body("multipart/form-data; boundary=B", body, sink,
                             limits);
        CRPC_CHECK_FALSE(st.ok());
        CRPC_CHECK(rpc_code::resource_exhausted == st.code());
    }
    {   // part count
        collector sink;
        multipart_limits limits;
        limits.max_parts = 1;
        std::string body = start() + part_header("form-data; name=\"a\"") + "1" +
                           delimiter() + "\r\n" +
                           part_header("form-data; name=\"b\"") + "2" +
                           closing();
        status st = run_body("multipart/form-data; boundary=B", body, sink,
                             limits);
        CRPC_CHECK_FALSE(st.ok());
        CRPC_CHECK(rpc_code::resource_exhausted == st.code());
    }
    {   // header size
        collector sink;
        multipart_limits limits;
        limits.max_part_header_bytes = 8;
        status st = run_body("multipart/form-data; boundary=B",
                             simple_body("f", "v"), sink, limits);
        CRPC_CHECK_FALSE(st.ok());
        CRPC_CHECK(rpc_code::resource_exhausted == st.code());
    }
}

CRPC_TEST(multipart_filename_sanitization) {
    CRPC_CHECK_EQ(std::string("passwd"),
                  crpc::http::sanitize_filename("../../etc/passwd").value());
    CRPC_CHECK_EQ(std::string("win.ini"),
                  crpc::http::sanitize_filename("..\\..\\win.ini").value());
    CRPC_CHECK_EQ(std::string("x.exe"),
                  crpc::http::sanitize_filename("C:\\Windows\\x.exe").value());
    CRPC_CHECK_EQ(std::string("autoexec.bat"),
                  crpc::http::sanitize_filename("C:autoexec.bat").value());
    CRPC_CHECK_EQ(std::string("shadow"),
                  crpc::http::sanitize_filename("/etc/shadow").value());
    CRPC_CHECK_EQ(std::string("normal.txt"),
                  crpc::http::sanitize_filename("normal.txt").value());
    CRPC_CHECK_EQ(std::string("trailing"),
                  crpc::http::sanitize_filename("trailing. . ").value());
    CRPC_CHECK_FALSE(crpc::http::sanitize_filename("..").ok());
    CRPC_CHECK_FALSE(crpc::http::sanitize_filename(".").ok());
    CRPC_CHECK_FALSE(crpc::http::sanitize_filename("").ok());
    CRPC_CHECK_FALSE(crpc::http::sanitize_filename("bad\tname").ok());
    std::string nul_name("a\0b", 3);
    CRPC_CHECK_FALSE(crpc::http::sanitize_filename(nul_name).ok());
}

CRPC_TEST(multipart_filename_in_body) {
    collector sink;
    const std::string body =
        start() + part_header("form-data; name=\"f\"; filename=\"../../etc/passwd\"") +
        "data" + closing();
    status st = run_body("multipart/form-data; boundary=B", body, sink);
    CRPC_CHECK(st.ok());
    CRPC_CHECK(sink.parts[0].has_filename);
    CRPC_CHECK_EQ(std::string("passwd"), sink.parts[0].filename);

    collector evil;
    const std::string evil_body =
        start() + part_header("form-data; name=\"f\"; filename=\"..\"") + "data" +
        closing();
    status st2 = run_body("multipart/form-data; boundary=B", evil_body, evil);
    CRPC_CHECK_FALSE(st2.ok());
    CRPC_CHECK(rpc_code::protocol_error == st2.code());

    collector escaped;
    const std::string escaped_body =
        start() + part_header("form-data; name=\"f\"; filename=\"a\\\"b.txt\"") + "x" +
        closing();
    status st3 = run_body("multipart/form-data; boundary=B", escaped_body,
                          escaped);
    CRPC_CHECK(st3.ok());
    CRPC_CHECK_EQ(std::string("a\"b.txt"), escaped.parts[0].filename);
}

CRPC_TEST(multipart_form_field_without_filename) {
    collector sink;
    const std::string body =
        start() + part_header("form-data; name=\"field\"") + "text" + closing();
    status st = run_body("multipart/form-data; boundary=B", body, sink);
    CRPC_CHECK(st.ok());
    CRPC_CHECK_FALSE(sink.parts[0].has_filename);
    CRPC_CHECK_EQ(std::string(), sink.parts[0].filename);
    CRPC_CHECK_EQ(std::string("field"), sink.parts[0].name);
}

CRPC_TEST(multipart_malformed) {
    collector sink;
    result<multipart_parser> created =
        multipart_parser::create("text/plain; boundary=B", sink);
    CRPC_CHECK_FALSE(created.ok());
    CRPC_CHECK(rpc_code::protocol_error == created.error().code);

    collector sink2;
    result<multipart_parser> missing =
        multipart_parser::create("multipart/form-data", sink2);
    CRPC_CHECK_FALSE(missing.ok());

    collector sink3;
    status st = run_body("multipart/form-data; boundary=B",
                         "--B\r\nContent-Disposition: form-data; name=\"f\"\r\n"
                         "\r\nx\r\n",
                         sink3);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(rpc_code::protocol_error == st.code());
}

CRPC_TEST(multipart_no_disposition) {
    collector sink;
    const std::string body = "--B\r\nContent-Type: text/plain\r\n\r\nx" +
                             std::string("\r\n--B--\r\n");
    status st = run_body("multipart/form-data; boundary=B", body, sink);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(rpc_code::protocol_error == st.code());
}

CRPC_TEST_MAIN()
