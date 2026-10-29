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
#include <vector>

namespace {

constexpr std::uint32_t kFfiRequestDisconnect = 4;
constexpr std::uint32_t kFfiResponseConnect = 3;
constexpr std::uint32_t kFfiResponseDisconnect = 4;
constexpr std::uint32_t kFfiResponseNewPlatformAudio = 76;
constexpr std::uint32_t kFfiResponseReadyForRoomEvent = 82;
constexpr std::uint32_t kAsyncIdField = 1;
constexpr std::uint32_t kDisconnectRoomHandleField = 1;
constexpr std::uint32_t kOwnedMessageField = 1;
constexpr std::uint32_t kOwnedHandleField = 1;
constexpr std::uint32_t kHandleIdField = 1;

// Stands in for the library. It records every request, answers the scripted
// responses in order, and keeps its own event router as the library does.
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

[[nodiscard]] std::vector<std::uint8_t> AsyncResponse(
		std::uint32_t responseField,
		std::uint64_t asyncId) {
	Impulse::ProtoWriter inner;
	inner.writeVarintField(kAsyncIdField, asyncId);
	Impulse::ProtoWriter outer;
	outer.writeMessageField(responseField, inner);
	return outer.takeBytes();
}

// The response of the PlatformAudio request that connect sends after the
// connect callback, with the given handle.
[[nodiscard]] std::vector<std::uint8_t> PlatformAudioResponse(
		std::uint64_t handle) {
	Impulse::ProtoWriter idMessage;
	idMessage.writeVarintField(kHandleIdField, handle);
	Impulse::ProtoWriter owned;
	owned.writeMessageField(kOwnedHandleField, idMessage);
	Impulse::ProtoWriter message;
	message.writeMessageField(kOwnedMessageField, owned);
	Impulse::ProtoWriter outer;
	outer.writeMessageField(kFfiResponseNewPlatformAudio, message);
	return outer.takeBytes();
}

// The response of the ReadyForRoomEvent request that connect sends after the
// PlatformAudio request. Its content is not read.
[[nodiscard]] std::vector<std::uint8_t> ReadyResponse() {
	const Impulse::ProtoWriter empty;
	Impulse::ProtoWriter outer;
	outer.writeMessageField(kFfiResponseReadyForRoomEvent, empty);
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

[[nodiscard]] Impulse::LiveKitEvent DisconnectEvent(std::uint64_t asyncId) {
	Impulse::LiveKitEvent event;
	event.kind = Impulse::LiveKitEventKind::Disconnect;
	event.asyncId = asyncId;
	return event;
}

[[nodiscard]] std::optional<std::uint64_t> DisconnectRoomHandle(
		const std::vector<std::uint8_t> &data) {
	Impulse::ProtoReader outer(data);
	while (outer.next()) {
		if (outer.field() != kFfiRequestDisconnect) {
			outer.skip();
			continue;
		}
		const std::span<const std::uint8_t> payload = outer.readBytes();
		Impulse::ProtoReader inner(payload);
		while (inner.next()) {
			if (inner.field() == kDisconnectRoomHandleField) {
				return inner.readVarint();
			}
			inner.skip();
		}
	}
	return std::nullopt;
}

} // namespace

IMPULSE_TEST(LiveKitEventRouterDropsEventsOfClosedRoute) {
	Impulse::LiveKitEventRouter router;
	int firstCount = 0;
	int secondCount = 0;
	const Impulse::LiveKitEventRouter::RouteId first = router.addRoute(
		[&firstCount](const Impulse::LiveKitEvent &) {
			++firstCount;
		});
	const Impulse::LiveKitEventRouter::RouteId second = router.addRoute(
		[&secondCount](const Impulse::LiveKitEvent &) {
			++secondCount;
		});
	router.addKey(first, 7);
	router.addKey(second, 8);
	router.addKey(second, 9);

	router.dispatch(DisconnectEvent(7));
	IMPULSE_CHECK(firstCount == 1);
	IMPULSE_CHECK(secondCount == 0);

	router.dispatch(DisconnectEvent(8));
	IMPULSE_CHECK(firstCount == 1);
	IMPULSE_CHECK(secondCount == 1);

	router.dispatch(DisconnectEvent(42));
	IMPULSE_CHECK(firstCount == 1);
	IMPULSE_CHECK(secondCount == 1);

	router.removeRoute(first);
	router.dispatch(DisconnectEvent(7));
	IMPULSE_CHECK(firstCount == 1);

	router.clearKeys(second);
	router.dispatch(DisconnectEvent(8));
	IMPULSE_CHECK(secondCount == 1);
}

IMPULSE_TEST(LiveKitEventRouterRoutesHandleKeys) {
	Impulse::LiveKitEventRouter router;
	int count = 0;
	const Impulse::LiveKitEventRouter::RouteId route = router.addRoute(
		[&count](const Impulse::LiveKitEvent &) {
			++count;
		});
	router.addKey(route, 21);

	Impulse::LiveKitEvent event;
	event.kind = Impulse::LiveKitEventKind::PublishTrack;
	event.asyncId = 3;
	event.publicationHandle = 21;
	router.dispatch(event);
	IMPULSE_CHECK(count == 1);
}

IMPULSE_TEST(LiveKitTwoAdaptersReceiveOnlyTheirOwnEvents) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	// Requests in order: connect A, connect B, then after each connect callback
	// PlatformAudio and ReadyForRoomEvent, and finally the disconnect of A.
	fake->responses = {
		AsyncResponse(kFfiResponseConnect, 101),
		AsyncResponse(kFfiResponseConnect, 202),
		PlatformAudioResponse(31),
		ReadyResponse(),
		PlatformAudioResponse(32),
		ReadyResponse(),
		AsyncResponse(kFfiResponseDisconnect, 303),
	};
	RecordingDelegate delegateA;
	RecordingDelegate delegateB;
	Impulse::LiveKitAdapter adapterA(fake, delegateA, RunNow);
	Impulse::LiveKitAdapter adapterB(fake, delegateB, RunNow);

