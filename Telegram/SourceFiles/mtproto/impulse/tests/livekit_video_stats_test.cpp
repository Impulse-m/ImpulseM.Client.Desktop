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
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

// FfiEvent field of the GetSessionStatsCallback.
constexpr std::uint32_t kEventSessionStats = 19;

// RtcStats members and the RTP stream messages (stats.proto).
constexpr std::uint32_t kRtcOutboundRtp = 5;
constexpr std::uint32_t kRtcInboundRtp = 4;

// A member of the stats that the decoder does not know. It must be skipped.
constexpr std::uint32_t kUnknownMember = 99;

[[nodiscard]] Impulse::ProtoWriter RtcIdOf(std::uint64_t timestamp) {
	Impulse::ProtoWriter rtc;
	rtc.writeStringField(1, "rtc-id");
	rtc.writeVarintField(2, timestamp);
	return rtc;
}

// The RtpStreamStats of a stream, with its kind.
[[nodiscard]] Impulse::ProtoWriter StreamOf(const std::string &kind) {
	Impulse::ProtoWriter stream;
	stream.writeVarintField(1, 5);
	stream.writeStringField(2, kind);
	stream.writeStringField(3, "transport");
	stream.writeStringField(4, "codec");
	return stream;
}

// The outbound RTP member of a video stream: the sent counters, and the
// OutboundRtpStreamStats of the simulcast layer "h" with its encoder fields.
[[nodiscard]] Impulse::ProtoWriter OutboundVideo() {
	Impulse::ProtoWriter sent;
	sent.writeVarintField(1, 1200);
	sent.writeVarintField(2, 987654);
	Impulse::ProtoWriter outboundStats;
	outboundStats.writeStringField(4, "h");
	outboundStats.writeVarintField(6, 3);
	outboundStats.writeDoubleField(9, 250000.5);
	outboundStats.writeVarintField(11, 1280);
	outboundStats.writeVarintField(12, 720);
	outboundStats.writeDoubleField(13, 29.5);
	outboundStats.writeVarintField(14, 900);
	outboundStats.writeVarintField(16, 901);
	outboundStats.writeVarintField(17, 4);
	outboundStats.writeVarintField(21, 2);
	outboundStats.writeVarintField(24, 7);
	outboundStats.writeVarintField(26, 2);
	outboundStats.writeVarintField(29, 1);
	outboundStats.writeVarintField(kUnknownMember, 42);
	Impulse::ProtoWriter outbound;
	outbound.writeMessageField(1, RtcIdOf(1000));
	outbound.writeMessageField(2, StreamOf("video"));
	outbound.writeMessageField(3, sent);
	outbound.writeMessageField(4, outboundStats);
	Impulse::ProtoWriter rtc;
	rtc.writeMessageField(kRtcOutboundRtp, outbound);
	return rtc;
}

// The outbound RTP member of an audio stream, with only the sent counters.
[[nodiscard]] Impulse::ProtoWriter OutboundCountersOnly() {
	Impulse::ProtoWriter sent;
	sent.writeVarintField(1, 3);
	sent.writeVarintField(2, 300);
	Impulse::ProtoWriter outbound;
	outbound.writeMessageField(2, StreamOf("audio"));
	outbound.writeMessageField(3, sent);
	Impulse::ProtoWriter rtc;
	rtc.writeMessageField(kRtcOutboundRtp, outbound);
	return rtc;
}

