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
constexpr std::uint32_t kRequestNewPlatformAudio = 77;
constexpr std::uint32_t kRequestGetAudioDevices = 78;
constexpr std::uint32_t kRequestSetRecordingDevice = 79;
constexpr std::uint32_t kRequestSetPlayoutDevice = 80;
constexpr std::uint32_t kRequestReadyForRoomEvent = 83;

// FfiResponse oneof fields that the tests script.
constexpr std::uint32_t kResponseConnect = 3;
constexpr std::uint32_t kResponseNewPlatformAudio = 76;
constexpr std::uint32_t kResponseGetAudioDevices = 77;
constexpr std::uint32_t kResponseSetRecordingDevice = 78;
constexpr std::uint32_t kResponseSetPlayoutDevice = 79;
constexpr std::uint32_t kResponseReadyForRoomEvent = 82;

// The device ids: the stored one is in the case of the settings, the library
// lists the guid in upper case.
constexpr const char *kStoredPlayoutId =
	"{0.0.0.00000000}.{64b65051-f89c-4e68-ab68-1eb3b88dac2c}";
constexpr const char *kLibraryPlayoutId =
	"{0.0.0.00000000}.{64B65051-F89C-4E68-AB68-1EB3B88DAC2C}";
constexpr const char *kRecordingId =
	"{0.0.0.00000000}.{aa11bb22-cc33-4dd4-8ee5-ff6677889900}";

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

[[nodiscard]] std::optional<std::string> StringField(
		Span data,
		std::uint32_t field) {
	const std::optional<Span> bytes = MessageField(data, field);
	if (!bytes) {
		return std::nullopt;
	}
	return std::string(
		reinterpret_cast<const char *>(bytes->data()),
		bytes->size());
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

	void liveKitPublished(Impulse::LiveKitPublishKind) override {
		++published;
	}

	void liveKitFailed(const std::string &message) override {
		++failed;
		failures.push_back(message);
	}

	void liveKitDisconnected() override {
		++disconnected;
	}

	void liveKitLog(const std::string &line) override {
		logs.push_back(line);
	}

	int connected = 0;
	int published = 0;
	int failed = 0;
	int disconnected = 0;
	std::vector<std::string> failures;
	std::vector<std::string> logs;
};

void RunNow(std::function<void()> task) {
	task();
}

