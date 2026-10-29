// Connection identity of a signed-out slot. Minted on the pool fleet with an
// empty identity token, renewed on the update socket inside the refresh
// window, and persisted through a TokenStore. Owner thread only: every call
// and every carrier or scheduler callback runs on the thread that owns the
// manager.
#pragma once

#include "mtproto/impulse/core/token_policy.h"
#include "mtproto/impulse/glue/carrier_errors.h"
#include "mtproto/impulse/glue/token_store.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Impulse {

class IdentityListener {
public:
	virtual ~IdentityListener() = default;

	// A different identity replaced the current one. The update socket must
	// reconnect with it and declare again.
	virtual void onIdentityChanged() = 0;

	// The same identity got a renewed token. The live socket takes it.
	virtual void onIdentityRenewed(const std::string &identityToken) = 0;
};

// The reply of one connectionIdentity call. The data is the transcoded
// ConnectionIdentityResponse payload and is empty unless the outcome is a
// reply.
struct ConnectionIdentityReply {
	CarrierOutcome outcome;
	std::vector<std::uint8_t> data;
};

using ConnectionIdentityDone = std::function<void(ConnectionIdentityReply reply)>;

// Sends one connectionIdentity call with the given transcoded request payload.
// The mint goes to the pool fleet, a renewal to the update socket. The done
// callback runs exactly once on the owner thread.
using ConnectionIdentityCall = std::function<void(
	std::vector<std::uint8_t> payload,
	std::int64_t timeoutMs,
	ConnectionIdentityDone done)>;

// Runs task after delayMs on the owner thread.
using IdentityScheduler = std::function<void(
	std::int64_t delayMs,
	std::function<void()> task)>;

// Milliseconds since the unix epoch.
using IdentityClock = std::function<std::int64_t()>;

// The result of obtain(). The token is empty on failure, and failure then
// tells what went wrong. Local failures have the NetworkFailure kind.
struct IdentityObtainResult {
	std::string token;
	CarrierOutcome failure;
};

using IdentityObtainDone = std::function<void(IdentityObtainResult result)>;

struct IdentityManagerDependencies {
	TokenStore *store = nullptr;
	ConnectionIdentityCall mint;
	IdentityScheduler schedule;
	IdentityClock nowMs;
	RandomUnit random;
};

class IdentityManager {
private:
	struct Identity {
		std::string token;
		TokenClaims claims;
	};

	TokenStore *store = nullptr;
	ConnectionIdentityCall mintCall;
	ConnectionIdentityCall renewCall;
	IdentityScheduler schedule;
	IdentityClock nowMs;
	RandomUnit random;
	std::int32_t apiId = 0;
	IdentityListener *listener = nullptr;

	std::optional<Identity> identity;
	std::uint64_t epoch = 0;
	bool minting = false;
	std::vector<IdentityObtainDone> waiters;

	// Scheduled tasks hold a weak copy and do nothing once the manager is
	// destroyed.
	std::shared_ptr<bool> alive;

public:
	// Loads the stored identity. A token still usable for more than 60 s
	// becomes current and its renewal is scheduled; anything else is cleared
	// from the store.
	explicit IdentityManager(IdentityManagerDependencies dependencies);

	IdentityManager(const IdentityManager &other) = delete;
	IdentityManager &operator=(const IdentityManager &other) = delete;

	// Sent in every connectionIdentity request. Zero leaves api_id out.
	void setApiId(std::int32_t value);

	// The update socket that renews the identity. Renewal is skipped while
	// the call is empty.
	void setRenewalCall(ConnectionIdentityCall call);

	// Not owned. May be null.
	void setListener(IdentityListener *target);

	// The current token when it is usable for more than 60 s, else an empty
	// string. An identity past that point is discarded here. Never sends
	// anything.
	[[nodiscard]] std::string current();

	// The current token, or a new one minted on the pool fleet. Concurrent
	// callers share one mint. The mint times out after 30 s.
	void obtain(IdentityObtainDone done);

	// Forgets the identity in memory and in the store, and cancels the
	// renewal. Callers waiting on a mint fail with a local error.
	void discard();

	// The server refused this token. When it is the current one, it is
	// discarded and the listener is told to reconnect.
	void refuse(const std::string &refusedToken);

private:
	[[nodiscard]] static std::optional<Identity> readIdentity(
		const std::string &token);
	[[nodiscard]] static std::optional<Identity> identityFromReply(
		const ConnectionIdentityReply &reply,
		std::int64_t currentMs);
	[[nodiscard]] bool isCurrent(
		const Identity &candidate,
		std::uint64_t expectedEpoch) const;

	void expireIfNeeded();
	void forgetLocked();
	void persist(const std::string &identityToken);
	void later(
		std::int64_t delayMs,
		std::function<void()> task);
	void finishMint(
		std::uint64_t startedEpoch,
		ConnectionIdentityReply reply);
	void resolveWaiters(IdentityObtainResult result);
	void scheduleRenewal(
		const Identity &renewing,
		std::uint64_t installEpoch);
	void renew(
		const Identity &renewing,
		std::uint64_t startedEpoch,
		std::int64_t retryDelayMs);
	void finishRenewal(
		const Identity &renewing,
		std::uint64_t startedEpoch,
		std::int64_t retryDelayMs,
		ConnectionIdentityReply reply);
	void retryRenewal(
		const Identity &renewing,
		std::uint64_t startedEpoch,
		std::int64_t retryDelayMs);
};

} // namespace Impulse
