/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/livekit/livekit_adapter.h"
#include "calls/livekit/livekit_event_router.h"
#include "calls/livekit/livekit_ffi_codec.h"
#include "calls/livekit/livekit_ffi_loader.h"
#include "mtproto/impulse/core/wire.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

// FfiRequest and FfiResponse oneof numbers used by the checks.
constexpr std::uint32_t kRequestConnect = 3;
constexpr std::uint32_t kRequestNewVideoStream = 20;
constexpr std::uint32_t kRequestReadyForRoomEvent = 83;
constexpr std::uint32_t kResponseConnect = 3;
constexpr std::uint32_t kResponseDisconnect = 4;
constexpr std::uint32_t kResponseNewPlatformAudio = 76;
constexpr std::uint32_t kResponseNewVideoStream = 20;
constexpr std::uint32_t kResponseReadyForRoomEvent = 82;
constexpr std::uint32_t kFfiEventRoom = 1;
constexpr std::uint32_t kFfiEventVideoStream = 3;

// ConnectRequest and RoomOptions fields.
constexpr std::uint32_t kConnectOptions = 3;
constexpr std::uint32_t kRoomEncryption = 7;

// E2eeOptions and KeyProviderOptions fields.
constexpr std::uint32_t kE2eeType = 1;
constexpr std::uint32_t kE2eeKeyProvider = 2;
constexpr std::uint32_t kKeyProviderSharedKey = 1;
constexpr std::uint32_t kKeyProviderRatchetWindow = 2;
constexpr std::uint32_t kKeyProviderRatchetSalt = 3;
constexpr std::uint32_t kKeyProviderFailureTolerance = 4;
constexpr std::uint32_t kKeyProviderKeyRing = 5;
constexpr std::uint32_t kKeyProviderDerivation = 6;

// Common message fields: a handle and an owned wrapper of a handle.
constexpr std::uint32_t kHandleId = 1;
constexpr std::uint32_t kOwnedHandle = 1;
constexpr std::uint32_t kOwnedMessage = 1;
constexpr std::uint32_t kAsyncId = 1;

// Room events and track messages.
constexpr std::uint32_t kRoomEventHandle = 1;
constexpr std::uint32_t kRoomEventTrackSubscribed = 9;
constexpr std::uint32_t kRoomEventTrackUnsubscribed = 10;
constexpr std::uint32_t kSubscribedIdentity = 1;
constexpr std::uint32_t kSubscribedTrack = 2;
constexpr std::uint32_t kUnsubscribedIdentity = 1;
constexpr std::uint32_t kUnsubscribedSid = 2;
constexpr std::uint32_t kOwnedTrackInfo = 2;
constexpr std::uint32_t kTrackInfoSid = 1;
constexpr std::uint32_t kTrackInfoKind = 3;
constexpr std::uint64_t kTrackKindAudio = 1;
constexpr std::uint64_t kTrackKindVideo = 2;

// Video stream events and buffers.
constexpr std::uint32_t kStreamEventHandle = 1;
constexpr std::uint32_t kStreamEventFrame = 2;
constexpr std::uint32_t kFrameBuffer = 1;
constexpr std::uint32_t kFrameTimestamp = 2;
constexpr std::uint32_t kOwnedBufferInfo = 2;
constexpr std::uint32_t kBufferInfoType = 1;
constexpr std::uint32_t kBufferInfoWidth = 2;
constexpr std::uint32_t kBufferInfoHeight = 3;
constexpr std::uint32_t kBufferInfoComponents = 7;
constexpr std::uint32_t kComponentDataPtr = 1;
constexpr std::uint32_t kComponentStride = 2;
constexpr std::uint32_t kComponentSize = 3;
constexpr std::uint64_t kBufferTypeI420 = 5;

// NewVideoStreamRequest fields.
constexpr std::uint32_t kStreamTrackHandle = 1;
constexpr std::uint32_t kStreamType = 2;
constexpr std::uint32_t kStreamFormat = 3;
constexpr std::uint32_t kStreamNormalizeStride = 4;

