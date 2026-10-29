// Login capture of one account. A sign-in, a sign-up or a QR login ends with
// an auth.authorization that carries the session pair. The pair is read from
// the transcoded reply and handed to the owner, which stores it, rekeys the
// update socket to the access token and declares the connection again. The
// identity lane of a signed-out account is the subject of its connection
// identity. Qt-free. Owner thread only.
#pragma once

#include "mtproto/impulse/core/transcoder.h"
#include "mtproto/impulse/glue/token_manager.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace Impulse {

// The identity lane of a connection identity token: the subject of the token
// without its "i:" prefix, which names the identity:{id} lane. Empty when the
// token is empty or is not a connection identity.
[[nodiscard]] std::string IdentityLaneId(const std::string &identityToken);

// The user id of an auth.authorization result of the transcoder, read from
// its TL bytes. An auth.loginTokenSuccess result is read through its
// authorization. Nullopt when the result is not an authorization with a user.
[[nodiscard]] std::optional<std::int64_t> AuthorizationUserId(
	std::span<const std::uint8_t> tl);

class LoginFlow final {
public:
	// Receives the complete pair of a login. It runs on the owner thread.
	using PairHandler = std::function<void(const SessionTokens &tokens)>;

	// The transcoder is not owned and may be null, which captures nothing.
	LoginFlow(const Transcoder *transcoderRef, PairHandler handler);

	// Reads the session pair of one reply of a login method. Returns true and
	// hands the pair to the handler when both tokens and the user id are
	// present. A reply that does not decode captures nothing.
	bool onReply(std::uint32_t methodId, std::span<const std::uint8_t> proto);

private:
	const Transcoder *transcoder = nullptr;
	PairHandler handler;
};

} // namespace Impulse
