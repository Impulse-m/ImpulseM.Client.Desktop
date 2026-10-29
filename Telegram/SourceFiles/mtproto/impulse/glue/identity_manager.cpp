#include "mtproto/impulse/glue/identity_manager.h"

#include "mtproto/impulse/core/json.h"
#include "mtproto/impulse/core/wire.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

namespace Impulse {
namespace {

constexpr std::int64_t IdentityMintTimeoutMs = 30000;
constexpr std::int64_t IdentityRenewTimeoutMs = 30000;
constexpr std::int64_t MsPerSecond = 1000;
constexpr std::uint32_t ConnectionIdentityTokenField = 1;
constexpr std::uint32_t ConnectionIdentityApiIdField = 2;
constexpr std::uint32_t Base64UrlBufferMask = 0xFFFF;

[[nodiscard]] std::vector<std::uint8_t> encodeConnectionIdentity(
		const std::string &identityToken,
		std::int32_t apiId) {
	ProtoWriter writer;
	if (!identityToken.empty()) {
		writer.writeStringField(ConnectionIdentityTokenField, identityToken);
	}
	if (apiId != 0) {
		writer.writeVarintField(
			ConnectionIdentityApiIdField,
			static_cast<std::uint64_t>(apiId));
	}
	return writer.takeBytes();
}

// The identity_token of a ConnectionIdentityResponse. Empty when absent.
// Throws ProtoWireError on malformed data.
[[nodiscard]] std::string decodeConnectionIdentity(
		const std::vector<std::uint8_t> &data) {
	std::string token;
	ProtoReader reader(data);
	while (reader.next()) {
		if (reader.field() == ConnectionIdentityTokenField
			&& reader.wireType() == ProtoWireType::LengthDelimited) {
			const std::span<const std::uint8_t> bytes = reader.readBytes();
			token.assign(
				reinterpret_cast<const char *>(bytes.data()),
				bytes.size());
		}
	}
	return token;
}

[[nodiscard]] int base64UrlValue(char symbol) {
	if (symbol >= 'A' && symbol <= 'Z') {
		return symbol - 'A';
	} else if (symbol >= 'a' && symbol <= 'z') {
		return symbol - 'a' + 26;
	} else if (symbol >= '0' && symbol <= '9') {
		return symbol - '0' + 52;
	} else if (symbol == '-' || symbol == '+') {
		return 62;
	} else if (symbol == '_' || symbol == '/') {
		return 63;
	}
	return -1;
}

// Decodes the unpadded base64url alphabet of a JWT segment. Padding ends the
// data; any other symbol outside the alphabet is an error.
[[nodiscard]] std::optional<std::string> decodeBase64Url(
		std::string_view text) {
	std::string out;
	std::uint32_t buffer = 0;
	int bits = 0;
	for (const char symbol : text) {
		if (symbol == '=') {
			break;
		}
		const int value = base64UrlValue(symbol);
		if (value < 0) {
			return std::nullopt;
		}
		buffer = ((buffer << 6) | static_cast<std::uint32_t>(value))
			& Base64UrlBufferMask;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
		}
	}
	return out;
}

// A positive numeric claim, or 0 when it is absent or not positive.
[[nodiscard]] std::int64_t positiveClaim(
		const Json::Value &root,
		std::string_view name) {
	const Json::Value *node = root.find(name);
	if (node == nullptr) {
		return 0;
	}
	const std::optional<std::int64_t> value = node->asInt();
	return (value && *value > 0) ? *value : 0;
}

// Reads the claims of a JWT payload. The signature is not checked: the server
// verifies the token, the client only schedules around its times. Nullopt when
// the token is not a JWT whose payload is a JSON object with a string sub.
[[nodiscard]] std::optional<TokenClaims> readClaims(const std::string &token) {
	const std::size_t first = token.find('.');
	if (first == std::string::npos) {
		return std::nullopt;
	}
	const std::size_t second = token.find('.', first + 1);
	if (second == std::string::npos) {
		return std::nullopt;
	}
	const std::optional<std::string> payload = decodeBase64Url(
		std::string_view(token).substr(first + 1, second - first - 1));
	if (!payload) {
		return std::nullopt;
	}
	const Json::ParseResult parsed = Json::parse(*payload);
	if (!parsed.value || parsed.value->kind() != Json::Kind::Object) {
		return std::nullopt;
	}
	const Json::Value &root = *parsed.value;
	const Json::Value *subNode = root.find("sub");
	const std::string *sub = subNode ? subNode->asString() : nullptr;
	if (sub == nullptr) {
		return std::nullopt;
	}
	TokenClaims claims;
	claims.exp = positiveClaim(root, "exp");
	claims.iat = positiveClaim(root, "iat");
	claims.origIat = positiveClaim(root, "orig_iat");
	claims.sub = *sub;
	return claims;
}

[[nodiscard]] std::string tokenFromBlob(const TokenBlob &blob) {
	return std::string(
		reinterpret_cast<const char *>(blob.data()),
		blob.size());
}

[[nodiscard]] TokenBlob blobFromToken(const std::string &token) {
	const auto *data = reinterpret_cast<const std::uint8_t *>(token.data());
	return TokenBlob(data, data + token.size());
}

[[nodiscard]] IdentityObtainResult localFailure() {
	IdentityObtainResult result;
	result.failure.kind = CarrierOutcomeKind::NetworkFailure;
	return result;
}

} // namespace