// ReadyForRoomEventRequest field.
constexpr std::uint32_t kReadyRoomHandle = 1;

// DisconnectRequest field of the request_async_id, and the request field.
constexpr std::uint32_t kRequestDisconnect = 4;
constexpr std::uint32_t kDisconnectAsyncId = 2;

// The request_async_id of a connect or disconnect request.
[[nodiscard]] std::optional<std::uint64_t> RequestAsyncId(
		std::span<const std::uint8_t> data) {
	Impulse::ProtoReader outer(data);
	while (outer.next()) {
		const std::uint32_t field = outer.field();
		std::uint32_t asyncField = 0;
		if (field == kRequestConnect) {
			asyncField = 4;
		} else if (field == kRequestDisconnect) {
			asyncField = kDisconnectAsyncId;
		} else {
			outer.skip();
			continue;
		}
		const std::span<const std::uint8_t> payload = outer.readBytes();
		Impulse::ProtoReader inner(payload);
		while (inner.next()) {
			if (inner.field() == asyncField) {
				return inner.readVarint();
			}
			inner.skip();
		}
		return std::nullopt;
	}
	return std::nullopt;
}

// The async id of the request at the index, or 0 when there is no such request.
[[nodiscard]] std::uint64_t AsyncIdAt(
		const std::vector<std::vector<std::uint8_t>> &sent,
		std::size_t index) {
	if (index >= sent.size()) {
		return 0;
	}
	return RequestAsyncId(sent[index]).value_or(0);
}

// The outer field number of an encoded response.
[[nodiscard]] std::uint32_t ResponseField(
		const std::vector<std::uint8_t> &response) {
	Impulse::ProtoReader reader(response);
	return reader.next() ? reader.field() : 0;
}

// Stands in for the library. It records every request and answers the scripted
// responses in order. It keeps its own event router, as the library does. An
// async request is answered with its own request_async_id, as the library does.
class FakeFfi final : public Impulse::LiveKitFfi {
public:
	bool loaded() const override {
		return true;
	}

	std::optional<std::vector<std::uint8_t>> request(
			std::span<const std::uint8_t> data) override {
		sent.emplace_back(data.begin(), data.end());
		if (responses.empty()) {
			return std::nullopt;
		}
		std::vector<std::uint8_t> next = responses.front();
		responses.erase(responses.begin());
		const std::optional<std::uint64_t> requestId = RequestAsyncId(data);
		if (requestId) {
			Impulse::ProtoWriter inner;
			inner.writeVarintField(kAsyncId, *requestId);
			Impulse::ProtoWriter outer;
			outer.writeMessageField(ResponseField(next), inner);
			next = outer.takeBytes();
		}
		return next;
	}

	void dropHandle(std::uint64_t handle) override {
		dropped.push_back(handle);
	}

	Impulse::LiveKitEventRouter &eventRouter() override {
		return router;
	}

	std::vector<std::vector<std::uint8_t>> sent;
	std::vector<std::vector<std::uint8_t>> responses;
	std::vector<std::uint64_t> dropped;
	Impulse::LiveKitEventRouter router;
};

class RecordingDelegate final : public Impulse::LiveKitAdapterDelegate {
public:
	void liveKitConnected() override {
		++connected;
	}

	void liveKitPublished(Impulse::LiveKitPublishKind) override {
	}

	void liveKitFailed(const std::string &) override {
		++failed;
	}

	void liveKitDisconnected() override {
		++disconnected;
	}

	int connected = 0;
	int failed = 0;
	int disconnected = 0;
};

class RecordingSink final : public Impulse::LiveKitVideoSink {
public:
	void liveKitVideoFrame(const Impulse::LiveKitVideoFrame &frame) override {
		++count;
		last = frame;
	}

	int count = 0;
	Impulse::LiveKitVideoFrame last;
};

void RunNow(std::function<void()> task) {
	task();
}

// Wraps a message in the FfiResponse or FfiEvent field with the given number.
[[nodiscard]] std::vector<std::uint8_t> Wrap(
		std::uint32_t field,
		const Impulse::ProtoWriter &message) {
	Impulse::ProtoWriter outer;
	outer.writeMessageField(field, message);
	return outer.takeBytes();
}

