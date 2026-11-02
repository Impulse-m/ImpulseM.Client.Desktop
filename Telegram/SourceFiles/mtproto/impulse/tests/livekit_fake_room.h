/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "calls/livekit/livekit_adapter.h"
#include "calls/livekit/livekit_adapter_interface.h"
#include "calls/livekit/livekit_event_router.h"
#include "calls/livekit/livekit_ffi_codec.h"
#include "calls/livekit/livekit_ffi_loader.h"
#include "mtproto/impulse/core/wire.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The fake library and the room helpers that the LiveKit adapter tests share.
// The library answers the requests with scripted responses, and the room events
// are dispatched by the tests through the router of the fake.
namespace ImpulseLiveKitTest {

using Bytes = std::vector<std::uint8_t>;
using Span = std::span<const std::uint8_t>;

// FfiRequest oneof fields that the tests read or send.
inline constexpr std::uint32_t kRequestConnect = 3;
inline constexpr std::uint32_t kRequestDisconnect = 4;
inline constexpr std::uint32_t kRequestPublishTrack = 5;
inline constexpr std::uint32_t kRequestUnpublishTrack = 6;
inline constexpr std::uint32_t kRequestCreateVideoTrack = 15;
inline constexpr std::uint32_t kRequestNewVideoSource = 21;

// FfiResponse oneof fields that the tests script.
inline constexpr std::uint32_t kResponseConnect = 3;
inline constexpr std::uint32_t kResponseDisconnect = 4;
inline constexpr std::uint32_t kResponsePublishTrack = 5;
inline constexpr std::uint32_t kResponseUnpublishTrack = 6;
inline constexpr std::uint32_t kResponseCreateAudioTrack = 16;
inline constexpr std::uint32_t kResponseCreateVideoTrack = 15;
inline constexpr std::uint32_t kResponseLocalTrackMute = 17;
inline constexpr std::uint32_t kResponseNewAudioSource = 26;
inline constexpr std::uint32_t kResponseNewVideoSource = 21;
inline constexpr std::uint32_t kResponseNewPlatformAudio = 76;
inline constexpr std::uint32_t kResponseReadyForRoomEvent = 82;

// The first field number of an encoded request or response.
[[nodiscard]] inline std::uint32_t OuterField(const Bytes &data) {
	Impulse::ProtoReader reader(data);
	return reader.next() ? reader.field() : 0;
}

// The payload of the first length-delimited field with the number.
[[nodiscard]] inline std::optional<Span> MessageField(
		Span data,
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

[[nodiscard]] inline std::optional<std::uint64_t> VarintField(
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

[[nodiscard]] inline std::string StringField(Span data, std::uint32_t field) {
	Impulse::ProtoReader reader(data);
	while (reader.next()) {
		if (reader.field() == field
			&& reader.wireType() == Impulse::ProtoWireType::LengthDelimited) {
			const Span bytes = reader.readBytes();
			return std::string(
				reinterpret_cast<const char *>(bytes.data()),
				bytes.size());
		}
		reader.skip();
	}
	return std::string();
}

// The field of the async id inside the request message, or 0 when the request
// carries no async id that the fake library answers.
[[nodiscard]] inline std::uint32_t AsyncFieldOf(std::uint32_t requestField) {
	if (requestField == kRequestConnect) {
		return 4;
	} else if (requestField == kRequestDisconnect) {
		return 2;
	} else if (requestField == kRequestPublishTrack
		|| requestField == kRequestUnpublishTrack) {
		return 4;
	}
	return 0;
}

// The async id of a request that carries one, or nullopt.
[[nodiscard]] inline std::optional<std::uint64_t> RequestAsyncId(Span data) {
	Impulse::ProtoReader outer(data);
	while (outer.next()) {
		const std::uint32_t asyncField = AsyncFieldOf(outer.field());
		if (!asyncField) {
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
[[nodiscard]] inline std::uint64_t AsyncIdAt(
		const std::vector<Bytes> &sent,
		std::size_t index) {
	if (index >= sent.size()) {
		return 0;
	}
	return RequestAsyncId(sent[index]).value_or(0);
}

[[nodiscard]] inline std::size_t CountRequests(
		const std::vector<Bytes> &sent,
		std::uint32_t field) {
	std::size_t count = 0;
	for (const Bytes &request : sent) {
		if (OuterField(request) == field) {
			++count;
		}
	}
	return count;
}

[[nodiscard]] inline std::size_t DroppedCount(
		const std::vector<std::uint64_t> &dropped,
		std::uint64_t handle) {
	std::size_t count = 0;
	for (const std::uint64_t value : dropped) {
		if (value == handle) {
			++count;
		}
	}
	return count;
}

// Stands in for the library. It records every request, answers the scripted
// responses in order, and keeps its own event router as the library does. A
// request that carries an async id is answered with that id, as the library does.
// A request with no scripted response gets no answer, as a rejected request does.
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

// Counts the callbacks of the adapter, by kind where the kind matters.
class RecordingDelegate final : public Impulse::LiveKitAdapterDelegate {
public:
	void liveKitConnected() override {
		++connected;
	}

	void liveKitPublished(Impulse::LiveKitPublishKind kind) override {
		if (kind == Impulse::LiveKitPublishKind::Camera) {
			++cameraPublished;
		} else if (kind == Impulse::LiveKitPublishKind::ScreenShare) {
			++screenPublished;
		}
	}

	void liveKitUnpublished(Impulse::LiveKitPublishKind) override {
		++unpublished;
	}

	void liveKitFailed(const std::string &) override {
		++failed;
	}

	void liveKitDisconnected() override {
		++disconnected;
	}

	int connected = 0;
	int cameraPublished = 0;
	int screenPublished = 0;
	int unpublished = 0;
	int failed = 0;
	int disconnected = 0;
};

inline void RunNow(std::function<void()> task) {
	task();
}

[[nodiscard]] inline Bytes AsyncResponse(
		std::uint32_t responseField,
		std::uint64_t asyncId) {
	Impulse::ProtoWriter inner;
	inner.writeVarintField(1, asyncId);
	Impulse::ProtoWriter outer;
	outer.writeMessageField(responseField, inner);
	return outer.takeBytes();
}

// A response that creates one owned handle, as the new source and the new track
// and the platform audio do.
[[nodiscard]] inline Bytes HandleResponse(
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

[[nodiscard]] inline Bytes MuteResponse(bool muted) {
	Impulse::ProtoWriter inner;
	inner.writeVarintField(1, muted ? 1 : 0);
	Impulse::ProtoWriter outer;
	outer.writeMessageField(kResponseLocalTrackMute, inner);
	return outer.takeBytes();
}

[[nodiscard]] inline Bytes ReadyResponse() {
	const Impulse::ProtoWriter empty;
	Impulse::ProtoWriter outer;
	outer.writeMessageField(kResponseReadyForRoomEvent, empty);
	return outer.takeBytes();
}

[[nodiscard]] inline Impulse::LiveKitEvent ConnectEvent(
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

[[nodiscard]] inline Impulse::LiveKitEvent PublishEvent(
		std::uint64_t asyncId,
		std::uint64_t publicationHandle,
		const std::string &sid) {
	Impulse::LiveKitEvent event;
	event.kind = Impulse::LiveKitEventKind::PublishTrack;
	event.asyncId = asyncId;
	event.publicationHandle = publicationHandle;
	event.trackSid = sid;
	return event;
}

[[nodiscard]] inline Impulse::LiveKitEvent UnpublishEvent(
		std::uint64_t asyncId,
		const std::string &error) {
	Impulse::LiveKitEvent event;
	event.kind = Impulse::LiveKitEventKind::UnpublishTrack;
	event.asyncId = asyncId;
	event.error = error;
	return event;
}

[[nodiscard]] inline Impulse::LiveKitEvent DisconnectEvent(std::uint64_t asyncId) {
	Impulse::LiveKitEvent event;
	event.kind = Impulse::LiveKitEventKind::Disconnect;
	event.asyncId = asyncId;
	return event;
}

// Connects a room of the fake library, and the room events are then ready. The
// room has the handle 11 and the local participant the handle 12.
inline void ConnectRoom(FakeFfi &fake, Impulse::LiveKitAdapter &adapter) {
	fake.responses.push_back(AsyncResponse(kResponseConnect, 0));
	fake.responses.push_back(HandleResponse(kResponseNewPlatformAudio, 31));
	fake.responses.push_back(ReadyResponse());
	adapter.connect("wss://room", "token");
	fake.router.dispatch(ConnectEvent(AsyncIdAt(fake.sent, 0), 11, 12));
}

} // namespace ImpulseLiveKitTest
