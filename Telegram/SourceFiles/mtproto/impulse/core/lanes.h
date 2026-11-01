#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace Impulse {

inline constexpr int kMaxChannelLanes = 32;

struct LaneCandidate {
	int64_t channelId = 0;
	int32_t lastMessageDate = 0;
};

// Channel ids in priority order: the opened channel first, then the
// candidates by lastMessageDate descending. Ids that are not positive are
// ignored, duplicates are dropped, and the result holds at most cap ids.
[[nodiscard]] std::vector<int64_t> SelectChannelLanes(
	const std::vector<LaneCandidate> &candidates,
	int64_t openedChannelId,
	int cap = kMaxChannelLanes);

// The channels whose publications the server sends to the update connection
// without a client subscription: session:{userId}:{sessionHash} and
// system:config. Any other server-side publication is not accepted.
[[nodiscard]] bool IsServerSideChannel(std::string_view channel);

} // namespace Impulse
