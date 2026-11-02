/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/livekit/livekit_adapter.h"
#include "calls/livekit/livekit_ffi_codec.h"
#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/tests/livekit_fake_room.h"

#include <cstdint>
#include <memory>
#include <optional>

using namespace ImpulseLiveKitTest;

namespace {

// TrackPublishOptions is field 3 of PublishTrackRequest, and its simulcast and
// source fields are 6 and 7.
constexpr std::uint32_t kPublishOptionsField = 3;
constexpr std::uint32_t kPublishOptionsSimulcast = 6;
constexpr std::uint32_t kPublishOptionsSource = 7;

// The TrackPublishOptions of a PublishTrack request.
[[nodiscard]] std::optional<Span> PublishOptions(const Bytes &request) {
	const std::optional<Span> publish = MessageField(request, kRequestPublishTrack);
	if (!publish) {
		return std::nullopt;
	}
	return MessageField(*publish, kPublishOptionsField);
}

} // namespace

IMPULSE_TEST(LiveKitPublishRequestWritesNoSimulcastUnlessGiven) {
	const Bytes request = Impulse::EncodeLiveKitPublishTrackRequest(
		1,
		2,
		Impulse::LiveKitTrackSource::Camera,
		5);
	const std::optional<Span> options = PublishOptions(request);
	IMPULSE_CHECK(options.has_value());
	if (options) {
		IMPULSE_CHECK(!VarintField(*options, kPublishOptionsSimulcast).has_value());
		IMPULSE_CHECK(VarintField(*options, kPublishOptionsSource) == 1);
	}
}

IMPULSE_TEST(LiveKitPublishRequestWritesSimulcastWhenGiven) {
	const Bytes on = Impulse::EncodeLiveKitPublishTrackRequest(
		1,
		2,
		Impulse::LiveKitTrackSource::ScreenShare,
		5,
		true);
	const std::optional<Span> onOptions = PublishOptions(on);
	IMPULSE_CHECK(onOptions.has_value());
	if (onOptions) {
		IMPULSE_CHECK(VarintField(*onOptions, kPublishOptionsSimulcast) == 1);
		IMPULSE_CHECK(VarintField(*onOptions, kPublishOptionsSource) == 3);
	}

	const Bytes off = Impulse::EncodeLiveKitPublishTrackRequest(
		1,
		2,
		Impulse::LiveKitTrackSource::Camera,
		5,
		false);
	const std::optional<Span> offOptions = PublishOptions(off);
	IMPULSE_CHECK(offOptions.has_value());
	if (offOptions) {
		IMPULSE_CHECK(VarintField(*offOptions, kPublishOptionsSimulcast) == 0);
	}
}

IMPULSE_TEST(LiveKitSimulcastIsWrittenToScreenShareOnceSet) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);
	IMPULSE_CHECK(delegate.connected == 1);

	// A camera publish before the option is set writes no simulcast.
	fake->responses.push_back(HandleResponse(kResponseNewVideoSource, 41));
	fake->responses.push_back(HandleResponse(kResponseCreateVideoTrack, 42));
	fake->responses.push_back(MuteResponse(false));
	fake->responses.push_back(AsyncResponse(kResponsePublishTrack, 0));
	IMPULSE_CHECK(adapter.publishCamera(640, 360));
	const std::optional<Span> cameraOptions = PublishOptions(fake->sent.back());
	IMPULSE_CHECK(cameraOptions.has_value());
	if (cameraOptions) {
		IMPULSE_CHECK(!VarintField(*cameraOptions, kPublishOptionsSimulcast).has_value());
	}

	// Once set, the screen share publish writes it.
	adapter.setVideoSimulcast(true);
	fake->responses.push_back(HandleResponse(kResponseNewVideoSource, 51));
	fake->responses.push_back(HandleResponse(kResponseCreateVideoTrack, 52));
	fake->responses.push_back(AsyncResponse(kResponsePublishTrack, 0));
	IMPULSE_CHECK(adapter.publishScreenShare(1280, 720));
	const std::optional<Span> screenOptions = PublishOptions(fake->sent.back());
	IMPULSE_CHECK(screenOptions.has_value());
	if (screenOptions) {
		IMPULSE_CHECK(VarintField(*screenOptions, kPublishOptionsSimulcast) == 1);
		IMPULSE_CHECK(VarintField(*screenOptions, kPublishOptionsSource) == 3);
	}
}

IMPULSE_TEST(LiveKitSimulcastFalseIsWrittenToCamera) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);

	adapter.setVideoSimulcast(false);
	fake->responses.push_back(HandleResponse(kResponseNewVideoSource, 41));
	fake->responses.push_back(HandleResponse(kResponseCreateVideoTrack, 42));
	fake->responses.push_back(MuteResponse(false));
	fake->responses.push_back(AsyncResponse(kResponsePublishTrack, 0));
	IMPULSE_CHECK(adapter.publishCamera(640, 360));
	const std::optional<Span> options = PublishOptions(fake->sent.back());
	IMPULSE_CHECK(options.has_value());
	if (options) {
		IMPULSE_CHECK(VarintField(*options, kPublishOptionsSimulcast) == 0);
	}
}

IMPULSE_TEST(LiveKitMicrophoneNeverWritesSimulcast) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);

	adapter.setVideoSimulcast(true);
	fake->responses.push_back(HandleResponse(kResponseNewAudioSource, 61));
	fake->responses.push_back(HandleResponse(kResponseCreateAudioTrack, 62));
	fake->responses.push_back(MuteResponse(true));
	fake->responses.push_back(AsyncResponse(kResponsePublishTrack, 0));
	IMPULSE_CHECK(adapter.publishMicrophone());
	const std::optional<Span> options = PublishOptions(fake->sent.back());
	IMPULSE_CHECK(options.has_value());
	if (options) {
		IMPULSE_CHECK(!VarintField(*options, kPublishOptionsSimulcast).has_value());
		IMPULSE_CHECK(VarintField(*options, kPublishOptionsSource) == 2);
	}
}
