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
constexpr std::uint32_t kRequestGetSessionStats = 12;

// FfiResponse oneof fields that the tests script.
constexpr std::uint32_t kResponseConnect = 3;
constexpr std::uint32_t kResponseGetSessionStats = 12;
constexpr std::uint32_t kResponseNewPlatformAudio = 76;
constexpr std::uint32_t kResponseReadyForRoomEvent = 82;

// FfiEvent fields of the library log and of the session stats callback.
constexpr std::uint32_t kEventLogs = 18;
constexpr std::uint32_t kEventSessionStats = 19;

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

	void liveKitPublished(Impulse::LiveKitPublishKind) override {
	}

	void liveKitFailed(const std::string &) override {
		++failed;
	}

	void liveKitDisconnected() override {
		++disconnected;
	}

	void liveKitSessionStats(const Impulse::LiveKitSessionStats &stats) override {
		statsList.push_back(stats);
	}

	int connected = 0;
	int failed = 0;
	int disconnected = 0;
	std::vector<Impulse::LiveKitSessionStats> statsList;
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

// The NewPlatformAudioResponse with the handle.
[[nodiscard]] Bytes PlatformAudioResponse(std::uint64_t handle) {
	Impulse::ProtoWriter handleMessage;
	handleMessage.writeVarintField(1, handle);
	Impulse::ProtoWriter owned;
	owned.writeMessageField(1, handleMessage);
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

// Connects a room of the fake library. The responses are appended, so a test
// that scripts more responses appends them after these.
void ConnectRoom(FakeFfi &fake, Impulse::LiveKitAdapter &adapter) {
	fake.responses.push_back(AsyncResponse(kResponseConnect, 0));
	fake.responses.push_back(PlatformAudioResponse(31));
	fake.responses.push_back(ReadyResponse());
	adapter.connect("wss://room", "token");
	fake.router.dispatch(ConnectEvent(AsyncIdAt(fake.sent, 0), 11, 12));
}

// The RtcStats of one RTP stream direction and the stats of the session.
[[nodiscard]] Impulse::ProtoWriter RtcIdOf(std::uint64_t timestamp) {
	Impulse::ProtoWriter rtc;
	rtc.writeStringField(1, "rtc-id");
	rtc.writeVarintField(2, timestamp);
	return rtc;
}

[[nodiscard]] Impulse::ProtoWriter StreamOf(const std::string &kind) {
	Impulse::ProtoWriter stream;
	stream.writeVarintField(1, 5);
	stream.writeStringField(2, kind);
	stream.writeStringField(3, "transport");
	stream.writeStringField(4, "codec");
	return stream;
}

// The outbound RTP member of an RtcStats, with its sent counters.
[[nodiscard]] Impulse::ProtoWriter OutboundRtp(
		const std::string &kind,
		std::uint64_t packetsSent,
		std::uint64_t bytesSent) {
	Impulse::ProtoWriter sent;
	sent.writeVarintField(1, packetsSent);
	sent.writeVarintField(2, bytesSent);
	Impulse::ProtoWriter outbound;
	outbound.writeMessageField(1, RtcIdOf(1000));
	outbound.writeMessageField(2, StreamOf(kind));
	outbound.writeMessageField(3, sent);
	Impulse::ProtoWriter rtc;
	rtc.writeMessageField(5, outbound);
	return rtc;
}

// The inbound RTP member of an audio stream.
[[nodiscard]] Impulse::ProtoWriter InboundAudio(
		std::uint64_t packetsReceived,
		std::uint64_t packetsLost,
		std::uint64_t bytesReceived,
		std::uint64_t samplesReceived,
		double audioLevel) {
	Impulse::ProtoWriter received;
	received.writeVarintField(1, packetsReceived);
	received.writeVarintField(2, packetsLost);
	Impulse::ProtoWriter inboundStats;
	inboundStats.writeVarintField(25, bytesReceived);
	inboundStats.writeVarintField(35, samplesReceived);
	inboundStats.writeDoubleField(41, audioLevel);
	Impulse::ProtoWriter inbound;
	inbound.writeMessageField(1, RtcIdOf(2000));
	inbound.writeMessageField(2, StreamOf("audio"));
	inbound.writeMessageField(3, received);
	inbound.writeMessageField(4, inboundStats);
	Impulse::ProtoWriter rtc;
	rtc.writeMessageField(4, inbound);
	return rtc;
}

// The inbound RTP member of a video stream, with its decoded frame count.
[[nodiscard]] Impulse::ProtoWriter InboundVideo(
		std::uint64_t packetsReceived,
		std::uint64_t bytesReceived,
		std::uint64_t framesDecoded) {
	Impulse::ProtoWriter received;
	received.writeVarintField(1, packetsReceived);
	received.writeVarintField(2, 0);
	Impulse::ProtoWriter inboundStats;
	inboundStats.writeVarintField(4, framesDecoded);
	inboundStats.writeVarintField(25, bytesReceived);
	Impulse::ProtoWriter inbound;
	inbound.writeMessageField(1, RtcIdOf(3000));
	inbound.writeMessageField(2, StreamOf("video"));
	inbound.writeMessageField(3, received);
	inbound.writeMessageField(4, inboundStats);
	Impulse::ProtoWriter rtc;
	rtc.writeMessageField(4, inbound);
	return rtc;
}

// A member of RtcStats that the adapter does not read. It must be skipped.
[[nodiscard]] Impulse::ProtoWriter UnreadMember() {
	Impulse::ProtoWriter codec;
	codec.writeStringField(1, "codec");
	Impulse::ProtoWriter rtc;
	rtc.writeMessageField(3, codec);
	return rtc;
}

// The GetSessionStatsCallback with the publisher and the subscriber stats.
[[nodiscard]] Bytes SessionStatsEvent(
		std::uint64_t asyncId,
		const std::vector<Impulse::ProtoWriter> &publisher,
		const std::vector<Impulse::ProtoWriter> &subscriber) {
	Impulse::ProtoWriter result;
	for (const Impulse::ProtoWriter &rtc : publisher) {
		result.writeMessageField(1, rtc);
	}
	for (const Impulse::ProtoWriter &rtc : subscriber) {
		result.writeMessageField(2, rtc);
	}
	Impulse::ProtoWriter callback;
	callback.writeVarintField(1, asyncId);
	callback.writeMessageField(3, result);
	Impulse::ProtoWriter event;
	event.writeMessageField(kEventSessionStats, callback);
	return event.takeBytes();
}

// A GetSessionStatsCallback that carries the error text of the library.
[[nodiscard]] Bytes SessionStatsErrorEvent(
		std::uint64_t asyncId,
		const std::string &error) {
	Impulse::ProtoWriter callback;
	callback.writeVarintField(1, asyncId);
	callback.writeStringField(2, error);
	Impulse::ProtoWriter event;
	event.writeMessageField(kEventSessionStats, callback);
	return event.takeBytes();
}

// One LogRecord of a LogBatch: the level, the target and the message.
[[nodiscard]] Impulse::ProtoWriter LogRecordOf(
		std::uint64_t level,
		const std::string &target,
		const std::string &message) {
	Impulse::ProtoWriter record;
	record.writeVarintField(1, level);
	record.writeStringField(2, target);
	record.writeStringField(3, "module::path");
	record.writeStringField(6, message);
	return record;
}

// The FfiEvent of a LogBatch with the records.
[[nodiscard]] Bytes LogBatchEvent(const std::vector<Impulse::ProtoWriter> &records) {
	Impulse::ProtoWriter batch;
	for (const Impulse::ProtoWriter &record : records) {
		batch.writeMessageField(1, record);
	}
	Impulse::ProtoWriter event;
	event.writeMessageField(kEventLogs, batch);
	return event.takeBytes();
}

[[nodiscard]] Impulse::LiveKitLogRecord Record(
		Impulse::LiveKitLogLevel level,
		const std::string &target,
		const std::string &message) {
	Impulse::LiveKitLogRecord record;
	record.level = level;
	record.target = target;
	record.message = message;
	return record;
}

[[nodiscard]] bool LineIs(
		const std::optional<std::string> &line,
		const std::string &expected) {
	return line && *line == expected;
}

} // namespace