[[nodiscard]] bool LogContains(
		const RecordingDelegate &delegate,
		const std::string &line) {
	for (const std::string &entry : delegate.logs) {
		if (entry == line) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool LogMentions(
		const RecordingDelegate &delegate,
		const std::string &text) {
	for (const std::string &entry : delegate.logs) {
		if (entry.find(text) != std::string::npos) {
			return true;
		}
	}
	return false;
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
[[nodiscard]] Bytes PlatformAudioResponse(
		std::uint64_t handle,
		std::uint64_t recordingCount,
		std::uint64_t playoutCount) {
	Impulse::ProtoWriter handleMessage;
	handleMessage.writeVarintField(1, handle);
	Impulse::ProtoWriter info;
	info.writeVarintField(1, recordingCount);
	info.writeVarintField(2, playoutCount);
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

// One AudioDeviceInfo of a device list.
[[nodiscard]] Impulse::ProtoWriter DeviceInfo(
		std::uint32_t index,
		const std::string &name,
		const std::string &guid) {
	Impulse::ProtoWriter device;
	device.writeVarintField(1, index);
	device.writeStringField(2, name);
	device.writeStringField(3, guid);
	return device;
}

// The GetAudioDevicesResponse with the playout and the recording lists, and the
// error text when it is not empty.
[[nodiscard]] Bytes DeviceListResponse(
		const std::vector<Impulse::ProtoWriter> &playout,
		const std::vector<Impulse::ProtoWriter> &recording,
		const std::string &error) {
	Impulse::ProtoWriter list;
	for (const Impulse::ProtoWriter &device : playout) {
		list.writeMessageField(1, device);
	}
	for (const Impulse::ProtoWriter &device : recording) {
		list.writeMessageField(2, device);
	}
	if (!error.empty()) {
		list.writeStringField(3, error);
	}
	Impulse::ProtoWriter outer;
	outer.writeMessageField(kResponseGetAudioDevices, list);
	return outer.takeBytes();
}

// A SetRecordingDeviceResponse or a SetPlayoutDeviceResponse, with the error
// text when it is not empty.
[[nodiscard]] Bytes SetDeviceResponse(
		std::uint32_t responseField,
		const std::string &error) {
	Impulse::ProtoWriter message;
	if (!error.empty()) {
		message.writeStringField(1, error);
	}
	Impulse::ProtoWriter outer;
	outer.writeMessageField(responseField, message);
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

// Connects a room that has no stored device. The responses are the connect, the
// PlatformAudio and the ready response, and the connect callback is dispatched.
void ConnectWithoutDevices(FakeFfi &fake, Impulse::LiveKitAdapter &adapter) {
	fake.responses = {
		AsyncResponse(kResponseConnect, 0),
		PlatformAudioResponse(31, 2, 3),
		ReadyResponse(),
	};
	adapter.connect("wss://room", "token");
	fake.router.dispatch(ConnectEvent(AsyncIdAt(fake.sent, 0), 11, 12));
}

} // namespace

IMPULSE_TEST(LiveKitPlatformAudioDeviceRequestsCarryTheHandleAndId) {
	const Bytes get = Impulse::EncodeLiveKitGetAudioDevicesRequest(9);
	IMPULSE_CHECK(OuterField(get) == kRequestGetAudioDevices);
	const std::optional<Span> getBody = MessageField(get, kRequestGetAudioDevices);
	IMPULSE_CHECK(getBody.has_value());
	if (getBody) {
		IMPULSE_CHECK(VarintField(*getBody, 1) == 9);
	}

	const Bytes recording = Impulse::EncodeLiveKitSetRecordingDeviceRequest(
		9,
		kRecordingId);
	IMPULSE_CHECK(OuterField(recording) == kRequestSetRecordingDevice);
	const std::optional<Span> recordingBody = MessageField(
		recording,
		kRequestSetRecordingDevice);
	IMPULSE_CHECK(recordingBody.has_value());
	if (recordingBody) {
		IMPULSE_CHECK(VarintField(*recordingBody, 1) == 9);
		IMPULSE_CHECK(StringField(*recordingBody, 2) == kRecordingId);
	}

	const Bytes playout = Impulse::EncodeLiveKitSetPlayoutDeviceRequest(
		9,
		kLibraryPlayoutId);
	IMPULSE_CHECK(OuterField(playout) == kRequestSetPlayoutDevice);
	const std::optional<Span> playoutBody = MessageField(
		playout,
		kRequestSetPlayoutDevice);
	IMPULSE_CHECK(playoutBody.has_value());
	if (playoutBody) {
		IMPULSE_CHECK(VarintField(*playoutBody, 1) == 9);
		IMPULSE_CHECK(StringField(*playoutBody, 2) == kLibraryPlayoutId);
	}
}

IMPULSE_TEST(LiveKitAudioDeviceListDecodesBothLists) {
	const std::vector<Impulse::ProtoWriter> playout = {
		DeviceInfo(0, "Speakers", kLibraryPlayoutId),
	};
	const std::vector<Impulse::ProtoWriter> recording = {
		DeviceInfo(0, "Microphone", kRecordingId),
		DeviceInfo(1, "Default Microphone", ""),
	};
	const std::optional<Impulse::LiveKitAudioDeviceList> list =
		Impulse::DecodeLiveKitAudioDevices(
			DeviceListResponse(playout, recording, ""));
	IMPULSE_CHECK(list.has_value());
	if (!list) {
		return;
	}
	IMPULSE_CHECK(list->error.empty());
	IMPULSE_CHECK(list->playout.size() == 1);
	IMPULSE_CHECK(list->recording.size() == 2);
	if (list->playout.size() == 1) {
		IMPULSE_CHECK(list->playout[0].index == 0);
		IMPULSE_CHECK(list->playout[0].name == "Speakers");
		IMPULSE_CHECK(list->playout[0].guid == kLibraryPlayoutId);
	}
	if (list->recording.size() == 2) {
		IMPULSE_CHECK(list->recording[0].guid == kRecordingId);
		IMPULSE_CHECK(list->recording[1].index == 1);
		IMPULSE_CHECK(list->recording[1].name == "Default Microphone");
		IMPULSE_CHECK(list->recording[1].guid.empty());
	}
}

IMPULSE_TEST(LiveKitAudioDeviceListCarriesTheError) {
	const std::optional<Impulse::LiveKitAudioDeviceList> list =
		Impulse::DecodeLiveKitAudioDevices(DeviceListResponse({}, {}, "boom"));
	IMPULSE_CHECK(list.has_value());
	if (list) {
		IMPULSE_CHECK(list->error == "boom");
		IMPULSE_CHECK(list->playout.empty());
		IMPULSE_CHECK(list->recording.empty());
	}
}

IMPULSE_TEST(LiveKitAudioDeviceListNeedsItsMessage) {
	const Bytes empty;
	IMPULSE_CHECK(!Impulse::DecodeLiveKitAudioDevices(empty).has_value());
	IMPULSE_CHECK(!Impulse::DecodeLiveKitAudioDevices(
		SetDeviceResponse(kResponseSetRecordingDevice, "")).has_value());
}

IMPULSE_TEST(LiveKitSetDeviceResponseReadsTheError) {
	const std::optional<std::string> ok = Impulse::DecodeLiveKitSetDeviceResponse(
		SetDeviceResponse(kResponseSetRecordingDevice, ""),
		kResponseSetRecordingDevice);
	IMPULSE_CHECK(ok.has_value());
	if (ok) {
		IMPULSE_CHECK(ok->empty());
	}

	const std::optional<std::string> failed = Impulse::DecodeLiveKitSetDeviceResponse(
		SetDeviceResponse(kResponseSetPlayoutDevice, "boom"),
		kResponseSetPlayoutDevice);
	IMPULSE_CHECK(failed.has_value());
	if (failed) {
		IMPULSE_CHECK(*failed == "boom");
	}

	// The playout response carries no recording message.
	IMPULSE_CHECK(!Impulse::DecodeLiveKitSetDeviceResponse(
		SetDeviceResponse(kResponseSetPlayoutDevice, ""),
		kResponseSetRecordingDevice).has_value());
}

IMPULSE_TEST(LiveKitPlatformAudioInfoDecodesTheCounts) {
	const std::optional<Impulse::LiveKitPlatformAudioInfo> info =
		Impulse::DecodePlatformAudioInfo(PlatformAudioResponse(31, 2, 3));
	IMPULSE_CHECK(info.has_value());
	if (info) {
		IMPULSE_CHECK(info->recordingDeviceCount == 2);
		IMPULSE_CHECK(info->playoutDeviceCount == 3);
	}
	// A handle without the info gives no counts.
	IMPULSE_CHECK(!Impulse::DecodePlatformAudioInfo(
		HandleResponse(kResponseNewPlatformAudio, 31)).has_value());
}

IMPULSE_TEST(LiveKitPlaybackDeviceStoredBeforePlatformAudioIsAppliedAfterIt) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);

	// Before PlatformAudio exists the id is only stored: no request is sent.
	IMPULSE_CHECK(adapter.setPlayoutDevice(kStoredPlayoutId));
	IMPULSE_CHECK(fake->sent.empty());

	fake->responses = {
		AsyncResponse(kResponseConnect, 0),
		PlatformAudioResponse(31, 2, 3),
		DeviceListResponse({ DeviceInfo(0, "Speakers", kLibraryPlayoutId) }, {}, ""),
		SetDeviceResponse(kResponseSetPlayoutDevice, ""),
		ReadyResponse(),
	};
	IMPULSE_CHECK(adapter.connect("wss://room", "token"));
	fake->router.dispatch(ConnectEvent(AsyncIdAt(fake->sent, 0), 11, 12));
	IMPULSE_CHECK(delegate.connected == 1);
	IMPULSE_CHECK(delegate.failed == 0);

	// connect, PlatformAudio, the device list, the playout selection and ready.
	IMPULSE_CHECK(fake->sent.size() == 5);
	if (fake->sent.size() == 5) {
		IMPULSE_CHECK(OuterField(fake->sent[1]) == kRequestNewPlatformAudio);
		IMPULSE_CHECK(OuterField(fake->sent[2]) == kRequestGetAudioDevices);
		IMPULSE_CHECK(OuterField(fake->sent[3]) == kRequestSetPlayoutDevice);
		IMPULSE_CHECK(OuterField(fake->sent[4]) == kRequestReadyForRoomEvent);
		const std::optional<Span> selected = MessageField(
			fake->sent[3],
			kRequestSetPlayoutDevice);
		IMPULSE_CHECK(selected.has_value());
		if (selected) {
			// The device is selected with the guid that the library lists.
			IMPULSE_CHECK(VarintField(*selected, 1) == 31);
			IMPULSE_CHECK(StringField(*selected, 2) == kLibraryPlayoutId);
		}
	}
	IMPULSE_CHECK(LogContains(delegate, "PlatformAudio created, recording devices 2, playout devices 3"));
	IMPULSE_CHECK(LogContains(delegate, "playout device: Speakers"));
}

IMPULSE_TEST(LiveKitPlaybackDeviceNotListedKeepsTheDefault) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	IMPULSE_CHECK(adapter.setPlayoutDevice("{0.0.0.00000000}.{not-listed}"));

	fake->responses = {
		AsyncResponse(kResponseConnect, 0),
		PlatformAudioResponse(31, 2, 3),
		DeviceListResponse({ DeviceInfo(0, "Speakers", kLibraryPlayoutId) }, {}, ""),
		ReadyResponse(),
	};
	IMPULSE_CHECK(adapter.connect("wss://room", "token"));
	fake->router.dispatch(ConnectEvent(AsyncIdAt(fake->sent, 0), 11, 12));

	// No selection is sent, and the room connects.
	IMPULSE_CHECK(fake->sent.size() == 4);
	IMPULSE_CHECK(delegate.connected == 1);
	IMPULSE_CHECK(delegate.failed == 0);
	IMPULSE_CHECK(LogContains(
		delegate,
		"playout device not listed, the id format of the setting may not match the library guid"));
}

IMPULSE_TEST(LiveKitPlaybackDeviceErrorIsReportedWithoutFailingTheRoom) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectWithoutDevices(*fake, adapter);
	IMPULSE_CHECK(delegate.connected == 1);
	const std::size_t before = fake->sent.size();

	fake->responses = {
		DeviceListResponse({ DeviceInfo(0, "Speakers", kLibraryPlayoutId) }, {}, ""),
		SetDeviceResponse(kResponseSetPlayoutDevice, "boom"),
	};
	IMPULSE_CHECK(!adapter.setPlayoutDevice(kStoredPlayoutId));
	IMPULSE_CHECK(fake->sent.size() == before + 2);

	// The library error is not kept in the log, and the room stays connected.
	IMPULSE_CHECK(!LogMentions(delegate, "boom"));
	IMPULSE_CHECK(LogContains(delegate, "LiveKit playout device selection failed"));
	IMPULSE_CHECK(delegate.failed == 0);
	IMPULSE_CHECK(adapter.connected());
}

