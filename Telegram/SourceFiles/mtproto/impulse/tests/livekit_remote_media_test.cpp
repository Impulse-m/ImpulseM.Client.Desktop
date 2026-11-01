/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/livekit/livekit_adapter.h"
#include "calls/livekit/livekit_adapter_interface.h"
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

using Bytes = std::vector<std::uint8_t>;
using Span = std::span<const std::uint8_t>;

// FfiRequest oneof fields that the tests read.
constexpr std::uint32_t kRequestConnect = 3;
constexpr std::uint32_t kRequestDisconnect = 4;
constexpr std::uint32_t kRequestPublishTrack = 5;
constexpr std::uint32_t kRequestGetSessionStats = 12;
constexpr std::uint32_t kRequestCreateVideoTrack = 15;
constexpr std::uint32_t kRequestLocalTrackMute = 17;
constexpr std::uint32_t kRequestNewVideoSource = 21;

// FfiResponse oneof fields that the tests script.
constexpr std::uint32_t kResponseConnect = 3;
constexpr std::uint32_t kResponsePublishTrack = 5;
constexpr std::uint32_t kResponseCreateVideoTrack = 15;
constexpr std::uint32_t kResponseLocalTrackMute = 17;
constexpr std::uint32_t kResponseNewVideoStream = 20;
constexpr std::uint32_t kResponseNewVideoSource = 21;
constexpr std::uint32_t kResponseNewPlatformAudio = 76;
constexpr std::uint32_t kResponseReadyForRoomEvent = 82;

// FfiEvent and RoomEvent fields that the tests build.
constexpr std::uint32_t kEventRoom = 1;
constexpr std::uint32_t kEventVideoStream = 3;
constexpr std::uint32_t kRoomEventHandle = 1;
constexpr std::uint32_t kRoomEventParticipantDisconnected = 3;
constexpr std::uint32_t kRoomEventTrackPublished = 7;
constexpr std::uint32_t kRoomEventTrackUnpublished = 8;
constexpr std::uint32_t kRoomEventTrackSubscribed = 9;
constexpr std::uint32_t kRoomEventTrackUnsubscribed = 10;
constexpr std::uint32_t kRoomEventTrackMuted = 12;
constexpr std::uint32_t kRoomEventTrackUnmuted = 13;
constexpr std::uint32_t kRoomEventDisconnected = 22;

// The kinds and sources of the publication info.
constexpr std::uint64_t kTrackKindAudio = 1;
constexpr std::uint64_t kTrackKindVideo = 2;
constexpr std::uint64_t kSourceCamera = 1;
constexpr std::uint64_t kSourceMicrophone = 2;
constexpr std::uint64_t kSourceScreenShare = 3;
constexpr std::uint64_t kSourceScreenShareAudio = 4;

// The first field number of an encoded request or response.
[[nodiscard]] std::uint32_t OuterField(const Bytes &data) {
	Impulse::ProtoReader reader(data);
	return reader.next() ? reader.field() : 0;
}

