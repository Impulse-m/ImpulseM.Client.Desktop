#include "mtproto/impulse/core/lanes.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <vector>

IMPULSE_TEST(LanesOpenedChannelFirstThenNewest) {
	const std::vector<Impulse::LaneCandidate> candidates = {
		{ 10, 100 },
		{ 20, 300 },
		{ 30, 200 },
	};
	const std::vector<int64_t> expected = { 30, 20, 10 };
	const std::vector<int64_t> result = Impulse::SelectChannelLanes(candidates, 30);
	IMPULSE_CHECK(result == expected);
}

IMPULSE_TEST(LanesIgnoreNonPositiveIds) {
	const std::vector<Impulse::LaneCandidate> candidates = {
		{ 0, 500 },
		{ -4, 400 },
		{ 5, 100 },
	};
	const std::vector<int64_t> expected = { 5 };
	const std::vector<int64_t> result = Impulse::SelectChannelLanes(candidates, 0);
	IMPULSE_CHECK(result == expected);
}

IMPULSE_TEST(LanesNoOpenedChannelWhenNonPositive) {
	const std::vector<Impulse::LaneCandidate> candidates = {
		{ 7, 100 },
	};
	const std::vector<int64_t> expected = { 7 };
	const std::vector<int64_t> result = Impulse::SelectChannelLanes(candidates, -1);
	IMPULSE_CHECK(result == expected);
}

IMPULSE_TEST(LanesDeduplicateKeepingFirstPriority) {
	const std::vector<Impulse::LaneCandidate> candidates = {
		{ 7, 300 },
		{ 7, 200 },
		{ 8, 100 },
	};
	const std::vector<int64_t> expected = { 7, 8 };
	const std::vector<int64_t> result = Impulse::SelectChannelLanes(candidates, 7);
	IMPULSE_CHECK(result == expected);
}

IMPULSE_TEST(LanesOpenedChannelAddedWithoutCandidate) {
	const std::vector<Impulse::LaneCandidate> candidates = {
		{ 8, 100 },
	};
	const std::vector<int64_t> expected = { 99, 8 };
	const std::vector<int64_t> result = Impulse::SelectChannelLanes(candidates, 99);
	IMPULSE_CHECK(result == expected);
}

IMPULSE_TEST(LanesTiesKeepInputOrder) {
	const std::vector<Impulse::LaneCandidate> candidates = {
		{ 40, 100 },
		{ 41, 100 },
		{ 42, 100 },
	};
	const std::vector<int64_t> expected = { 40, 41, 42 };
	const std::vector<int64_t> result = Impulse::SelectChannelLanes(candidates, 0);
	IMPULSE_CHECK(result == expected);
}

IMPULSE_TEST(LanesCappedAtMaxIncludingOpened) {
	std::vector<Impulse::LaneCandidate> candidates;
	for (int32_t index = 1; index <= 40; ++index) {
		candidates.push_back({ 1000 + index, index });
	}
	const std::vector<int64_t> result = Impulse::SelectChannelLanes(candidates, 5);
	IMPULSE_CHECK(static_cast<int>(result.size()) == Impulse::kMaxChannelLanes);
	IMPULSE_CHECK(result.front() == 5);
	IMPULSE_CHECK(result[1] == 1040);
	IMPULSE_CHECK(result.back() == 1000 + 40 - (Impulse::kMaxChannelLanes - 2));
}

IMPULSE_TEST(LanesZeroCapGivesNothing) {
	const std::vector<Impulse::LaneCandidate> candidates = {
		{ 3, 100 },
	};
	const std::vector<int64_t> result = Impulse::SelectChannelLanes(candidates, 3, 0);
	IMPULSE_CHECK(result.empty());
}