// FfiOwnedHandle with the given id.
[[nodiscard]] Impulse::ProtoWriter OwnedHandle(std::uint64_t id) {
	Impulse::ProtoWriter handle;
	handle.writeVarintField(kHandleId, id);
	return handle;
}

// An owned message that wraps the handle in its field 1.
[[nodiscard]] Impulse::ProtoWriter OwnedWrapper(std::uint64_t id) {
	Impulse::ProtoWriter owned;
	owned.writeMessageField(kOwnedHandle, OwnedHandle(id));
	return owned;
}

[[nodiscard]] std::vector<std::uint8_t> AsyncResponse(
		std::uint32_t responseField,
		std::uint64_t asyncId) {
	Impulse::ProtoWriter inner;
	inner.writeVarintField(kAsyncId, asyncId);
	return Wrap(responseField, inner);
}

// The response of a request that creates one owned handle.
[[nodiscard]] std::vector<std::uint8_t> HandleResponse(
		std::uint32_t responseField,
		std::uint64_t id) {
	Impulse::ProtoWriter message;
	message.writeMessageField(kOwnedMessage, OwnedWrapper(id));
	return Wrap(responseField, message);
}

[[nodiscard]] std::vector<std::uint8_t> ReadyResponse() {
	return Wrap(kResponseReadyForRoomEvent, Impulse::ProtoWriter());
}

[[nodiscard]] Impulse::LiveKitEvent ConnectEvent(
		std::uint64_t asyncId,
		std::uint64_t roomHandle,
		std::uint64_t participantHandle) {
	Impulse::LiveKitEvent event;
	event.kind = Impulse::LiveKitEventKind::Connect;
	event.asyncId = asyncId;
	event.roomHandle = roomHandle;
	event.localParticipantHandle = participantHandle;
	return event;
}

[[nodiscard]] Impulse::LiveKitEvent DisconnectEvent(std::uint64_t asyncId) {
	Impulse::LiveKitEvent event;
	event.kind = Impulse::LiveKitEventKind::Disconnect;
	event.asyncId = asyncId;
	return event;
}

// The encoded FfiEvent of a room event that subscribes a track.
[[nodiscard]] std::vector<std::uint8_t> SubscribedEventBytes(
		std::uint64_t roomHandle,
		const std::string &identity,
		const std::string &sid,
		std::uint64_t trackHandle,
		std::uint64_t kind) {
	Impulse::ProtoWriter info;
	info.writeStringField(kTrackInfoSid, sid);
	info.writeVarintField(kTrackInfoKind, kind);
	Impulse::ProtoWriter track;
	track.writeMessageField(kOwnedHandle, OwnedHandle(trackHandle));
	track.writeMessageField(kOwnedTrackInfo, info);
	Impulse::ProtoWriter subscribed;
	subscribed.writeStringField(kSubscribedIdentity, identity);
	subscribed.writeMessageField(kSubscribedTrack, track);
	Impulse::ProtoWriter room;
	room.writeVarintField(kRoomEventHandle, roomHandle);
	room.writeMessageField(kRoomEventTrackSubscribed, subscribed);
	return Wrap(kFfiEventRoom, room);
}

[[nodiscard]] std::vector<std::uint8_t> UnsubscribedEventBytes(
		std::uint64_t roomHandle,
		const std::string &identity,
		const std::string &sid) {
	Impulse::ProtoWriter unsubscribed;
	unsubscribed.writeStringField(kUnsubscribedIdentity, identity);
	unsubscribed.writeStringField(kUnsubscribedSid, sid);
	Impulse::ProtoWriter room;
	room.writeVarintField(kRoomEventHandle, roomHandle);
	room.writeMessageField(kRoomEventTrackUnsubscribed, unsubscribed);
	return Wrap(kFfiEventRoom, room);
}

