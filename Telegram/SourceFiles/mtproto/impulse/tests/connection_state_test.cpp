#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/core/connection_state.h"

#include <chrono>

IMPULSE_TEST(ConnectionStateIdentityThenSessionPath) {
	Impulse::ConnectionState state;
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::Disconnected);
	IMPULSE_CHECK(state.onMintSucceeded(false));
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::ConnectingIdentity);
	IMPULSE_CHECK(state.onIdentityAcknowledged());
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::IdentityReady);
	IMPULSE_CHECK(state.onSessionAvailable());
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::ConnectingSession);
	IMPULSE_CHECK(state.onSessionAcknowledged());
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::SessionReady);
}

IMPULSE_TEST(ConnectionStateStoredSessionSkipsIdentity) {
	Impulse::ConnectionState state;
	IMPULSE_CHECK(state.onMintSucceeded(true));
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::ConnectingSession);
	IMPULSE_CHECK(state.onSessionAcknowledged());
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::SessionReady);
}

IMPULSE_TEST(ConnectionStateRejectsOutOfOrderEvents) {
	Impulse::ConnectionState state;
	IMPULSE_CHECK(!state.onIdentityAcknowledged());
	IMPULSE_CHECK(!state.onSessionAvailable());
	IMPULSE_CHECK(!state.onSessionAcknowledged());
	IMPULSE_CHECK(!state.onRefreshDue());
	IMPULSE_CHECK(!state.onRefreshSucceeded());
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::Disconnected);
	IMPULSE_CHECK(state.onMintSucceeded(false));
	IMPULSE_CHECK(!state.onMintSucceeded(false));
	IMPULSE_CHECK(!state.onSessionAcknowledged());
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::ConnectingIdentity);
}

IMPULSE_TEST(ConnectionStateRefreshRoundTrip) {
	Impulse::ConnectionState state;
	IMPULSE_CHECK(state.onMintSucceeded(true));
	IMPULSE_CHECK(state.onSessionAcknowledged());
	IMPULSE_CHECK(state.onRefreshDue());
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::Refreshing);
	IMPULSE_CHECK(state.onRefreshSucceeded());
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::SessionReady);
	IMPULSE_CHECK(state.currentFailure() == Impulse::FailureReason::None);
}

IMPULSE_TEST(ConnectionStateRecoverableRefreshStaysRefreshing) {
	Impulse::ConnectionState state;
	IMPULSE_CHECK(state.onMintSucceeded(true));
	IMPULSE_CHECK(state.onSessionAcknowledged());
	IMPULSE_CHECK(state.onRefreshDue());
	IMPULSE_CHECK(state.onRefreshFailed(false));
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::Refreshing);
	IMPULSE_CHECK(state.currentFailure() == Impulse::FailureReason::None);
}

IMPULSE_TEST(ConnectionStateUnrecoverableRefreshFailsWithAuth) {
	Impulse::ConnectionState state;
	IMPULSE_CHECK(state.onMintSucceeded(true));
	IMPULSE_CHECK(state.onSessionAcknowledged());
	IMPULSE_CHECK(state.onRefreshDue());
	IMPULSE_CHECK(state.onRefreshFailed(true));
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::Failed);
	IMPULSE_CHECK(state.currentFailure() == Impulse::FailureReason::Auth);
}

IMPULSE_TEST(ConnectionStateNetworkLossFromAnyPhase) {
	Impulse::ConnectionState identity;
	IMPULSE_CHECK(identity.onMintSucceeded(false));
	IMPULSE_CHECK(identity.onNetworkLost());
	IMPULSE_CHECK(identity.currentPhase() == Impulse::ConnectionPhase::Disconnected);

	Impulse::ConnectionState session;
	IMPULSE_CHECK(session.onMintSucceeded(true));
	IMPULSE_CHECK(session.onSessionAcknowledged());
	IMPULSE_CHECK(session.onRefreshDue());
	IMPULSE_CHECK(session.onNetworkLost());
	IMPULSE_CHECK(session.currentPhase() == Impulse::ConnectionPhase::Disconnected);
}

IMPULSE_TEST(ConnectionStateNetworkLossClearsFailedAuth) {
	Impulse::ConnectionState state;
	IMPULSE_CHECK(state.onMintSucceeded(true));
	IMPULSE_CHECK(state.onSessionAcknowledged());
	IMPULSE_CHECK(state.onRefreshDue());
	IMPULSE_CHECK(state.onRefreshFailed(true));
	IMPULSE_CHECK(state.onNetworkLost());
	IMPULSE_CHECK(state.currentPhase() == Impulse::ConnectionPhase::Disconnected);
	IMPULSE_CHECK(state.currentFailure() == Impulse::FailureReason::None);
	IMPULSE_CHECK(state.onMintSucceeded(false));
}

