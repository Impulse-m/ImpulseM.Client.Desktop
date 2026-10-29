#include "mtproto/impulse/core/gap_tracker.h"

namespace Impulse {
namespace {

constexpr std::string_view kUserLanePrefix = "user:";

[[nodiscard]] bool IsUserLane(std::string_view channel) {
	return channel.substr(0, kUserLanePrefix.size()) == kUserLanePrefix;
}

} // namespace

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
	bool wasRecovering) {
	const bool dropped = positionDropped.erase(std::string(channel)) > 0;
	if (IsUserLane(channel)) {
		return (dropped || !recovered)
			? GapAction::AccountDifference
			: GapAction::None;
	}
	return (dropped || (wasRecovering && !recovered))
		? GapAction::ChannelDifference
		: GapAction::None;
}

} // namespace Impulse