// The encoded FfiEvent of one I420 frame of a video stream. The planes point
// at the given memory, which the test keeps alive.
[[nodiscard]] std::vector<std::uint8_t> FrameEventBytes(
		std::uint64_t streamHandle,
		std::uint64_t bufferHandle,
		std::uint32_t width,
		std::uint32_t height,
		const std::vector<std::uint8_t> &planeY,
		const std::vector<std::uint8_t> &planeU,
		const std::vector<std::uint8_t> &planeV) {
	const std::uint32_t strideY = width;
	const std::uint32_t strideChroma = (width + 1) / 2;
	const std::uint64_t planes[3] = {
		reinterpret_cast<std::uintptr_t>(planeY.data()),
		reinterpret_cast<std::uintptr_t>(planeU.data()),
		reinterpret_cast<std::uintptr_t>(planeV.data()),
	};
	const std::uint32_t strides[3] = {
		strideY,
		strideChroma,
		strideChroma,
	};
	Impulse::ProtoWriter info;
	info.writeVarintField(kBufferInfoType, kBufferTypeI420);
	info.writeVarintField(kBufferInfoWidth, width);
	info.writeVarintField(kBufferInfoHeight, height);
	for (std::size_t index = 0; index < 3; ++index) {
		Impulse::ProtoWriter component;
		component.writeVarintField(kComponentDataPtr, planes[index]);
		component.writeVarintField(kComponentStride, strides[index]);
		const std::vector<std::uint8_t> &plane = index == 0
			? planeY
			: (index == 1 ? planeU : planeV);
		component.writeVarintField(kComponentSize, plane.size());
		info.writeMessageField(kBufferInfoComponents, component);
	}
	Impulse::ProtoWriter buffer;
	buffer.writeMessageField(kOwnedHandle, OwnedHandle(bufferHandle));
	buffer.writeMessageField(kOwnedBufferInfo, info);
	Impulse::ProtoWriter frame;
	frame.writeMessageField(kFrameBuffer, buffer);
	frame.writeVarintField(kFrameTimestamp, 1234);
	Impulse::ProtoWriter stream;
	stream.writeVarintField(kStreamEventHandle, streamHandle);
	stream.writeMessageField(kStreamEventFrame, frame);
	return Wrap(kFfiEventVideoStream, stream);
}

[[nodiscard]] Impulse::LiveKitEvent DecodeEvent(
		const std::vector<std::uint8_t> &bytes) {
	return Impulse::DecodeLiveKitEvent(bytes);
}

// Reads the first varint of a field, or nullopt when the field is absent.
[[nodiscard]] std::optional<std::uint64_t> Varint(
		std::span<const std::uint8_t> data,
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

// The payload of the first length-delimited field of the number.
[[nodiscard]] std::optional<std::span<const std::uint8_t>> Message(
		std::span<const std::uint8_t> data,
		std::uint32_t field) {
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

[[nodiscard]] std::string Text(
		std::span<const std::uint8_t> data,
		std::uint32_t field) {
	const std::optional<std::span<const std::uint8_t>> bytes = Message(
		data,
		field);
	if (!bytes) {
		return std::string();
	}
	return std::string(
		reinterpret_cast<const char *>(bytes->data()),
		bytes->size());
}

// The E2eeOptions message of the connect request in the request bytes.
[[nodiscard]] std::optional<std::span<const std::uint8_t>> ConnectEncryption(
		const std::vector<std::uint8_t> &request) {
	const std::optional<std::span<const std::uint8_t>> connect = Message(
		request,
		kRequestConnect);
	if (!connect) {
		return std::nullopt;
	}
	const std::optional<std::span<const std::uint8_t>> options = Message(
		*connect,
		kConnectOptions);
	if (!options) {
		return std::nullopt;
	}
	return Message(*options, kRoomEncryption);
}

[[nodiscard]] std::vector<std::uint8_t> Bytes(
		std::span<const std::uint8_t> data) {
	return std::vector<std::uint8_t>(data.begin(), data.end());
}

} // namespace

IMPULSE_TEST(LiveKitConnectSendsSharedKeyEncryption) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	fake->responses = { AsyncResponse(kResponseConnect, 101) };
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	const std::vector<std::uint8_t> key(256, 0x5a);
	adapter.setSharedKey(key);
	IMPULSE_CHECK(adapter.connect("wss://room", "token"));

	IMPULSE_CHECK(fake->sent.size() == 1);
	if (fake->sent.size() != 1) {
		return;
	}
	const std::optional<std::span<const std::uint8_t>> e2ee =
		ConnectEncryption(fake->sent[0]);
	IMPULSE_CHECK(e2ee.has_value());
	if (!e2ee) {
		return;
	}
	// EncryptionType.GCM.
	IMPULSE_CHECK(Varint(*e2ee, kE2eeType) == 1);
	const std::optional<std::span<const std::uint8_t>> provider =
		Message(*e2ee, kE2eeKeyProvider);
	IMPULSE_CHECK(provider.has_value());
	if (!provider) {
		return;
	}
	const std::optional<std::span<const std::uint8_t>> sharedKey =
		Message(*provider, kKeyProviderSharedKey);
	IMPULSE_CHECK(sharedKey.has_value());
	if (sharedKey) {
		IMPULSE_CHECK(Bytes(*sharedKey) == key);
	}
	IMPULSE_CHECK(Varint(*provider, kKeyProviderRatchetWindow) == 0);
	IMPULSE_CHECK(
		Text(*provider, kKeyProviderRatchetSalt) == "LKFrameEncryptionKey");
	// Failure tolerance -1 is sent as the 64-bit two's complement varint.
	IMPULSE_CHECK(
		Varint(*provider, kKeyProviderFailureTolerance)
			== static_cast<std::uint64_t>(std::int64_t(-1)));
	IMPULSE_CHECK(Varint(*provider, kKeyProviderKeyRing) == 16);
	// KeyDerivationFunction.HKDF.
	IMPULSE_CHECK(Varint(*provider, kKeyProviderDerivation) == 1);
}

