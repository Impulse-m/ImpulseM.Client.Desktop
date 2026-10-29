#include "calls/livekit/livekit_event_router.h"

#include <algorithm>
#include <utility>

namespace Impulse {

LiveKitEventRouter::RouteId LiveKitEventRouter::addRoute(Route route) {
	std::lock_guard<std::mutex> lock(mutex);
	const RouteId id = nextRouteId++;
	routes.emplace(id, std::move(route));
	return id;
}

void LiveKitEventRouter::addKey(RouteId id, std::uint64_t key) {
	if (!key) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex);
	if (routes.find(id) == routes.end()) {
		return;
	}
	RouteId &owner = owners[key];
	if (owner == id) {
		return;
	}
	owner = id;
	keysByRoute[id].push_back(key);
}

void LiveKitEventRouter::removeKey(RouteId id, std::uint64_t key) {
	std::lock_guard<std::mutex> lock(mutex);
	const std::map<std::uint64_t, RouteId>::iterator owner = owners.find(key);
	if (owner == owners.end() || owner->second != id) {
		return;
	}
	owners.erase(owner);
	const std::map<RouteId, std::vector<std::uint64_t>>::iterator found =
		keysByRoute.find(id);
	if (found == keysByRoute.end()) {
		return;
	}
	std::vector<std::uint64_t> &keys = found->second;
	keys.erase(
		std::remove(keys.begin(), keys.end(), key),
		keys.end());
}

void LiveKitEventRouter::clearKeys(RouteId id) {
	std::lock_guard<std::mutex> lock(mutex);
	dropKeys(id);
}

void LiveKitEventRouter::removeRoute(RouteId id) {
	std::lock_guard<std::mutex> lock(mutex);
	dropKeys(id);
	routes.erase(id);
}

void LiveKitEventRouter::dispatch(const LiveKitEvent &event) {
	const std::uint64_t keys[] = {
		event.asyncId,
		event.roomHandle.value_or(0),
		event.localParticipantHandle.value_or(0),
		event.publicationHandle.value_or(0),
		event.streamHandle.value_or(0),
	};
	Route route;
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (const std::uint64_t key : keys) {
			if (!key) {
				continue;
			}
			const std::map<std::uint64_t, RouteId>::const_iterator owner =
				owners.find(key);
			if (owner == owners.end()) {
				continue;
			}
			const std::map<RouteId, Route>::const_iterator found =
				routes.find(owner->second);
			if (found != routes.end()) {
				route = found->second;
			}
			break;
		}
	}
	if (route) {
		route(event);
	}
}

void LiveKitEventRouter::dropKeys(RouteId id) {
	const std::map<RouteId, std::vector<std::uint64_t>>::iterator found =
		keysByRoute.find(id);
	if (found == keysByRoute.end()) {
		return;
	}
	for (const std::uint64_t key : found->second) {
		const std::map<std::uint64_t, RouteId>::iterator owner = owners.find(key);
		if (owner != owners.end() && owner->second == id) {
			owners.erase(owner);
		}
	}
	keysByRoute.erase(found);
}

} // namespace Impulse
