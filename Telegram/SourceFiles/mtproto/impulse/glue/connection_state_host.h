// Qt-free host of the connection state machine. Forwards events to one
// ConnectionState and reports every accepted transition through a callback.
#pragma once

#include "mtproto/impulse/core/connection_state.h"

#include <functional>

namespace Impulse {

class ConnectionStateHost {
public:
	using PhaseCallback = std::function<void(
		ConnectionPhase phase,
		FailureReason failure)>;

private:
	ConnectionState state;
	PhaseCallback phaseChanged;

public:
	void setPhaseCallback(PhaseCallback callback);

	[[nodiscard]] ConnectionPhase currentPhase() const;
	[[nodiscard]] FailureReason currentFailure() const;

	bool onMintSucceeded(bool sessionStored);
	bool onIdentityAcknowledged();
	bool onSessionAvailable();
	bool onSessionAcknowledged();
	bool onRefreshDue();
	bool onRefreshSucceeded();
	bool onRefreshFailed(bool unrecoverable);
	bool onNetworkLost();

private:
	bool notify(bool moved);
};

} // namespace Impulse
