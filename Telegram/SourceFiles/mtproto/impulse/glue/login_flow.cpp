#include "mtproto/impulse/glue/login_flow.h"

#include "mtproto/impulse/core/login_user_id.h"
#include "mtproto/impulse/glue/identity_manager.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Impulse {
namespace {

// The subject of a connection identity is "i:" followed by 32 hex digits.
constexpr std::string_view IdentitySubjectPrefix = "i:";

} // namespace

std::string IdentityLaneId(const std::string &identityToken) {
	if (identityToken.empty()) {
		return std::string();
	}
	const std::optional<TokenClaims> claims = readTokenClaims(identityToken);
	if (!claims
		|| claims->sub.size() <= IdentitySubjectPrefix.size()
		|| claims->sub.compare(0, IdentitySubjectPrefix.size(), IdentitySubjectPrefix) != 0) {
		return std::string();
	}
	return claims->sub.substr(IdentitySubjectPrefix.size());
}

LoginFlow::LoginFlow(const Transcoder *transcoderRef, PairHandler pairHandler)
: transcoder(transcoderRef)
, handler(std::move(pairHandler)) {
}

bool LoginFlow::onReply(
		std::uint32_t methodId,
		std::span<const std::uint8_t> proto) {
	if (!transcoder || !handler) {
		return false;
	}

	SessionTokens pair;
	std::vector<std::uint8_t> tl;
	try {
		tl = transcoder->decodeResult(
			methodId,
			proto,
			[&pair](
					std::string_view protoMessage,
					std::string_view fieldName,
					std::string_view value) {
				static_cast<void>(protoMessage);
				if (fieldName == "session_token") {
					pair.accessToken = std::string(value);
				} else if (fieldName == "refresh_token") {
					pair.refreshToken = std::string(value);
				}
			});
	} catch (const TranscodeError &) {
		return false;
	}
	if (pair.accessToken.empty() || pair.refreshToken.empty()) {
		return false;
	}

	const std::optional<std::int64_t> userId = AuthorizationUserId(tl);
	if (!userId) {
		return false;
	}
	pair.userId = *userId;
	handler(pair);
	return true;
}

} // namespace Impulse