// The async id of a request that carries one, or nullopt.
[[nodiscard]] std::optional<std::uint64_t> RequestAsyncId(Span data) {
	Impulse::ProtoReader outer(data);
	while (outer.next()) {
		const std::uint32_t field = outer.field();
		std::uint32_t asyncField = 0;
		if (field == kRequestConnect) {
			asyncField = 4;
		} else if (field == kRequestDisconnect) {
			asyncField = 2;
		} else if (field == kRequestPublishTrack) {
			asyncField = 4;
		} else if (field == kRequestGetSessionStats) {
			asyncField = 2;
		} else {
			outer.skip();
			continue;
		}
		const Span payload = outer.readBytes();
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
		const std::vector<Bytes> &sent,
		std::size_t index) {
	if (index >= sent.size()) {
		return 0;
	}
	return RequestAsyncId(sent[index]).value_or(0);
}

// The payload of the first length-delimited field with the number.
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

// Stands in for the library. It records every request, answers the scripted
// responses in order, and keeps its own event router as the library does. The
// requests that carry an async id are answered with that id, as the library
// does.
class FakeFfi final : public Impulse::LiveKitFfi {
public:
	bool loaded() const override {
		return true;
	}

	std::optional<Bytes> request(Span data) override {
		sent.emplace_back(data.begin(), data.end());
		if (responses.empty()) {
			return std::nullopt;
		}
		Bytes next = responses.front();
		responses.erase(responses.begin());
		const std::optional<std::uint64_t> requestId = RequestAsyncId(data);
		if (requestId) {
			Impulse::ProtoWriter inner;
			inner.writeVarintField(1, *requestId);
			Impulse::ProtoWriter outer;
			outer.writeMessageField(OuterField(next), inner);
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

	std::vector<Bytes> sent;
	std::vector<Bytes> responses;
	std::vector<std::uint64_t> dropped;
	Impulse::LiveKitEventRouter router;
};

class RecordingDelegate final : public Impulse::LiveKitAdapterDelegate {
public:
	void liveKitConnected() override {
		++connected;
	}

	void liveKitPublished(Impulse::LiveKitPublishKind kind) override {
		publishedKinds.push_back(kind);
	}

	void liveKitFailed(const std::string &) override {
		++failed;
	}

	void liveKitDisconnected() override {
		++disconnected;
	}

	void liveKitRemoteMediaChanged(
			const std::vector<Impulse::LiveKitRemoteMedia> &media) override {
		snapshots.push_back(media);
	}

	int connected = 0;
	int failed = 0;
	int disconnected = 0;
	std::vector<Impulse::LiveKitPublishKind> publishedKinds;
	std::vector<std::vector<Impulse::LiveKitRemoteMedia>> snapshots;
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

[[nodiscard]] Bytes AsyncResponse(
		std::uint32_t responseField,
		std::uint64_t asyncId) {
	Impulse::ProtoWriter inner;
	inner.writeVarintField(1, asyncId);
	Impulse::ProtoWriter outer;
	outer.writeMessageField(responseField, inner);
	return outer.takeBytes();
}

// A response that creates one owned handle, as the library answers the
// synchronous creation requests.
[[nodiscard]] Bytes HandleResponse(
		std::uint32_t responseField,
		std::uint64_t handle) {
	Impulse::ProtoWriter handleMessage;
	handleMessage.writeVarintField(1, handle);
	Impulse::ProtoWriter owned;
	owned.writeMessageField(1, handleMessage);
	Impulse::ProtoWriter message;
	message.writeMessageField(1, owned);
	Impulse::ProtoWriter outer;
	outer.writeMessageField(responseField, message);
	return outer.takeBytes();
}

// The NewPlatformAudioResponse with the handle and the device counts.
[[nodiscard]] Bytes PlatformAudioResponse(std::uint64_t handle) {
	Impulse::ProtoWriter handleMessage;
	handleMessage.writeVarintField(1, handle);
	Impulse::ProtoWriter info;
	info.writeVarintField(1, 0);
	info.writeVarintField(2, 0);
	Impulse::ProtoWriter owned;
	owned.writeMessageField(1, handleMessage);
	owned.writeMessageField(2, info);
	Impulse::ProtoWriter message;
	message.writeMessageField(1, owned);
	Impulse::ProtoWriter outer;
	outer.writeMessageField(kResponseNewPlatformAudio, message);
	return outer.takeBytes();
}

[[nodiscard]] Bytes ReadyResponse() {
	const Impulse::ProtoWriter empty;
	Impulse::ProtoWriter outer;
	outer.writeMessageField(kResponseReadyForRoomEvent, empty);
	return outer.takeBytes();
}

// The LocalTrackMuteResponse with the state the library has now.
[[nodiscard]] Bytes LocalTrackMuteResponse(bool muted) {
	Impulse::ProtoWriter inner;
	inner.writeVarintField(1, muted ? 1 : 0);
	Impulse::ProtoWriter outer;
	outer.writeMessageField(kResponseLocalTrackMute, inner);
	return outer.takeBytes();
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

// Connects a room of the fake library. The responses of the connect, the
// PlatformAudio and the ready request are appended, so a test that scripts
// more responses appends them after these.
void ConnectRoom(FakeFfi &fake, Impulse::LiveKitAdapter &adapter) {
	fake.responses.push_back(AsyncResponse(kResponseConnect, 0));
	fake.responses.push_back(PlatformAudioResponse(31));
	fake.responses.push_back(ReadyResponse());
	adapter.connect("wss://room", "token");
	fake.router.dispatch(ConnectEvent(AsyncIdAt(fake.sent, 0), 11, 12));
}

// A RoomEvent wrapped in the FfiEvent envelope of the room field.
[[nodiscard]] Bytes WrapRoomEvent(const Impulse::ProtoWriter &room) {
	Impulse::ProtoWriter event;
	event.writeMessageField(kEventRoom, room);
	return event.takeBytes();
}

[[nodiscard]] Impulse::ProtoWriter HandleMessage(std::uint64_t id) {
	Impulse::ProtoWriter handle;
	handle.writeVarintField(1, id);
	return handle;
}

// A TrackPublished of a remote participant, with the publication info.
[[nodiscard]] Bytes TrackPublishedEvent(
		std::uint64_t roomHandle,
		const std::string &identity,
		const std::string &sid,
		std::uint64_t kind,
		std::uint64_t source,
		bool muted,
		std::uint64_t publicationHandle) {
	Impulse::ProtoWriter info;
	info.writeStringField(1, sid);
	info.writeVarintField(3, kind);
	info.writeVarintField(4, source);
	info.writeVarintField(9, muted ? 1 : 0);
	Impulse::ProtoWriter publication;
	publication.writeMessageField(1, HandleMessage(publicationHandle));
	publication.writeMessageField(2, info);
	Impulse::ProtoWriter published;
	published.writeStringField(1, identity);
	published.writeMessageField(2, publication);
	Impulse::ProtoWriter room;
	room.writeVarintField(kRoomEventHandle, roomHandle);
	room.writeMessageField(kRoomEventTrackPublished, published);
	return WrapRoomEvent(room);
}

// A TrackSubscribed of a remote participant, with the track info.
[[nodiscard]] Bytes TrackSubscribedEvent(
		std::uint64_t roomHandle,
		const std::string &identity,
		const std::string &sid,
		std::uint64_t trackHandle,
		std::uint64_t kind,
		bool muted) {
	Impulse::ProtoWriter info;
	info.writeStringField(1, sid);
	info.writeVarintField(3, kind);
	info.writeVarintField(5, muted ? 1 : 0);
	Impulse::ProtoWriter track;
	track.writeMessageField(1, HandleMessage(trackHandle));
	track.writeMessageField(2, info);
	Impulse::ProtoWriter subscribed;
	subscribed.writeStringField(1, identity);
	subscribed.writeMessageField(2, track);
	Impulse::ProtoWriter room;
	room.writeVarintField(kRoomEventHandle, roomHandle);
	room.writeMessageField(kRoomEventTrackSubscribed, subscribed);
	return WrapRoomEvent(room);
}

// A TrackMuted or a TrackUnmuted of the room, named by the track sid.
[[nodiscard]] Bytes TrackMuteEvent(
		std::uint64_t roomHandle,
		std::uint32_t field,
		const std::string &identity,
		const std::string &sid) {
	Impulse::ProtoWriter muted;
	muted.writeStringField(1, identity);
	muted.writeStringField(2, sid);
	Impulse::ProtoWriter room;
	room.writeVarintField(kRoomEventHandle, roomHandle);
	room.writeMessageField(field, muted);
	return WrapRoomEvent(room);
}

[[nodiscard]] Bytes TrackUnsubscribedEvent(
		std::uint64_t roomHandle,
		const std::string &identity,
		const std::string &sid) {
	Impulse::ProtoWriter unsubscribed;
	unsubscribed.writeStringField(1, identity);
	unsubscribed.writeStringField(2, sid);
	Impulse::ProtoWriter room;
	room.writeVarintField(kRoomEventHandle, roomHandle);
	room.writeMessageField(kRoomEventTrackUnsubscribed, unsubscribed);
	return WrapRoomEvent(room);
}

[[nodiscard]] Bytes TrackUnpublishedEvent(
		std::uint64_t roomHandle,
		const std::string &identity,
		const std::string &sid) {
	Impulse::ProtoWriter unpublished;
	unpublished.writeStringField(1, identity);
	unpublished.writeStringField(2, sid);
	Impulse::ProtoWriter room;
	room.writeVarintField(kRoomEventHandle, roomHandle);
	room.writeMessageField(kRoomEventTrackUnpublished, unpublished);
	return WrapRoomEvent(room);
}

[[nodiscard]] Bytes ParticipantDisconnectedEvent(
		std::uint64_t roomHandle,
		const std::string &identity) {
	Impulse::ProtoWriter left;
	left.writeStringField(1, identity);
	left.writeVarintField(2, 1);
	Impulse::ProtoWriter room;
	room.writeVarintField(kRoomEventHandle, roomHandle);
	room.writeMessageField(kRoomEventParticipantDisconnected, left);
	return WrapRoomEvent(room);
}

[[nodiscard]] Bytes RoomDisconnectedEvent(
		std::uint64_t roomHandle,
		std::uint64_t reason) {
	Impulse::ProtoWriter disconnected;
	disconnected.writeVarintField(1, reason);
	Impulse::ProtoWriter room;
	room.writeVarintField(kRoomEventHandle, roomHandle);
	room.writeMessageField(kRoomEventDisconnected, disconnected);
	return WrapRoomEvent(room);
}

// One component of an I420 buffer, which points at the plane bytes.
[[nodiscard]] Impulse::ProtoWriter ComponentOf(
		const std::vector<std::uint8_t> &plane,
		std::uint64_t stride) {
	Impulse::ProtoWriter component;
	component.writeVarintField(
		1,
		static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(plane.data())));
	component.writeVarintField(2, stride);
	component.writeVarintField(3, plane.size());
	return component;
}

// A VideoStreamEvent frame of a 4x2 I420 buffer with the given planes. The
// planes must outlive the decode, since the adapter copies them there.
[[nodiscard]] Bytes VideoFrameEvent(
		std::uint64_t streamHandle,
		std::uint64_t bufferHandle,
		const std::vector<std::uint8_t> &planeY,
		const std::vector<std::uint8_t> &planeU,
		const std::vector<std::uint8_t> &planeV) {
	constexpr std::uint64_t kBufferTypeI420 = 5;
	Impulse::ProtoWriter info;
	info.writeVarintField(1, kBufferTypeI420);
	info.writeVarintField(2, 4);
	info.writeVarintField(3, 2);
	info.writeMessageField(7, ComponentOf(planeY, 4));
	info.writeMessageField(7, ComponentOf(planeU, 2));
	info.writeMessageField(7, ComponentOf(planeV, 2));
	Impulse::ProtoWriter buffer;
	buffer.writeMessageField(1, HandleMessage(bufferHandle));
	buffer.writeMessageField(2, info);
	Impulse::ProtoWriter frame;
	frame.writeMessageField(1, buffer);
	frame.writeVarintField(2, 1234);
	Impulse::ProtoWriter stream;
	stream.writeVarintField(1, streamHandle);
	stream.writeMessageField(2, frame);
	Impulse::ProtoWriter event;
	event.writeMessageField(kEventVideoStream, stream);
	return event.takeBytes();
}

// The LiveKitEvent of a TrackPublished of a source, for the codec checks.
[[nodiscard]] Impulse::LiveKitEvent DecodePublished(
		std::uint64_t source,
		bool muted) {
	return Impulse::DecodeLiveKitEvent(TrackPublishedEvent(
		4,
		"alice",
		"TR_source",
		kTrackKindAudio,
		source,
		muted,
		42));
}

} // namespace

IMPULSE_TEST(LiveKitTrackMuteEventsDecode) {
	const Impulse::LiveKitEvent muted = Impulse::DecodeLiveKitEvent(
		TrackMuteEvent(4, kRoomEventTrackMuted, "alice", "TR_mic"));
	IMPULSE_CHECK(muted.kind == Impulse::LiveKitEventKind::TrackMuted);
	IMPULSE_CHECK(muted.roomHandle == 4);
	IMPULSE_CHECK(muted.participantIdentity == "alice");
	IMPULSE_CHECK(muted.trackSid == "TR_mic");

	const Impulse::LiveKitEvent unmuted = Impulse::DecodeLiveKitEvent(
		TrackMuteEvent(4, kRoomEventTrackUnmuted, "alice", "TR_mic"));
	IMPULSE_CHECK(unmuted.kind == Impulse::LiveKitEventKind::TrackUnmuted);
	IMPULSE_CHECK(unmuted.roomHandle == 4);
	IMPULSE_CHECK(unmuted.participantIdentity == "alice");
	IMPULSE_CHECK(unmuted.trackSid == "TR_mic");
}

IMPULSE_TEST(LiveKitParticipantDisconnectedEventDecodes) {
	const Impulse::LiveKitEvent decoded = Impulse::DecodeLiveKitEvent(
		ParticipantDisconnectedEvent(4, "bob"));
	IMPULSE_CHECK(decoded.kind == Impulse::LiveKitEventKind::ParticipantDisconnected);
	IMPULSE_CHECK(decoded.roomHandle == 4);
	IMPULSE_CHECK(decoded.participantIdentity == "bob");
}

IMPULSE_TEST(LiveKitTrackPublishedCarriesSourceAndMutedState) {
	const Impulse::LiveKitEvent mic = DecodePublished(kSourceMicrophone, true);
	IMPULSE_CHECK(mic.kind == Impulse::LiveKitEventKind::TrackPublished);
	IMPULSE_CHECK(mic.trackKind == Impulse::LiveKitTrackKind::Audio);
	IMPULSE_CHECK(mic.trackSource == Impulse::LiveKitTrackSource::Microphone);
	IMPULSE_CHECK(mic.trackMuted);
	IMPULSE_CHECK(mic.publicationHandle == 42);

	IMPULSE_CHECK(DecodePublished(kSourceCamera, false).trackSource
		== Impulse::LiveKitTrackSource::Camera);
	IMPULSE_CHECK(DecodePublished(kSourceScreenShare, false).trackSource
		== Impulse::LiveKitTrackSource::ScreenShare);
	IMPULSE_CHECK(DecodePublished(kSourceScreenShareAudio, false).trackSource
		== Impulse::LiveKitTrackSource::ScreenShareAudio);
	IMPULSE_CHECK(DecodePublished(0, false).trackSource
		== Impulse::LiveKitTrackSource::Unknown);
	IMPULSE_CHECK(!DecodePublished(kSourceCamera, false).trackMuted);
}

IMPULSE_TEST(LiveKitTrackSubscribedCarriesTheMutedState) {
	const Impulse::LiveKitEvent decoded = Impulse::DecodeLiveKitEvent(
		TrackSubscribedEvent(4, "alice", "TR_mic", 55, kTrackKindAudio, true));
	IMPULSE_CHECK(decoded.kind == Impulse::LiveKitEventKind::TrackSubscribed);
	IMPULSE_CHECK(decoded.trackHandle == 55);
	IMPULSE_CHECK(decoded.trackKind == Impulse::LiveKitTrackKind::Audio);
	IMPULSE_CHECK(decoded.trackMuted);
}

IMPULSE_TEST(LiveKitRoomDisconnectCarriesTheReason) {
	const Impulse::LiveKitEvent dropped = Impulse::DecodeLiveKitEvent(
		RoomDisconnectedEvent(4, 4));
	IMPULSE_CHECK(dropped.kind == Impulse::LiveKitEventKind::RoomDisconnected);
	IMPULSE_CHECK(dropped.disconnectReason == 4);
	IMPULSE_CHECK(!dropped.clientInitiated);

	const Impulse::LiveKitEvent left = Impulse::DecodeLiveKitEvent(
		RoomDisconnectedEvent(4, 1));
	IMPULSE_CHECK(left.disconnectReason == 1);
	IMPULSE_CHECK(left.clientInitiated);
}

IMPULSE_TEST(LiveKitRemoteMediaFollowsPublishSubscribeMuteAndUnpublish) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);
	IMPULSE_CHECK(delegate.connected == 1);

	// Published: the track is listed, not subscribed, with its source and state.
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackPublishedEvent(
		11,
		"alice",
		"TR_mic",
		kTrackKindAudio,
		kSourceMicrophone,
		false,
		42)));
	IMPULSE_CHECK(delegate.snapshots.size() == 1);
	if (delegate.snapshots.size() == 1) {
		const std::vector<Impulse::LiveKitRemoteMedia> &media = delegate.snapshots[0];
		IMPULSE_CHECK(media.size() == 1);
		if (media.size() == 1) {
			IMPULSE_CHECK(media[0].participantIdentity == "alice");
			IMPULSE_CHECK(media[0].trackSid == "TR_mic");
			IMPULSE_CHECK(media[0].kind == Impulse::LiveKitTrackKind::Audio);
			IMPULSE_CHECK(media[0].source == Impulse::LiveKitTrackSource::Microphone);
			IMPULSE_CHECK(!media[0].muted);
			IMPULSE_CHECK(!media[0].subscribed);
		}
	}

	// Subscribed: the audio track opens no stream, so only the snapshot changes.
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackSubscribedEvent(
		11,
		"alice",
		"TR_mic",
		55,
		kTrackKindAudio,
		false)));
	IMPULSE_CHECK(delegate.snapshots.size() == 2);
	if (delegate.snapshots.size() == 2 && delegate.snapshots[1].size() == 1) {
		IMPULSE_CHECK(delegate.snapshots[1][0].subscribed);
	}

	// Muted, and the same mute again: only the first one is a change.
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		TrackMuteEvent(11, kRoomEventTrackMuted, "alice", "TR_mic")));
	IMPULSE_CHECK(delegate.snapshots.size() == 3);
	if (delegate.snapshots.size() == 3 && delegate.snapshots[2].size() == 1) {
		IMPULSE_CHECK(delegate.snapshots[2][0].muted);
	}
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		TrackMuteEvent(11, kRoomEventTrackMuted, "alice", "TR_mic")));
	IMPULSE_CHECK(delegate.snapshots.size() == 3);

	// Unmuted.
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		TrackMuteEvent(11, kRoomEventTrackUnmuted, "alice", "TR_mic")));
	IMPULSE_CHECK(delegate.snapshots.size() == 4);
	if (delegate.snapshots.size() == 4 && delegate.snapshots[3].size() == 1) {
		IMPULSE_CHECK(!delegate.snapshots[3][0].muted);
	}

	// Unsubscribed: the publication stays listed, and its track handle is released.
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		TrackUnsubscribedEvent(11, "alice", "TR_mic")));
	IMPULSE_CHECK(delegate.snapshots.size() == 5);
	if (delegate.snapshots.size() == 5 && delegate.snapshots[4].size() == 1) {
		IMPULSE_CHECK(!delegate.snapshots[4][0].subscribed);
	}
	IMPULSE_CHECK(std::find(fake->dropped.begin(), fake->dropped.end(), std::uint64_t(55)) != fake->dropped.end());

	// A mute of a track that is not known changes nothing.
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		TrackMuteEvent(11, kRoomEventTrackMuted, "alice", "TR_unknown")));
	IMPULSE_CHECK(delegate.snapshots.size() == 5);

	// Unpublished: the snapshot is empty and the publication handle is released.
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		TrackUnpublishedEvent(11, "alice", "TR_mic")));
	IMPULSE_CHECK(delegate.snapshots.size() == 6);
	if (delegate.snapshots.size() == 6) {
		IMPULSE_CHECK(delegate.snapshots[5].empty());
	}
	IMPULSE_CHECK(std::find(fake->dropped.begin(), fake->dropped.end(), std::uint64_t(42)) != fake->dropped.end());
}

