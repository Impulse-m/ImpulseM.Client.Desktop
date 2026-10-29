#include "mtproto/impulse/core/connection_state.h"

namespace Impulse {

ConnectionPhase ConnectionState::currentPhase() const {
	return phase;
}

FailureReason ConnectionState::currentFailure() const {
	return failure;
}

bool ConnectionState::onMintSucceeded(bool sessionStored) {
	if (phase != ConnectionPhase::Disconnected) {
		return false;
	}
	phase = sessionStored
		? ConnectionPhase::ConnectingSession
		: ConnectionPhase::ConnectingIdentity;
	return true;
}

bool ConnectionState::onIdentityAcknowledged() {
	if (phase != ConnectionPhase::ConnectingIdentity) {
		return false;
	}
	phase = ConnectionPhase::IdentityReady;
	return true;
}

bool ConnectionState::onSessionAvailable() {
	if (phase != ConnectionPhase::IdentityReady) {
		return false;
	}
	phase = ConnectionPhase::ConnectingSession;
	return true;
}

bool ConnectionState::onSessionAcknowledged() {
	if (phase != ConnectionPhase::ConnectingSession) {
		return false;
	}
	phase = ConnectionPhase::SessionReady;
	return true;
}

bool ConnectionState::onRefreshDue() {
	if (phase != ConnectionPhase::SessionReady) {
		return false;
	}
	phase = ConnectionPhase::Refreshing;
	return true;
}

bool ConnectionState::onRefreshSucceeded() {
	if (phase != ConnectionPhase::Refreshing) {
		return false;
	}
	phase = ConnectionPhase::SessionReady;
	return true;
}

bool ConnectionState::onRefreshFailed(bool unrecoverable) {
	if (phase != ConnectionPhase::Refreshing) {
		return false;
	}
	if (unrecoverable) {
		phase = ConnectionPhase::Failed;
		failure = FailureReason::Auth;
	}
	return true;
}

bool ConnectionState::onNetworkLost() {
	phase = ConnectionPhase::Disconnected;
	failure = FailureReason::None;
	return true;
}

SyncPlanner::SyncPlanner(std::chrono::milliseconds period)
: planPeriod(period) {
}

std::chrono::milliseconds SyncPlanner::period() const {
	return planPeriod;
}

SyncPlanner::Plan SyncPlanner::plan(
		int64_t userId,
		bool sessionPresent,
		bool online) {
	const bool loggedIn = userId != 0 && sessionPresent;
	const bool cameBack = online && !wasOnline;
	wasOnline = online;

	const int64_t previous = laneUserId;
	laneUserId = loggedIn ? userId : 0;
	const int64_t drop = (previous != 0 && previous != laneUserId)
		? previous
		: 0;

	Plan result;
	result.dropUserLane = drop;
	result.stopLanes = !loggedIn || drop != 0;
	if (!online) {
		result.disconnect = true;
		return result;
	}

	const SocketKind kind = sessionPresent
		? SocketKind::Session
		: SocketKind::Identity;
	const bool kindChanged = connectedKind.has_value()
		&& *connectedKind != kind;
	connectedKind = kind;

	// The very first connect has nothing to drop, so it is not a rekey.
	result.rekey = cameBack || drop != 0 || kindChanged;
	result.disconnect = result.rekey;
	result.subscribeUserLane = loggedIn ? userId : 0;
	result.connect = true;
	result.kind = kind;
	return result;
}

} // namespace Impulse
