#include "mtproto/impulse/glue/lanes_observer.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

IMPULSE_TEST(LanesObserverAccountLanesHaveNoSessionLane) {
	std::vector<std::string> subscribed;
	Impulse::LanesObserver observer(
		[]() -> std::vector<Impulse::LaneCandidate> {
			return {};
		},
		[&subscribed](const std::string &lane) {
			subscribed.push_back(lane);
		},
		[](const std::string &) {
		},
		[](std::chrono::milliseconds, std::function<void()>) {
		});

	Impulse::LaneIdentity identity;
	identity.userId = 42;
	identity.identityId = "abc";
	observer.setIdentity(identity);
	observer.start();

	const std::vector<std::string> expected = { "identity:abc", "user:42" };
	IMPULSE_CHECK(subscribed == expected);
	IMPULSE_CHECK(!observer.isSubscribed("session:42:9f8e7d"));
}

IMPULSE_TEST(LanesObserverUserLaneWithoutIdentityLane) {
	std::vector<std::string> subscribed;
	Impulse::LanesObserver observer(
		[]() -> std::vector<Impulse::LaneCandidate> {
			return {};
		},
		[&subscribed](const std::string &lane) {
			subscribed.push_back(lane);
		},
		[](const std::string &) {
		},
		[](std::chrono::milliseconds, std::function<void()>) {
		});

	Impulse::LaneIdentity identity;
	identity.userId = 42;
	observer.setIdentity(identity);
	observer.start();

	const std::vector<std::string> expected = { "user:42" };
	IMPULSE_CHECK(subscribed == expected);
}