IMPULSE_TEST(LiveKitParticipantLeaveRemovesItsRemoteTracks) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);

	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackPublishedEvent(
		11, "alice", "TR_alice", kTrackKindAudio, kSourceMicrophone, false, 42)));
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackSubscribedEvent(
		11, "alice", "TR_alice", 55, kTrackKindAudio, false)));
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackPublishedEvent(
		11, "bob", "TR_bob", kTrackKindAudio, kSourceMicrophone, false, 43)));
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackSubscribedEvent(
		11, "bob", "TR_bob", 56, kTrackKindAudio, false)));
	IMPULSE_CHECK(delegate.snapshots.size() == 4);
	if (delegate.snapshots.size() == 4) {
		IMPULSE_CHECK(delegate.snapshots[3].size() == 2);
	}

	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		ParticipantDisconnectedEvent(11, "alice")));
	IMPULSE_CHECK(delegate.snapshots.size() == 5);
	if (delegate.snapshots.size() == 5) {
		const std::vector<Impulse::LiveKitRemoteMedia> &media = delegate.snapshots[4];
		IMPULSE_CHECK(media.size() == 1);
		if (media.size() == 1) {
			IMPULSE_CHECK(media[0].participantIdentity == "bob");
			IMPULSE_CHECK(media[0].trackSid == "TR_bob");
		}
	}
	// The handles of the leaving participant are released, and no others.
	IMPULSE_CHECK(std::find(fake->dropped.begin(), fake->dropped.end(), std::uint64_t(42)) != fake->dropped.end());
	IMPULSE_CHECK(std::find(fake->dropped.begin(), fake->dropped.end(), std::uint64_t(55)) != fake->dropped.end());
	IMPULSE_CHECK(std::find(fake->dropped.begin(), fake->dropped.end(), std::uint64_t(43)) == fake->dropped.end());
	IMPULSE_CHECK(std::find(fake->dropped.begin(), fake->dropped.end(), std::uint64_t(56)) == fake->dropped.end());
}

