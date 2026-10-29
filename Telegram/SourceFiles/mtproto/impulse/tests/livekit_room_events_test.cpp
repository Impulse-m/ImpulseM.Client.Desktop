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

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

// FfiRequest oneof numbers of the requests the codec encodes.
constexpr std::uint32_t kRequestConnect = 3;
constexpr std::uint32_t kRequestDisconnect = 4;
constexpr std::uint32_t kRequestPublishTrack = 5;
constexpr std::uint32_t kRequestLocalTrackMute = 17;

// FfiResponse oneof numbers the adapter reads.
constexpr std::uint32_t kResponseConnect = 3;
constexpr std::uint32_t kResponseDisconnect = 4;
constexpr std::uint32_t kResponseLocalTrackMute = 17;
constexpr std::uint32_t kResponseNewPlatformAudio = 76;
constexpr std::uint32_t kResponseReadyForRoomEvent = 82;

// FfiEvent room_event, and the RoomEvent fields the test builds.
constexpr std::uint32_t kEventRoom = 1;
constexpr std::uint32_t kRoomEventHandle = 1;
constexpr std::uint32_t kRoomEventDisconnected = 22;
constexpr std::uint32_t kRoomEventE2eeStateChanged = 25;
constexpr std::uint32_t kDisconnectedReason = 1;
constexpr std::uint32_t kE2eeParticipantIdentity = 1;
constexpr std::uint32_t kE2eeState = 2;

// ConnectRequest, RoomOptions and DisconnectRequest fields.
constexpr std::uint32_t kConnectUrl = 1;
constexpr std::uint32_t kConnectToken = 2;
constexpr std::uint32_t kConnectOptions = 3;
constexpr std::uint32_t kConnectRequestAsyncId = 4;
constexpr std::uint32_t kDisconnectRoomHandle = 1;
constexpr std::uint32_t kDisconnectRequestAsyncId = 2;

// PublishTrackRequest and TrackPublishOptions fields.
constexpr std::uint32_t kPublishLocalParticipant = 1;
constexpr std::uint32_t kPublishTrackHandle = 2;
constexpr std::uint32_t kPublishOptions = 3;
constexpr std::uint32_t kPublishRequestAsyncId = 4;
constexpr std::uint32_t kPublishOptionsSource = 7;

// LocalTrackMuteRequest and LocalTrackMuteResponse fields.
constexpr std::uint32_t kMuteTrackHandle = 1;
constexpr std::uint32_t kMuteMute = 2;
constexpr std::uint32_t kMutedField = 1;

// Async responses, owned handles and the ids they hold.
constexpr std::uint32_t kAsyncIdField = 1;
constexpr std::uint32_t kOwnedMessageField = 1;
constexpr std::uint32_t kOwnedHandleField = 1;
constexpr std::uint32_t kHandleIdField = 1;

// The top bit of a client async id; see the adapter.
constexpr std::uint64_t kClientAsyncIdFlag = std::uint64_t(1) << 63;

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

// The first varint of the number, or nullopt when the field is absent.
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