// The inbound RTP member of a video stream, with the frame and the decoder fields
// of InboundRtpStreamStats.
[[nodiscard]] Impulse::ProtoWriter InboundVideo() {
	Impulse::ProtoWriter received;
	received.writeVarintField(1, 300);
	received.writeVarintField(2, 4);
	Impulse::ProtoWriter inboundStats;
	inboundStats.writeVarintField(4, 77);
	inboundStats.writeVarintField(5, 3);
	inboundStats.writeVarintField(7, 5);
	inboundStats.writeVarintField(8, 640);
	inboundStats.writeVarintField(9, 360);
	inboundStats.writeDoubleField(10, 24.0);
	inboundStats.writeVarintField(15, 1);
	inboundStats.writeVarintField(17, 2);
	inboundStats.writeVarintField(25, 5000);
	Impulse::ProtoWriter inbound;
	inbound.writeMessageField(1, RtcIdOf(2000));
	inbound.writeMessageField(2, StreamOf("video"));
	inbound.writeMessageField(3, received);
	inbound.writeMessageField(4, inboundStats);
	Impulse::ProtoWriter rtc;
	rtc.writeMessageField(kRtcInboundRtp, inbound);
	return rtc;
}

// The inbound RTP member of an audio stream that carries frame fields. The video
// fields are read for video streams only, so the frame size of the audio stream
// stays zero.
[[nodiscard]] Impulse::ProtoWriter InboundAudioWithFrameFields() {
	Impulse::ProtoWriter received;
	received.writeVarintField(1, 50);
	received.writeVarintField(2, 0);
	Impulse::ProtoWriter inboundStats;
	inboundStats.writeVarintField(8, 640);
	inboundStats.writeVarintField(35, 480000);
	inboundStats.writeDoubleField(41, 0.25);
	Impulse::ProtoWriter inbound;
	inbound.writeMessageField(1, RtcIdOf(3000));
	inbound.writeMessageField(2, StreamOf("audio"));
	inbound.writeMessageField(3, received);
	inbound.writeMessageField(4, inboundStats);
	Impulse::ProtoWriter rtc;
	rtc.writeMessageField(kRtcInboundRtp, inbound);
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

} // namespace

IMPULSE_TEST(LiveKitOutboundVideoStatsDecode) {
	const Impulse::LiveKitEvent decoded = Impulse::DecodeLiveKitEvent(
		SessionStatsEvent(7, { OutboundVideo() }, {}));
	IMPULSE_CHECK(decoded.kind == Impulse::LiveKitEventKind::SessionStats);
	IMPULSE_CHECK(decoded.sessionStats.has_value());
	if (!decoded.sessionStats) {
		return;
	}
	const Impulse::LiveKitSessionStats &stats = *decoded.sessionStats;
	IMPULSE_CHECK(!stats.error);
	IMPULSE_CHECK(stats.outbound.size() == 1);
	if (stats.outbound.size() != 1) {
		return;
	}
	const Impulse::LiveKitOutboundRtpStats &video = stats.outbound[0];
	IMPULSE_CHECK(video.kind == Impulse::LiveKitTrackKind::Video);
	IMPULSE_CHECK(video.packetsSent == 1200);
	IMPULSE_CHECK(video.bytesSent == 987654);
	IMPULSE_CHECK(video.rid == "h");
	IMPULSE_CHECK(video.frameWidth == 1280);
	IMPULSE_CHECK(video.frameHeight == 720);
	IMPULSE_CHECK(video.framesPerSecond == 29.5);
	IMPULSE_CHECK(video.framesSent == 900);
	IMPULSE_CHECK(video.framesEncoded == 901);
	IMPULSE_CHECK(video.keyFramesEncoded == 4);
	IMPULSE_CHECK(video.retransmittedPacketsSent == 3);
	IMPULSE_CHECK(video.targetBitrate == 250000.5);
	IMPULSE_CHECK(video.qualityLimitationReason == 2);
	IMPULSE_CHECK(video.active);
	IMPULSE_CHECK(video.pliCount == 2);
	IMPULSE_CHECK(video.nackCount == 7);
}