IMPULSE_TEST(LiveKitVideoFrameCarriesItsSourceAndTrackSid) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	RecordingSink sink;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	adapter.setVideoSink(&sink);
	ConnectRoom(*fake, adapter);

	// A screen share and a camera, each with its own video stream.
	fake->responses.push_back(HandleResponse(kResponseNewVideoStream, 77));
	fake->responses.push_back(HandleResponse(kResponseNewVideoStream, 78));
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackPublishedEvent(
		11, "alice", "TR_screen", kTrackKindVideo, kSourceScreenShare, false, 44)));
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackSubscribedEvent(
		11, "alice", "TR_screen", 57, kTrackKindVideo, false)));
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackPublishedEvent(
		11, "alice", "TR_cam", kTrackKindVideo, kSourceCamera, false, 45)));
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackSubscribedEvent(
		11, "alice", "TR_cam", 58, kTrackKindVideo, false)));

	const std::vector<std::uint8_t> planeY = { 1, 2, 3, 4, 5, 6, 7, 8 };
	const std::vector<std::uint8_t> planeU = { 10, 11 };
	const std::vector<std::uint8_t> planeV = { 20, 21 };
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		VideoFrameEvent(77, 66, planeY, planeU, planeV)));
	IMPULSE_CHECK(sink.count == 1);
	IMPULSE_CHECK(sink.last.participantIdentity == "alice");
	IMPULSE_CHECK(sink.last.trackSid == "TR_screen");
	IMPULSE_CHECK(sink.last.source == Impulse::LiveKitTrackSource::ScreenShare);
	IMPULSE_CHECK(sink.last.planes[0].bytes == planeY);
	IMPULSE_CHECK(std::find(fake->dropped.begin(), fake->dropped.end(), std::uint64_t(66)) != fake->dropped.end());

	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		VideoFrameEvent(78, 67, planeY, planeU, planeV)));
	IMPULSE_CHECK(sink.count == 2);
	IMPULSE_CHECK(sink.last.trackSid == "TR_cam");
	IMPULSE_CHECK(sink.last.source == Impulse::LiveKitTrackSource::Camera);
	IMPULSE_CHECK(sink.last.participantIdentity == "alice");
}