// The request_async_id of a connect, disconnect or publish request.
[[nodiscard]] std::optional<std::uint64_t> RequestAsyncId(
		std::span<const std::uint8_t> data) {
	std::uint32_t requestField = 0;
	std::uint32_t asyncField = 0;
	Impulse::ProtoReader outer(data);
	while (outer.next()) {
		const std::uint32_t field = outer.field();
		if (field == kRequestConnect) {
			requestField = field;
			asyncField = kConnectRequestAsyncId;
			break;
		} else if (field == kRequestDisconnect) {
			requestField = field;
			asyncField = kDisconnectRequestAsyncId;
			break;
		} else if (field == kRequestPublishTrack) {
			requestField = field;
			asyncField = kPublishRequestAsyncId;
			break;
		}
		outer.skip();
	}
	if (!requestField) {
		return std::nullopt;
	}
	const std::optional<std::span<const std::uint8_t>> payload = Message(
		data,
		requestField);
	if (!payload) {
		return std::nullopt;
	}
	return Varint(*payload, asyncField);
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

// Wraps a message in the field with the given number.
[[nodiscard]] std::vector<std::uint8_t> Wrap(
		std::uint32_t field,
		const Impulse::ProtoWriter &message) {
	Impulse::ProtoWriter outer;
	outer.writeMessageField(field, message);
	return outer.takeBytes();
}

[[nodiscard]] std::vector<std::uint8_t> AsyncResponse(
		std::uint32_t responseField,
		std::uint64_t asyncId) {
	Impulse::ProtoWriter inner;
	inner.writeVarintField(kAsyncIdField, asyncId);
	return Wrap(responseField, inner);
}

// The response of a request that creates one owned handle.
[[nodiscard]] std::vector<std::uint8_t> HandleResponse(
		std::uint32_t responseField,
		std::uint64_t id) {
	Impulse::ProtoWriter handle;
	handle.writeVarintField(kHandleIdField, id);
	Impulse::ProtoWriter owned;
	owned.writeMessageField(kOwnedHandleField, handle);
	Impulse::ProtoWriter message;
	message.writeMessageField(kOwnedMessageField, owned);
	return Wrap(responseField, message);
}

[[nodiscard]] std::vector<std::uint8_t> ReadyResponse() {
	return Wrap(kResponseReadyForRoomEvent, Impulse::ProtoWriter());
}

[[nodiscard]] std::vector<std::uint8_t> LocalTrackMuteResponse(bool muted) {
	Impulse::ProtoWriter message;
	message.writeVarintField(kMutedField, muted ? 1 : 0);
	return Wrap(kResponseLocalTrackMute, message);
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

[[nodiscard]] Impulse::LiveKitEvent RoomDisconnectEvent(
		std::uint64_t roomHandle,
		bool clientInitiated) {
	Impulse::LiveKitEvent event;
	event.kind = Impulse::LiveKitEventKind::RoomDisconnected;
	event.roomHandle = roomHandle;
	event.clientInitiated = clientInitiated;
	return event;
}

[[nodiscard]] Impulse::LiveKitEvent EncryptionEvent(
		std::uint64_t roomHandle,
		Impulse::LiveKitEncryptionState state) {
	Impulse::LiveKitEvent event;
	event.kind = Impulse::LiveKitEventKind::EncryptionStateChanged;
	event.roomHandle = roomHandle;
	event.participantIdentity = "u42";
	event.encryptionState = state;
	return event;
}

// The encoded FfiEvent of a room event that carries one message in the given
// RoomEvent field.
[[nodiscard]] std::vector<std::uint8_t> RoomEventBytes(
		std::uint64_t roomHandle,
		std::uint32_t field,
		const Impulse::ProtoWriter &message) {
	Impulse::ProtoWriter room;
	room.writeVarintField(kRoomEventHandle, roomHandle);
	room.writeMessageField(field, message);
	return Wrap(kEventRoom, room);
}

// Stands in for the library. It records every request, answers the scripted
// responses in order and keeps its own event router. An async request is
// answered with its own request_async_id, as the library does. With
// deliverConnectEarly, the room event of the connect is reported before the
// connect request is answered, as a library thread can do.
class FakeFfi final : public Impulse::LiveKitFfi {
public:
	bool loaded() const override {
		return true;
	}

	std::optional<std::vector<std::uint8_t>> request(
			std::span<const std::uint8_t> data) override {
		sent.emplace_back(data.begin(), data.end());
		const std::optional<std::uint64_t> requestId = RequestAsyncId(data);
		if (deliverConnectEarly && IsConnectRequest(data)) {
			router.dispatch(ConnectEvent(requestId.value_or(0), 11, 12));
		}
		if (responses.empty()) {
			return std::nullopt;
		}
		std::vector<std::uint8_t> next = responses.front();
		responses.erase(responses.begin());
		if (requestId) {
			Impulse::ProtoWriter inner;
			inner.writeVarintField(kAsyncIdField, *requestId);
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

	bool deliverConnectEarly = false;
	std::vector<std::vector<std::uint8_t>> sent;
	std::vector<std::vector<std::uint8_t>> responses;
	std::vector<std::uint64_t> dropped;
	Impulse::LiveKitEventRouter router;

private:
	[[nodiscard]] static bool IsConnectRequest(
			std::span<const std::uint8_t> data) {
		return Message(data, kRequestConnect).has_value();
	}
};

class RecordingDelegate final : public Impulse::LiveKitAdapterDelegate {
public:
	void liveKitConnected() override {
		++connected;
	}

	void liveKitPublished(Impulse::LiveKitPublishKind) override {
		++published;
	}

	void liveKitFailed(const std::string &) override {
		++failed;
	}

	void liveKitDisconnected() override {
		++disconnected;
	}

	int connected = 0;
	int published = 0;
	int failed = 0;
	int disconnected = 0;
};

void RunNow(std::function<void()> task) {
	task();
}

// Runs the tasks that the adapter posted to its owner thread, in order.
void RunPosted(std::vector<std::function<void()>> &tasks) {
	while (!tasks.empty()) {
		std::function<void()> task = std::move(tasks.front());
		tasks.erase(tasks.begin());
		task();
	}
}

// A connected adapter of room 11 with the PlatformAudio and ReadyForRoomEvent
// requests answered. Its connect has the async id of the first request.
[[nodiscard]] std::uint64_t ConnectRoom(
		FakeFfi &fake,
		Impulse::LiveKitAdapter &adapter) {
	fake.responses.push_back(AsyncResponse(kResponseConnect, 0));
	fake.responses.push_back(HandleResponse(kResponseNewPlatformAudio, 31));
	fake.responses.push_back(ReadyResponse());
	const bool started = adapter.connect("wss://room", "token");
	const std::uint64_t connectId = AsyncIdAt(fake.sent, 0);
	if (!started || connectId == 0) {
		return 0;
	}
	fake.router.dispatch(ConnectEvent(connectId, 11, 12));
	return connectId;
}

} // namespace

IMPULSE_TEST(LiveKitRoomEventsRequestFieldNumbers) {
	const std::vector<std::uint8_t> connect =
		Impulse::EncodeLiveKitConnectRequest(
			"wss://room",
			"token",
			Impulse::LiveKitRoomOptions(),
			9,
			std::nullopt);
	const std::optional<std::span<const std::uint8_t>> connectMessage =
		Message(connect, kRequestConnect);
	IMPULSE_CHECK(connectMessage.has_value());
	if (connectMessage) {
		IMPULSE_CHECK(Message(*connectMessage, kConnectUrl).has_value());
		IMPULSE_CHECK(Message(*connectMessage, kConnectToken).has_value());
		IMPULSE_CHECK(Message(*connectMessage, kConnectOptions).has_value());
		IMPULSE_CHECK(Varint(*connectMessage, kConnectRequestAsyncId) == 9);
	}

	const std::vector<std::uint8_t> disconnect =
		Impulse::EncodeLiveKitDisconnectRequest(11, 12);
	const std::optional<std::span<const std::uint8_t>> disconnectMessage =
		Message(disconnect, kRequestDisconnect);
	IMPULSE_CHECK(disconnectMessage.has_value());
	if (disconnectMessage) {
		IMPULSE_CHECK(Varint(*disconnectMessage, kDisconnectRoomHandle) == 11);
		IMPULSE_CHECK(
			Varint(*disconnectMessage, kDisconnectRequestAsyncId) == 12);
	}

	const std::vector<std::uint8_t> publish =
		Impulse::EncodeLiveKitPublishTrackRequest(
			21,
			22,
			Impulse::LiveKitTrackSource::Microphone,
			23);
	const std::optional<std::span<const std::uint8_t>> publishMessage =
		Message(publish, kRequestPublishTrack);
	IMPULSE_CHECK(publishMessage.has_value());
	if (publishMessage) {
		IMPULSE_CHECK(Varint(*publishMessage, kPublishLocalParticipant) == 21);
		IMPULSE_CHECK(Varint(*publishMessage, kPublishTrackHandle) == 22);
		IMPULSE_CHECK(Varint(*publishMessage, kPublishRequestAsyncId) == 23);
		const std::optional<std::span<const std::uint8_t>> options = Message(
			*publishMessage,
			kPublishOptions);
		IMPULSE_CHECK(options.has_value());
		if (options) {
			// TrackSource.SOURCE_MICROPHONE is 2.
			IMPULSE_CHECK(Varint(*options, kPublishOptionsSource) == 2);
		}
	}

	const std::vector<std::uint8_t> mute =
		Impulse::EncodeLiveKitLocalTrackMuteRequest(33, true);
	const std::optional<std::span<const std::uint8_t>> muteMessage =
		Message(mute, kRequestLocalTrackMute);
	IMPULSE_CHECK(muteMessage.has_value());
	if (muteMessage) {
		IMPULSE_CHECK(Varint(*muteMessage, kMuteTrackHandle) == 33);
		IMPULSE_CHECK(Varint(*muteMessage, kMuteMute) == 1);
	}
	IMPULSE_CHECK(Impulse::DecodeLiveKitLocalTrackMuteResponse(
		LocalTrackMuteResponse(true)) == std::optional<bool>(true));
	IMPULSE_CHECK(Impulse::DecodeLiveKitLocalTrackMuteResponse(
		LocalTrackMuteResponse(false)) == std::optional<bool>(false));
}

IMPULSE_TEST(LiveKitRoomEventsDecodeRoomDisconnect) {
	Impulse::ProtoWriter serverLeave;
	serverLeave.writeVarintField(kDisconnectedReason, 3);
	const Impulse::LiveKitEvent dropped = Impulse::DecodeLiveKitEvent(
		RoomEventBytes(11, kRoomEventDisconnected, serverLeave));
	IMPULSE_CHECK(dropped.kind == Impulse::LiveKitEventKind::RoomDisconnected);
	IMPULSE_CHECK(dropped.roomHandle == 11);
	IMPULSE_CHECK(!dropped.clientInitiated);

	// DisconnectReason.CLIENT_INITIATED is 1.
	Impulse::ProtoWriter clientLeave;
	clientLeave.writeVarintField(kDisconnectedReason, 1);
	const Impulse::LiveKitEvent left = Impulse::DecodeLiveKitEvent(
		RoomEventBytes(11, kRoomEventDisconnected, clientLeave));
	IMPULSE_CHECK(left.kind == Impulse::LiveKitEventKind::RoomDisconnected);
	IMPULSE_CHECK(left.clientInitiated);
}

IMPULSE_TEST(LiveKitRoomEventsDecodeEncryptionState) {
	const std::pair<std::uint64_t, Impulse::LiveKitEncryptionState> cases[] = {
		{ 0, Impulse::LiveKitEncryptionState::New },
		{ 1, Impulse::LiveKitEncryptionState::Ok },
		{ 2, Impulse::LiveKitEncryptionState::EncryptionFailed },
		{ 3, Impulse::LiveKitEncryptionState::DecryptionFailed },
		{ 4, Impulse::LiveKitEncryptionState::MissingKey },
		{ 5, Impulse::LiveKitEncryptionState::KeyRatcheted },
		{ 6, Impulse::LiveKitEncryptionState::InternalError },
		{ 9, Impulse::LiveKitEncryptionState::Unknown },
	};
	for (const std::pair<std::uint64_t, Impulse::LiveKitEncryptionState> &item
			: cases) {
		Impulse::ProtoWriter change;
		change.writeStringField(kE2eeParticipantIdentity, "u42");
		change.writeVarintField(kE2eeState, item.first);
		const Impulse::LiveKitEvent event = Impulse::DecodeLiveKitEvent(
			RoomEventBytes(11, kRoomEventE2eeStateChanged, change));
		IMPULSE_CHECK(
			event.kind == Impulse::LiveKitEventKind::EncryptionStateChanged);
		IMPULSE_CHECK(event.roomHandle == 11);
		IMPULSE_CHECK(event.participantIdentity == "u42");
		IMPULSE_CHECK(event.encryptionState == item.second);
	}
}

IMPULSE_TEST(LiveKitRoomEventsServerDropFailsRoom) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	IMPULSE_CHECK(ConnectRoom(*fake, adapter) != 0);
	IMPULSE_CHECK(delegate.connected == 1);

	fake->router.dispatch(RoomDisconnectEvent(11, false));
	IMPULSE_CHECK(delegate.failed == 1);
	IMPULSE_CHECK(delegate.disconnected == 0);
	IMPULSE_CHECK(!adapter.connected());

	// The failed room takes no further events.
	fake->router.dispatch(RoomDisconnectEvent(11, false));
	IMPULSE_CHECK(delegate.failed == 1);
}

IMPULSE_TEST(LiveKitRoomEventsClientLeaveIsNotAFailure) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	const std::uint64_t connectId = ConnectRoom(*fake, adapter);
	IMPULSE_CHECK(connectId != 0);

	// The room reports the leave that the adapter asked for, and no failure
	// follows; the disconnect callback finishes the leave.
	fake->responses.push_back(AsyncResponse(kResponseDisconnect, 0));
	adapter.disconnect();
	IMPULSE_CHECK(fake->sent.size() == 4);
	fake->router.dispatch(RoomDisconnectEvent(11, true));
	IMPULSE_CHECK(delegate.failed == 0);
	IMPULSE_CHECK(delegate.disconnected == 0);

	fake->router.dispatch(DisconnectEvent(AsyncIdAt(fake->sent, 3)));
	IMPULSE_CHECK(delegate.failed == 0);
	IMPULSE_CHECK(delegate.disconnected == 1);
}

IMPULSE_TEST(LiveKitRoomEventsEncryptionFailureEndsCall) {
	const Impulse::LiveKitEncryptionState failures[] = {
		Impulse::LiveKitEncryptionState::EncryptionFailed,
		Impulse::LiveKitEncryptionState::DecryptionFailed,
		Impulse::LiveKitEncryptionState::MissingKey,
		Impulse::LiveKitEncryptionState::InternalError,
	};
	for (const Impulse::LiveKitEncryptionState state : failures) {
		const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
		RecordingDelegate delegate;
		Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
		IMPULSE_CHECK(ConnectRoom(*fake, adapter) != 0);
		fake->router.dispatch(EncryptionEvent(11, state));
		IMPULSE_CHECK(delegate.failed == 1);
		IMPULSE_CHECK(delegate.connected == 1);
	}

	// The states that are not failures keep the call.
	const Impulse::LiveKitEncryptionState others[] = {
		Impulse::LiveKitEncryptionState::New,
		Impulse::LiveKitEncryptionState::Ok,
		Impulse::LiveKitEncryptionState::KeyRatcheted,
	};
	for (const Impulse::LiveKitEncryptionState state : others) {
		const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
		RecordingDelegate delegate;
		Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
		IMPULSE_CHECK(ConnectRoom(*fake, adapter) != 0);
		fake->router.dispatch(EncryptionEvent(11, state));
		IMPULSE_CHECK(delegate.failed == 0);
		IMPULSE_CHECK(adapter.connected());
	}
}

IMPULSE_TEST(LiveKitRoomEventsConnectEventBeforeResponseIsRouted) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	fake->deliverConnectEarly = true;
	fake->responses = {
		AsyncResponse(kResponseConnect, 0),
		HandleResponse(kResponseNewPlatformAudio, 31),
		ReadyResponse(),
	};
	RecordingDelegate delegate;
	std::vector<std::function<void()>> posted;
	Impulse::LiveKitAdapter adapter(
		fake,
		delegate,
		[&posted](std::function<void()> task) {
			posted.push_back(std::move(task));
		});
	IMPULSE_CHECK(adapter.connect("wss://room", "token"));

	// The request carries a client async id, so its route key was registered
	// before the request left and the connect event was not lost.
	const std::uint64_t connectId = AsyncIdAt(fake->sent, 0);
	IMPULSE_CHECK(connectId & kClientAsyncIdFlag);
	RunPosted(posted);
	IMPULSE_CHECK(delegate.connected == 1);
	IMPULSE_CHECK(delegate.failed == 0);
}

IMPULSE_TEST(LiveKitRoomEventsUntakenVideoBufferIsReleased) {
	std::vector<std::uint64_t> released;
	Impulse::LiveKitEventRouter router(
		[&released](std::uint64_t handle) {
			released.push_back(handle);
		});

	// A frame of a stream that no route owns releases its buffer.
	Impulse::LiveKitEvent frame;
	frame.kind = Impulse::LiveKitEventKind::VideoFrame;
	frame.streamHandle = 77;
	Impulse::LiveKitVideoBuffer buffer;
	buffer.bufferHandle = 66;
	frame.videoBuffer = buffer;
	router.dispatch(frame);
	IMPULSE_CHECK(released.size() == 1 && released[0] == 66);

	// A route that takes the frame releases the buffer itself.
	int taken = 0;
	const Impulse::LiveKitEventRouter::RouteId route = router.addRoute(
		[&taken](const Impulse::LiveKitEvent &) {
			++taken;
		});
	router.addKey(route, 77);
	buffer.bufferHandle = 67;
	frame.videoBuffer = buffer;
	router.dispatch(frame);
	IMPULSE_CHECK(taken == 1);
	IMPULSE_CHECK(released.size() == 1);
}