IMPULSE_TEST(SyncPlannerFirstConnectIsNotRekey) {
	Impulse::SyncPlanner planner;
	const Impulse::SyncPlanner::Plan plan = planner.plan(0, false, true);
	IMPULSE_CHECK(plan.connect);
	IMPULSE_CHECK(!plan.disconnect);
	IMPULSE_CHECK(!plan.rekey);
	IMPULSE_CHECK(plan.kind == Impulse::SocketKind::Identity);
	IMPULSE_CHECK(plan.subscribeUserLane == 0);
	IMPULSE_CHECK(plan.stopLanes);
}

IMPULSE_TEST(SyncPlannerFirstSessionConnectIsNotRekey) {
	Impulse::SyncPlanner planner;
	const Impulse::SyncPlanner::Plan plan = planner.plan(42, true, true);
	IMPULSE_CHECK(plan.connect);
	IMPULSE_CHECK(!plan.disconnect);
	IMPULSE_CHECK(!plan.rekey);
	IMPULSE_CHECK(plan.kind == Impulse::SocketKind::Session);
	IMPULSE_CHECK(plan.subscribeUserLane == 42);
	IMPULSE_CHECK(!plan.stopLanes);
	IMPULSE_CHECK(plan.dropUserLane == 0);
}

IMPULSE_TEST(SyncPlannerSteadyStateNoAction) {
	Impulse::SyncPlanner planner;
	IMPULSE_CHECK(!planner.plan(42, true, true).rekey);
	const Impulse::SyncPlanner::Plan plan = planner.plan(42, true, true);
	IMPULSE_CHECK(!plan.rekey);
	IMPULSE_CHECK(!plan.disconnect);
	IMPULSE_CHECK(plan.connect);
	IMPULSE_CHECK(plan.subscribeUserLane == 42);
}

IMPULSE_TEST(SyncPlannerOfflineGivesDisconnect) {
	Impulse::SyncPlanner planner;
	IMPULSE_CHECK(!planner.plan(42, true, true).rekey);
	const Impulse::SyncPlanner::Plan plan = planner.plan(42, true, false);
	IMPULSE_CHECK(plan.disconnect);
	IMPULSE_CHECK(!plan.connect);
	IMPULSE_CHECK(!plan.rekey);
	IMPULSE_CHECK(plan.subscribeUserLane == 0);
}

IMPULSE_TEST(SyncPlannerNetworkReturnRekeys) {
	Impulse::SyncPlanner planner;
	IMPULSE_CHECK(!planner.plan(42, true, true).rekey);
	IMPULSE_CHECK(planner.plan(42, true, false).disconnect);
	const Impulse::SyncPlanner::Plan plan = planner.plan(42, true, true);
	IMPULSE_CHECK(plan.rekey);
	IMPULSE_CHECK(plan.disconnect);
	IMPULSE_CHECK(plan.connect);
}

IMPULSE_TEST(SyncPlannerNetworkReturnAfterStartupOfflineRekeys) {
	Impulse::SyncPlanner planner;
	IMPULSE_CHECK(planner.plan(0, false, false).disconnect);
	const Impulse::SyncPlanner::Plan plan = planner.plan(0, false, true);
	IMPULSE_CHECK(plan.rekey);
	IMPULSE_CHECK(plan.connect);
}

IMPULSE_TEST(SyncPlannerKindChangeRekeys) {
	Impulse::SyncPlanner planner;
	IMPULSE_CHECK(!planner.plan(0, false, true).rekey);
	const Impulse::SyncPlanner::Plan plan = planner.plan(42, true, true);
	IMPULSE_CHECK(plan.rekey);
	IMPULSE_CHECK(plan.disconnect);
	IMPULSE_CHECK(plan.kind == Impulse::SocketKind::Session);
}

IMPULSE_TEST(SyncPlannerUserChangeRekeysAndDropsOldLane) {
	Impulse::SyncPlanner planner;
	IMPULSE_CHECK(!planner.plan(1, true, true).rekey);
	const Impulse::SyncPlanner::Plan plan = planner.plan(2, true, true);
	IMPULSE_CHECK(plan.rekey);
	IMPULSE_CHECK(plan.dropUserLane == 1);
	IMPULSE_CHECK(plan.subscribeUserLane == 2);
	IMPULSE_CHECK(plan.stopLanes);
}

IMPULSE_TEST(SyncPlannerLogoutDropsLaneAndRekeysAsIdentity) {
	Impulse::SyncPlanner planner;
	IMPULSE_CHECK(!planner.plan(1, true, true).rekey);
	const Impulse::SyncPlanner::Plan plan = planner.plan(0, false, true);
	IMPULSE_CHECK(plan.rekey);
	IMPULSE_CHECK(plan.dropUserLane == 1);
	IMPULSE_CHECK(plan.stopLanes);
	IMPULSE_CHECK(plan.kind == Impulse::SocketKind::Identity);
	IMPULSE_CHECK(plan.subscribeUserLane == 0);
}

IMPULSE_TEST(SyncPlannerPeriodIsParameter) {
	Impulse::SyncPlanner standard;
	IMPULSE_CHECK(standard.period() == std::chrono::milliseconds(5000));
	Impulse::SyncPlanner fast(std::chrono::milliseconds(250));
	IMPULSE_CHECK(fast.period() == std::chrono::milliseconds(250));
}