IMPULSE_TEST(LiveKitSessionStatsRequestCarriesRoomAndAsyncId) {
	const Bytes request = Impulse::EncodeLiveKitGetSessionStatsRequest(7, 99);
	IMPULSE_CHECK(OuterField(request) == kRequestGetSessionStats);
	const std::optional<Span> body = MessageField(request, kRequestGetSessionStats);
	IMPULSE_CHECK(body.has_value());
	if (body) {
		IMPULSE_CHECK(VarintField(*body, 1) == 7);
		IMPULSE_CHECK(VarintField(*body, 2) == 99);
	}
}

IMPULSE_TEST(LiveKitSessionStatsCallbackDecodesRtpStats) {
	const Bytes event = SessionStatsEvent(
		99,
		{ OutboundRtp("video", 10, 2000) },
		{
			InboundAudio(50, 3, 9000, 480000, 0.25),
			InboundVideo(20, 5000, 77),
			UnreadMember(),
		});
	const Impulse::LiveKitEvent decoded = Impulse::DecodeLiveKitEvent(event);
	IMPULSE_CHECK(decoded.kind == Impulse::LiveKitEventKind::SessionStats);
	IMPULSE_CHECK(decoded.asyncId == 99);
	IMPULSE_CHECK(decoded.sessionStats.has_value());
	if (!decoded.sessionStats) {
		return;
	}
	const Impulse::LiveKitSessionStats &stats = *decoded.sessionStats;
	IMPULSE_CHECK(!stats.error);

	IMPULSE_CHECK(stats.outbound.size() == 1);
	if (stats.outbound.size() == 1) {
		IMPULSE_CHECK(stats.outbound[0].kind == Impulse::LiveKitTrackKind::Video);
		IMPULSE_CHECK(stats.outbound[0].packetsSent == 10);
		IMPULSE_CHECK(stats.outbound[0].bytesSent == 2000);
	}

	IMPULSE_CHECK(stats.inbound.size() == 2);
	if (stats.inbound.size() == 2) {
		const Impulse::LiveKitInboundRtpStats &audio = stats.inbound[0];
		IMPULSE_CHECK(audio.kind == Impulse::LiveKitTrackKind::Audio);
		IMPULSE_CHECK(audio.packetsReceived == 50);
		IMPULSE_CHECK(audio.packetsLost == 3);
		IMPULSE_CHECK(audio.bytesReceived == 9000);
		IMPULSE_CHECK(audio.totalSamplesReceived == 480000);
		IMPULSE_CHECK(audio.audioLevel == 0.25);
		IMPULSE_CHECK(!audio.framesDecoded.has_value());

		const Impulse::LiveKitInboundRtpStats &video = stats.inbound[1];
		IMPULSE_CHECK(video.kind == Impulse::LiveKitTrackKind::Video);
		IMPULSE_CHECK(video.packetsReceived == 20);
		IMPULSE_CHECK(video.bytesReceived == 5000);
		IMPULSE_CHECK(video.framesDecoded.has_value());
		if (video.framesDecoded) {
			IMPULSE_CHECK(*video.framesDecoded == 77);
		}
		IMPULSE_CHECK(video.totalSamplesReceived == 0);
	}
}

