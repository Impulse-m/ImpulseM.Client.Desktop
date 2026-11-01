#include "mtproto/impulse/core/gap_tracker.h"

namespace Impulse {
namespace {

constexpr std::string_view kUserLanePrefix = "user:";

[[nodiscard]] bool IsUserLane(std::string_view channel) {
	return channel.substr(0, kUserLanePrefix.size()) == kUserLanePrefix;
}

} // namespace

void GapTracker::onConnected() {
	if (connectedBefore) {
		reconnectPending = true;
	}
	connectedBefore = true;
}

void GapTracker::onUnsubscribed(std::string_view channel, int code) {
	const std::string key(channel);
	if (code == kPositionDroppedCode) {
		positionDropped.insert(key);
	} else {
		positionDropped.erase(key);
	}
}

GapAction GapTracker::onSubscribed(
	std::string_view channel,
	bool recovered,
	bool wasRecovering,
	GapReason *reason) {
	const bool dropped = positionDropped.erase(std::string(channel)) > 0;
	GapAction action = GapAction::None;
	GapReason cause = GapReason::None;
	if (IsUserLane(channel)) {
		// The first user lane subscribe after a reconnect spends the pending
		// reconnect, whether or not the lane recovers.
		const bool reconnect = reconnectPending;
		reconnectPending = false;
		if (!recovered) {
			action = GapAction::AccountDifference;
			cause = GapReason::NotRecovered;
		} else if (dropped) {
			action = GapAction::AccountDifference;
			cause = GapReason::PositionDropped;
		} else if (reconnect) {
			action = GapAction::AccountDifference;
			cause = GapReason::SocketReconnected;
		}
	} else if (dropped || (wasRecovering && !recovered)) {
		action = GapAction::ChannelDifference;
		cause = recovered ? GapReason::PositionDropped : GapReason::NotRecovered;
	}
	if (reason) {
		*reason = cause;
	}
	return action;
}

} // namespace Impulse
