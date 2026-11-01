/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/livekit/livekit_call_logic.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using Impulse::LiveKitRemoteMedia;
using Impulse::LiveKitTrackKind;
using Impulse::LiveKitTrackSource;

LiveKitRemoteMedia Track(
		const std::string &identity,
		const std::string &sid,
		LiveKitTrackKind kind,
		LiveKitTrackSource source,
		bool subscribed,
		bool muted) {
	LiveKitRemoteMedia media;
	media.participantIdentity = identity;
	media.trackSid = sid;
	media.kind = kind;
	media.source = source;
	media.subscribed = subscribed;
	media.muted = muted;
	return media;
}

LiveKitRemoteMedia Camera(bool subscribed, bool muted) {
	return Track(
		"peer",
		"TR_camera",
		LiveKitTrackKind::Video,
		LiveKitTrackSource::Camera,
		subscribed,
		muted);
}

LiveKitRemoteMedia Screen(bool subscribed, bool muted) {
	return Track(
		"peer",
		"TR_screen",
		LiveKitTrackKind::Video,
		LiveKitTrackSource::ScreenShare,
		subscribed,
		muted);
}

LiveKitRemoteMedia Microphone(bool subscribed, bool muted) {
	return Track(
		"peer",
		"TR_mic",
		LiveKitTrackKind::Audio,
		LiveKitTrackSource::Microphone,
		subscribed,
		muted);
}

} // namespace

IMPULSE_TEST(CallLogicRemoteVideoPrefersScreenShare) {
	using Impulse::ChooseRemoteVideo;
	const std::vector<LiveKitRemoteMedia> media = {
		Camera(true, false),
		Screen(true, false),
	};
	const std::optional<LiveKitRemoteMedia> shown = ChooseRemoteVideo(media);
	IMPULSE_CHECK(shown.has_value());
	if (shown) {
		IMPULSE_CHECK(shown->trackSid == "TR_screen");
		IMPULSE_CHECK(shown->source == LiveKitTrackSource::ScreenShare);
	}
}

IMPULSE_TEST(CallLogicRemoteVideoFallsBackToCamera) {
	using Impulse::ChooseRemoteVideo;
	const std::vector<LiveKitRemoteMedia> muted = {
		Camera(true, false),
		Screen(true, true),
	};
	const std::vector<LiveKitRemoteMedia> unsubscribed = {
		Camera(true, false),
		Screen(false, false),
	};
	const std::optional<LiveKitRemoteMedia> byMute = ChooseRemoteVideo(muted);
	const std::optional<LiveKitRemoteMedia> bySubscription
		= ChooseRemoteVideo(unsubscribed);
	IMPULSE_CHECK(byMute.has_value());
	if (byMute) {
		IMPULSE_CHECK(byMute->trackSid == "TR_camera");
	}
	IMPULSE_CHECK(bySubscription.has_value());
	if (bySubscription) {
		IMPULSE_CHECK(bySubscription->trackSid == "TR_camera");
	}
}

IMPULSE_TEST(CallLogicRemoteVideoNoneWhenNothingPlays) {
	using Impulse::ChooseRemoteVideo;
	IMPULSE_CHECK(!ChooseRemoteVideo({}).has_value());
	IMPULSE_CHECK(!ChooseRemoteVideo({ Camera(true, true) }).has_value());
	IMPULSE_CHECK(!ChooseRemoteVideo({ Camera(false, false) }).has_value());
	IMPULSE_CHECK(!ChooseRemoteVideo({ Microphone(true, false) }).has_value());
}

IMPULSE_TEST(CallLogicRemoteVideoTransitions) {
	using Impulse::ChooseRemoteVideo;
	IMPULSE_CHECK(ChooseRemoteVideo({ Camera(true, false) }).has_value());
	IMPULSE_CHECK(!ChooseRemoteVideo({ Camera(true, true) }).has_value());
	IMPULSE_CHECK(ChooseRemoteVideo({ Camera(true, false) }).has_value());
	IMPULSE_CHECK(!ChooseRemoteVideo({ Camera(false, false) }).has_value());

	// A screen share that starts while the camera plays takes over, and it
	// gives the view back to the camera when it is muted again.
	IMPULSE_CHECK(ChooseRemoteVideo({
		Camera(true, false),
		Screen(true, false),
	})->trackSid == "TR_screen");
	IMPULSE_CHECK(ChooseRemoteVideo({
		Camera(true, false),
		Screen(true, true),
	})->trackSid == "TR_camera");
}

IMPULSE_TEST(CallLogicRemoteVideoIgnoresAudioTracks) {
	using Impulse::ChooseRemoteVideo;
	IMPULSE_CHECK(!ChooseRemoteVideo({ Microphone(true, false) }).has_value());
}

