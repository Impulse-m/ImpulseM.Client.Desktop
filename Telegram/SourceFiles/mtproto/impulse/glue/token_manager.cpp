#include "mtproto/impulse/glue/token_manager.h"

#include "mtproto/impulse/core/errors.h"
#include "mtproto/impulse/core/wire.h"
#include "mtproto/impulse/glue/identity_manager.h"

#include <algorithm>
#include <span>
#include <utility>

namespace Impulse {
namespace {

constexpr std::int64_t RefreshTimeoutMs = 30000;
constexpr std::int64_t MsPerSecond = 1000;
constexpr int RateLimitedCode = 420;
constexpr std::uint32_t RefreshRequestRefreshTokenField = 1;
constexpr std::uint32_t RefreshResponseUserIdField = 1;
constexpr std::uint32_t RefreshResponseSessionTokenField = 2;
constexpr std::uint32_t RefreshResponseRefreshTokenField = 3;
constexpr std::uint32_t RefreshResponseGraceField = 4;

struct RefreshAuthorization {
	SessionTokens tokens;
	std::int64_t graceSeconds = 0;
};

// A usable session has both tokens and a non-zero user id.
[[nodiscard]] bool isComplete(const SessionTokens &tokens) {
	return !tokens.accessToken.empty()
		&& !tokens.refreshToken.empty()
		&& tokens.userId != 0;
}

[[nodiscard]] bool isUnrecoverable(const RpcError &error) {
	return error.text == "AUTH_TOKEN_INVALID"
		|| error.text == "AUTH_TOKEN_EXPIRED"
		|| error.text == "SESSION_EXPIRED";
}

// The socket refused its own access token, so the pool fleet takes the
// refresh instead.
[[nodiscard]] bool isTokenRefusal(const RpcError &error) {
	return error.text == "AUTH_KEY_UNREGISTERED"
		|| error.text == "AUTH_TOKEN_EXPIRED"
		|| error.text == "AUTH_TOKEN_INVALID";
}

[[nodiscard]] std::string stringOf(std::span<const std::uint8_t> bytes) {
	return std::string(
		reinterpret_cast<const char *>(bytes.data()),
		bytes.size());
}

[[nodiscard]] std::vector<std::uint8_t> encodeRefreshSession(
		const std::string &refreshToken) {
	ProtoWriter writer;
	writer.writeStringField(RefreshRequestRefreshTokenField, refreshToken);
	return writer.takeBytes();
}

// The fields of a RefreshSessionResponse. Nullopt on malformed data.
[[nodiscard]] std::optional<RefreshAuthorization> decodeRefreshSession(
		const std::vector<std::uint8_t> &data) {
	RefreshAuthorization result;
	try {
		ProtoReader reader(data);
		while (reader.next()) {
			const std::uint32_t field = reader.field();
			const ProtoWireType type = reader.wireType();
			if (field == RefreshResponseUserIdField
				&& type == ProtoWireType::Varint) {
				result.tokens.userId = static_cast<std::int64_t>(
					reader.readVarint());
			} else if (field == RefreshResponseSessionTokenField
				&& type == ProtoWireType::LengthDelimited) {
				result.tokens.accessToken = stringOf(reader.readBytes());
			} else if (field == RefreshResponseRefreshTokenField
				&& type == ProtoWireType::LengthDelimited) {
				result.tokens.refreshToken = stringOf(reader.readBytes());
			} else if (field == RefreshResponseGraceField
				&& type == ProtoWireType::Varint) {
				result.graceSeconds = static_cast<std::int64_t>(
					reader.readVarint());
			}
		}
	} catch (const ProtoWireError &) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] RefreshResult makeResult(
		RefreshStatus status,
		CarrierOutcome failure) {
	RefreshResult result;
	result.status = status;
	result.failure = std::move(failure);
	return result;
}

} // namespace

TokenManager::TokenManager(TokenManagerDependencies dependencies)
	: store(dependencies.store)
	, schedule(std::move(dependencies.schedule))
	, nowMs(std::move(dependencies.nowMs))
	, random(std::move(dependencies.random))
	, alive(std::make_shared<bool>(true)) {
	const std::optional<SessionTokens> stored = store->load();
	if (!stored || !isComplete(*stored)) {
		return;
	}
	hold(*stored);
	schedulePoint(firstRetryDelayMs());
}

void TokenManager::setRefreshCarriers(
		RefreshCarrierCall updateSocket,
		RefreshCarrierCall poolFleet) {
	updateCall = std::move(updateSocket);
	poolCall = std::move(poolFleet);
}

void TokenManager::setSessionChanged(std::function<void()> callback) {
	sessionChanged = std::move(callback);
}

bool TokenManager::hasSession() const {
	return held.has_value();
}

std::string TokenManager::bearer() const {
	return held ? held->accessToken : std::string();
}

bool TokenManager::accessUnusable() const {
	if (!held) {
		return false;
	}
	const std::string &access = held->accessToken;
	if (!access.empty() && access == refusedAccessToken) {
		return true;
	}
	return heldExpSeconds > 0
		&& nowMs() >= heldExpSeconds * MsPerSecond;
}

void TokenManager::markRefused(const std::string &accessToken) {
	refusedAccessToken = accessToken;
}

void TokenManager::onLoginTokens(const SessionTokens &tokens) {
	if (!isComplete(tokens)) {
		return;
	}
	installPair(tokens);
	cancelDeferred();
	notifySessionChanged();
}

void TokenManager::clear() {
	store->clear();
	held.reset();
	hasPoint = false;
	heldExpSeconds = 0;
	++pointGeneration;
	refusedAccessToken.clear();
	replacedRefreshToken.clear();
	cancelDeferred();
}

void TokenManager::refreshBlocking(RefreshDone done) {
	if (deferredPending) {
		done(makeResult(RefreshStatus::Fresh, CarrierOutcome()));
		return;
	}
	waiters.push_back(std::move(done));
	if (refreshing) {
		return;
	}
	refreshing = true;
	if (!held || held->refreshToken.empty()) {
		finishRefresh(makeResult(RefreshStatus::Ended, CarrierOutcome()));
		return;
	}
	if (!updateCall || !poolCall) {
		finishRefresh(makeResult(RefreshStatus::Failed, CarrierOutcome()));
		return;
	}
	const bool unusable = accessUnusable();
	sendRefresh(
		unusable ? poolCall : updateCall,
		held->refreshToken,
		held->accessToken,
		!unusable);
}

void TokenManager::connectionToken(ConnectionTokenDone done) {
	if (!held) {
		ConnectionTokenResult result;
		result.status = RefreshStatus::Ended;
		done(std::move(result));
		return;
	}
	if (!accessUnusable()) {
		ConnectionTokenResult result;
		result.token = held->accessToken;
		done(std::move(result));
		return;
	}
	refreshBlocking([this, done = std::move(done)](RefreshResult outcome) {
		ConnectionTokenResult result;
		result.status = outcome.status;
		if (outcome.status == RefreshStatus::Fresh) {
			result.token = bearer();
		}
		done(std::move(result));
	});
}

void TokenManager::hold(const SessionTokens &tokens) {
	held = tokens;
	++pointGeneration;
	const std::optional<TokenClaims> claims = readTokenClaims(
		tokens.accessToken);
	hasPoint = claims && claims->exp > 0;
	heldExpSeconds = hasPoint ? claims->exp : 0;
	if (hasPoint) {
		pointAtMs = refreshPointMs(claims->exp, nowMs(), random);
	}
}

void TokenManager::installPair(const SessionTokens &tokens) {
	hold(tokens);
	refusedAccessToken.clear();
	store->save(tokens);
	schedulePoint(firstRetryDelayMs());
}

void TokenManager::schedulePoint(std::int64_t retryMs) {
	if (!hasPoint) {
		return;
	}
	const std::uint64_t generation = pointGeneration;
	later(
		std::max<std::int64_t>(0, pointAtMs - nowMs()),
		[this, generation, retryMs]() {
			pointReached(generation, retryMs);
		});
}

void TokenManager::pointReached(std::uint64_t generation, std::int64_t retryMs) {
	if (generation != pointGeneration || !hasPoint) {
		return;
	}
	refreshBlocking([this, generation, retryMs](RefreshResult outcome) {
		if (outcome.status == RefreshStatus::Failed) {
			retryPoint(generation, retryMs);
		}
	});
}

// A failed point refresh is retried while the retry lands before exp - 120 s.
// Past that the pool path refreshes the expired token instead.
void TokenManager::retryPoint(std::uint64_t generation, std::int64_t retryMs) {
	if (generation != pointGeneration || !hasPoint) {
		return;
	}
	if (!retryLandsBeforeWindowEnd(heldExpSeconds, nowMs(), retryMs)) {
		return;
	}
	later(retryMs, [this, generation, retryMs]() {
		pointReached(generation, nextRetryDelayMs(retryMs));
	});
}

void TokenManager::sendRefresh(
		RefreshCarrierCall call,
		std::string refreshToken,
		std::string accessAtStart,
		bool viaUpdate) {
	const std::weak_ptr<bool> guard = alive;
	call(
		encodeRefreshSession(refreshToken),
		RefreshTimeoutMs,
		[this, guard, refreshToken, accessAtStart, viaUpdate](
				RefreshCarrierReply reply) {
			if (!guard.expired()) {
				onRefreshReply(
					std::move(reply),
					refreshToken,
					accessAtStart,
					viaUpdate);
			}
		});
}

void TokenManager::onRefreshReply(
		RefreshCarrierReply reply,
		const std::string &refreshToken,
		const std::string &accessAtStart,
		bool viaUpdate) {
	// The pair was replaced or cleared while the refresh was on the wire.
	if (!held || held->refreshToken != refreshToken) {
		finishRefresh(makeResult(
			held ? RefreshStatus::Fresh : RefreshStatus::Ended,
			CarrierOutcome()));
		return;
	}
	if (reply.outcome.kind == CarrierOutcomeKind::RpcError) {
		const RpcError error = reply.outcome.error;
		if (viaUpdate && isTokenRefusal(error)) {
			markRefused(accessAtStart);
			sendRefresh(poolCall, refreshToken, accessAtStart, false);
			return;
		}
		if (isUnrecoverable(error)) {
			// A replay of the token this refresh replaced, inside the grace,
			// answers the same session: the newer pair stays in place.
			if (refreshToken == replacedRefreshToken
				&& withinRefreshGrace(replacedAtMs, nowMs(), graceMs)) {
				finishRefresh(makeResult(
					RefreshStatus::Fresh,
					CarrierOutcome()));
				return;
			}
			clear();
			finishRefresh(makeResult(
				RefreshStatus::Ended,
				CarrierOutcome()));
			return;
		}
		const int waitSeconds = floodWaitSeconds(error);
		if (error.code == RateLimitedCode
			&& waitSeconds >= 0
			&& deferRefresh(waitSeconds)) {
			finishRefresh(makeResult(RefreshStatus::Fresh, CarrierOutcome()));
			return;
		}
		finishRefresh(makeResult(RefreshStatus::Failed, reply.outcome));
		return;
	}
	if (reply.outcome.kind != CarrierOutcomeKind::Reply) {
		finishRefresh(makeResult(RefreshStatus::Failed, reply.outcome));
		return;
	}
	const std::optional<RefreshAuthorization> authorization
		= decodeRefreshSession(reply.data);
	if (!authorization || !isComplete(authorization->tokens)) {
		finishRefresh(makeResult(RefreshStatus::Failed, CarrierOutcome()));
		return;
	}
	applyRefreshed(
		refreshToken,
		authorization->tokens,
		authorization->graceSeconds);
	finishRefresh(makeResult(RefreshStatus::Fresh, CarrierOutcome()));
}

void TokenManager::applyRefreshed(
		const std::string &replacedToken,
		const SessionTokens &next,
		std::int64_t serverGraceSeconds) {
	replacedRefreshToken = replacedToken;
	replacedAtMs = nowMs();
	graceMs = refreshGraceMs(serverGraceSeconds);
	installPair(next);
	notifySessionChanged();
}

bool TokenManager::deferRefresh(std::int64_t waitSeconds) {
	if (!schedule
		|| !floodDeferralAllowed(heldExpSeconds, nowMs(), waitSeconds)) {
		return false;
	}
	deferredPending = true;
	const std::uint64_t generation = ++deferredGeneration;
	later(waitSeconds * MsPerSecond, [this, generation]() {
		runDeferredRefresh(generation);
	});
	return true;
}

void TokenManager::runDeferredRefresh(std::uint64_t generation) {
	if (deferredGeneration != generation) {
		return;
	}
	deferredPending = false;
	refreshBlocking([](RefreshResult) {});
}

void TokenManager::cancelDeferred() {
	deferredPending = false;
	++deferredGeneration;
}

void TokenManager::later(std::int64_t delayMs, std::function<void()> task) {
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

void TokenManager::finishRefresh(RefreshResult result) {
	refreshing = false;
	std::vector<RefreshDone> ready = std::move(waiters);
	waiters.clear();
	for (RefreshDone &done : ready) {
		done(result);
	}
}

void TokenManager::notifySessionChanged() {
	if (sessionChanged) {
		sessionChanged();
	}
}

} // namespace Impulse
