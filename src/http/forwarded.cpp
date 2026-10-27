// CRPC -- reverse-proxy client resolution.
#include <crpc/http/forwarded.hpp>

#include <utility>

namespace crpc {
namespace http {
namespace {

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// Splits on `delimiter` at the top level: a delimiter inside a quoted string is
// part of the value, and a backslash escape inside a quoted string is skipped.
[[nodiscard]] std::vector<std::string_view> split_top_level(std::string_view text,
                                                           char delimiter) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '\\' && i + 1 < text.size()) {
                ++i;
            } else if (c == '"') {
                quoted = false;
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
            continue;
        }
        if (c == delimiter) {
            parts.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    parts.push_back(trim(text.substr(start)));
    return parts;
}

// Removes the surrounding quotes and the backslash escapes RFC 7239 allows.
[[nodiscard]] std::string_view unquote(std::string_view text) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return text;
    }
    // The caller's storage is the header string, which outlives the parse, so an
    // unescaped quoted value has to be materialised; that is why the quoted path
    // returns by value below.
    return text.substr(1, text.size() - 2);
}

[[nodiscard]] std::string unescape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            ++i;
        }
        out.push_back(text[i]);
    }
    return out;
}

// One `for=`/`by=` node: an IP with an optional port, `[v6]:port`, or a token the
// RFC allows for obfuscation (`_hidden`, `unknown`).
struct node_address {
    net::ip_address address;
    std::uint16_t port = 0;
    bool parsed = false;
};

[[nodiscard]] node_address parse_node(std::string_view value) {
    node_address node;
    value = trim(value);

    // A quoted value may still hold a bracketed v6 with a port.
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        // Re-create the unescaped text; quoted values are small and rare.
        static thread_local std::string storage;
        storage = unescape(value.substr(1, value.size() - 2));
        value = storage;
    }

    if (value.empty()) {
        return node;
    }
    // Obfuscated identifiers are explicitly not addresses.
    if (value.front() == '_' || value == "unknown") {
        return node;
    }

    std::string_view host = value;
    std::string_view port_text;
    if (value.front() == '[') {
        const std::size_t close = value.find(']');
        if (close == std::string_view::npos) {
            return node;
        }
        host = value.substr(1, close - 1);
        if (close + 1 < value.size()) {
            if (value[close + 1] != ':') {
                return node;
            }
            port_text = value.substr(close + 2);
        }
    } else {
        const std::size_t colon = value.rfind(':');
        // A bare v6 literal has several colons; only a single colon can be a port
        // separator, and only when the remainder is numeric.
        if (colon != std::string_view::npos && value.find(':') == colon) {
            host = value.substr(0, colon);
            port_text = value.substr(colon + 1);
        }
    }

    result<net::ip_address> address = net::ip_address::parse(host);
    if (!address) {
        return node;
    }
    node.address = std::move(address.value());
    node.parsed = true;

    if (!port_text.empty()) {
        if (port_text.size() > 5) {
            node.parsed = false;
            return node;
        }
        unsigned port = 0;
        for (const char c : port_text) {
            if (c < '0' || c > '9') {
                node.parsed = false;
                return node;
            }
            port = port * 10 + static_cast<unsigned>(c - '0');
        }
        if (port == 0 || port > 65535) {
            node.parsed = false;
            return node;
        }
        node.port = static_cast<std::uint16_t>(port);
    }
    return node;
}

