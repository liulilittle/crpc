// CRPC -- SCRAM-SHA-256, the mechanism MongoDB uses for authentication.
//
// SCRAM is a challenge-response protocol, not a "send the password" one, which is why it
// needs state rather than a single hash:
//
//   client-first: "n,,n=<user>,r=<client nonce>"
//   server-first: "r=<combined nonce>,s=<salt base64>,i=<iterations>"
//   client-final: "c=biws,r=<combined nonce>,p=<proof>"
//   server-final: "v=<server signature>"
//
// The proof is `ClientKey XOR HMAC(StoredKey, auth message)` where
// `ClientKey = HMAC(SaltedPassword, "Client Key")` and
// `StoredKey = SHA256(ClientKey)`. The server signature is
// `HMAC(HMAC(SaltedPassword, "Server Key"), auth message)`, and verifying it matters:
// a client that skips that step will authenticate to anyone who echoes a proof back.
//
// The gs2 header "n,," means "no channel binding, no authorization identity". MongoDB
// accepts no other form.
#ifndef CRPC_DB_MONGO_AUTH_HPP
#define CRPC_DB_MONGO_AUTH_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace db {
namespace mongo {

// The mechanisms a client can negotiate. SCRAM-SHA-256 is what every server since 4.0
// offers; SCRAM-SHA-1 is still accepted for older users and is kept because a schema
// created before 4.0 has only the SHA-1 credentials.
enum class auth_mechanism : std::uint8_t { scram_sha_256, scram_sha_1 };

[[nodiscard]] CRPC_API const char* to_string(auth_mechanism mechanism) noexcept;

// One SCRAM exchange. The three steps are separate calls because the server drives them:
// the client cannot compute a proof before it has the salt and the iteration count.
class CRPC_API scram_client {
public:
    // `nonce` must be unpredictable and unique per connection; the caller supplies it so
    // a test can be deterministic. Empty means "generate one from the system".
    scram_client(std::string user, std::string password, auth_mechanism mechanism,
                 std::string nonce = {});

    // "n,,n=<user>,r=<nonce>", to be sent as the saslStart payload.
    [[nodiscard]] const std::string& client_first() const noexcept { return client_first_; }

    // Parses the server-first payload and produces the client-final payload.
    [[nodiscard]] result<std::string> on_server_first(std::string_view payload);

    // Verifies the server-final payload. A mismatch is an authentication failure: the
    // server could not prove it knows the password, so the session is not authentic even
    // though the server accepted ours.
    [[nodiscard]] status on_server_final(std::string_view payload);

    // The salt and iteration count the server chose, for diagnostics.
    [[nodiscard]] std::uint32_t iterations() const noexcept { return iterations_; }

    // The mechanism this exchange was constructed for, for diagnostics. It was
    // stored but unreadable, so clang reported the field as unused; surfacing it
    // is more useful than deleting it, since a failed handshake is exactly when
    // the caller wants to know which mechanism was attempted.
    [[nodiscard]] auth_mechanism mechanism() const noexcept { return mechanism_; }

private:
    std::string user_;
    std::string password_;
    auth_mechanism mechanism_;
    std::string nonce_;
    std::string client_first_;
    std::string client_first_bare_;
    std::string server_first_;
    std::string client_final_without_proof_;
    std::uint32_t iterations_ = 0;
    // Kept between steps: the server signature is computed from it.
    std::string auth_message_;
    std::string expected_server_signature_;
    bool first_done_ = false;
};

} // namespace mongo
} // namespace db
} // namespace crpc

#endif // CRPC_DB_MONGO_AUTH_HPP