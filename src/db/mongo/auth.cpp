// CRPC -- SCRAM-SHA-256 implementation.
#include <crpc/db/mongo/auth.hpp>

#include <array>
#include <cstring>
#include <random>
#include <utility>

#include <crpc/crypto/base64.hpp>
#include <crpc/crypto/hash.hpp>

namespace crpc {
namespace db {
namespace mongo {
namespace {

// One overload: a std::string converts to string_view implicitly, and two overloads made
// every call with a std::string ambiguous.
[[nodiscard]] span<const std::byte> bytes_of(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

// The nonce has to be unpredictable: a predictable one lets an attacker precompute the
// exchange. 24 bytes of OS entropy is what the driver specifications ask for.
[[nodiscard]] std::string random_nonce() {
    std::array<std::byte, 24> raw{};
    std::random_device device;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const unsigned value = device();
        raw[i] = static_cast<std::byte>(value & 0xffu);
    }
    return crypto::base64_encode(span<const std::byte>(raw.data(), raw.size()));
}

// SCRAM forbids ',' and '=' in a user name; escaping them is the only correct way to
// handle a name that contains one, and a name sent raw would split the message.
[[nodiscard]] std::string escape_user(std::string_view user) {
    std::string out;
    out.reserve(user.size());
    for (const char c : user) {
        if (c == '=') {
            out += "=3D";
        } else if (c == ',') {
            out += "=2C";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Splits a SCRAM message into its attributes. Duplicate attributes are a protocol error:
// accepting the last one lets a server override the nonce it already committed to.
[[nodiscard]] status split_attributes(std::string_view message,
                                      std::vector<std::pair<char, std::string>>& out) {
    out.clear();
    std::size_t start = 0;
    while (start <= message.size()) {
        const std::size_t comma = message.find(',', start);
        const std::string_view piece = message.substr(
            start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        if (piece.size() >= 2 && piece[1] == '=') {
            const char key = piece[0];
            for (const auto& existing : out) {
                if (existing.first == key) {
                    return status{rpc_code::protocol_error,
                                  "a SCRAM message repeats an attribute"};
                }
            }
            out.emplace_back(key, std::string(piece.substr(2)));
        } else if (!piece.empty()) {
            return status{rpc_code::protocol_error, "a SCRAM attribute is malformed"};
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return status{};
}

[[nodiscard]] const std::string* find_attribute(
    const std::vector<std::pair<char, std::string>>& attributes, char key) {
    for (const auto& item : attributes) {
        if (item.first == key) {
            return &item.second;
        }
    }
    return nullptr;
}

[[nodiscard]] bool starts_with(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

const char* to_string(auth_mechanism mechanism) noexcept {
    return mechanism == auth_mechanism::scram_sha_256 ? "SCRAM-SHA-256" : "SCRAM-SHA-1";
}

scram_client::scram_client(std::string user, std::string password, auth_mechanism mechanism,
                           std::string nonce)
    : user_(std::move(user)),
      password_(std::move(password)),
      mechanism_(mechanism),
      nonce_(nonce.empty() ? random_nonce() : std::move(nonce)) {
    // The gs2 header "n,," carries no channel binding and no authorization identity, and
    // it is also the base64 value sent later as `c=biws` ("n,," encoded).
    client_first_bare_ = "n=" + escape_user(user_) + ",r=" + nonce_;
    client_first_ = "n,," + client_first_bare_;
}

result<std::string> scram_client::on_server_first(std::string_view payload) {
    if (first_done_) {
        return rpc_error{rpc_code::failed_precondition, "the SCRAM exchange already advanced"};
    }
    server_first_.assign(payload);
    std::vector<std::pair<char, std::string>> attributes;
    const status split = split_attributes(payload, attributes);
    if (!split) {
        return split.error();
    }
    // An error attribute means the server refused; its value is the reason.
    if (const std::string* error = find_attribute(attributes, 'e')) {
        return rpc_error{rpc_code::failed_precondition, *error};
    }
    const std::string* combined_nonce = find_attribute(attributes, 'r');
    const std::string* salt_text = find_attribute(attributes, 's');
    const std::string* iteration_text = find_attribute(attributes, 'i');
    if (combined_nonce == nullptr || salt_text == nullptr || iteration_text == nullptr) {
        return rpc_error{rpc_code::protocol_error,
                         "a SCRAM server-first message is missing an attribute"};
    }
    // The server's nonce must extend ours. If it does not, the message belongs to another
    // exchange, and continuing would authenticate against the wrong challenge.
    if (!starts_with(*combined_nonce, nonce_)) {
        return rpc_error{rpc_code::protocol_error,
                         "a SCRAM server nonce does not extend the client nonce"};
    }
    const std::string* salt_error = nullptr;
    (void)salt_error;
    result<std::string> salt = crypto::base64_decode(*salt_text);
    if (!salt) {
        return salt.error();
    }
    if (iteration_text->empty()) {
        return rpc_error{rpc_code::protocol_error, "a SCRAM iteration count is empty"};
    }
    std::uint32_t iterations = 0;
    for (const char c : *iteration_text) {
        if (c < '0' || c > '9') {
            return rpc_error{rpc_code::protocol_error,
                             "a SCRAM iteration count is not a number"};
        }
        const std::uint32_t digit = static_cast<std::uint32_t>(c - '0');
        if (iterations > (100000000u - digit) / 10u) {
            return rpc_error{rpc_code::protocol_error, "a SCRAM iteration count overflows"};
        }
        iterations = iterations * 10u + digit;
    }
    // A tiny iteration count makes the password guessable offline; refusing is the
    // documented client behaviour, and a server that asks for one is misconfigured.
    if (iterations < 4096) {
        return rpc_error{rpc_code::failed_precondition,
                         "the server asked for fewer SCRAM iterations than the minimum"};
    }
    iterations_ = iterations;

    // SaltedPassword = Hi(password, salt, iterations)
    const crypto::sha256_digest salted =
        crypto::pbkdf2_sha256(bytes_of(password_), bytes_of(salt.value()), iterations);

    crypto::sha256_digest client_key = crypto::hmac_sha256(
        span<const std::byte>(reinterpret_cast<const std::byte*>(salted.data()), salted.size()),
        bytes_of("Client Key"));
    const crypto::sha256_digest stored_key = crypto::sha256(
        span<const std::byte>(reinterpret_cast<const std::byte*>(client_key.data()),
                              client_key.size()));

    client_final_without_proof_ = "c=biws,r=" + *combined_nonce;
    auth_message_ = client_first_bare_ + "," + server_first_ + "," + client_final_without_proof_;

    const crypto::sha256_digest client_signature = crypto::hmac_sha256(
        span<const std::byte>(reinterpret_cast<const std::byte*>(stored_key.data()),
                              stored_key.size()),
        bytes_of(auth_message_));
    std::array<std::byte, 32> proof{};
    for (std::size_t i = 0; i < proof.size(); ++i) {
        proof[i] = static_cast<std::byte>(client_key[i] ^ client_signature[i]);
    }

    // The expected server signature is computed now, while the salted password is at
    // hand, and compared in `on_server_final`.
    const crypto::sha256_digest server_key = crypto::hmac_sha256(
        span<const std::byte>(reinterpret_cast<const std::byte*>(salted.data()), salted.size()),
        bytes_of("Server Key"));
    const crypto::sha256_digest server_signature = crypto::hmac_sha256(
        span<const std::byte>(reinterpret_cast<const std::byte*>(server_key.data()),
                              server_key.size()),
        bytes_of(auth_message_));
    expected_server_signature_ = crypto::base64_encode(
        span<const std::byte>(reinterpret_cast<const std::byte*>(server_signature.data()),
                              server_signature.size()));

    first_done_ = true;
    return client_final_without_proof_ + ",p=" +
           crypto::base64_encode(span<const std::byte>(proof.data(), proof.size()));
}

status scram_client::on_server_final(std::string_view payload) {
    if (!first_done_) {
        return status{rpc_code::failed_precondition,
                      "the SCRAM exchange has not produced a proof yet"};
    }
    std::vector<std::pair<char, std::string>> attributes;
    const status split = split_attributes(payload, attributes);
    if (!split) {
        return split.error();
    }
    if (const std::string* error = find_attribute(attributes, 'e')) {
        return status{rpc_code::failed_precondition, *error};
    }
    const std::string* signature = find_attribute(attributes, 'v');
    if (signature == nullptr) {
        return status{rpc_code::protocol_error,
                      "a SCRAM server-final message carries no signature"};
    }
    // A constant-time comparison: an early return on the first differing byte leaks how
    // much of a guessed signature was right.
    const std::string& expected = expected_server_signature_;
    if (signature->size() != expected.size()) {
        return status{rpc_code::failed_precondition,
                      "the server could not prove it knows the password"};
    }
    unsigned difference = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        difference |= static_cast<unsigned>(static_cast<unsigned char>((*signature)[i]) ^
                                            static_cast<unsigned char>(expected[i]));
    }
    if (difference != 0) {
        return status{rpc_code::failed_precondition,
                      "the server could not prove it knows the password"};
    }
    return status{};
}

} // namespace mongo
} // namespace db
} // namespace crpc