IMPULSE_TEST(LiveKitSessionStatsErrorIsFlaggedWithoutText) {
	const Impulse::LiveKitEvent failed = Impulse::DecodeLiveKitEvent(
		SessionStatsErrorEvent(99, "wss://host/rtc?access_token=secret"));
	IMPULSE_CHECK(failed.kind == Impulse::LiveKitEventKind::SessionStats);
	IMPULSE_CHECK(failed.asyncId == 99);
	IMPULSE_CHECK(failed.sessionStats.has_value());
	if (failed.sessionStats) {
		IMPULSE_CHECK(failed.sessionStats->error);
		IMPULSE_CHECK(failed.sessionStats->outbound.empty());
		IMPULSE_CHECK(failed.sessionStats->inbound.empty());
	}

	// A callback with neither a result nor an error is an error too.
	Impulse::ProtoWriter callback;
	callback.writeVarintField(1, 100);
	Impulse::ProtoWriter event;
	event.writeMessageField(kEventSessionStats, callback);
	const Impulse::LiveKitEvent empty = Impulse::DecodeLiveKitEvent(event.takeBytes());
	IMPULSE_CHECK(empty.sessionStats.has_value());
	if (empty.sessionStats) {
		IMPULSE_CHECK(empty.sessionStats->error);
	}
}

