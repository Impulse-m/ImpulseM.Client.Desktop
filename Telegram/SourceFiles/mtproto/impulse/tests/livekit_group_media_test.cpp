/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/livekit/livekit_adapter.h"
#include "calls/livekit/livekit_adapter_interface.h"
#include "calls/livekit/livekit_ffi_codec.h"
#include "mtproto/impulse/core/wire.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
using Span = std::span<const std::uint8_t>;

constexpr std::uint32_t kFfiRequestPublishTrack = 5;
constexpr std::uint32_t kFfiRequestNewPlatformAudioSource = 26;
constexpr std::uint32_t kFfiRequestNewVideoSource = 21;
constexpr std::uint32_t kFfiRequestCaptureVideoFrame = 22;
constexpr std::uint32_t kFfiResponseCaptureVideoFrame = 22;
constexpr std::uint32_t kFfiRequestRemotePublicationQuality = 68;
constexpr std::uint32_t kFfiResponseRemotePublicationQuality = 67;
constexpr std::uint32_t kFfiEventRoom = 1;
constexpr std::uint32_t kRoomEventHandle = 1;
constexpr std::uint32_t kRoomEventTrackPublished = 7;
constexpr std::uint32_t kRoomEventTrackUnpublished = 8;

// The first message with the field number, or nullopt. The span points into
// the data, which must outlive it.
[[nodiscard]] std::optional<Span> MessageField(Span data, std::uint32_t field) {
	Impulse::ProtoReader reader(data);
	while (reader.next()) {
		if (reader.field() == field
			&& reader.wireType() == Impulse::ProtoWireType::LengthDelimited) {
			return reader.readBytes();
		}
		reader.skip();
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> VarintField(
		Span data,
		std::uint32_t field) {
	Impulse::ProtoReader reader(data);
	while (reader.next()) {
		if (reader.field() == field
			&& reader.wireType() == Impulse::ProtoWireType::Varint) {
			return reader.readVarint();
		}
		reader.skip();
	}
	return std::nullopt;
}

// Every message with the field number, in the order of the data.
[[nodiscard]] std::vector<Span> MessageFields(Span data, std::uint32_t field) {
	std::vector<Span> result;
	Impulse::ProtoReader reader(data);
	while (reader.next()) {
		if (reader.field() == field
			&& reader.wireType() == Impulse::ProtoWireType::LengthDelimited) {
			result.push_back(reader.readBytes());
		} else {
			reader.skip();
		}
	}
	return result;
}

// Wraps a RoomEvent into the FfiEvent envelope of the room field.
[[nodiscard]] Bytes WrapRoomEvent(const Impulse::ProtoWriter &roomEvent) {
	Impulse::ProtoWriter event;
	event.writeMessageField(kFfiEventRoom, roomEvent);
	return event.takeBytes();
}

} // namespace

IMPULSE_TEST(LiveKitCaptureVideoFrameRequestCarriesThePlanes) {
	Impulse::LiveKitCaptureFrame frame;
	frame.width = 4;
	frame.height = 2;
	frame.timestampUs = 1000;
	Impulse::LiveKitVideoComponent y;
	y.dataPtr = 0x1000;
	y.stride = 4;
	y.size = 8;
	Impulse::LiveKitVideoComponent u;
	u.dataPtr = 0x2000;
	u.stride = 2;
	u.size = 2;
	Impulse::LiveKitVideoComponent v;
	v.dataPtr = 0x3000;
	v.stride = 2;
	v.size = 2;
	frame.components = {y, u, v};

	const Bytes request = Impulse::EncodeLiveKitCaptureVideoFrameRequest(7, frame);
	const std::optional<Span> capture = MessageField(
		request,
		kFfiRequestCaptureVideoFrame);
	IMPULSE_CHECK(capture.has_value());
	if (!capture) {
		return;
	}
	IMPULSE_CHECK(VarintField(*capture, 1) == 7);
	IMPULSE_CHECK(VarintField(*capture, 3) == 1000);
	IMPULSE_CHECK(VarintField(*capture, 4) == 0);

	const std::optional<Span> buffer = MessageField(*capture, 2);
	IMPULSE_CHECK(buffer.has_value());
	if (!buffer) {
		return;
	}
	// VideoBufferType.I420 is 5.
	IMPULSE_CHECK(VarintField(*buffer, 1) == 5);
	IMPULSE_CHECK(VarintField(*buffer, 2) == 4);
	IMPULSE_CHECK(VarintField(*buffer, 3) == 2);
	IMPULSE_CHECK(VarintField(*buffer, 4) == 0x1000);
	const std::vector<Span> planes = MessageFields(*buffer, 7);
	IMPULSE_CHECK(planes.size() == 3);
	if (planes.size() == 3) {
		IMPULSE_CHECK(VarintField(planes[0], 1) == 0x1000);
		IMPULSE_CHECK(VarintField(planes[0], 2) == 4);
		IMPULSE_CHECK(VarintField(planes[0], 3) == 8);
		IMPULSE_CHECK(VarintField(planes[1], 1) == 0x2000);
		IMPULSE_CHECK(VarintField(planes[2], 1) == 0x3000);
		IMPULSE_CHECK(VarintField(planes[2], 3) == 2);
	}
}

IMPULSE_TEST(LiveKitCaptureVideoFrameResponseNeedsItsMessage) {
	Impulse::ProtoWriter empty;
	Impulse::ProtoWriter answered;
	answered.writeMessageField(kFfiResponseCaptureVideoFrame, empty);
	IMPULSE_CHECK(Impulse::DecodeLiveKitCaptureVideoFrameResponse(
		answered.takeBytes()));

	Impulse::ProtoWriter other;
	other.writeVarintField(1, 1);
	Impulse::ProtoWriter unrelated;
	unrelated.writeMessageField(kFfiRequestRemotePublicationQuality, other);
	IMPULSE_CHECK(!Impulse::DecodeLiveKitCaptureVideoFrameResponse(
		unrelated.takeBytes()));
}

IMPULSE_TEST(LiveKitRemotePublicationQualityRequestCarriesTheQuality) {
	const Bytes request = Impulse::EncodeLiveKitRemotePublicationQualityRequest(
		9,
		Impulse::LiveKitVideoQuality::Medium);
	const std::optional<Span> body = MessageField(
		request,
		kFfiRequestRemotePublicationQuality);
	IMPULSE_CHECK(body.has_value());
	if (!body) {
		return;
	}
	IMPULSE_CHECK(VarintField(*body, 1) == 9);
	// VideoQuality.MEDIUM is 1.
	IMPULSE_CHECK(VarintField(*body, 2) == 1);

	Impulse::ProtoWriter answered;
	answered.writeMessageField(
		kFfiResponseRemotePublicationQuality,
		Impulse::ProtoWriter());
	IMPULSE_CHECK(Impulse::DecodeLiveKitRemotePublicationQualityResponse(
		answered.takeBytes()));
}

IMPULSE_TEST(LiveKitScreenShareSourceIsMarkedAsScreencast) {
	const Bytes shared = Impulse::EncodeLiveKitNewVideoSourceRequest(
		1280,
		720,
		true);
	const std::optional<Span> sharedBody = MessageField(
		shared,
		kFfiRequestNewVideoSource);
	IMPULSE_CHECK(sharedBody.has_value());
	if (sharedBody) {
		IMPULSE_CHECK(VarintField(*sharedBody, 3) == 1);
	}

	const Bytes camera = Impulse::EncodeLiveKitNewVideoSourceRequest(
		640,
		480,
		false);
	const std::optional<Span> cameraBody = MessageField(
		camera,
		kFfiRequestNewVideoSource);
	IMPULSE_CHECK(cameraBody.has_value());
	if (cameraBody) {
		IMPULSE_CHECK(!VarintField(*cameraBody, 3).has_value());
	}
}

IMPULSE_TEST(LiveKitPlatformAudioSourceCarriesNoiseSuppression) {
	const std::uint32_t kAudioOptionsEcho = 1;
	const std::uint32_t kAudioOptionsNoise = 2;
	const std::uint32_t kAudioOptionsGain = 3;
	const std::uint32_t kAudioSourceOptions = 2;
	const std::uint32_t kAudioSourceHandle = 6;

	for (const bool noise : {false, true}) {
		const Bytes request = Impulse::EncodeLiveKitNewPlatformAudioSourceRequest(
			5,
			noise);
		const std::optional<Span> body = MessageField(
			request,
			kFfiRequestNewPlatformAudioSource);
		IMPULSE_CHECK(body.has_value());
		if (!body) {
			continue;
		}
		IMPULSE_CHECK(VarintField(*body, kAudioSourceHandle) == 5);
		const std::optional<Span> options = MessageField(*body, kAudioSourceOptions);
		IMPULSE_CHECK(options.has_value());
		if (!options) {
			continue;
		}
		IMPULSE_CHECK(VarintField(*options, kAudioOptionsEcho) == 1);
		IMPULSE_CHECK(VarintField(*options, kAudioOptionsNoise) == (noise ? 1 : 0));
		IMPULSE_CHECK(VarintField(*options, kAudioOptionsGain) == 1);
	}
}

IMPULSE_TEST(LiveKitScreenSharePublishesTheScreenShareSource) {
	const Bytes publish = Impulse::EncodeLiveKitPublishTrackRequest(
		1,
		2,
		Impulse::LiveKitTrackSource::ScreenShare,
		3);
	const std::optional<Span> body = MessageField(publish, kFfiRequestPublishTrack);
	IMPULSE_CHECK(body.has_value());
	if (!body) {
		return;
	}
	const std::optional<Span> options = MessageField(*body, 3);
	IMPULSE_CHECK(options.has_value());
	if (options) {
		// TrackSource.SOURCE_SCREENSHARE is 3, in TrackPublishOptions.source (7).
		IMPULSE_CHECK(VarintField(*options, 7) == 3);
	}
}

IMPULSE_TEST(LiveKitTrackPublishedEventCarriesTheRemotePublication) {
	Impulse::ProtoWriter info;
	info.writeStringField(1, "TR_video");
	info.writeVarintField(3, 2);
	Impulse::ProtoWriter handle;
	handle.writeVarintField(1, 42);
	Impulse::ProtoWriter publication;
	publication.writeMessageField(1, handle);
	publication.writeMessageField(2, info);
	Impulse::ProtoWriter published;
	published.writeStringField(1, "alice");
	published.writeMessageField(2, publication);
	Impulse::ProtoWriter room;
	room.writeVarintField(kRoomEventHandle, 4);
	room.writeMessageField(kRoomEventTrackPublished, published);

	const Impulse::LiveKitEvent decoded = Impulse::DecodeLiveKitEvent(
		WrapRoomEvent(room));
	IMPULSE_CHECK(decoded.kind == Impulse::LiveKitEventKind::TrackPublished);
	IMPULSE_CHECK(decoded.roomHandle == 4);
	IMPULSE_CHECK(decoded.participantIdentity == "alice");
	IMPULSE_CHECK(decoded.trackSid == "TR_video");
	IMPULSE_CHECK(decoded.trackKind == Impulse::LiveKitTrackKind::Video);
	IMPULSE_CHECK(decoded.publicationHandle == 42);
}

IMPULSE_TEST(LiveKitTrackUnpublishedEventNamesTheSid) {
	Impulse::ProtoWriter unpublished;
	unpublished.writeStringField(1, "alice");
	unpublished.writeStringField(2, "TR_video");
	Impulse::ProtoWriter room;
	room.writeVarintField(kRoomEventHandle, 4);
	room.writeMessageField(kRoomEventTrackUnpublished, unpublished);

	const Impulse::LiveKitEvent decoded = Impulse::DecodeLiveKitEvent(
		WrapRoomEvent(room));
	IMPULSE_CHECK(decoded.kind == Impulse::LiveKitEventKind::TrackUnpublished);
	IMPULSE_CHECK(decoded.roomHandle == 4);
	IMPULSE_CHECK(decoded.participantIdentity == "alice");
	IMPULSE_CHECK(decoded.trackSid == "TR_video");
	IMPULSE_CHECK(!decoded.publicationHandle.has_value());
}

IMPULSE_TEST(LiveKitAdapterImplementsTheInterface) {
	IMPULSE_CHECK((std::is_base_of<Impulse::LiveKitAdapterInterface, Impulse::LiveKitAdapter>::value));
	IMPULSE_CHECK(std::is_abstract<Impulse::LiveKitAdapterInterface>::value);
}