IMPULSE_TEST(LiveKitConnectWithoutKeyHasNoEncryption) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	fake->responses = { AsyncResponse(kResponseConnect, 101) };
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	IMPULSE_CHECK(adapter.connect("wss://room", "token"));

	IMPULSE_CHECK(fake->sent.size() == 1);
	if (fake->sent.size() == 1) {
		IMPULSE_CHECK(!ConnectEncryption(fake->sent[0]).has_value());
	}
}

IMPULSE_TEST(LiveKitNewVideoStreamRequestIsI420NormalizedNative) {
	const std::vector<std::uint8_t> request =
		Impulse::EncodeLiveKitNewVideoStreamRequest(9);
	const std::optional<std::span<const std::uint8_t>> stream = Message(
		request,
		kRequestNewVideoStream);
	IMPULSE_CHECK(stream.has_value());
	if (!stream) {
		return;
	}
	IMPULSE_CHECK(Varint(*stream, kStreamTrackHandle) == 9);
	// VideoStreamType.NATIVE is 0.
	IMPULSE_CHECK(Varint(*stream, kStreamType) == 0);
	IMPULSE_CHECK(Varint(*stream, kStreamFormat) == kBufferTypeI420);
	IMPULSE_CHECK(Varint(*stream, kStreamNormalizeStride) == 1);
}

IMPULSE_TEST(LiveKitReadyForRoomEventRequestCarriesRoom) {
	const std::vector<std::uint8_t> request =
		Impulse::EncodeLiveKitReadyForRoomEventRequest(11);
	const std::optional<std::span<const std::uint8_t>> ready = Message(
		request,
		kRequestReadyForRoomEvent);
	IMPULSE_CHECK(ready.has_value());
	if (ready) {
		IMPULSE_CHECK(Varint(*ready, kReadyRoomHandle) == 11);
	}
}

IMPULSE_TEST(LiveKitDecodesTrackSubscribedEvent) {
	const Impulse::LiveKitEvent event = DecodeEvent(
		SubscribedEventBytes(11, "u42", "TR_video", 55, kTrackKindVideo));
	IMPULSE_CHECK(event.kind == Impulse::LiveKitEventKind::TrackSubscribed);
	IMPULSE_CHECK(event.roomHandle == 11);
	IMPULSE_CHECK(event.participantIdentity == "u42");
	IMPULSE_CHECK(event.trackSid == "TR_video");
	IMPULSE_CHECK(event.trackHandle == 55);
	IMPULSE_CHECK(event.trackKind == Impulse::LiveKitTrackKind::Video);
}