IMPULSE_TEST(LiveKitShownVideoTrackDropsOtherStreamsBeforeTheCopy) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	RecordingSink sink;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	adapter.setVideoSink(&sink);
	ConnectRoom(*fake, adapter);

	fake->responses.push_back(HandleResponse(kResponseNewVideoStream, 77));
	fake->responses.push_back(HandleResponse(kResponseNewVideoStream, 78));
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackPublishedEvent(
		11, "alice", "TR_screen", kTrackKindVideo, kSourceScreenShare, false, 44)));
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackSubscribedEvent(
		11, "alice", "TR_screen", 57, kTrackKindVideo, false)));
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackPublishedEvent(
		11, "alice", "TR_cam", kTrackKindVideo, kSourceCamera, false, 45)));
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(TrackSubscribedEvent(
		11, "alice", "TR_cam", 58, kTrackKindVideo, false)));

	const std::vector<std::uint8_t> planeY = { 1, 2, 3, 4, 5, 6, 7, 8 };
	const std::vector<std::uint8_t> planeU = { 10, 11 };
	const std::vector<std::uint8_t> planeV = { 20, 21 };

	// Only the camera is shown: the frame of the screen share is released
	// without being copied or delivered.
	adapter.setShownVideoTrack("TR_cam");
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		VideoFrameEvent(77, 66, planeY, planeU, planeV)));
	IMPULSE_CHECK(sink.count == 0);
	IMPULSE_CHECK(std::find(fake->dropped.begin(), fake->dropped.end(), std::uint64_t(66)) != fake->dropped.end());

	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		VideoFrameEvent(78, 67, planeY, planeU, planeV)));
	IMPULSE_CHECK(sink.count == 1);
	IMPULSE_CHECK(sink.last.trackSid == "TR_cam");

	// With no track shown, the frames of both streams are released unseen.
	adapter.setShownVideoTrack(std::string());
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		VideoFrameEvent(78, 68, planeY, planeU, planeV)));
	IMPULSE_CHECK(sink.count == 1);
	IMPULSE_CHECK(std::find(fake->dropped.begin(), fake->dropped.end(), std::uint64_t(68)) != fake->dropped.end());
}

