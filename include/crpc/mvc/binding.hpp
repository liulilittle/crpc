// CRPC -- MVC parameter binding.
//
// A request DTO is a reflected type (RTTI_DEFINE_TYPE). Each field may carry a
// `"crpc.from"` field attribute that names where the value comes from:
//
//   route   -> a path parameter captured by the router
//   query   -> the request query string
//   header  -> a request header
//   form    -> a urlencoded or multipart form field
//   file    -> an uploaded file part
//   body    -> the request body document (the default when no attribute is set)
//
// Binding order is fixed and documented:
//
//   1. the query string is percent-decoded ('+' is a space);
//   2. if the body is a document format (JSON/XML/TOML/INI/crpc-binary) it is
//      decoded into the whole DTO, filling every field without an explicit
//      non-body source (unknown keys are ignored);
//   3. every field with a route/query/header/form/file attribute is overwritten
//      from that source. A named source that is missing for a non-optional
//      field is 400 `invalid_argument`; a value that cannot be converted is
//      also 400 `invalid_argument`.
//
// Limits are enforced before decoding: the body byte count, the number of query
// parameters, the multipart part count and per-part/total sizes.
//
// Thread safety: every function here is a pure function of its arguments plus
// the immutable codec registry; no global mutable state is touched.
#ifndef CRPC_MVC_BINDING_HPP
#define CRPC_MVC_BINDING_HPP

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <crpc/codec.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/span.hpp>

// `crpc/codec.hpp` (included above) is the single authority: it defines
// CRPC_HAVE_RTTI to 1 or 0. A numeric test is required because `defined()` is
// true for the value 0 as well, which used to pull in <rtti/reflection.hpp> in
// builds without RTTI and fail with a missing header.
#if CRPC_HAVE_RTTI
#include <rtti/reflection.hpp>
#endif

namespace crpc::mvc {

// --- Captured name/value pairs ----------------------------------------------

// Ordered list of decoded name/value pairs. `find` returns the first match, so
// a repeated query key yields the first value; all pairs stay in the list.
struct CRPC_API route_params {
    std::vector<std::pair<std::string, std::string>> entries;

    [[nodiscard]] const std::string* find(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
};

// --- Uploaded file part ------------------------------------------------------

// One multipart part that carried a filename. v1 keeps the bytes in memory;
// the spill-to-disk path described in the design spec is out of v1 scope.
struct CRPC_API file_part {
    std::string name;
    std::string filename;
    std::string content_type;
    std::string data;