IMPULSE_TEST(LiveKitPlaybackDeviceSwitchesBackToDefault) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectWithoutDevices(*fake, adapter);
	const std::size_t before = fake->sent.size();

	// A specific device is selected first.
	fake->responses = {
		DeviceListResponse({ DeviceInfo(0, "Speakers", kLibraryPlayoutId) }, {}, ""),
		SetDeviceResponse(kResponseSetPlayoutDevice, ""),
	};
	IMPULSE_CHECK(adapter.setPlayoutDevice(kStoredPlayoutId));
	IMPULSE_CHECK(fake->sent.size() == before + 2);

	// Switching back to the default device selects the first listed device, so
	// the earlier device is not kept by the library.
	const std::size_t selectedAt = fake->sent.size();
	fake->responses = {
		DeviceListResponse({ DeviceInfo(0, "Speakers", kLibraryPlayoutId) }, {}, ""),
		SetDeviceResponse(kResponseSetPlayoutDevice, ""),
	};
	IMPULSE_CHECK(adapter.setPlayoutDevice(""));
	IMPULSE_CHECK(fake->sent.size() == selectedAt + 2);
	if (fake->sent.size() == selectedAt + 2) {
		IMPULSE_CHECK(OuterField(fake->sent[selectedAt]) == kRequestGetAudioDevices);
		IMPULSE_CHECK(OuterField(fake->sent[selectedAt + 1]) == kRequestSetPlayoutDevice);
		const std::optional<Span> selected = MessageField(
			fake->sent[selectedAt + 1],
			kRequestSetPlayoutDevice);
		IMPULSE_CHECK(selected.has_value());
		if (selected) {
			IMPULSE_CHECK(StringField(*selected, 2) == kLibraryPlayoutId);
		}
	}
	IMPULSE_CHECK(LogContains(delegate, "playout device: Speakers"));
	IMPULSE_CHECK(delegate.failed == 0);

	// After the switch back, a further empty id has no selection to undo, so it
	// sends nothing.
	const std::size_t afterSwitch = fake->sent.size();
	IMPULSE_CHECK(adapter.setPlayoutDevice(""));
	IMPULSE_CHECK(fake->sent.size() == afterSwitch);
}