IMPULSE_TEST(CallLogicRemoteStateFollowsAudioAndVideo) {
	using Impulse::ComputeRemoteState;
	const Impulse::LiveKitRemoteState empty = ComputeRemoteState({});
	IMPULSE_CHECK(!empty.audioActive);
	IMPULSE_CHECK(!empty.videoActive);

	const Impulse::LiveKitRemoteState live = ComputeRemoteState({
		Microphone(true, false),
		Camera(true, false),
	});
	IMPULSE_CHECK(live.audioActive);
	IMPULSE_CHECK(live.videoActive);

	const Impulse::LiveKitRemoteState audioMuted = ComputeRemoteState({
		Microphone(true, true),
		Camera(true, false),
	});
	IMPULSE_CHECK(!audioMuted.audioActive);
	IMPULSE_CHECK(audioMuted.videoActive);

	const Impulse::LiveKitRemoteState screenOnly = ComputeRemoteState({
		Microphone(false, false),
		Screen(true, false),
	});
	IMPULSE_CHECK(!screenOnly.audioActive);
	IMPULSE_CHECK(screenOnly.videoActive);
}

IMPULSE_TEST(CallLogicCountsRemoteIdentities) {
	using Impulse::CountRemoteIdentities;
	IMPULSE_CHECK(CountRemoteIdentities({}) == 0);
	IMPULSE_CHECK(CountRemoteIdentities({
		Camera(true, false),
		Microphone(true, false),
	}) == 1);
	IMPULSE_CHECK(CountRemoteIdentities({
		Camera(true, false),
		Track("other", "TR_other", LiveKitTrackKind::Audio, LiveKitTrackSource::Microphone, true, false),
	}) == 2);
}

IMPULSE_TEST(CallLogicDebugJsonIsPlainWithoutError) {
	using Impulse::BuildLiveKitDebugJson;
	IMPULSE_CHECK(BuildLiveKitDebugJson(true, "")
		== "{\"impulsem\":\"livekit\",\"connected\":true,\"error\":\"\"}");
	IMPULSE_CHECK(BuildLiveKitDebugJson(false, "failed")
		== "{\"impulsem\":\"livekit\",\"connected\":false,\"error\":\"failed\"}");
}

IMPULSE_TEST(CallLogicDebugJsonEscapesError) {
	using Impulse::BuildLiveKitDebugJson;
	IMPULSE_CHECK(BuildLiveKitDebugJson(false, "say \"no\"")
		== "{\"impulsem\":\"livekit\",\"connected\":false,\"error\":\"say \\\"no\\\"\"}");
	IMPULSE_CHECK(BuildLiveKitDebugJson(false, "a\\b")
		== "{\"impulsem\":\"livekit\",\"connected\":false,\"error\":\"a\\\\b\"}");
	IMPULSE_CHECK(BuildLiveKitDebugJson(false, "line\nnext\ttab")
		== "{\"impulsem\":\"livekit\",\"connected\":false,\"error\":\"line\\nnext\\ttab\"}");
	const std::string control = std::string(1, '\x01');
	IMPULSE_CHECK(BuildLiveKitDebugJson(false, control)
		== "{\"impulsem\":\"livekit\",\"connected\":false,\"error\":\"\\u0001\"}");
}

IMPULSE_TEST(CallLogicStatsSummaryAddsStreams) {
	using Impulse::SummarizeStats;
	Impulse::LiveKitSessionStats stats;
	Impulse::LiveKitOutboundRtpStats outAudio;
	outAudio.kind = LiveKitTrackKind::Audio;
	outAudio.bytesSent = 100;
	outAudio.packetsSent = 4;
	Impulse::LiveKitOutboundRtpStats outVideo;
	outVideo.kind = LiveKitTrackKind::Video;
	outVideo.bytesSent = 900;
	outVideo.packetsSent = 30;
	stats.outbound = { outAudio, outVideo };

	Impulse::LiveKitInboundRtpStats inAudioLow;
	inAudioLow.kind = LiveKitTrackKind::Audio;
	inAudioLow.bytesReceived = 50;
	inAudioLow.packetsReceived = 2;
	inAudioLow.packetsLost = 1;
	inAudioLow.totalSamplesReceived = 960;
	inAudioLow.audioLevel = 0.25;
	Impulse::LiveKitInboundRtpStats inAudioHigh = inAudioLow;
	inAudioHigh.bytesReceived = 70;
	inAudioHigh.packetsReceived = 3;
	inAudioHigh.packetsLost = 2;
	inAudioHigh.audioLevel = 0.5;
	Impulse::LiveKitInboundRtpStats inVideo;
	inVideo.kind = LiveKitTrackKind::Video;
	inVideo.bytesReceived = 4000;
	inVideo.packetsReceived = 80;
	inVideo.framesDecoded = 12;
	stats.inbound = { inAudioLow, inAudioHigh, inVideo };

	const Impulse::LiveKitStatsSummary summary = SummarizeStats(stats);
	IMPULSE_CHECK(summary.outboundAudioBytes == 100);
	IMPULSE_CHECK(summary.outboundAudioPackets == 4);
	IMPULSE_CHECK(summary.outboundVideoBytes == 900);
	IMPULSE_CHECK(summary.outboundVideoPackets == 30);
	IMPULSE_CHECK(summary.inboundAudioBytes == 120);
	IMPULSE_CHECK(summary.inboundAudioPackets == 5);
	IMPULSE_CHECK(summary.inboundAudioLost == 3);
	IMPULSE_CHECK(summary.inboundAudioSamples == 1920);
	IMPULSE_CHECK(summary.inboundAudioLevel == 0.5);
	IMPULSE_CHECK(summary.inboundVideoBytes == 4000);
	IMPULSE_CHECK(summary.inboundVideoPackets == 80);
	IMPULSE_CHECK(summary.inboundVideoFrames == 12);
}