// Splits a host value into host and optional port, honouring brackets.
void split_host_port(std::string_view text, std::string& host, std::uint16_t& port) {
    text = trim(text);
    if (!text.empty() && text.front() == '"' && text.back() == '"') {
        text = unquote(text);
    }
    if (text.empty()) {
        return;
    }
    if (text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos) {
            return;
        }
        host.assign(text.substr(1, close - 1));
        if (close + 1 < text.size() && text[close + 1] == ':') {
            const std::string_view port_text = text.substr(close + 2);
            unsigned value = 0;
            for (const char c : port_text) {
                if (c < '0' || c > '9') {
                    return;
                }
                value = value * 10 + static_cast<unsigned>(c - '0');
            }
            if (value != 0 && value <= 65535) {
                port = static_cast<std::uint16_t>(value);
            }
        }
        return;
    }
    const std::size_t colon = text.rfind(':');
    if (colon != std::string_view::npos && text.find(':') == colon) {
        host.assign(text.substr(0, colon));
        const std::string_view port_text = text.substr(colon + 1);
        unsigned value = 0;
        for (const char c : port_text) {
            if (c < '0' || c > '9') {
                return;
            }
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        if (value != 0 && value <= 65535) {
            port = static_cast<std::uint16_t>(value);
        }
        return;
    }
    host.assign(text);
}

[[nodiscard]] forwarded_proto parse_proto(std::string_view text) {
    text = trim(text);
    if (!text.empty() && text.front() == '"' && text.back() == '"') {
        text = unquote(text);
    }
    if (text == "http") {
        return forwarded_proto::http;
    }
    if (text == "https") {
        return forwarded_proto::https;
    }
    return forwarded_proto::unknown;
}

// Joins repeated header fields in wire order: HTTP allows a list header to be
// split across fields, and the chain is only correct when they are concatenated.
[[nodiscard]] std::string join_fields(const header_list& headers, std::string_view name) {
    std::string out;
    for (const std::string_view value : headers.get_all(name)) {
        if (!out.empty()) {
            out += ", ";
        }
        out.append(value.data(), value.size());
    }
    return out;
}

struct chain_hop {
    net::ip_address address;
    std::uint16_t port = 0;
    bool parsed = false;
};

} // namespace

bool trusted_proxies::trusts(const net::ip_address& address) const noexcept {
    if (!address.valid()) {
        return false;
    }
    if (trust_any) {
        return true;
    }
    for (const net::ip_network& network : networks) {
        if (network.contains(address)) {
            return true;
        }
    }
    return false;
}

