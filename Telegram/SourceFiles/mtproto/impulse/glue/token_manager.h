// Account session of the signed-in slot: the access and refresh tokens, the
// refresh point of the access token and the refresh itself. The refresh
// rotates the refresh token and honours the server grace. Owner thread only:
// every call and every carrier or scheduler callback runs on the thread that
// owns the manager.
#pragma once

#include "mtproto/impulse/core/token_policy.h"
#include "mtproto/impulse/glue/carrier_errors.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Impulse {

// The account token pair. A usable session has both tokens and a user id.
struct SessionTokens {
	std::string accessToken;
	std::string refreshToken;
	std::int64_t userId = 0;
};

// Persists the session pair in the account store. Implemented by the account
// blob. Called on the owner thread only.
class SessionStore {
public:
	virtual ~SessionStore() = default;

	// Nullopt when nothing is stored.
	[[nodiscard]] virtual std::optional<SessionTokens> load() = 0;

	virtual void save(const SessionTokens &tokens) = 0;
	virtual void clear() = 0;
};

// The reply of one refreshSession call. The data is the protobuf
// RefreshSessionResponse payload and is empty unless the outcome is a reply.
struct RefreshCarrierReply {
	CarrierOutcome outcome;
	std::vector<std::uint8_t> data;
};

using RefreshCarrierDone = std::function<void(RefreshCarrierReply reply)>;

// Sends refreshSession with the protobuf request payload over one carrier:
// the update socket or the pool fleet. The done callback runs exactly once on
// the owner thread.
using RefreshCarrierCall = std::function<void(
	std::vector<std::uint8_t> payload,
	std::int64_t timeoutMs,
	RefreshCarrierDone done)>;

// Runs task after delayMs on the owner thread.
using RefreshScheduler = std::function<void(
	std::int64_t delayMs,
	std::function<void()> task)>;

// Milliseconds since the unix epoch.
using RefreshClock = std::function<std::int64_t()>;

enum class RefreshStatus {
	// A fresh session is in place. A FLOOD_WAIT deferral and a replay within
	// the grace also end here, with the current tokens kept.
	Fresh,

	// The session is gone: an unrecoverable answer cleared it, or there was
	// none to refresh.
	Ended,

	// A transport or unexpected failure. The session is kept.
	Failed,
};

struct RefreshResult {
	RefreshStatus status = RefreshStatus::Failed;
	CarrierOutcome failure;
};

using RefreshDone = std::function<void(RefreshResult result)>;

struct ConnectionTokenResult {
	// The access token the update socket connects with. Empty unless the
	// status is Fresh.
	std::string token;
	RefreshStatus status = RefreshStatus::Fresh;
};

using ConnectionTokenDone = std::function<void(ConnectionTokenResult result)>;

struct TokenManagerDependencies {
	SessionStore *store = nullptr;
	RefreshScheduler schedule;
	RefreshClock nowMs;
	RandomUnit random;
};

class TokenManager {
private:
	SessionStore *store = nullptr;
	RefreshCarrierCall updateCall;
	RefreshCarrierCall poolCall;
	RefreshScheduler schedule;
	RefreshClock nowMs;
	RandomUnit random;
	std::function<void()> sessionChanged;

	std::optional<SessionTokens> held;
	bool hasPoint = false;
	std::int64_t pointAtMs = 0;
	std::int64_t heldExpSeconds = 0;
	std::uint64_t pointGeneration = 0;
	std::string refusedAccessToken;

	bool refreshing = false;
	std::vector<RefreshDone> waiters;
	bool deferredPending = false;
	std::uint64_t deferredGeneration = 0;

	// The refresh token the last successful refresh replaced, and the grace
	// during which it still answers the same session.
	std::string replacedRefreshToken;
	std::int64_t replacedAtMs = 0;
	std::int64_t graceMs = 0;

	// Scheduled tasks hold a weak copy and do nothing once the manager is
	// destroyed.
	std::shared_ptr<bool> alive;

public:
	// Loads the stored pair. A complete pair gets its refresh point scheduled;
	// anything else leaves the manager without a session.
	explicit TokenManager(TokenManagerDependencies dependencies);

	TokenManager(const TokenManager &other) = delete;
	TokenManager &operator=(const TokenManager &other) = delete;

	// The update socket and the pool fleet that carry refreshSession. Set
	// after the manager is built, because the socket is built after it. A
	// refresh fails with a local error until both are set.
	void setRefreshCarriers(
		RefreshCarrierCall updateSocket,
		RefreshCarrierCall poolFleet);

	// Called after a login or a refresh stored a new pair. Never called on
	// clear.
	void setSessionChanged(std::function<void()> callback);

	[[nodiscard]] bool hasSession() const;

	// The stored access token, or an empty string without a session.
	[[nodiscard]] std::string bearer() const;

	// True when the access token is refused or its exp has passed.
	[[nodiscard]] bool accessUnusable() const;

	// The server refused this access token, e.g. with 103 or 109 on the
	// socket.
	void markRefused(const std::string &accessToken);

	// Stores a pair from a login, drops refusal state and cancels a deferred
	// refresh.
	void onLoginTokens(const SessionTokens &tokens);

	// Forgets the session in memory and in the store.
	void clear();

	// Single-flight refresh. While one refresh is on the wire, every other
	// caller waits for its outcome instead of sending a second request, which
	// would replace the refresh token again. The carrier is the pool fleet
	// once the access token is unusable, the update socket otherwise, and the
	// pool fleet again when the socket refuses the token. The session is
	// cleared on AUTH_TOKEN_INVALID, AUTH_TOKEN_EXPIRED and SESSION_EXPIRED.
	// The done callback runs on the owner thread, possibly before this call
	// returns when no refresh is needed.
	void refreshBlocking(RefreshDone done);

	// The access token for the update socket. An unusable token is refreshed
	// first through refreshBlocking.
	void connectionToken(ConnectionTokenDone done);

private:
	void hold(const SessionTokens &tokens);
	void installPair(const SessionTokens &tokens);
	void schedulePoint(std::int64_t retryMs);
	void pointReached(std::uint64_t generation, std::int64_t retryMs);
	void retryPoint(std::uint64_t generation, std::int64_t retryMs);
	void sendRefresh(
		RefreshCarrierCall call,
		std::string refreshToken,
		std::string accessAtStart,
		bool viaUpdate);
	void onRefreshReply(
		RefreshCarrierReply reply,
		const std::string &refreshToken,
		const std::string &accessAtStart,
		bool viaUpdate);
	void applyRefreshed(
		const std::string &replacedToken,
		const SessionTokens &next,
		std::int64_t serverGraceSeconds);
	[[nodiscard]] bool deferRefresh(std::int64_t waitSeconds);
	void runDeferredRefresh(std::uint64_t generation);
	void cancelDeferred();
	void later(std::int64_t delayMs, std::function<void()> task);
	void finishRefresh(RefreshResult result);
	void notifySessionChanged();
};

} // namespace Impulse
