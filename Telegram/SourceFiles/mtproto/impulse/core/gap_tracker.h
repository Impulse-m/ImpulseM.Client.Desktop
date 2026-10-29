// Decides when a (re)subscribe means the client may have missed updates.
#pragma once

#include <set>
#include <string>
#include <string_view>

namespace Impulse {

inline constexpr int kPositionDroppedCode = 2502;

enum class GapAction {
	None,
	AccountDifference,
	ChannelDifference,
};

// Not thread-safe: the owner calls it from the MTP worker thread only.
// A user lane ("user:" prefix) answers AccountDifference. Any other lane
// answers ChannelDifference; the caller runs getChannelDifference for that
// channel and falls back to the account getDifference when it fails.
class GapTracker {
private:
	std::set<std::string> positionDropped;

public:
	// Records the unsubscribe push of a lane. Code 2502 marks the stored
	// position as dropped, so the next subscribe counts as a gap. Any other
	// code clears the mark.
	void onUnsubscribed(std::string_view channel, int code);

	// Returns the action the caller must run after a subscribe of the lane.
	// recovered is the server flag of the subscribe, wasRecovering tells
	// whether the lane was recovering before this subscribe.
	[[nodiscard]] GapAction onSubscribed(
		std::string_view channel,
		bool recovered,
		bool wasRecovering);
};

} // namespace Impulse
