#include "mtproto/impulse/glue/connection_state_host.h"

#include <utility>

namespace Impulse {

void ConnectionStateHost::setPhaseCallback(PhaseCallback callback) {
	phaseChanged = std::move(callback);
}

ConnectionPhase ConnectionStateHost::currentPhase() const {
	return state.currentPhase();
}

FailureReason ConnectionStateHost::currentFailure() const {
	return state.currentFailure();
}

bool ConnectionStateHost::onMintSucceeded(bool sessionStored) {
	return notify(state.onMintSucceeded(sessionStored));
}

bool ConnectionStateHost::onIdentityAcknowledged() {
	return notify(state.onIdentityAcknowledged());
}

bool ConnectionStateHost::onSessionAvailable() {
	return notify(state.onSessionAvailable());
}

bool ConnectionStateHost::onSessionAcknowledged() {
	return notify(state.onSessionAcknowledged());
}

bool ConnectionStateHost::onRefreshDue() {
	return notify(state.onRefreshDue());
}

bool ConnectionStateHost::onRefreshSucceeded() {
	return notify(state.onRefreshSucceeded());
}

bool ConnectionStateHost::onRefreshFailed(bool unrecoverable) {
	return notify(state.onRefreshFailed(unrecoverable));
}

bool ConnectionStateHost::onNetworkLost() {
	return notify(state.onNetworkLost());
}

bool ConnectionStateHost::notify(bool moved) {
	if (moved && phaseChanged) {
		phaseChanged(state.currentPhase(), state.currentFailure());
	}
	return moved;
}

} // namespace Impulse