	IMPULSE_CHECK(adapterA.connect("wss://room-a", "token"));
	IMPULSE_CHECK(adapterB.connect("wss://room-b", "token"));

	fake->router.dispatch(ConnectEvent(202, 22, 23));
	IMPULSE_CHECK(delegateA.connected == 0);
	IMPULSE_CHECK(delegateB.connected == 1);

	fake->router.dispatch(ConnectEvent(101, 11, 12));
	IMPULSE_CHECK(delegateA.connected == 1);
	IMPULSE_CHECK(delegateB.connected == 1);

	// Room A is the first room that was connected, so its disconnect request
	// carries its own handle and only its own event answers it.
	adapterA.disconnect();
	IMPULSE_CHECK(fake->sent.size() == 7);
	if (fake->sent.size() == 7) {
		IMPULSE_CHECK(DisconnectRoomHandle(fake->sent[6]) == 11);
	}
	fake->router.dispatch(DisconnectEvent(303));
	IMPULSE_CHECK(delegateA.disconnected == 1);
	IMPULSE_CHECK(delegateB.disconnected == 0);
	IMPULSE_CHECK(delegateB.failed == 0);
}

IMPULSE_TEST(LiveKitDestroyedWhileConnectingDisconnectsRoom) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	fake->responses = {
		AsyncResponse(kFfiResponseConnect, 101),
		AsyncResponse(kFfiResponseDisconnect, 303),
	};
	RecordingDelegate delegate;
	std::unique_ptr<Impulse::LiveKitAdapter> adapter =
		std::make_unique<Impulse::LiveKitAdapter>(fake, delegate, RunNow);
	IMPULSE_CHECK(adapter->connect("wss://room", "token"));

	// The room is not known to the adapter yet, so nothing is disconnected.
	adapter.reset();
	IMPULSE_CHECK(fake->sent.size() == 1);

	// The library reports the room; the adapter disconnects it explicitly.
	fake->router.dispatch(ConnectEvent(101, 11, 12));
	IMPULSE_CHECK(fake->sent.size() == 2);
	if (fake->sent.size() == 2) {
		IMPULSE_CHECK(DisconnectRoomHandle(fake->sent[1]) == 11);
	}
	IMPULSE_CHECK(delegate.connected == 0);

	// The library confirms the disconnect, and the room handles are freed.
	fake->router.dispatch(DisconnectEvent(303));
	IMPULSE_CHECK(delegate.disconnected == 0);
	IMPULSE_CHECK(fake->dropped.size() == 2);

	// The room is closed, so a late event of it reaches nobody.
	fake->router.dispatch(DisconnectEvent(303));
	IMPULSE_CHECK(fake->sent.size() == 2);
}

IMPULSE_TEST(LiveKitDestroyedWhileConnectedDisconnectsRoom) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	fake->responses = {
		AsyncResponse(kFfiResponseConnect, 101),
		PlatformAudioResponse(31),
		ReadyResponse(),
		AsyncResponse(kFfiResponseDisconnect, 303),
	};
	RecordingDelegate delegate;
	std::unique_ptr<Impulse::LiveKitAdapter> adapter =
		std::make_unique<Impulse::LiveKitAdapter>(fake, delegate, RunNow);
	IMPULSE_CHECK(adapter->connect("wss://room", "token"));
	fake->router.dispatch(ConnectEvent(101, 11, 12));
	IMPULSE_CHECK(delegate.connected == 1);

	adapter.reset();
	IMPULSE_CHECK(fake->sent.size() == 4);
	if (fake->sent.size() == 4) {
		IMPULSE_CHECK(DisconnectRoomHandle(fake->sent[3]) == 11);
	}
	fake->router.dispatch(DisconnectEvent(303));
	IMPULSE_CHECK(delegate.disconnected == 0);
	// The platform audio, the local participant and the room handles.
	IMPULSE_CHECK(fake->dropped.size() == 3);
}