IMPULSE_TEST(LiveKitScreenShareDisabledBeforePublishIsAppliedAtPublish) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);
	const std::size_t before = fake->sent.size();

	// Before the publish nothing is sent; the value is kept.
	IMPULSE_CHECK(adapter.setScreenShareEnabled(false));
	IMPULSE_CHECK(fake->sent.size() == before);

	fake->responses.push_back(HandleResponse(kResponseNewVideoSource, 51));
	fake->responses.push_back(HandleResponse(kResponseCreateVideoTrack, 52));
	fake->responses.push_back(LocalTrackMuteResponse(true));
	fake->responses.push_back(AsyncResponse(kResponsePublishTrack, 0));
	IMPULSE_CHECK(adapter.publishScreenShare(640, 360));

	// New source, track, the mute of the track, then the publish.
	IMPULSE_CHECK(fake->sent.size() == before + 4);
	if (fake->sent.size() == before + 4) {
		IMPULSE_CHECK(OuterField(fake->sent[before]) == kRequestNewVideoSource);
		IMPULSE_CHECK(OuterField(fake->sent[before + 1]) == kRequestCreateVideoTrack);
		IMPULSE_CHECK(OuterField(fake->sent[before + 2]) == kRequestLocalTrackMute);
		IMPULSE_CHECK(OuterField(fake->sent[before + 3]) == kRequestPublishTrack);
		const std::optional<Span> mute = MessageField(
			fake->sent[before + 2],
			kRequestLocalTrackMute);
		IMPULSE_CHECK(mute.has_value());
		if (mute) {
			IMPULSE_CHECK(VarintField(*mute, 1) == 52);
			IMPULSE_CHECK(VarintField(*mute, 2) == 1);
		}
	}

	Impulse::LiveKitEvent published;
	published.kind = Impulse::LiveKitEventKind::PublishTrack;
	published.asyncId = AsyncIdAt(fake->sent, before + 3);
	published.publicationHandle = 53;
	fake->router.dispatch(published);
	IMPULSE_CHECK(delegate.publishedKinds.size() == 1);
	if (delegate.publishedKinds.size() == 1) {
		IMPULSE_CHECK(delegate.publishedKinds[0] == Impulse::LiveKitPublishKind::ScreenShare);
	}
}

