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

// Why the action is due. None when no difference is due. SocketReconnected
// is the only cause when the subscribe recovered and the position was not
// dropped, so the difference comes from the socket reconnect alone.
enum class GapReason {
	None,
	PositionDropped,
	NotRecovered,
	SocketReconnected,
};

// Not thread-safe: the owner calls it from the MTP worker thread only.
// A user lane ("user:" prefix) answers AccountDifference. Any other lane
// answers ChannelDifference; the caller runs getChannelDifference for that
// channel and falls back to the account getDifference when it fails.
class GapTracker {
private:
	std::set<std::string> positionDropped;
	bool connectedBefore = false;
	bool reconnectPending = false;

public:
	// Records a connected reply of the update socket. A reply that follows an
	// earlier one is a reconnect, which the next user lane subscribe spends.
	void onConnected();

	// Records the unsubscribe push of a lane. Code 2502 marks the stored
	// position as dropped, so the next subscribe counts as a gap. Any other
	// code clears the mark.
	void onUnsubscribed(std::string_view channel, int code);

	// Returns the action the caller must run after a subscribe of the lane.
	// recovered is the server flag of the subscribe, wasRecovering tells
	// whether the lane was recovering before this subscribe. A user lane
	// subscribe also spends a pending reconnect, so one reconnect costs one
	// account difference even when the user lane recovers. When reason is
	// given, it receives the cause of the returned action.
	[[nodiscard]] GapAction onSubscribed(
		std::string_view channel,
		bool recovered,
		bool wasRecovering,
		GapReason *reason = nullptr);
};

} // namespace Impulse
