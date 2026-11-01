#include "mtproto/impulse/core/lanes.h"

#include <algorithm>
#include <string_view>

namespace Impulse {

namespace {

constexpr std::string_view kSessionChannelPrefix = "session:";
constexpr std::string_view kSystemConfigChannel = "system:config";

[[nodiscard]] bool ContainsId(
		const std::vector<int64_t> &ids,
		int64_t id) {
	return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

bool IsServerSideChannel(std::string_view channel) {
	return channel.starts_with(kSessionChannelPrefix)
		|| channel == kSystemConfigChannel;
}

std::vector<int64_t> SelectChannelLanes(
		const std::vector<LaneCandidate> &candidates,
		int64_t openedChannelId,
		int cap) {
	std::vector<int64_t> result;
	if (cap <= 0) {
		return result;
	}
	if (openedChannelId > 0) {
		result.push_back(openedChannelId);
	}

	std::vector<LaneCandidate> sorted = candidates;
	std::stable_sort(
		sorted.begin(),
		sorted.end(),
		[](
			const LaneCandidate &a,
			const LaneCandidate &b) {
			return a.lastMessageDate > b.lastMessageDate;
		});

	for (const LaneCandidate &candidate : sorted) {
		if (static_cast<int>(result.size()) >= cap) {
			break;
		}
		const int64_t id = candidate.channelId;
		if (id > 0 && !ContainsId(result, id)) {
			result.push_back(id);
		}
	}
	return result;
}

} // namespace Impulse