IMPULSE_TEST(LiveKitScreenShareEnabledAfterPublishSendsMuteRequests) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);

	// A screen share starts enabled, so its publish sends no mute request.
	fake->responses.push_back(HandleResponse(kResponseNewVideoSource, 51));
	fake->responses.push_back(HandleResponse(kResponseCreateVideoTrack, 52));
	fake->responses.push_back(AsyncResponse(kResponsePublishTrack, 0));
	IMPULSE_CHECK(adapter.publishScreenShare(640, 360));
	const std::size_t published = fake->sent.size();
	IMPULSE_CHECK(OuterField(fake->sent[published - 1]) == kRequestPublishTrack);
	IMPULSE_CHECK(OuterField(fake->sent[published - 2]) == kRequestCreateVideoTrack);

	fake->responses.push_back(LocalTrackMuteResponse(true));
	IMPULSE_CHECK(adapter.setScreenShareEnabled(false));
	IMPULSE_CHECK(fake->sent.size() == published + 1);
	if (fake->sent.size() == published + 1) {
		const std::optional<Span> mute = MessageField(
			fake->sent[published],
			kRequestLocalTrackMute);
		IMPULSE_CHECK(mute.has_value());
		if (mute) {
			IMPULSE_CHECK(VarintField(*mute, 1) == 52);
			IMPULSE_CHECK(VarintField(*mute, 2) == 1);
		}
	}

	fake->responses.push_back(LocalTrackMuteResponse(false));
	IMPULSE_CHECK(adapter.setScreenShareEnabled(true));
	IMPULSE_CHECK(fake->sent.size() == published + 2);
	if (fake->sent.size() == published + 2) {
		const std::optional<Span> mute = MessageField(
			fake->sent[published + 1],
			kRequestLocalTrackMute);
		IMPULSE_CHECK(mute.has_value());
		if (mute) {
			IMPULSE_CHECK(VarintField(*mute, 2) == 0);
		}
	}
	IMPULSE_CHECK(delegate.failed == 0);
	IMPULSE_CHECK(adapter.connected());
}
