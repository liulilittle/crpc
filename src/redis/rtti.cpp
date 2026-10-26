// CRPC -- Redis RTTI binding, non-template implementations.
//
// The templates that walk a reflected type live in rtti.hpp; this file holds
// the leaf operations that do not depend on a type parameter: the RESP reply
// bridge and the scalar flattening / numeric parsing helpers.
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <crpc/redis/rtti.hpp>

#if CRPC_HAVE_RTTI

namespace crpc {
namespace redis {
namespace rtti_detail {

bool parse_i64_text(const std::string& text, i64& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

bool parse_u64_text(const std::string& text, u64& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

bool parse_double_text(const std::string& text, double& out) noexcept {
    if (text.empty()) {
        return false;
    }
    if (text == "inf" || text == "+inf") {
        out = std::numeric_limits<double>::infinity();
        return true;
    }
    if (text == "-inf") {
        out = -std::numeric_limits<double>::infinity();
        return true;
    }
    if (text == "nan" || text == "-nan" || text == "+nan") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    for (char c : text) {
        const bool ok = (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                        c == '+' || c == '-';
        if (!ok) {
            return false;
        }
    }
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) {
        return false;
    }
    out = parsed;
    return true;
}

namespace {

std::string format_real(double value) {
    char buffer[64];
    int written = 0;
    if (std::isnan(value)) {
        written = std::snprintf(buffer, sizeof(buffer), "nan");
    } else if (std::isinf(value)) {
        written = std::snprintf(buffer, sizeof(buffer), "%sinf", value > 0 ? "" : "-");
    } else {
        written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    }
    if (written <= 0) {
        return std::string();
    }
    return std::string(buffer, static_cast<std::size_t>(written));
}

} // namespace

bool scalar_arg(const value& v, std::string& out) noexcept {
    switch (v.kind()) {
    case value_kind::null:
        out.clear();
        return true;
    case value_kind::string:
        out = v.as_string();
        return true;
    case value_kind::integer:
        out = std::to_string(v.as_int());
        return true;
    case value_kind::real:
        out = format_real(v.as_real());
        return true;
    case value_kind::boolean:
        out = v.as_bool() ? "1" : "0";
        return true;
    case value_kind::array:
    case value_kind::map:
    case value_kind::set:
    case value_kind::push:
    case value_kind::error:
        return false;
    }
    return false;
}

} // namespace rtti_detail

status value_from_resp(const resp_value& reply, value& out) {
    switch (reply.type()) {
    case resp_type::null_value:
        out = value{nullptr};
        return status{};
    case resp_type::simple_string:
        out = value{reply.as_simple_string()};
        return status{};
    case resp_type::error:
        out = value{error_value{reply.as_error()}};
        return status{};
    case resp_type::integer:
        out = value{reply.as_integer()};
        return status{};
    case resp_type::bulk_string:
        out = value{reply.as_bulk_string()};
        return status{};
    case resp_type::boolean:
        out = value{reply.as_boolean()};
        return status{};
    case resp_type::double_value:
        out = value{reply.as_double()};
        return status{};
    case resp_type::big_number:
        out = value{reply.as_big_number()};
        return status{};
    case resp_type::blob_error:
        out = value{error_value{reply.as_blob_error()}};
        return status{};
    case resp_type::verbatim_string:
        out = value{reply.as_verbatim_string()};
        return status{};
    case resp_type::array: {
        array_value items;
        items.reserve(reply.as_array().size());
        for (const resp_value& element : reply.as_array()) {
            value decoded;
            const status st = value_from_resp(element, decoded);
            if (!st.ok()) {
                out = value{nullptr};
                return st;
            }
            items.push_back(std::move(decoded));
        }
        out = value{std::move(items)};
        return status{};
    }
    case resp_type::map_value: {
        map_value entries;
        entries.reserve(reply.as_map().size());
        for (const auto& entry : reply.as_map()) {
            value key;
            value mapped;
            status st = value_from_resp(entry.first, key);
            if (!st.ok()) {
                out = value{nullptr};
                return st;
            }
            st = value_from_resp(entry.second, mapped);
            if (!st.ok()) {
                out = value{nullptr};
                return st;
            }
            entries.emplace_back(std::move(key), std::move(mapped));
        }
        out = value{std::move(entries)};
        return status{};
    }
    case resp_type::set_value: {
        set_value items;
        items.items.reserve(reply.as_set().size());
        for (const resp_value& element : reply.as_set()) {
            value decoded;
            const status st = value_from_resp(element, decoded);
            if (!st.ok()) {
                out = value{nullptr};
                return st;
            }
            items.items.push_back(std::move(decoded));
        }
        out = value{std::move(items)};
        return status{};
    }
    case resp_type::push: {
        push_value items;
        items.items.reserve(reply.as_push().size());
        for (const resp_value& element : reply.as_push()) {
            value decoded;
            const status st = value_from_resp(element, decoded);
            if (!st.ok()) {
                out = value{nullptr};
                return st;
            }
            items.items.push_back(std::move(decoded));
        }
        out = value{std::move(items)};
        return status{};
    }
    }
    return status{rpc_code::decode_error};
}

} // namespace redis
} // namespace crpc

#endif // CRPC_HAVE_RTTI