std::optional<TokenClaims> readTokenClaims(const std::string &token) {
	return readClaims(token);
}

IdentityManager::IdentityManager(IdentityManagerDependencies dependencies)
	: store(dependencies.store)
	, mintCall(std::move(dependencies.mint))
	, schedule(std::move(dependencies.schedule))
	, nowMs(std::move(dependencies.nowMs))
	, random(std::move(dependencies.random))
	, alive(std::make_shared<bool>(true)) {
	const std::string stored = tokenFromBlob(store->load());
	if (stored.empty()) {
		return;
	}
	const std::optional<Identity> loaded = readIdentity(stored);
	if (loaded && identityUsable(loaded->claims.exp, nowMs())) {
		identity = *loaded;
		scheduleRenewal(*identity, epoch);
		return;
	}
	store->clear();
}

void IdentityManager::setApiId(std::int32_t value) {
	apiId = value;
}

void IdentityManager::setRenewalCall(ConnectionIdentityCall call) {
	renewCall = std::move(call);
}

void IdentityManager::setListener(IdentityListener *target) {
	listener = target;
}

std::string IdentityManager::current() {
	expireIfNeeded();
	return identity ? identity->token : std::string();
}

void IdentityManager::obtain(IdentityObtainDone done) {
	expireIfNeeded();
	if (identity) {
		IdentityObtainResult result;
		result.token = identity->token;
		done(std::move(result));
		return;
	}
	waiters.push_back(std::move(done));
	if (minting) {
		return;
	}
	if (!mintCall) {
		resolveWaiters(localFailure());
		return;
	}
	minting = true;
	const std::uint64_t startedEpoch = epoch;
	const std::weak_ptr<bool> guard = alive;
	mintCall(
		encodeConnectionIdentity(std::string(), apiId),
		IdentityMintTimeoutMs,
		[this, guard, startedEpoch](ConnectionIdentityReply reply) {
			if (!guard.expired()) {
				finishMint(startedEpoch, std::move(reply));
			}
		});
}

void IdentityManager::discard() {
	forgetLocked();
	minting = false;
	resolveWaiters(localFailure());
}

void IdentityManager::refuse(const std::string &refusedToken) {
	if (!identity || identity->token != refusedToken) {
		return;
	}
	forgetLocked();
	if (listener) {
		listener->onIdentityChanged();
	}
}

std::optional<IdentityManager::Identity> IdentityManager::readIdentity(
		const std::string &token) {
	const std::optional<TokenClaims> claims = readClaims(token);
	if (!claims) {
		return std::nullopt;
	}
	return Identity{ token, *claims };
}

std::optional<IdentityManager::Identity> IdentityManager::identityFromReply(
		const ConnectionIdentityReply &reply,
		std::int64_t currentMs) {
	if (reply.outcome.kind != CarrierOutcomeKind::Reply) {
		return std::nullopt;
	}
	std::string token;
	try {
		token = decodeConnectionIdentity(reply.data);
	} catch (const ProtoWireError &) {
		return std::nullopt;
	}
	if (token.empty()) {
		return std::nullopt;
	}
	std::optional<Identity> identity = readIdentity(token);
	if (!identity || identity->claims.exp * MsPerSecond <= currentMs) {
		return std::nullopt;
	}
	return identity;
}

bool IdentityManager::isCurrent(
		const Identity &candidate,
		std::uint64_t expectedEpoch) const {
	return epoch == expectedEpoch
		&& identity.has_value()
		&& identity->token == candidate.token;
}