IMPULSE_TEST(LiveKitSessionStatsRoutesByAsyncIdAndIsDelivered) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);
	IMPULSE_CHECK(delegate.connected == 1);

	fake->responses.push_back(AsyncResponse(kResponseGetSessionStats, 0));
	IMPULSE_CHECK(adapter.requestSessionStats());
	const std::uint64_t asyncId = AsyncIdAt(fake->sent, fake->sent.size() - 1);
	IMPULSE_CHECK(asyncId != 0);

	fake->router.dispatch(Impulse::DecodeLiveKitEvent(SessionStatsEvent(
		asyncId,
		{ OutboundRtp("audio", 3, 300) },
		{})));
	IMPULSE_CHECK(delegate.statsList.size() == 1);
	if (delegate.statsList.size() == 1) {
		IMPULSE_CHECK(!delegate.statsList[0].error);
		IMPULSE_CHECK(delegate.statsList[0].outbound.size() == 1);
	}

	// The answer is taken once: a repeated callback of the id is ignored.
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(SessionStatsEvent(
		asyncId,
		{},
		{})));
	IMPULSE_CHECK(delegate.statsList.size() == 1);

	// An error callback is delivered as stats with the flag, and the room stays.
	fake->responses.push_back(AsyncResponse(kResponseGetSessionStats, 0));
	IMPULSE_CHECK(adapter.requestSessionStats());
	const std::uint64_t errorId = AsyncIdAt(fake->sent, fake->sent.size() - 1);
	fake->router.dispatch(Impulse::DecodeLiveKitEvent(
		SessionStatsErrorEvent(errorId, "boom")));
	IMPULSE_CHECK(delegate.statsList.size() == 2);
	if (delegate.statsList.size() == 2) {
		IMPULSE_CHECK(delegate.statsList[1].error);
	}
	IMPULSE_CHECK(delegate.failed == 0);
	IMPULSE_CHECK(adapter.connected());
}

IMPULSE_TEST(LiveKitRequestSessionStatsNeedsAConnectedRoom) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	IMPULSE_CHECK(!adapter.requestSessionStats());
	IMPULSE_CHECK(fake->sent.empty());
}

IMPULSE_TEST(LiveKitLogBatchDecodesRecords) {
	const Impulse::LiveKitEvent decoded = Impulse::DecodeLiveKitEvent(LogBatchEvent({
		LogRecordOf(1, "livekit_room", "joined"),
		LogRecordOf(2, "libwebrtc", "stun"),
	}));
	IMPULSE_CHECK(decoded.kind == Impulse::LiveKitEventKind::Log);
	IMPULSE_CHECK(decoded.logRecords.size() == 2);
	if (decoded.logRecords.size() == 2) {
		IMPULSE_CHECK(decoded.logRecords[0].level == Impulse::LiveKitLogLevel::Warn);
		IMPULSE_CHECK(decoded.logRecords[0].target == "livekit_room");
		IMPULSE_CHECK(decoded.logRecords[0].message == "joined");
		IMPULSE_CHECK(decoded.logRecords[1].level == Impulse::LiveKitLogLevel::Info);
		IMPULSE_CHECK(decoded.logRecords[1].target == "libwebrtc");
		IMPULSE_CHECK(decoded.logRecords[1].message == "stun");
	}

	const Impulse::LiveKitEvent unknown = Impulse::DecodeLiveKitEvent(LogBatchEvent({
		LogRecordOf(9, "livekit", "future level"),
	}));
	IMPULSE_CHECK(unknown.logRecords.size() == 1);
	if (unknown.logRecords.size() == 1) {
		IMPULSE_CHECK(unknown.logRecords[0].level == Impulse::LiveKitLogLevel::Unknown);
	}
}

IMPULSE_TEST(LiveKitLogSinkReceivesTheRecordsOfTheRouter) {
	Impulse::LiveKitEventRouter router;
	std::vector<Impulse::LiveKitLogRecord> received;
	router.setLogSink([&received](const Impulse::LiveKitLogRecord &record) {
		received.push_back(record);
	});
	int routed = 0;
	const Impulse::LiveKitEventRouter::RouteId route = router.addRoute(
		[&routed](const Impulse::LiveKitEvent &) {
			++routed;
		});
	router.addKey(route, 5);

	router.dispatch(Impulse::DecodeLiveKitEvent(LogBatchEvent({
		LogRecordOf(1, "livekit", "a"),
		LogRecordOf(2, "libwebrtc", "b"),
	})));
	IMPULSE_CHECK(received.size() == 2);
	IMPULSE_CHECK(routed == 0);
	if (received.size() == 2) {
		IMPULSE_CHECK(received[0].message == "a");
		IMPULSE_CHECK(received[1].target == "libwebrtc");
	}
}

