/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/livekit/livekit_ffi_codec.h"
#include "mtproto/impulse/core/wire.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace {

constexpr std::uint32_t kFfiRequestLocalTrackMute = 17;
constexpr std::uint32_t kFfiResponseLocalTrackMute = 17;
constexpr std::uint32_t kTrackHandleField = 1;
constexpr std::uint32_t kMuteField = 2;
constexpr std::uint32_t kMutedField = 1;

// Stands in for the library: it records every encoded request in the order it
// was sent and answers with the response it was given.
class FakeFfi final {
public:
	std::optional<std::vector<std::uint8_t>> request(
			const std::vector<std::uint8_t> &data) {
		sent.push_back(data);
		return response;
	}

	std::vector<std::vector<std::uint8_t>> sent;
	std::optional<std::vector<std::uint8_t>> response;
};

struct DecodedMute final {
	bool found = false;
	std::uint64_t trackHandle = 0;
	bool mute = false;
};

[[nodiscard]] DecodedMute DecodeMute(const std::vector<std::uint8_t> &data) {
	DecodedMute result;
	Impulse::ProtoReader outer(data);
	while (outer.next()) {
		if (outer.field() != kFfiRequestLocalTrackMute) {
			outer.skip();
			continue;
		}
		const std::span<const std::uint8_t> payload = outer.readBytes();
		Impulse::ProtoReader inner(payload);
		while (inner.next()) {
			if (inner.field() == kTrackHandleField) {
				result.trackHandle = inner.readVarint();
			} else if (inner.field() == kMuteField) {
				result.mute = inner.readVarint() != 0;
			} else {
				inner.skip();
			}
		}
		result.found = true;
	}
	return result;
}

[[nodiscard]] std::vector<std::uint8_t> MuteResponse(bool muted) {
	Impulse::ProtoWriter inner;
	inner.writeVarintField(kMutedField, muted ? 1 : 0);
	Impulse::ProtoWriter outer;
	outer.writeMessageField(kFfiResponseLocalTrackMute, inner);
	return outer.takeBytes();
}

} // namespace

IMPULSE_TEST(LiveKitMuteRequestsRecordedInOrder) {
	using Impulse::EncodeLiveKitLocalTrackMuteRequest;
	FakeFfi fake;
	fake.response = MuteResponse(true);

	fake.request(EncodeLiveKitLocalTrackMuteRequest(7, true));
	fake.request(EncodeLiveKitLocalTrackMuteRequest(7, false));
	fake.request(EncodeLiveKitLocalTrackMuteRequest(9, true));

	IMPULSE_CHECK(fake.sent.size() == 3);
	if (fake.sent.size() == 3) {
		const DecodedMute first = DecodeMute(fake.sent[0]);
		const DecodedMute second = DecodeMute(fake.sent[1]);
		const DecodedMute third = DecodeMute(fake.sent[2]);
		IMPULSE_CHECK(first.found);
		IMPULSE_CHECK(first.trackHandle == 7);
		IMPULSE_CHECK(first.mute);
		IMPULSE_CHECK(second.found);
		IMPULSE_CHECK(second.trackHandle == 7);
		IMPULSE_CHECK(!second.mute);
		IMPULSE_CHECK(third.found);
		IMPULSE_CHECK(third.trackHandle == 9);
		IMPULSE_CHECK(third.mute);
	}
}

IMPULSE_TEST(LiveKitMuteResponseReadsMutedState) {
	using Impulse::DecodeLiveKitLocalTrackMuteResponse;
	IMPULSE_CHECK(DecodeLiveKitLocalTrackMuteResponse(MuteResponse(true)) == true);
	IMPULSE_CHECK(DecodeLiveKitLocalTrackMuteResponse(MuteResponse(false)) == false);
}

IMPULSE_TEST(LiveKitMuteResponseWithoutMessageIsRejected) {
	using Impulse::DecodeLiveKitLocalTrackMuteResponse;
	Impulse::ProtoWriter other;
	other.writeVarintField(1, 1);
	Impulse::ProtoWriter outer;
	outer.writeMessageField(kFfiResponseLocalTrackMute + 1, other);
	IMPULSE_CHECK(!DecodeLiveKitLocalTrackMuteResponse(outer.takeBytes()).has_value());
}