void IdentityManager::expireIfNeeded() {
	if (identity && !identityUsable(identity->claims.exp, nowMs())) {
		forgetLocked();
	}
}

void IdentityManager::forgetLocked() {
	++epoch;
	identity.reset();
	store->clear();
}

// A failed save keeps the identity in memory; the next launch then mints
// a new one.
void IdentityManager::persist(const std::string &identityToken) {
	const TokenBlob blob = blobFromToken(identityToken);
	static_cast<void>(store->save(blob));
}

void IdentityManager::later(
		std::int64_t delayMs,
		std::function<void()> task) {
	if (!schedule) {
		return;
	}
	const std::weak_ptr<bool> guard = alive;
	schedule(delayMs, [guard, task = std::move(task)]() {
		if (!guard.expired()) {
			task();
		}
	});
}

void IdentityManager::finishMint(
		std::uint64_t startedEpoch,
		ConnectionIdentityReply reply) {
	// Discarded while minting: the waiters now belong to a later mint.
	if (startedEpoch != epoch) {
		return;
	}
	minting = false;
	if (reply.outcome.kind != CarrierOutcomeKind::Reply) {
		IdentityObtainResult result;
		result.failure = reply.outcome;
		resolveWaiters(std::move(result));
		return;
	}
	const std::optional<Identity> minted = identityFromReply(reply, nowMs());
	if (!minted) {
		resolveWaiters(localFailure());
		return;
	}
	identity = *minted;
	persist(minted->token);
	scheduleRenewal(*identity, epoch);
	IdentityObtainResult result;
	result.token = minted->token;
	resolveWaiters(std::move(result));
}

void IdentityManager::resolveWaiters(IdentityObtainResult result) {
	std::vector<IdentityObtainDone> ready = std::move(waiters);
	waiters.clear();
	for (IdentityObtainDone &done : ready) {
		done(result);
	}
}

void IdentityManager::scheduleRenewal(
		const Identity &renewing,
		std::uint64_t installEpoch) {
	const std::int64_t now = nowMs();
	const std::int64_t point = refreshPointMs(
		renewing.claims.exp,
		now,
		random);
	later(
		std::max<std::int64_t>(0, point - now),
		[this, renewing, installEpoch]() {
			renew(renewing, installEpoch, firstRetryDelayMs());
		});
}

void IdentityManager::renew(
		const Identity &renewing,
		std::uint64_t startedEpoch,
		std::int64_t retryDelayMs) {
	if (!renewCall || !isCurrent(renewing, startedEpoch)) {
		return;
	}
	const std::weak_ptr<bool> guard = alive;
	renewCall(
		encodeConnectionIdentity(renewing.token, apiId),
		IdentityRenewTimeoutMs,
		[this, guard, renewing, startedEpoch, retryDelayMs](
				ConnectionIdentityReply reply) {
			if (!guard.expired()) {
				finishRenewal(
					renewing,
					startedEpoch,
					retryDelayMs,
					std::move(reply));
			}
		});
}

void IdentityManager::finishRenewal(
		const Identity &renewing,
		std::uint64_t startedEpoch,
		std::int64_t retryDelayMs,
		ConnectionIdentityReply reply) {
	if (!isCurrent(renewing, startedEpoch)) {
		return;
	}
	const std::optional<Identity> next = identityFromReply(reply, nowMs());
	if (!next) {
		retryRenewal(renewing, startedEpoch, retryDelayMs);
		return;
	}
	const bool same = isSameIdentity(renewing.claims, next->claims);
	if (!same) {
		++epoch;
	}
	identity = *next;
	persist(next->token);
	scheduleRenewal(*identity, epoch);
	if (listener) {
		if (same) {
			listener->onIdentityRenewed(next->token);
		} else {
			listener->onIdentityChanged();
		}
	}
}

void IdentityManager::retryRenewal(
		const Identity &renewing,
		std::uint64_t startedEpoch,
		std::int64_t retryDelayMs) {
	if (!retryLandsBeforeWindowEnd(
			renewing.claims.exp,
			nowMs(),
			retryDelayMs)) {
		return;
	}
	later(
		retryDelayMs,
		[this, renewing, startedEpoch, retryDelayMs]() {
			renew(renewing, startedEpoch, nextRetryDelayMs(retryDelayMs));
		});
}

} // namespace Impulse
