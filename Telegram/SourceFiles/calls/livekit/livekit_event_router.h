// Routes the events of the LiveKit library to the room that owns them. A room
// registers its async ids and its handles as keys. An event goes only to the
// route that owns its async id or one of its handles, and an event with no
// owner is dropped, so a closed room receives nothing. Std-only, so the tests
// link it without the Windows base headers.
#pragma once

#include "calls/livekit/livekit_ffi_codec.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace Impulse {

class LiveKitEventRouter final {
public:
	using Route = std::function<void(const LiveKitEvent &event)>;
	using RouteId = std::uint64_t;
	using ReleaseHandle = std::function<void(std::uint64_t handle)>;

	// releaseHandle releases the library buffer handle of a video frame that no
	// route takes, so the buffer does not stay owned by the library.
	explicit LiveKitEventRouter(ReleaseHandle releaseHandle = nullptr);
	LiveKitEventRouter(const LiveKitEventRouter &other) = delete;
	LiveKitEventRouter &operator=(const LiveKitEventRouter &other) = delete;

	// Starts a route that owns no keys yet and returns its id.
	[[nodiscard]] RouteId addRoute(Route route);

	// Makes the route own the key, an async id or a handle. Key 0 is ignored,
	// and a key that another route owns is taken over.
	void addKey(RouteId id, std::uint64_t key);

	// Drops one key of the route, so its events are no longer routed there.
	// A key that the route does not own is ignored.
	void removeKey(RouteId id, std::uint64_t key);

	// Drops every key of the route. Its events are dropped until new keys are
	// added.
	void clearKeys(RouteId id);

	// Stops the route and drops its keys.
	void removeRoute(RouteId id);

	// Hands the event to the route that owns its async id or one of its
	// handles. The route runs on the calling thread, outside the lock. A video
	// frame that no route owns has its buffer handle released.
	void dispatch(const LiveKitEvent &event);

private:
	void dropKeys(RouteId id);

	const ReleaseHandle releaseHandle;
	std::mutex mutex;
	RouteId nextRouteId = 1;
	std::map<RouteId, Route> routes;
	std::map<RouteId, std::vector<std::uint64_t>> keysByRoute;
	std::map<std::uint64_t, RouteId> owners;
};

} // namespace Impulse