    [[nodiscard]] std::uint64_t size() const noexcept {
        return static_cast<std::uint64_t>(data.size());
    }
};

// --- Limits ------------------------------------------------------------------

// Every limit is inclusive; exceeding one is a `resource_exhausted` failure.
struct binding_limits {
    std::size_t max_body_bytes = 8u * 1024u * 1024u;
    std::size_t max_body_depth = 32;
    std::size_t max_query_params = 256;
    std::size_t max_form_fields = 256;
    std::size_t max_parts = 32;
    std::size_t max_part_bytes = 64u * 1024u * 1024u;
    std::size_t max_total_upload_bytes = 256u * 1024u * 1024u;
    std::size_t max_part_header_bytes = 8u * 1024u;
};

[[nodiscard]] CRPC_API const binding_limits& default_binding_limits() noexcept;

// --- Decoding helpers (implemented in src/mvc/binding.cpp) -------------------

// The media type of a Content-Type value with parameters stripped and
// surrounding whitespace removed. Case is preserved; callers compare with
// detail::ascii_iequals.
[[nodiscard]] CRPC_API std::string_view media_type(std::string_view content_type) noexcept;

// Parses `a=1&b=2`. Percent-decoded, '+' is a space. Empty pairs are skipped.
[[nodiscard]] CRPC_API status parse_query(std::string_view query, route_params& out,
                                 std::size_t limit);

// Parses an application/x-www-form-urlencoded body. Same rules as parse_query.
[[nodiscard]] CRPC_API status parse_form(std::string_view body, route_params& out,
                                std::size_t limit);

// Parses a multipart/form-data body. Form fields land in `fields`, parts with a
// filename land in `files`. A missing boundary or a malformed part is 400.
[[nodiscard]] CRPC_API status parse_multipart(std::string_view body,
                                     std::string_view content_type,
                                     const binding_limits& limits,
                                     std::vector<file_part>& files,
                                     route_params& fields);

[[nodiscard]] CRPC_API const file_part* find_file(const std::vector<file_part>& files,
                                         std::string_view name) noexcept;

// --- Scalar conversion -------------------------------------------------------

namespace detail {

inline bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
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

#if CRPC_HAVE_RTTI
template <typename E, typename = void>
struct enum_registered : std::false_type {};
template <typename E>
struct enum_registered<E, std::void_t<decltype(rtti::enum_table<E>::count)>>
    : std::true_type {};

template <typename E>
bool convert_enum(std::string_view text, E& out) noexcept {
    if constexpr (enum_registered<E>::value) {
        for (std::size_t i = 0; i < rtti::enum_table<E>::count; ++i) {
            if (std::string_view(rtti::enum_table<E>::array[i].name) == text) {
                out = rtti::enum_table<E>::array[i].value;
                return true;
            }
        }
    }
    using underlying = std::underlying_type_t<E>;
    underlying raw{};
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars(first, last, raw);
    if (parsed.ec != std::errc() || parsed.ptr != last) {
        return false;
    }
    out = static_cast<E>(raw);
    return true;
}
#endif

} // namespace detail

// Converts one decoded text value to `T`. Returns false (never throws) when the
// text is not a valid representation. Supported: std::string, bool (true/false/
// 1/0), every integral type, float/double, and enums (label first, then numeric).
template <typename T>
[[nodiscard]] bool convert_scalar(std::string_view text, T& out) noexcept {
    if constexpr (std::is_same_v<T, std::string>) {
        out.assign(text.data(), text.size());
        return true;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (detail::ascii_iequals(text, "true") || text == "1") {
            out = true;
            return true;
        }
        if (detail::ascii_iequals(text, "false") || text == "0") {
            out = false;
            return true;
        }
        return false;
    } else if constexpr (std::is_enum_v<T>) {
#if CRPC_HAVE_RTTI
        return detail::convert_enum<T>(text, out);
#else
        (void)text;
        (void)out;
        return false;
#endif
    } else if constexpr (std::is_integral_v<T>) {
        const char* first = text.data();
        const char* last = text.data() + text.size();
        T value{};
        const std::from_chars_result parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc() || parsed.ptr != last) {
            return false;
        }
        out = value;
        return true;
    } else if constexpr (std::is_floating_point_v<T>) {
        const char* first = text.data();
        const char* last = text.data() + text.size();
        T value{};
        const std::from_chars_result parsed =
            std::from_chars(first, last, value, std::chars_format::general);
        if (parsed.ec != std::errc() || parsed.ptr != last) {
            return false;
        }
        out = value;
        return true;
    } else {
        (void)text;
        (void)out;
        return false;
    }
}

// --- Field binding -----------------------------------------------------------

namespace detail {

// `std::optional` detection that does not depend on the RTTI library: the
// binder must compile (and degrade to "no optional field" behaviour) when CRPC
// is built without RTTI.
template <typename T>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

[[nodiscard]] inline status missing_field(std::string_view name) {
    return status{rpc_error(rpc_code::invalid_argument,
                            "missing required field: " + std::string(name))};
}

[[nodiscard]] inline status invalid_field(std::string_view name) {
    return status{rpc_error(rpc_code::invalid_argument,
                            "invalid value for field: " + std::string(name))};
}

template <typename F>
[[nodiscard]] status assign_value(F& field, const std::string* found,
                                  std::string_view field_name) {
    if constexpr (detail::is_optional<F>::value) {
        using element = typename F::value_type;
        if (found == nullptr) {
            return status{};
        }
        element value{};
        if (!convert_scalar<element>(*found, value)) {
            return invalid_field(field_name);
        }
        field = std::move(value);
        return status{};
    } else {
        if (found == nullptr) {
            return missing_field(field_name);
        }
        F value{};
        if (!convert_scalar<F>(*found, value)) {
            return invalid_field(field_name);
        }
        field = std::move(value);
        return status{};
    }
}

template <typename F>
struct is_file_part : std::false_type {};
template <>
struct is_file_part<file_part> : std::true_type {};


// SFINAE-safe `F::value_type` so a compound condition can name it for any F.
template <typename F, typename = void>
struct optional_value {
    using type = void;
};
template <typename F>
struct optional_value<F, std::void_t<typename F::value_type>> {
    using type = typename F::value_type;
};

template <typename F>
struct is_file_part_vector : std::false_type {};
template <typename Alloc>
struct is_file_part_vector<std::vector<file_part, Alloc>> : std::true_type {};

// True when a field holds an uploaded file (directly, optionally or as a list).
template <typename F>
constexpr bool is_file_like() noexcept {
    if constexpr (is_file_part<F>::value) {
        return true;
    } else if constexpr (detail::is_optional<F>::value &&
                         is_file_part<typename optional_value<F>::type>::value) {
        return true;
    } else {
        return is_file_part_vector<F>::value;
    }
}

// True when any field of Req is a file field. Such a DTO is populated from a
// multipart body and is never document-decoded, because a file part has no
// codec representation.
// Detecting the fields of `Req` needs reflection, so without RTTI there is no
// file-field detection: every DTO takes the document-decoding path. That is the
// documented behaviour of a no-RTTI build (see codec.hpp).
#if CRPC_HAVE_RTTI
template <typename Req, std::size_t... I>
constexpr bool has_file_field_impl(std::index_sequence<I...>) noexcept {
    return (is_file_like<std::remove_cv_t<std::remove_reference_t<
                decltype(std::declval<Req&>().*
                         std::get<I>(rtti::fields_of<Req>::members))>>>() ||
            ...);
}

template <typename Req>
constexpr bool has_file_field() noexcept {
    return has_file_field_impl<Req>(
        std::make_index_sequence<rtti::field_count<Req>()>{});
}
#else
template <typename Req>
constexpr bool has_file_field() noexcept {
    return false;
}
#endif

template <typename F>
[[nodiscard]] status assign_file(F& field, std::string_view name,
                                 const std::vector<file_part>& files) {
    if constexpr (is_file_part<F>::value) {
        const file_part* part = find_file(files, name);
        if (part == nullptr) {
            return missing_field(name);
        }
        field = *part;
        return status{};
    } else if constexpr (detail::is_optional<F>::value &&
                         is_file_part<typename optional_value<F>::type>::value) {
        const file_part* part = find_file(files, name);
        if (part != nullptr) {
            field = *part;
        }
        return status{};
    } else if constexpr (is_file_part_vector<F>::value) {
        for (const file_part& part : files) {
            if (part.name == name) {
                field.push_back(part);
            }
        }
        return status{};
    } else {
        return status{rpc_error(rpc_code::failed_precondition,
                                "CRPC_FROM_FILE requires a file_part field")};
    }
}

#if CRPC_HAVE_RTTI
template <typename Req, std::size_t I>
void bind_field(const http::request& request, const route_params& route,
                const route_params& query, const route_params& form,
                const std::vector<file_part>& files, Req& out, status& result) {
    if (!result.ok()) {
        return;
    }
    const auto member = std::get<I>(rtti::fields_of<Req>::members);
    const std::string_view field_name(rtti::fields<Req>()[I]->name);
    const rtti::attr* attr =
        rtti::field_find_attr<Req>(field_name, "crpc.from");
    if (attr == nullptr) {
        return; // body-decoded field, nothing to override
    }
    const std::string_view source =
        attr->args.empty() ? std::string_view{} : attr->args[0].as_string();
    const std::string_view source_name =
        attr->args.size() > 1 ? attr->args[1].as_string() : field_name;
    auto& field = out.*member;
    if (source == "route") {
        result = assign_value(field, route.find(source_name), source_name);
    } else if (source == "query") {
        result = assign_value(field, query.find(source_name), source_name);
    } else if (source == "header") {
        result = assign_value(field, request.headers.find(source_name),
                              source_name);
    } else if (source == "form") {
        result = assign_value(field, form.find(source_name), source_name);
    } else if (source == "file") {
        result = assign_file(field, source_name, files);
    } else if (source == "body") {
        // The body document was already decoded into the whole DTO.
    } else {
        result = status{rpc_error(rpc_code::failed_precondition,
                                  "unknown crpc.from source")};
    }
}

template <typename Req, std::size_t... I>
void bind_fields(const http::request& request, const route_params& route,
                 const route_params& query, const route_params& form,
                 const std::vector<file_part>& files, Req& out, status& result,
                 std::index_sequence<I...>) {
    const auto step = [&](auto index_constant) {
        constexpr std::size_t index = decltype(index_constant)::value;
        bind_field<Req, index>(request, route, query, form, files, out, result);
    };
    (step(std::integral_constant<std::size_t, I>{}), ...);
}
#endif

} // namespace detail

// Binds a request into a reflected DTO. `route` holds the path parameters
// captured by the router. Returns `ok` or a 400-mapped `rpc_error`.
template <typename Req>
[[nodiscard]] status bind_request(const http::request& request,
                                  const route_params& route,
                                  const codec_registry& registry,
                                  const binding_limits& limits, Req& out) {
#if CRPC_HAVE_RTTI
    if constexpr (!rtti::is_reflected<Req>::value) {
        (void)request;
        (void)route;
        (void)registry;
        (void)limits;
        (void)out;
        return status{rpc_error(rpc_code::failed_precondition,
                                "request DTO is not reflected")};
    } else {
        route_params query;
        status query_status =
            parse_query(request.query, query, limits.max_query_params);
        if (!query_status.ok()) {
            return query_status;
        }

        const std::string* content_type = request.headers.find("content-type");
        const std::string_view media =
            content_type != nullptr ? media_type(*content_type)
                                    : std::string_view{};
        route_params form;
        std::vector<file_part> files;

        if (detail::ascii_iequals(media, "multipart/form-data")) {
            if (request.body.size() > limits.max_body_bytes) {
                return status{rpc_error(rpc_code::resource_exhausted,
                                        "request body exceeds limit")};
            }
            status parsed = parse_multipart(
                request.body,
                content_type != nullptr ? std::string_view(*content_type)
                                        : std::string_view{},
                limits, files, form);
            if (!parsed.ok()) {
                return parsed;
            }
        } else if (detail::ascii_iequals(media,
                                         "application/x-www-form-urlencoded")) {
            if (request.body.size() > limits.max_body_bytes) {
                return status{rpc_error(rpc_code::resource_exhausted,
                                        "request body exceeds limit")};
            }
            status parsed = parse_form(request.body, form, limits.max_form_fields);
            if (!parsed.ok()) {
                return parsed;
            }
        } else if (!request.body.empty()) {
            if (request.body.size() > limits.max_body_bytes) {
                return status{rpc_error(rpc_code::resource_exhausted,
                                        "request body exceeds limit")};
            }
            // A DTO with a file field has no document representation, so it is
            // populated exclusively by the form/file sources above.
            if constexpr (!detail::has_file_field<Req>()) {
                const span<const std::byte> bytes(
                    reinterpret_cast<const std::byte*>(request.body.data()),
                    request.body.size());
                const codec* decoder =
                    content_type != nullptr ? registry.find(media) : nullptr;
                if (decoder == nullptr) {
                    decoder = registry.find(codec_id::json);
                }
                if (decoder == nullptr) {
                    return status{rpc_error(rpc_code::decode_error,
                                            "no codec for the request body")};
                }
                status decoded;
                if (decoder->id() == codec_id::crpc_binary) {
                    const binary_codec bounded(limits.max_body_depth, true);
                    decoded = decode_from(bounded, bytes, out);
                } else {
                    decoded = decode_from(*decoder, bytes, out);
                }
                if (!decoded.ok()) {
                    return status{rpc_error(
                        rpc_code::decode_error,
                        "request body decode failed: " + decoded.message())};
                }
            }
        }

        status result;
        constexpr std::size_t count = rtti::field_count<Req>();
        detail::bind_fields<Req>(
            request, route, query, form, files, out, result,
            std::make_index_sequence<count>{});
        return result;
    }
#else
    (void)request;
    (void)route;
    (void)registry;
    (void)limits;
    (void)out;
    return status{rpc_error(rpc_code::failed_precondition,
                            "binding requires the RTTI dependency")};
#endif
}

} // namespace crpc::mvc

#endif // CRPC_MVC_BINDING_HPP