IMPULSE_TEST(LiveKitDecodesAudioTrackSubscribedKind) {
	const Impulse::LiveKitEvent event = DecodeEvent(
		SubscribedEventBytes(11, "u42", "TR_audio", 56, kTrackKindAudio));
	IMPULSE_CHECK(event.kind == Impulse::LiveKitEventKind::TrackSubscribed);
	IMPULSE_CHECK(event.trackKind == Impulse::LiveKitTrackKind::Audio);
}

IMPULSE_TEST(LiveKitDecodesTrackUnsubscribedEvent) {
	const Impulse::LiveKitEvent event = DecodeEvent(
		UnsubscribedEventBytes(11, "u42", "TR_video"));
	IMPULSE_CHECK(event.kind == Impulse::LiveKitEventKind::TrackUnsubscribed);
	IMPULSE_CHECK(event.roomHandle == 11);
	IMPULSE_CHECK(event.participantIdentity == "u42");
	IMPULSE_CHECK(event.trackSid == "TR_video");
}

IMPULSE_TEST(LiveKitDecodesI420FrameEvent) {
	const std::vector<std::uint8_t> planeY = { 1, 2, 3, 4, 5, 6, 7, 8 };
	const std::vector<std::uint8_t> planeU = { 10, 11 };
	const std::vector<std::uint8_t> planeV = { 20, 21 };
	const Impulse::LiveKitEvent event = DecodeEvent(FrameEventBytes(
		77,
		66,
		4,
		2,
		planeY,
		planeU,
		planeV));
	IMPULSE_CHECK(event.kind == Impulse::LiveKitEventKind::VideoFrame);
	IMPULSE_CHECK(event.streamHandle == 77);
	IMPULSE_CHECK(event.videoBuffer.has_value());
	if (!event.videoBuffer) {
		return;
	}
	const Impulse::LiveKitVideoBuffer &buffer = *event.videoBuffer;
	IMPULSE_CHECK(buffer.bufferHandle == 66);
	IMPULSE_CHECK(buffer.type == kBufferTypeI420);
	IMPULSE_CHECK(buffer.width == 4);
	IMPULSE_CHECK(buffer.height == 2);
	IMPULSE_CHECK(buffer.timestampUs == 1234);
	IMPULSE_CHECK(buffer.components.size() == 3);
	if (buffer.components.size() == 3) {
		IMPULSE_CHECK(buffer.components[0].stride == 4);
		IMPULSE_CHECK(buffer.components[0].size == 8);
		IMPULSE_CHECK(buffer.components[1].stride == 2);
		IMPULSE_CHECK(buffer.components[2].size == 2);
		IMPULSE_CHECK(
			buffer.components[0].dataPtr
				== reinterpret_cast<std::uintptr_t>(planeY.data()));
	}
}