IMPULSE_TEST(CallLogicStatsLineReportsErrorAndTotals) {
	using Impulse::FormatStatsLine;
	Impulse::LiveKitSessionStats failed;
	failed.error = true;
	IMPULSE_CHECK(FormatStatsLine(failed) == "session stats: unavailable");

	Impulse::LiveKitSessionStats stats;
	Impulse::LiveKitOutboundRtpStats outAudio;
	outAudio.kind = LiveKitTrackKind::Audio;
	outAudio.bytesSent = 10;
	stats.outbound = { outAudio };
	const std::string line = FormatStatsLine(stats);
	IMPULSE_CHECK(line.find("out_audio_bytes=10") != std::string::npos);
	IMPULSE_CHECK(line.find("in_audio_level=0.000") != std::string::npos);
	IMPULSE_CHECK(line.find("in_video_frames=0") != std::string::npos);
}

IMPULSE_TEST(CallLogicDebugJsonReplacesInvalidUtf8) {
	using Impulse::BuildLiveKitDebugJson;
	const std::string replacement = "\xEF\xBF\xBD";

	// A byte that starts no sequence is replaced; the valid two-byte sequence is
	// kept as it is.
	IMPULSE_CHECK(BuildLiveKitDebugJson(false, std::string("bad\xFF" "byte"))
		== "{\"impulsem\":\"livekit\",\"connected\":false,\"error\":\"bad"
			+ replacement + "byte\"}");
	IMPULSE_CHECK(BuildLiveKitDebugJson(false, std::string("caf\xC3\xA9"))
		== "{\"impulsem\":\"livekit\",\"connected\":false,\"error\":\"caf\xC3\xA9\"}");

	// A truncated sequence at the end is replaced byte by byte.
	IMPULSE_CHECK(BuildLiveKitDebugJson(false, std::string("\xE2\x82"))
		== "{\"impulsem\":\"livekit\",\"connected\":false,\"error\":\""
			+ replacement + replacement + "\"}");

	// A surrogate is not valid UTF-8 either.
	IMPULSE_CHECK(BuildLiveKitDebugJson(false, std::string("\xED\xA0\x80"))
		== "{\"impulsem\":\"livekit\",\"connected\":false,\"error\":\""
			+ replacement + replacement + replacement + "\"}");
}

IMPULSE_TEST(CallLogicSubscribedBeforePublishIsNotShown) {
	using Impulse::ChooseRemoteVideo;
	// The track is subscribed before its publication arrives, so its source is
	// still unknown: it is not shown as a camera.
	const std::vector<LiveKitRemoteMedia> pending = {
		Track(
			"peer",
			"TR_pending",
			LiveKitTrackKind::Video,
			LiveKitTrackSource::Unknown,
			true,
			false),
	};
	IMPULSE_CHECK(!ChooseRemoteVideo(pending).has_value());

	// Once the publication names a screen share, it is shown as one.
	const std::vector<LiveKitRemoteMedia> named = {
		Track(
			"peer",
			"TR_pending",
			LiveKitTrackKind::Video,
			LiveKitTrackSource::ScreenShare,
			true,
			false),
	};
	const std::optional<LiveKitRemoteMedia> shown = ChooseRemoteVideo(named);
	IMPULSE_CHECK(shown.has_value());
	if (shown) {
		IMPULSE_CHECK(shown->source == LiveKitTrackSource::ScreenShare);
	}
}
