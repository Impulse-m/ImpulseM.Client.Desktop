#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/core/gap_tracker.h"

IMPULSE_TEST(GapTrackerRecoveredUserLaneNeedsNothing) {
	Impulse::GapTracker tracker;
	IMPULSE_CHECK(tracker.onSubscribed("user:5", true, true)
		== Impulse::GapAction::None);
}

IMPULSE_TEST(GapTrackerFreshUserLaneNeedsAccountDifference) {
	Impulse::GapTracker tracker;
	IMPULSE_CHECK(tracker.onSubscribed("user:5", false, false)
		== Impulse::GapAction::AccountDifference);
	IMPULSE_CHECK(tracker.onSubscribed("user:5", false, true)
		== Impulse::GapAction::AccountDifference);
}

IMPULSE_TEST(GapTrackerLostUserLanePositionNeedsAccountDifference) {
	Impulse::GapTracker tracker;
	IMPULSE_CHECK(tracker.onSubscribed("user:5", false, true)
		== Impulse::GapAction::AccountDifference);
}

IMPULSE_TEST(GapTrackerDroppedUserLanePositionIsForcedEvenIfRecovered) {
	Impulse::GapTracker tracker;
	tracker.onUnsubscribed("user:5", 2502);
	IMPULSE_CHECK(tracker.onSubscribed("user:5", true, true)
		== Impulse::GapAction::AccountDifference);
	IMPULSE_CHECK(tracker.onSubscribed("user:5", true, true)
		== Impulse::GapAction::None);
}

IMPULSE_TEST(GapTrackerFirstChannelSubscribeIsNoGap) {
	Impulse::GapTracker tracker;
	IMPULSE_CHECK(tracker.onSubscribed("channel:9", false, false)
		== Impulse::GapAction::None);
}

IMPULSE_TEST(GapTrackerRecoveredChannelResubscribeIsNoGap) {
	Impulse::GapTracker tracker;
	IMPULSE_CHECK(tracker.onSubscribed("channel:9", true, true)
		== Impulse::GapAction::None);
}

IMPULSE_TEST(GapTrackerFailedChannelRecoveryUsesChannelDifference) {
	Impulse::GapTracker tracker;
	IMPULSE_CHECK(tracker.onSubscribed("channel:9", false, true)
		== Impulse::GapAction::ChannelDifference);
}

IMPULSE_TEST(GapTrackerCode2502ChannelGapIsOnce) {
	Impulse::GapTracker tracker;
	tracker.onUnsubscribed("channel:9", 2502);
	IMPULSE_CHECK(tracker.onSubscribed("channel:9", false, false)
		== Impulse::GapAction::ChannelDifference);
	IMPULSE_CHECK(tracker.onSubscribed("channel:9", false, false)
		== Impulse::GapAction::None);
}

IMPULSE_TEST(GapTrackerOtherUnsubscribeCodesAreNoGap) {
	Impulse::GapTracker tracker;
	tracker.onUnsubscribed("channel:9", 2500);
	IMPULSE_CHECK(tracker.onSubscribed("channel:9", true, true)
		== Impulse::GapAction::None);
}

IMPULSE_TEST(GapTrackerTerminalUnsubscribeForgetsTheMark) {
	Impulse::GapTracker tracker;
	tracker.onUnsubscribed("channel:9", 2502);
	tracker.onUnsubscribed("channel:9", 2000);
	IMPULSE_CHECK(tracker.onSubscribed("channel:9", false, false)
		== Impulse::GapAction::None);
}

IMPULSE_TEST(GapTrackerMarkIsPerChannel) {
	Impulse::GapTracker tracker;
	tracker.onUnsubscribed("channel:9", 2502);
	IMPULSE_CHECK(tracker.onSubscribed("channel:10", false, false)
		== Impulse::GapAction::None);
	IMPULSE_CHECK(tracker.onSubscribed("channel:9", false, false)
		== Impulse::GapAction::ChannelDifference);
}