IMPULSE_TEST(LiveKitLogLineKeepsLevelsPerTarget) {
	// Info and above for the livekit targets.
	IMPULSE_CHECK(LineIs(
		Impulse::FormatLiveKitLogLine(Record(
			Impulse::LiveKitLogLevel::Info,
			"livekit_room",
			"joined")),
		"livekit_room: info"));
	IMPULSE_CHECK(LineIs(
		Impulse::FormatLiveKitLogLine(Record(
			Impulse::LiveKitLogLevel::Warn,
			"livekit",
			"w")),
		"livekit: warn"));
	IMPULSE_CHECK(!Impulse::FormatLiveKitLogLine(Record(
		Impulse::LiveKitLogLevel::Debug,
		"livekit_room",
		"x")).has_value());
	IMPULSE_CHECK(!Impulse::FormatLiveKitLogLine(Record(
		Impulse::LiveKitLogLevel::Trace,
		"livekit_room",
		"x")).has_value());

	// Warn and above for every other target.
	IMPULSE_CHECK(!Impulse::FormatLiveKitLogLine(Record(
		Impulse::LiveKitLogLevel::Info,
		"libwebrtc",
		"x")).has_value());
	IMPULSE_CHECK(LineIs(
		Impulse::FormatLiveKitLogLine(Record(
			Impulse::LiveKitLogLevel::Warn,
			"libwebrtc",
			"stall")),
		"libwebrtc: warn"));
	IMPULSE_CHECK(LineIs(
		Impulse::FormatLiveKitLogLine(Record(
			Impulse::LiveKitLogLevel::Error,
			"tokio",
			"bad")),
		"tokio: error"));
	IMPULSE_CHECK(!Impulse::FormatLiveKitLogLine(Record(
		Impulse::LiveKitLogLevel::Debug,
		"tokio",
		"x")).has_value());

	// An unknown level is never written.
	IMPULSE_CHECK(!Impulse::FormatLiveKitLogLine(Record(
		Impulse::LiveKitLogLevel::Unknown,
		"livekit",
		"x")).has_value());
}

IMPULSE_TEST(LiveKitLogLineNeverWritesMessageText) {
	// The message can carry the authenticated URL, a token, key bytes or an
	// address, and the line keeps none of it: only the target and the level.
	IMPULSE_CHECK(LineIs(
		Impulse::FormatLiveKitLogLine(Record(
			Impulse::LiveKitLogLevel::Info,
			"livekit",
			"connecting to wss://example.com/rtc?access_token=abc123 now")),
		"livekit: info"));
	IMPULSE_CHECK(LineIs(
		Impulse::FormatLiveKitLogLine(Record(
			Impulse::LiveKitLogLevel::Error,
			"livekit",
			"ws://a.b/c")),
		"livekit: error"));
	IMPULSE_CHECK(LineIs(
		Impulse::FormatLiveKitLogLine(Record(
			Impulse::LiveKitLogLevel::Warn,
			"libwebrtc",
			"auth Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.c2ln done")),
		"libwebrtc: warn"));
	IMPULSE_CHECK(LineIs(
		Impulse::FormatLiveKitLogLine(Record(
			Impulse::LiveKitLogLevel::Error,
			"livekit",
			"token=SECRET&room=r1")),
		"livekit: error"));
	IMPULSE_CHECK(LineIs(
		Impulse::FormatLiveKitLogLine(Record(
			Impulse::LiveKitLogLevel::Error,
			"livekit",
			"wss%3A%2F%2Fhost%2Frtc%3Faccess_token%3Dsecret")),
		"livekit: error"));
	IMPULSE_CHECK(LineIs(
		Impulse::FormatLiveKitLogLine(Record(
			Impulse::LiveKitLogLevel::Warn,
			"libwebrtc",
			"key 00ff10c3a1 ip 10.0.0.1 auth=abc key=def")),
		"libwebrtc: warn"));
}

IMPULSE_TEST(LiveKitLogLineNamesTheTargetOnly) {
	// The target is the module path of the library; the line has no other text.
	const std::optional<std::string> line = Impulse::FormatLiveKitLogLine(Record(
		Impulse::LiveKitLogLevel::Error,
		"livekit_ffi::room",
		"any text"));
	IMPULSE_CHECK(LineIs(line, "livekit_ffi::room: error"));
}