IMPULSE_TEST(LiveKitInboundVideoStatsDecode) {
	const Impulse::LiveKitEvent decoded = Impulse::DecodeLiveKitEvent(
		SessionStatsEvent(8, {}, { InboundVideo(), InboundAudioWithFrameFields() }));
	IMPULSE_CHECK(decoded.sessionStats.has_value());
	if (!decoded.sessionStats) {
		return;
	}
	const Impulse::LiveKitSessionStats &stats = *decoded.sessionStats;
	IMPULSE_CHECK(stats.inbound.size() == 2);
	if (stats.inbound.size() != 2) {
		return;
	}

	const Impulse::LiveKitInboundRtpStats &video = stats.inbound[0];
	IMPULSE_CHECK(video.kind == Impulse::LiveKitTrackKind::Video);
	IMPULSE_CHECK(video.packetsReceived == 300);
	IMPULSE_CHECK(video.bytesReceived == 5000);
	IMPULSE_CHECK(video.framesDecoded.has_value());
	if (video.framesDecoded) {
		IMPULSE_CHECK(*video.framesDecoded == 77);
	}
	IMPULSE_CHECK(video.keyFramesDecoded == 3);
	IMPULSE_CHECK(video.framesDropped == 5);
	IMPULSE_CHECK(video.frameWidth == 640);
	IMPULSE_CHECK(video.frameHeight == 360);
	IMPULSE_CHECK(video.framesPerSecond == 24.0);
	IMPULSE_CHECK(video.pauseCount == 1);
	IMPULSE_CHECK(video.freezeCount == 2);

	const Impulse::LiveKitInboundRtpStats &audio = stats.inbound[1];
	IMPULSE_CHECK(audio.kind == Impulse::LiveKitTrackKind::Audio);
	IMPULSE_CHECK(audio.totalSamplesReceived == 480000);
	IMPULSE_CHECK(audio.audioLevel == 0.25);
	IMPULSE_CHECK(!audio.framesDecoded.has_value());
	IMPULSE_CHECK(audio.frameWidth == 0);
	IMPULSE_CHECK(audio.frameHeight == 0);
	IMPULSE_CHECK(audio.keyFramesDecoded == 0);
	IMPULSE_CHECK(audio.freezeCount == 0);
}

IMPULSE_TEST(LiveKitOutboundStatsMissingFieldsStayZero) {
	const Impulse::LiveKitEvent decoded = Impulse::DecodeLiveKitEvent(
		SessionStatsEvent(9, { OutboundCountersOnly() }, {}));
	IMPULSE_CHECK(decoded.sessionStats.has_value());
	if (!decoded.sessionStats || decoded.sessionStats->outbound.size() != 1) {
		return;
	}
	const Impulse::LiveKitOutboundRtpStats &audio = decoded.sessionStats->outbound[0];
	IMPULSE_CHECK(audio.packetsSent == 3);
	IMPULSE_CHECK(audio.bytesSent == 300);
	IMPULSE_CHECK(audio.rid.empty());
	IMPULSE_CHECK(audio.frameWidth == 0);
	IMPULSE_CHECK(audio.frameHeight == 0);
	IMPULSE_CHECK(audio.framesPerSecond == 0.0);
	IMPULSE_CHECK(audio.framesSent == 0);
	IMPULSE_CHECK(audio.framesEncoded == 0);
	IMPULSE_CHECK(audio.keyFramesEncoded == 0);
	IMPULSE_CHECK(audio.retransmittedPacketsSent == 0);
	IMPULSE_CHECK(audio.targetBitrate == 0.0);
	IMPULSE_CHECK(audio.qualityLimitationReason == 0);
	IMPULSE_CHECK(!audio.active);
	IMPULSE_CHECK(audio.pliCount == 0);
	IMPULSE_CHECK(audio.nackCount == 0);
}

IMPULSE_TEST(LiveKitQualityLimitationNames) {
	IMPULSE_CHECK(Impulse::LiveKitQualityLimitationName(0) == "none");
	IMPULSE_CHECK(Impulse::LiveKitQualityLimitationName(1) == "cpu");
	IMPULSE_CHECK(Impulse::LiveKitQualityLimitationName(2) == "bandwidth");
	IMPULSE_CHECK(Impulse::LiveKitQualityLimitationName(3) == "other");
	// A number the library does not define is named as the OTHER reason.
	IMPULSE_CHECK(Impulse::LiveKitQualityLimitationName(9) == "other");
}
