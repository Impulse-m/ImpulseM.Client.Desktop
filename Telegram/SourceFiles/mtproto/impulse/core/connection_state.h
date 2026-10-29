// Connection state machine and the sync planner that drives it.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace Impulse {

enum class ConnectionPhase {
	Disconnected,
	ConnectingIdentity,
	IdentityReady,
	ConnectingSession,
	SessionReady,
	Refreshing,
	Failed,
};

enum class FailureReason {
	None,
	Auth,
};

// Not thread-safe: the owner calls it from the MTP worker thread only.
// Every event returns true when it moved the machine and false when the
// current phase has no such transition; a rejected event changes nothing.
class ConnectionState {
private:
	ConnectionPhase phase = ConnectionPhase::Disconnected;
	FailureReason failure = FailureReason::None;

public:
	[[nodiscard]] ConnectionPhase currentPhase() const;
	[[nodiscard]] FailureReason currentFailure() const;

	// Disconnected to ConnectingIdentity, or to ConnectingSession when a
	// session is stored.
	bool onMintSucceeded(bool sessionStored);

	// ConnectingIdentity to IdentityReady.
	bool onIdentityAcknowledged();

	// IdentityReady to ConnectingSession (login completed or session loaded).
	bool onSessionAvailable();

	// ConnectingSession to SessionReady.
	bool onSessionAcknowledged();

	// SessionReady to Refreshing (refresh point or 401 AUTH_KEY_UNREGISTERED).
	bool onRefreshDue();

	// Refreshing back to SessionReady.
	bool onRefreshSucceeded();

	// Refreshing to Failed(Auth) when the answer is unrecoverable. A
	// recoverable answer keeps the phase at Refreshing.
	bool onRefreshFailed(bool unrecoverable);

	// Network loss from any phase, including Failed, to Disconnected. Clears
	// the failure reason.
	bool onNetworkLost();
};

enum class SocketKind {
	Identity,
	Session,
};

inline constexpr std::chrono::milliseconds kSyncPlannerDefaultPeriod
	= std::chrono::seconds(5);

// Turns the observed login and network state into the connection actions.
// The remembered user, the last online flag and the last socket kind change
// inside one call, so the decision and the mutation are atomic.
// Not thread-safe.
class SyncPlanner {
public:
	struct Plan {
		// User whose lane must be dropped first, or 0.
		int64_t dropUserLane = 0;
		// Stop and clear the channel lanes.
		bool stopLanes = false;
		// The socket must be dropped now (offline, or a rekey).
		bool disconnect = false;
		// The drop is a rekey: network return, user change or kind change.
		// The first connect is never a rekey, and the backoff is skipped.
		bool rekey = false;
		// User whose lane must be subscribed, or 0.
		int64_t subscribeUserLane = 0;
		bool connect = false;
		// The credential the socket presents when it connects.
		SocketKind kind = SocketKind::Identity;
	};

private:
	int64_t laneUserId = 0;
	bool wasOnline = true;
	std::optional<SocketKind> connectedKind;
	std::chrono::milliseconds planPeriod;

public:
	explicit SyncPlanner(
		std::chrono::milliseconds period = kSyncPlannerDefaultPeriod);

	// The period at which the owner calls plan(). The planner does not run
	// a timer itself.
	[[nodiscard]] std::chrono::milliseconds period() const;

	[[nodiscard]] Plan plan(
		int64_t userId,
		bool sessionPresent,
		bool online);
};

} // namespace Impulse