IMPULSE_TEST(LiveKitSubscribedVideoReachesSinkAndReleasesOnUnsubscribe) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	fake->responses = {
		AsyncResponse(kResponseConnect, 101),
		HandleResponse(kResponseNewPlatformAudio, 31),
		ReadyResponse(),
		HandleResponse(kResponseNewVideoStream, 77),
		AsyncResponse(kResponseDisconnect, 303),
	};
	RecordingDelegate delegate;
	RecordingSink sink;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	adapter.setVideoSink(&sink);
	IMPULSE_CHECK(adapter.connect("wss://room", "token"));
	fake->router.dispatch(ConnectEvent(AsyncIdAt(fake->sent, 0), 11, 12));
	IMPULSE_CHECK(delegate.connected == 1);
	IMPULSE_CHECK(fake->sent.size() == 3);

	// The video track opens a native I420 stream of its track handle.
	fake->router.dispatch(DecodeEvent(
		SubscribedEventBytes(11, "u42", "TR_video", 55, kTrackKindVideo)));
	IMPULSE_CHECK(fake->sent.size() == 4);
	if (fake->sent.size() == 4) {
		IMPULSE_CHECK(Varint(
			*Message(fake->sent[3], kRequestNewVideoStream),
			kStreamTrackHandle) == 55);
	}

	const std::vector<std::uint8_t> planeY = { 1, 2, 3, 4, 5, 6, 7, 8 };
	const std::vector<std::uint8_t> planeU = { 10, 11 };
	const std::vector<std::uint8_t> planeV = { 20, 21 };
	fake->router.dispatch(DecodeEvent(FrameEventBytes(
		77,
		66,
		4,
		2,
		planeY,
		planeU,
		planeV)));
	IMPULSE_CHECK(sink.count == 1);
	IMPULSE_CHECK(sink.last.participantIdentity == "u42");
	IMPULSE_CHECK(sink.last.width == 4);
	IMPULSE_CHECK(sink.last.height == 2);
	IMPULSE_CHECK(sink.last.timestampUs == 1234);
	IMPULSE_CHECK(sink.last.planes[0].bytes == planeY);
	IMPULSE_CHECK(sink.last.planes[0].stride == 4);
	IMPULSE_CHECK(sink.last.planes[1].bytes == planeU);
	IMPULSE_CHECK(sink.last.planes[2].bytes == planeV);
	// The buffer handle is released once its planes are copied.
	IMPULSE_CHECK(fake->dropped.size() == 1 && fake->dropped[0] == 66);

	// An audio track is played by PlatformAudio and opens no stream.
	fake->router.dispatch(DecodeEvent(
		SubscribedEventBytes(11, "u42", "TR_audio", 56, kTrackKindAudio)));
	IMPULSE_CHECK(fake->sent.size() == 4);

	// Unsubscribing releases the stream and the track; its frames stop.
	fake->router.dispatch(DecodeEvent(
		UnsubscribedEventBytes(11, "u42", "TR_video")));
	IMPULSE_CHECK(fake->dropped.size() == 3);
	fake->router.dispatch(DecodeEvent(FrameEventBytes(
		77,
		67,
		4,
		2,
		planeY,
		planeU,
		planeV)));
	IMPULSE_CHECK(sink.count == 1);

	// Disconnect releases the subscribed audio track as well.
	adapter.disconnect();
	IMPULSE_CHECK(fake->sent.size() == 5);
	fake->router.dispatch(DisconnectEvent(AsyncIdAt(fake->sent, 4)));
	IMPULSE_CHECK(delegate.disconnected == 1);
	IMPULSE_CHECK(std::find(
		fake->dropped.begin(),
		fake->dropped.end(),
		std::uint64_t(56)) != fake->dropped.end());
}

IMPULSE_TEST(LiveKitRemoveSinkStopsFrames) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	fake->responses = {
		AsyncResponse(kResponseConnect, 101),
		HandleResponse(kResponseNewPlatformAudio, 31),
		ReadyResponse(),
		HandleResponse(kResponseNewVideoStream, 77),
	};
	RecordingDelegate delegate;
	RecordingSink sink;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	adapter.setVideoSink(&sink);
	IMPULSE_CHECK(adapter.connect("wss://room", "token"));
	fake->router.dispatch(ConnectEvent(AsyncIdAt(fake->sent, 0), 11, 12));
	fake->router.dispatch(DecodeEvent(
		SubscribedEventBytes(11, "u42", "TR_video", 55, kTrackKindVideo)));

	const std::vector<std::uint8_t> planeY = { 1, 2, 3, 4, 5, 6, 7, 8 };
	const std::vector<std::uint8_t> planeU = { 10, 11 };
	const std::vector<std::uint8_t> planeV = { 20, 21 };
	adapter.setVideoSink(nullptr);
	fake->router.dispatch(DecodeEvent(FrameEventBytes(
		77,
		66,
		4,
		2,
		planeY,
		planeU,
		planeV)));
	IMPULSE_CHECK(sink.count == 0);
	// The frame is still released when no sink takes it.
	IMPULSE_CHECK(fake->dropped.size() == 1 && fake->dropped[0] == 66);
}
