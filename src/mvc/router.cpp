// CRPC -- MVC route table, matching and dispatch (implementation).
#include <crpc/mvc/router.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crpc::mvc {

namespace {

char to_lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

char to_upper_ascii(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

std::string_view trim(std::string_view text) noexcept {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string exact_key(http::method verb, std::string_view path) {
    std::string key;
    key.reserve(path.size() + 2);
    key.push_back(static_cast<char>(static_cast<unsigned>(verb)));
    key.push_back('\x1f');
    key.append(path.data(), path.size());
    return key;
}

bool is_exact_entry(const route_entry& entry) noexcept {
    for (const compiled_segment& segment : entry.segments) {
        if (segment.type != compiled_segment::kind::static_) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> split_segments(std::string_view path) {
    std::vector<std::string> segments;
    std::size_t i = (!path.empty() && path[0] == '/') ? 1 : 0;
    while (i < path.size()) {
        std::size_t j = path.find('/', i);
        if (j == std::string_view::npos) {
            j = path.size();
        }
        segments.emplace_back(path.substr(i, j - i));
        i = j + 1;
    }
    return segments;
}

status decode_path_segments(std::string_view path,
                            std::vector<std::string>& out) {
    out.clear();
    std::size_t i = (!path.empty() && path[0] == '/') ? 1 : 0;
    while (i <= path.size()) {
        std::size_t j = path.find('/', i);
        const bool last = (j == std::string_view::npos);
        if (last) {
            j = path.size();
        }
        const std::string_view raw = path.substr(i, j - i);
        if (raw.empty()) {
            if (!last) {
                return status{rpc_error(rpc_code::invalid_argument,
                                        "empty path segment")};
            }
        } else {
            std::string decoded;
            const status decoded_status = http::percent_decode(raw, decoded);
            if (!decoded_status.ok()) {
                return decoded_status;
            }
            if (decoded.find('/') != std::string::npos) {
                return status{rpc_error(rpc_code::invalid_argument,
                                        "encoded slash in path")};
            }
            out.push_back(std::move(decoded));
        }
        if (last) {
            break;
        }
        i = j + 1;
    }
    return status{};
}

status canonical_path(std::string_view path, bool lower, std::string& out) {
    const std::string normalized = normalize_path(path, lower);
    std::vector<std::string> segments;
    const status decoded = decode_path_segments(normalized, segments);
    if (!decoded.ok()) {
        return decoded;
    }
    out = "/";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i != 0) {
            out.push_back('/');
        }
        out += segments[i];
    }
    return status{};
}

status compile_segments(route_entry& entry) {
    const std::vector<std::string> segments = split_segments(entry.path);
    entry.segments.clear();
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const std::string& text = segments[i];
        compiled_segment segment;
        if (text.empty()) {
            return status{rpc_error(rpc_code::failed_precondition,
                                    "empty route segment")};
        }
        if (text[0] == ':') {
            segment.type = compiled_segment::kind::parameter;
            segment.text = text.substr(1);
            if (segment.text.empty()) {
                return status{rpc_error(rpc_code::failed_precondition,
                                        "empty route parameter name")};
            }
        } else if (text[0] == '*') {
            if (i + 1 != segments.size()) {
                return status{rpc_error(rpc_code::failed_precondition,
                                        "catch-all must be the last segment")};
            }
            segment.type = compiled_segment::kind::catch_all;
            segment.text = text.substr(1);
        } else {
            segment.type = compiled_segment::kind::static_;
            segment.text = text;
        }
        entry.segments.push_back(std::move(segment));
    }
    return status{};
}

bool pattern_less(const route_entry& a, const route_entry& b) noexcept {
    const std::size_t shared =
        a.segments.size() < b.segments.size() ? a.segments.size()
                                              : b.segments.size();
    for (std::size_t i = 0; i < shared; ++i) {
        const auto ka = static_cast<int>(a.segments[i].type);
        const auto kb = static_cast<int>(b.segments[i].type);
        if (ka != kb) {
            return ka < kb;
        }
    }
    return a.segments.size() > b.segments.size();
}

bool match_segments(const route_entry& entry,
                    const std::vector<std::string>& segments,
                    route_params& out) {
    out.entries.clear();
    std::size_t i = 0;
    for (; i < entry.segments.size(); ++i) {
        const compiled_segment& pattern = entry.segments[i];
        if (pattern.type == compiled_segment::kind::catch_all) {
            std::string rest;
            for (std::size_t j = i; j < segments.size(); ++j) {
                if (j != i) {
                    rest.push_back('/');
                }
                rest += segments[j];
            }
            out.entries.emplace_back(pattern.text, std::move(rest));
            return true;
        }
        if (i >= segments.size()) {
            return false;
        }
        if (pattern.type == compiled_segment::kind::static_) {
            if (segments[i] != pattern.text) {
                return false;
            }
        } else {
            out.entries.emplace_back(pattern.text, segments[i]);
        }
    }
    return i == segments.size();
}

void append_allow(std::string& out, http::method verb) {
    const char* token = http::to_token(verb);
    if (token == nullptr || *token == '\0') {
        return;
    }
    if (!out.empty()) {
        out += ", ";
    }
    out += token;
}

void append_json_escaped(std::string& out, std::string_view text) {
    static const char hex[] = "0123456789abcdef";
    for (const char c : text) {
        const unsigned char value = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (value < 0x20) {
                out += "\\u00";
                out.push_back(hex[(value >> 4) & 0x0Fu]);
                out.push_back(hex[value & 0x0Fu]);
            } else {
                out.push_back(c);
            }
            break;
        }
    }
}

std::string problem_body(int status_code, std::string_view detail,
                         std::string_view instance) {
    std::string out = "{\"type\":\"about:blank\",\"status\":";
    out += std::to_string(status_code);
    out += ",\"detail\":\"";
    append_json_escaped(out, detail);
    out += "\",\"instance\":\"";
    append_json_escaped(out, instance);
    out += "\"}";
    return out;
}

void write_problem(http::response& response, int status_code,
                   std::string_view detail, std::string_view instance) {
    response.status_code = status_code;
    response.reason = reason_phrase(status_code);
    response.headers.add("Content-Type", "application/problem+json");
    response.body = problem_body(status_code, detail, instance);
}

const codec* negotiate_response(const http::request& request,
                                const codec_registry& registry) {
    const std::string* accept = request.headers.find("accept");
    if (accept == nullptr || accept->empty()) {
        return registry.find(codec_id::json);
    }
    const std::string_view text(*accept);
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t comma = text.find(',', pos);
        std::string_view token =
            comma == std::string_view::npos ? text.substr(pos)
                                            : text.substr(pos, comma - pos);
        const std::size_t semicolon = token.find(';');
        if (semicolon != std::string_view::npos) {
            token = token.substr(0, semicolon);
        }
        token = trim(token);
        if (!token.empty()) {
            if (token == "*/*" || token == "application/*") {
                const codec* json = registry.find(codec_id::json);
                if (json != nullptr) {
                    return json;
                }
            }
            const codec* found = registry.find(token);
            if (found != nullptr) {
                return found;
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return nullptr;
}

} // namespace

// --- Public helpers ----------------------------------------------------------

const char* reason_phrase(int status_code) noexcept {
    switch (status_code) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 206:
        return "Partial Content";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 406:
        return "Not Acceptable";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 415:
        return "Unsupported Media Type";
    case 416:
        return "Range Not Satisfiable";
    case 429:
        return "Too Many Requests";
    case 499:
        return "Client Closed Request";
    case 500:
        return "Internal Server Error";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Unknown";
    }
}

void write_error(http::response& response, const rpc_error& error,
                 std::string_view instance) {
    const int status_code = to_http_status(error.code);
    const std::string detail =
        error.message.empty() ? std::string(crpc::to_string(error.code))
                              : error.message;
    write_problem(response, status_code, detail, instance);
}

std::string normalize_path(std::string_view path, bool lower) {
    std::string_view head = path;
    const std::size_t query = head.find('?');
    if (query != std::string_view::npos) {
        head = head.substr(0, query);
    }
    const std::size_t fragment = head.find('#');
    if (fragment != std::string_view::npos) {
        head = head.substr(0, fragment);
    }
    std::string out;
    out.reserve(head.size() + 1);
    if (head.empty() || head.front() != '/') {
        out.push_back('/');
    }
    out.append(head.data(), head.size());
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    if (lower) {
        for (char& c : out) {
            c = to_lower_ascii(c);
        }
    }
    return out;
}

http::method effective_method(const http::request& request) {
    if (request.method != http::method::extension) {
        return request.method;
    }
    std::string upper;
    upper.reserve(request.method_token.size());
    for (const char c : request.method_token) {
        upper.push_back(to_upper_ascii(c));
    }
    return http::method_from_token(upper);
}

std::string join_route_path(std::string_view prefix, std::string_view path) {
    std::string out;
    std::string_view head = prefix;
    while (!head.empty() && head.back() == '/') {
        head.remove_suffix(1);
    }
    out.assign(head.data(), head.size());
    if (path.empty() || path == "/") {
        if (out.empty()) {
            out = "/";
        }
        return out;
    }
    if (path.front() != '/') {
        out.push_back('/');
    }
    out.append(path.data(), path.size());
    if (out.empty()) {
        out = "/";
    }
    return out;
}

std::vector<route_entry>& pending_routes() {
    static std::vector<route_entry> entries;
    return entries;
}

router& default_router() {
    static router instance = [] {
        router built;
        for (route_entry& entry : pending_routes()) {
            (void)built.add(std::move(entry));
        }
        (void)built.freeze();
        return built;
    }();
    return instance;
}

status dispatch(const router& routes, const http::request& request,
                http::response& response) {
    return routes.handle(request, response);
}

// --- router ------------------------------------------------------------------

status router::add(route_entry entry) {
    if (frozen_) {
        return status{rpc_error(rpc_code::failed_precondition,
                                "router is frozen")};
    }
    if (entry.invoke == nullptr) {
        return status{rpc_error(rpc_code::failed_precondition,
                                "route has no invoke function")};
    }
    if (entry.path.empty()) {
        return status{rpc_error(rpc_code::failed_precondition,
                                "route path is empty")};
    }
    entries_.push_back(std::move(entry));
    return status{};
}

status router::set_options(options value) {
    if (frozen_) {
        return status{rpc_error(rpc_code::failed_precondition,
                                "router is frozen")};
    }
    options_ = value;
    return status{};
}

status router::freeze() {
    if (frozen_) {
        return status{};
    }
    std::unordered_map<std::string, const route_entry*> seen;
    for (route_entry& entry : entries_) {
        std::string canonical;
        const status canonical_status =
            canonical_path(entry.path, options_.case_insensitive_routes,
                           canonical);
        if (!canonical_status.ok()) {
            return status{rpc_error(rpc_code::failed_precondition,
                                    "invalid route pattern: " + entry.path)};
        }
        entry.path = std::move(canonical);
        const status compiled = compile_segments(entry);
        if (!compiled.ok()) {
            return compiled;
        }
        const std::string key = exact_key(entry.method, entry.path);
        const auto it = seen.find(key);
        if (it != seen.end()) {
            return status{rpc_error(
                rpc_code::failed_precondition,
                "route conflict: " + entry.name + " and " + it->second->name)};
        }
        seen.emplace(key, &entry);
    }

    exact_.clear();
    parameters_.clear();
    for (route_entry& entry : entries_) {
        if (is_exact_entry(entry)) {
            exact_.emplace(exact_key(entry.method, entry.path), &entry);
        } else {
            parameters_.push_back(&entry);
        }
    }
    std::sort(parameters_.begin(), parameters_.end(),
              [](const route_entry* a, const route_entry* b) {
                  return pattern_less(*a, *b);
              });
    frozen_ = true;
    return status{};
}

const route_entry* router::lookup_exact(http::method verb,
                                        std::string_view path) const {
    const auto it = exact_.find(exact_key(verb, path));
    return it == exact_.end() ? nullptr : it->second;
}

match_result router::match(http::method verb, std::string_view path,
                           std::string* allow) const {
    match_result result;
    if (!frozen_) {
        return result;
    }
    std::string canonical;
    const status canonical_status =
        canonical_path(path, options_.case_insensitive_routes, canonical);
    if (!canonical_status.ok()) {
        result.kind = match_kind::bad_request;
        return result;
    }

    if (const route_entry* hit = lookup_exact(verb, canonical)) {
        result.kind = match_kind::matched;
        result.match.route = hit;
        return result;
    }

    const std::vector<std::string> segments = split_segments(canonical);
    for (const route_entry* entry : parameters_) {
        if (entry->method != verb) {
            continue;
        }
        route_params params;
        if (match_segments(*entry, segments, params)) {
            result.kind = match_kind::matched;
            result.match.route = entry;
            result.match.params = std::move(params);
            return result;
        }
    }

    std::string allow_list;
    bool path_exists = false;
    for (const route_entry& entry : entries_) {
        bool path_matched = false;
        if (is_exact_entry(entry)) {
            path_matched = (entry.path == canonical);
        } else {
            route_params ignored;
            path_matched = match_segments(entry, segments, ignored);
        }
        if (path_matched) {
            path_exists = true;
            append_allow(allow_list, entry.method);
        }
    }
    if (path_exists) {
        result.kind = match_kind::method_not_allowed;
        result.allow = allow_list;
        if (allow != nullptr) {
            *allow = allow_list;
        }
    }
    return result;
}

status router::handle(const http::request& request,
                      http::response& response) const noexcept {
    try {
        const http::method verb = effective_method(request);
        const match_result result = match(verb, request.path, nullptr);
        if (result.kind == match_kind::bad_request) {
            write_problem(response, 400, "malformed request path",
                          request.path);
            return status{};
        }
        if (result.kind == match_kind::not_found) {
            write_problem(response, 404,
                          "no route for " + std::string(request.path),
                          request.path);
            return status{};
        }
        if (result.kind == match_kind::method_not_allowed) {
            response.status_code = 405;
            response.reason = "Method Not Allowed";
            response.headers.add("Allow", result.allow);
            response.headers.add("Content-Type", "application/problem+json");
            response.body =
                problem_body(405, "method not allowed", request.path);
            return status{};
        }
        const codec* negotiated = negotiate_response(request, *registry_);
        if (negotiated == nullptr) {
            write_problem(response, 406, "no acceptable representation",
                          request.path);
            return status{};
        }
        const status invoked = result.match.route->invoke(
            result.match.route->controller, request, result.match.params,
            *negotiated, *registry_, response);
        if (!invoked.ok()) {
            write_error(response, invoked.error(), request.path);
        }
        return status{};
    } catch (...) {
        response = http::response{};
        write_problem(response, 500, "internal error", request.path);
        return status{};
    }
}

} // namespace crpc::mvc
