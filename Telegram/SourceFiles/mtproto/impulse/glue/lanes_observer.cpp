#include "mtproto/impulse/glue/lanes_observer.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace Impulse {

namespace {

constexpr std::string_view kChannelPrefix = "channel:";
constexpr std::string_view kUserPrefix = "user:";
constexpr std::string_view kSessionPrefix = "session:";
constexpr std::string_view kIdentityPrefix = "identity:";

[[nodiscard]] bool IsChannelLane(const std::string &lane) {
	return std::string_view(lane).starts_with(kChannelPrefix);
}

} // namespace

LanesObserver::LanesObserver(
		DialogsProvider dialogsProvider,
		LaneAction subscribe,
		LaneAction unsubscribe,
		Scheduler scheduler)
: dialogs(std::move(dialogsProvider))
, subscribeLane(std::move(subscribe))
, unsubscribeLane(std::move(unsubscribe))
, schedule(std::move(scheduler))
, alive(std::make_shared<bool>(true)) {
}

LanesObserver::~LanesObserver() {
	*alive = false;
}

void LanesObserver::start() {
	if (active) {
		return;
	}
	active = true;
	applyAccountLanes();
	requestRecompute();
}

void LanesObserver::stop() {
	if (!active) {
		return;
	}
	active = false;
	++generation;

	std::vector<std::string> channels;
	for (const std::string &lane : lanes) {
		if (IsChannelLane(lane)) {
			channels.push_back(lane);
		}
	}
	for (const std::string &lane : channels) {
		unsubscribe(lane);
	}
}

void LanesObserver::requestRecompute() {
	if (!active) {
		return;
	}
	const std::uint64_t current = ++generation;
	std::shared_ptr<bool> keepAlive = alive;
	schedule(kLanesRecomputeDelay, [this, keepAlive, current]() {
		if (!*keepAlive || !active || current != generation) {
			return;
		}
		snapshot();
	});
}

void LanesObserver::setOpenedChannel(int64_t channelId) {
	openedChannelId = channelId;
	requestRecompute();
}

void LanesObserver::setIdentity(LaneIdentity value) {
	identity = std::move(value);
	applyAccountLanes();
}

bool LanesObserver::isActive() const {
	return active;
}

bool LanesObserver::isSubscribed(const std::string &lane) const {
	return lanes.contains(lane);
}

void LanesObserver::snapshot() {
	if (!active) {
		return;
	}
	applyChannelLanes(SelectChannelLanes(dialogs(), openedChannelId));
}

void LanesObserver::applyChannelLanes(const std::vector<int64_t> &desired) {
	std::vector<std::string> wanted;
	wanted.reserve(desired.size());
	for (const int64_t channelId : desired) {
		wanted.push_back(std::string(kChannelPrefix) + std::to_string(channelId));
	}

	std::vector<std::string> stale;
	for (const std::string &lane : lanes) {
		if (IsChannelLane(lane)
				&& std::find(wanted.begin(), wanted.end(), lane) == wanted.end()) {
			stale.push_back(lane);
		}
	}
	for (const std::string &lane : stale) {
		unsubscribe(lane);
	}

	for (const std::string &lane : wanted) {
		if (!active) {
			return;
		}
		if (!lanes.contains(lane)) {
			subscribe(lane);
		}
	}
}

void LanesObserver::applyAccountLanes() {
	const std::set<std::string> desired = active
		? desiredAccountLanes()
		: std::set<std::string>();

	std::vector<std::string> stale;
	for (const std::string &lane : lanes) {
		if (!IsChannelLane(lane) && !desired.contains(lane)) {
			stale.push_back(lane);
		}
	}
	for (const std::string &lane : stale) {
		unsubscribe(lane);
	}

	for (const std::string &lane : desired) {
		if (!lanes.contains(lane)) {
			subscribe(lane);
		}
	}
}

void LanesObserver::subscribe(const std::string &lane) {
	lanes.insert(lane);
	subscribeLane(lane);
}

void LanesObserver::unsubscribe(const std::string &lane) {
	lanes.erase(lane);
	unsubscribeLane(lane);
}

std::set<std::string> LanesObserver::desiredAccountLanes() const {
	std::set<std::string> result;
	if (identity.userId > 0) {
		const std::string userId = std::to_string(identity.userId);
		result.insert(std::string(kUserPrefix) + userId);
		if (!identity.sessionHash.empty()) {
			result.insert(
				std::string(kSessionPrefix) + userId + ":" + identity.sessionHash);
		}
	}
	if (!identity.identityId.empty()) {
		result.insert(std::string(kIdentityPrefix) + identity.identityId);
	}
	return result;
}

} // namespace Impulse