IMPULSE_TEST(LiveKitRecordingDeviceAppliesAtOnceAfterPlatformAudio) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectWithoutDevices(*fake, adapter);
	const std::size_t before = fake->sent.size();

	fake->responses = {
		DeviceListResponse({}, { DeviceInfo(1, "Microphone", kRecordingId) }, ""),
		SetDeviceResponse(kResponseSetRecordingDevice, ""),
	};
	IMPULSE_CHECK(adapter.setRecordingDevice("{0.0.0.00000000}.{AA11BB22-CC33-4DD4-8EE5-FF6677889900}"));
	IMPULSE_CHECK(fake->sent.size() == before + 2);
	if (fake->sent.size() == before + 2) {
		IMPULSE_CHECK(OuterField(fake->sent[before]) == kRequestGetAudioDevices);
		IMPULSE_CHECK(OuterField(fake->sent[before + 1]) == kRequestSetRecordingDevice);
		const std::optional<Span> selected = MessageField(
			fake->sent[before + 1],
			kRequestSetRecordingDevice);
		IMPULSE_CHECK(selected.has_value());
		if (selected) {
			IMPULSE_CHECK(StringField(*selected, 2) == kRecordingId);
		}
	}
	IMPULSE_CHECK(LogContains(delegate, "recording device: Microphone"));
}

IMPULSE_TEST(LiveKitEmptyDeviceIdSendsNothingAfterPlatformAudio) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectWithoutDevices(*fake, adapter);
	const std::size_t before = fake->sent.size();

	IMPULSE_CHECK(adapter.setPlayoutDevice(""));
	IMPULSE_CHECK(adapter.setRecordingDevice(""));
	IMPULSE_CHECK(fake->sent.size() == before);
	IMPULSE_CHECK(LogContains(delegate, "playout device: default"));
	IMPULSE_CHECK(LogContains(delegate, "recording device: default"));
	IMPULSE_CHECK(delegate.failed == 0);
}

IMPULSE_TEST(LiveKitDeviceListErrorReturnsFalseWithoutFailingTheRoom) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectWithoutDevices(*fake, adapter);
	const std::size_t before = fake->sent.size();

	fake->responses = { DeviceListResponse({}, {}, "bad") };
	IMPULSE_CHECK(!adapter.setRecordingDevice(kRecordingId));
	IMPULSE_CHECK(fake->sent.size() == before + 1);
	IMPULSE_CHECK(delegate.failed == 0);
	IMPULSE_CHECK(adapter.connected());
}
