// Keeps the realtime lanes of one account in line with the chat list. Channel
// lanes follow the opened channel and the most recent channel dialogs. Account
// lanes (user, session and identity) follow the signed-in identity. Qt-free:
// the owner supplies the dialog snapshot, the lane actions and the timer, and
// calls every method from one thread.
#pragma once

#include "mtproto/impulse/core/lanes.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Impulse {

inline constexpr std::chrono::milliseconds kLanesRecomputeDelay
	= std::chrono::milliseconds(500);

struct LaneIdentity {
	int64_t userId = 0;
	std::string sessionHash;
	std::string identityId;
};

class LanesObserver {
public:
	using DialogsProvider = std::function<std::vector<LaneCandidate>()>;
	using LaneAction = std::function<void(const std::string &lane)>;
	using Scheduler = std::function<void(
		std::chrono::milliseconds delay,
		std::function<void()> task)>;

private:
	DialogsProvider dialogs;
	LaneAction subscribeLane;
	LaneAction unsubscribeLane;
	Scheduler schedule;
	std::shared_ptr<bool> alive;
	std::set<std::string> lanes;
	LaneIdentity identity;
	int64_t openedChannelId = 0;
	std::uint64_t generation = 0;
	bool active = false;

public:
	LanesObserver(
		DialogsProvider dialogsProvider,
		LaneAction subscribe,
		LaneAction unsubscribe,
		Scheduler scheduler);
	LanesObserver(const LanesObserver &other) = delete;
	LanesObserver &operator=(const LanesObserver &other) = delete;
	~LanesObserver();

	// Begins following the chat list and subscribes the account lanes.
	void start();

	// Stops the channel lanes: every channel lane is unsubscribed and a
	// pending recompute is dropped.
	void stop();

	// Schedules a recompute 500 ms after the last request.
	void requestRecompute();

	void setOpenedChannel(int64_t channelId);

	// Replaces the identity and reconciles the account lanes.
	void setIdentity(LaneIdentity value);

	[[nodiscard]] bool isActive() const;
	[[nodiscard]] bool isSubscribed(const std::string &lane) const;

private:
	void snapshot();
	void applyChannelLanes(const std::vector<int64_t> &desired);
	void applyAccountLanes();
	void subscribe(const std::string &lane);
	void unsubscribe(const std::string &lane);
	[[nodiscard]] std::set<std::string> desiredAccountLanes() const;
};

} // namespace Impulse