result<forwarded_client> resolve_forwarded(const request& request, const net::ip_address& peer,
                                           const trusted_proxies& trusted) {
    if (!peer.valid()) {
        return rpc_error{rpc_code::invalid_argument,
                         "proxy resolution needs the real peer address"};
    }

    forwarded_client client;
    // The default answer is always available and always safe: the peer is the
    // client, and nothing in a header is believed.
    client.address = peer;

    // An untrusted peer makes every forwarding header a client-supplied claim.
    if (!trusted.trusts(peer)) {
        return client;
    }

    std::vector<chain_hop> chain;
    bool used_forwarded = false;
    forwarded_proto proto = forwarded_proto::unknown;
    std::string host;
    std::uint16_t host_port = 0;

    // Presence is decided by the field, not by the joined text: an empty
    // `Forwarded:` field is malformed rather than absent, and falling through to
    // `X-Forwarded-*` because it was empty would silently change which header the
    // answer came from.
    const std::string forwarded = join_fields(request.headers, "Forwarded");
    if (request.headers.contains("Forwarded")) {
        used_forwarded = true;
        const std::vector<std::string_view> elements = split_top_level(forwarded, ',');
        if (elements.empty() || (elements.size() == 1 && elements[0].empty())) {
            return rpc_error{rpc_code::invalid_argument, "empty Forwarded header"};
        }
        // Elements are client-first: each proxy appends its own view.
        for (std::size_t index = 0; index < elements.size(); ++index) {
            const std::string_view element = elements[index];
            if (element.empty()) {
                return rpc_error{rpc_code::invalid_argument, "empty Forwarded element"};
            }
            const std::vector<std::string_view> pairs = split_top_level(element, ';');
            chain_hop hop;
            for (const std::string_view pair : pairs) {
                const std::size_t equals = pair.find('=');
                if (equals == std::string_view::npos) {
                    return rpc_error{rpc_code::invalid_argument, "malformed Forwarded parameter"};
                }
                const std::string_view raw_name = trim(pair.substr(0, equals));
                const std::string_view value = trim(pair.substr(equals + 1));
                // Parameter names are case-insensitive, so compare on a lowered copy.
                std::string name(raw_name);
                for (char& c : name) {
                    if (c >= 'A' && c <= 'Z') {
                        c = static_cast<char>(c - 'A' + 'a');
                    }
                }
                if (name == "for") {
                    const node_address node = parse_node(value);
                    // Inside `Forwarded` a `for` value must name a hop or use one
                    // of the RFC's obfuscated tokens; anything else is malformed,
                    // and silently ignoring it would leave the chain ambiguous.
                    const bool obfuscated =
                        !value.empty() && (value.front() == '_' || value == "unknown" ||
                                           (value.front() == '"' && value.size() > 1 &&
                                            value[1] == '_'));
                    if (!node.parsed && !obfuscated) {
                        return rpc_error{rpc_code::invalid_argument,
                                         "malformed Forwarded for= value"};
                    }
                    hop.address = node.address;
                    hop.port = node.port;
                    hop.parsed = node.parsed;
                } else if (name == "proto" && index == 0) {
                    // The leftmost element describes the client's own request.
                    proto = parse_proto(value);
                } else if (name == "host" && index == 0) {
                    split_host_port(value, host, host_port);
                }
                // `by` is not needed to identify the client.
            }
            chain.push_back(hop);
        }
    } else {
        const std::string forwarded_for = join_fields(request.headers, "X-Forwarded-For");
        if (!forwarded_for.empty()) {
            for (const std::string_view part : split_top_level(forwarded_for, ',')) {
                const node_address node = parse_node(part);
                chain.push_back(chain_hop{node.address, node.port, node.parsed});
            }
        }
        const std::string* proto_field = request.headers.find("X-Forwarded-Proto");
        if (proto_field != nullptr) {
            proto = parse_proto(*proto_field);
        }
        const std::string* host_field = request.headers.find("X-Forwarded-Host");
        if (host_field != nullptr) {
            split_host_port(*host_field, host, host_port);
        }
        const std::string* port_field = request.headers.find("X-Forwarded-Port");
        if (port_field != nullptr) {
            unsigned value = 0;
            bool digits = !port_field->empty() && port_field->size() <= 5;
            for (const char c : *port_field) {
                if (c < '0' || c > '9') {
                    digits = false;
                    break;
                }
                value = value * 10 + static_cast<unsigned>(c - '0');
            }
            if (digits && value != 0 && value <= 65535) {
                host_port = static_cast<std::uint16_t>(value);
            }
        }
    }

    // Walk right to left. The peer is the last hop and is already known to be
    // trusted, so the first hop that is not trusted is the client.
    std::size_t index = chain.size();
    std::size_t hops = 0;
    while (index > 0) {
        const chain_hop& hop = chain[index - 1];
        if (!hop.parsed) {
            // An obfuscated or broken hop is where trust stops: there is nothing
            // further to verify, so the client cannot be named.
            client.address = net::ip_address{};
            client.trusted_hops = hops;
            client.used_forwarded_header = used_forwarded;
            client.proto = proto;
            client.host = std::move(host);
            client.host_port = host_port;
            return client;
        }
        if (!trusted.trusts(hop.address)) {
            client.address = hop.address;
            client.port = hop.port;
            client.trusted_hops = hops;
            client.used_forwarded_header = used_forwarded;
            client.proto = proto;
            client.host = std::move(host);
            client.host_port = host_port;
            return client;
        }
        ++hops;
        --index;
    }

    // Every hop was trusted. The leftmost hop is the client as far as the chain
    // can tell; `trust_any` means believe the whole chain, so it lands here too.
    if (!chain.empty()) {
        client.address = chain.front().address;
        client.port = chain.front().port;
    }
    client.trusted_hops = hops;
    client.used_forwarded_header = used_forwarded;
    client.proto = proto;
    client.host = std::move(host);
    client.host_port = host_port;
    return client;
}

} // namespace http
} // namespace crpc