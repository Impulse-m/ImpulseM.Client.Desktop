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

using Impulse::LiveKitCaptureCounters;
using Impulse::LiveKitOutgoingVideo;
using Impulse::LiveKitPublishKind;
using Impulse::LiveKitVideoDrop;
using Impulse::LiveKitVideoFrameAction;
using Impulse::LiveKitVideoFrameDecision;
using Impulse::LiveKitVideoKindResult;

// Attaches a capture and answers its kind check, so the frames are decided.
std::uint64_t AttachAndConfirm(LiveKitOutgoingVideo &video, bool isScreen) {
	const std::uint64_t generation = video.attachCapture();
	IMPULSE_CHECK(video.kindConfirmed(generation, isScreen).accepted);
	return generation;
}

// Takes the publish decision of the first frame and records the publish as
// started, as the instance does when the adapter accepts it.
void PublishOnFirstFrame(LiveKitOutgoingVideo &video) {
	const LiveKitVideoFrameDecision decision = video.frame(1280, 720);
	IMPULSE_CHECK(decision.action == LiveKitVideoFrameAction::PublishThenPush);
	video.publishResult(decision.kind, true);
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

IMPULSE_TEST(CallLogicOutgoingVideoDropsWithoutCapture) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	const LiveKitVideoFrameDecision decision = video.frame(1280, 720);
	IMPULSE_CHECK(decision.action == LiveKitVideoFrameAction::Drop);
	IMPULSE_CHECK(decision.drop == LiveKitVideoDrop::NoCapture);
}

IMPULSE_TEST(CallLogicOutgoingVideoDropsWhileKindPending) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	const std::uint64_t generation = video.attachCapture();
	const LiveKitVideoFrameDecision pending = video.frame(1280, 720);
	IMPULSE_CHECK(pending.action == LiveKitVideoFrameAction::Drop);
	IMPULSE_CHECK(pending.drop == LiveKitVideoDrop::PendingKind);
	IMPULSE_CHECK(video.kindConfirmed(generation, false).accepted);
	const LiveKitVideoFrameDecision decision = video.frame(1280, 720);
	IMPULSE_CHECK(decision.action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(decision.kind == LiveKitPublishKind::Camera);
	IMPULSE_CHECK(decision.drop == LiveKitVideoDrop::None);
}

IMPULSE_TEST(CallLogicOutgoingVideoDropsBeforeConnect) {
	LiveKitOutgoingVideo video;
	AttachAndConfirm(video, false);
	const LiveKitVideoFrameDecision early = video.frame(1280, 720);
	IMPULSE_CHECK(early.action == LiveKitVideoFrameAction::Drop);
	IMPULSE_CHECK(early.drop == LiveKitVideoDrop::NotConnected);
	video.roomConnected();
	const LiveKitVideoFrameDecision connected = video.frame(1280, 720);
	IMPULSE_CHECK(connected.action == LiveKitVideoFrameAction::PublishThenPush);
}

IMPULSE_TEST(CallLogicOutgoingVideoDropsEmptyFrame) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, false);
	IMPULSE_CHECK(video.frame(0, 720).drop == LiveKitVideoDrop::EmptyFrame);
	IMPULSE_CHECK(video.frame(1280, 0).drop == LiveKitVideoDrop::EmptyFrame);
	IMPULSE_CHECK(video.frame(1280, 0).action == LiveKitVideoFrameAction::Drop);
	IMPULSE_CHECK(video.frame(1280, 720).action == LiveKitVideoFrameAction::PublishThenPush);
}

IMPULSE_TEST(CallLogicOutgoingVideoPushesAfterPublish) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, false);
	PublishOnFirstFrame(video);
	const LiveKitVideoFrameDecision next = video.frame(1280, 720);
	IMPULSE_CHECK(next.action == LiveKitVideoFrameAction::Push);
	IMPULSE_CHECK(next.kind == LiveKitPublishKind::Camera);
	IMPULSE_CHECK(next.drop == LiveKitVideoDrop::None);
}

IMPULSE_TEST(CallLogicOutgoingVideoRefusedPublishAsksAgain) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, true);
	const LiveKitVideoFrameDecision first = video.frame(1280, 720);
	IMPULSE_CHECK(first.action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(first.kind == LiveKitPublishKind::ScreenShare);
	video.publishResult(LiveKitPublishKind::ScreenShare, false);
	IMPULSE_CHECK(video.frame(1280, 720).action == LiveKitVideoFrameAction::PublishThenPush);
	video.publishResult(LiveKitPublishKind::ScreenShare, true);
	IMPULSE_CHECK(video.frame(1280, 720).action == LiveKitVideoFrameAction::Push);
}

IMPULSE_TEST(CallLogicOutgoingVideoStaleKindCheckIgnored) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	const std::uint64_t first = video.attachCapture();
	const std::optional<std::uint64_t> second = video.deviceSwitched();
	IMPULSE_CHECK(second.has_value());
	const std::uint64_t secondGeneration = second.value_or(0);
	IMPULSE_CHECK(!video.kindConfirmed(first, true).accepted);
	IMPULSE_CHECK(video.frame(1280, 720).drop == LiveKitVideoDrop::PendingKind);
	IMPULSE_CHECK(video.kindConfirmed(secondGeneration, true).accepted);
	const LiveKitVideoFrameDecision decision = video.frame(1280, 720);
	IMPULSE_CHECK(decision.action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(decision.kind == LiveKitPublishKind::ScreenShare);
}

IMPULSE_TEST(CallLogicOutgoingVideoKindCheckAnsweredOnce) {
	LiveKitOutgoingVideo video;
	const std::uint64_t generation = video.attachCapture();
	IMPULSE_CHECK(video.kindConfirmed(generation, false).accepted);
	IMPULSE_CHECK(!video.kindConfirmed(generation, true).accepted);
	IMPULSE_CHECK(!video.releaseCapture().has_value());
	IMPULSE_CHECK(!video.kindConfirmed(generation, false).accepted);
}

IMPULSE_TEST(CallLogicOutgoingVideoDeviceSwitchNeedsCapture) {
	LiveKitOutgoingVideo video;
	IMPULSE_CHECK(!video.deviceSwitched().has_value());
	AttachAndConfirm(video, false);
	IMPULSE_CHECK(video.deviceSwitched().has_value());
	IMPULSE_CHECK(!video.releaseCapture().has_value());
	IMPULSE_CHECK(!video.deviceSwitched().has_value());
}

IMPULSE_TEST(CallLogicOutgoingVideoDeviceSwitchSameKindKeepsTrack) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, false);
	PublishOnFirstFrame(video);
	const std::optional<std::uint64_t> generation = video.deviceSwitched();
	IMPULSE_CHECK(generation.has_value());
	IMPULSE_CHECK(video.frame(1280, 720).drop == LiveKitVideoDrop::PendingKind);
	const LiveKitVideoKindResult result = video.kindConfirmed(
		generation.value_or(0),
		false);
	IMPULSE_CHECK(result.accepted);
	IMPULSE_CHECK(!result.unpublish.has_value());
	IMPULSE_CHECK(video.frame(1280, 720).action == LiveKitVideoFrameAction::Push);
}

IMPULSE_TEST(CallLogicOutgoingVideoCameraToScreenUnpublishesCamera) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, false);
	PublishOnFirstFrame(video);
	const std::optional<std::uint64_t> generation = video.deviceSwitched();
	const LiveKitVideoKindResult result = video.kindConfirmed(
		generation.value_or(0),
		true);
	IMPULSE_CHECK(result.accepted);
	IMPULSE_CHECK(result.unpublish == LiveKitPublishKind::Camera);
	// The screen share waits until the camera is gone from the room.
	IMPULSE_CHECK(video.frame(1280, 720).drop == LiveKitVideoDrop::PublishInFlight);
	video.unpublishConfirmed(LiveKitPublishKind::Camera);
	const LiveKitVideoFrameDecision decision = video.frame(1280, 720);
	IMPULSE_CHECK(decision.action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(decision.kind == LiveKitPublishKind::ScreenShare);
}

IMPULSE_TEST(CallLogicOutgoingVideoScreenToCameraUnpublishesScreen) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, true);
	PublishOnFirstFrame(video);
	const std::optional<std::uint64_t> generation = video.deviceSwitched();
	const LiveKitVideoKindResult result = video.kindConfirmed(
		generation.value_or(0),
		false);
	IMPULSE_CHECK(result.accepted);
	IMPULSE_CHECK(result.unpublish == LiveKitPublishKind::ScreenShare);
	// The camera waits until the screen share is gone from the room.
	IMPULSE_CHECK(video.frame(1280, 720).drop == LiveKitVideoDrop::PublishInFlight);
	video.unpublishConfirmed(LiveKitPublishKind::ScreenShare);
	const LiveKitVideoFrameDecision decision = video.frame(1280, 720);
	IMPULSE_CHECK(decision.action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(decision.kind == LiveKitPublishKind::Camera);
}

IMPULSE_TEST(CallLogicOutgoingVideoReleaseReturnsPublishedKind) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, true);
	PublishOnFirstFrame(video);
	IMPULSE_CHECK(video.releaseCapture() == LiveKitPublishKind::ScreenShare);
	IMPULSE_CHECK(!video.releaseCapture().has_value());
	IMPULSE_CHECK(video.frame(1280, 720).drop == LiveKitVideoDrop::NoCapture);
}

IMPULSE_TEST(CallLogicOutgoingVideoReleaseWithoutPublishUnpublishesNothing) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, false);
	IMPULSE_CHECK(!video.releaseCapture().has_value());
}

IMPULSE_TEST(CallLogicOutgoingVideoCameraScreenCameraWithRelease) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, false);
	PublishOnFirstFrame(video);
	IMPULSE_CHECK(video.releaseCapture() == LiveKitPublishKind::Camera);
	video.unpublishConfirmed(LiveKitPublishKind::Camera);

	AttachAndConfirm(video, true);
	const LiveKitVideoFrameDecision screen = video.frame(1280, 720);
	IMPULSE_CHECK(screen.action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(screen.kind == LiveKitPublishKind::ScreenShare);
	video.publishResult(LiveKitPublishKind::ScreenShare, true);
	IMPULSE_CHECK(video.frame(1280, 720).action == LiveKitVideoFrameAction::Push);
	IMPULSE_CHECK(video.releaseCapture() == LiveKitPublishKind::ScreenShare);
	video.unpublishConfirmed(LiveKitPublishKind::ScreenShare);

	AttachAndConfirm(video, false);
	const LiveKitVideoFrameDecision camera = video.frame(1280, 720);
	IMPULSE_CHECK(camera.action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(camera.kind == LiveKitPublishKind::Camera);
}

IMPULSE_TEST(CallLogicOutgoingVideoReattachSameKindPublishesFreshTrack) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, false);
	PublishOnFirstFrame(video);
	IMPULSE_CHECK(video.releaseCapture() == LiveKitPublishKind::Camera);
	AttachAndConfirm(video, false);
	const LiveKitVideoFrameDecision decision = video.frame(1280, 720);
	IMPULSE_CHECK(decision.action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(decision.kind == LiveKitPublishKind::Camera);
}

IMPULSE_TEST(CallLogicOutgoingVideoStaleGenerationAfterRelease) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	const std::uint64_t first = video.attachCapture();
	IMPULSE_CHECK(!video.releaseCapture().has_value());
	const std::uint64_t second = video.attachCapture();
	IMPULSE_CHECK(first != second);
	IMPULSE_CHECK(!video.kindConfirmed(first, true).accepted);
	IMPULSE_CHECK(video.kindConfirmed(second, false).accepted);
	const LiveKitVideoFrameDecision decision = video.frame(1280, 720);
	IMPULSE_CHECK(decision.action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(decision.kind == LiveKitPublishKind::Camera);
}

IMPULSE_TEST(CallLogicOutgoingVideoLateResultAfterReleaseIgnored) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, false);
	IMPULSE_CHECK(video.frame(1280, 720).action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(!video.releaseCapture().has_value());
	video.publishResult(LiveKitPublishKind::Camera, true);
	AttachAndConfirm(video, false);
	IMPULSE_CHECK(video.frame(1280, 720).action == LiveKitVideoFrameAction::PublishThenPush);
}

IMPULSE_TEST(CallLogicOutgoingVideoSameKindSwitchCancelsUnpublish) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, false);
	PublishOnFirstFrame(video);

	// The camera goes to a screen share and back before the unpublish confirms.
	const std::optional<std::uint64_t> toScreen = video.deviceSwitched();
	const LiveKitVideoKindResult unpublish = video.kindConfirmed(
		toScreen.value_or(0),
		true);
	IMPULSE_CHECK(unpublish.unpublish == LiveKitPublishKind::Camera);
	const std::optional<std::uint64_t> toCamera = video.deviceSwitched();
	const LiveKitVideoKindResult back = video.kindConfirmed(
		toCamera.value_or(0),
		false);
	IMPULSE_CHECK(back.accepted);
	IMPULSE_CHECK(!back.unpublish.has_value());

	// The camera publishes again, which the adapter takes as the cancel of its
	// queued unpublish, and the track is kept with no new publish cycle.
	const LiveKitVideoFrameDecision decision = video.frame(1280, 720);
	IMPULSE_CHECK(decision.action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(decision.kind == LiveKitPublishKind::Camera);
	video.publishResult(LiveKitPublishKind::Camera, true);
	IMPULSE_CHECK(video.frame(1280, 720).action == LiveKitVideoFrameAction::Push);
	IMPULSE_CHECK(video.releaseCapture() == LiveKitPublishKind::Camera);
}

IMPULSE_TEST(CallLogicOutgoingVideoRefusedCancelKeepsDroppingUntilConfirmed) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, false);
	PublishOnFirstFrame(video);
	const std::optional<std::uint64_t> toScreen = video.deviceSwitched();
	IMPULSE_CHECK(video.kindConfirmed(toScreen.value_or(0), true).unpublish.has_value());

	// Back to the camera while the unpublish is already sent: the adapter refuses
	// the publish, so the frames wait for the unpublish callback.
	const std::optional<std::uint64_t> toCamera = video.deviceSwitched();
	IMPULSE_CHECK(!video.kindConfirmed(toCamera.value_or(0), false).unpublish.has_value());
	const LiveKitVideoFrameDecision asking = video.frame(1280, 720);
	IMPULSE_CHECK(asking.action == LiveKitVideoFrameAction::PublishThenPush);
	video.publishResult(LiveKitPublishKind::Camera, false);
	IMPULSE_CHECK(video.frame(1280, 720).action == LiveKitVideoFrameAction::PublishThenPush);

	// The callback confirms the unpublish, and the next frame publishes fresh.
	video.unpublishConfirmed(LiveKitPublishKind::Camera);
	const LiveKitVideoFrameDecision fresh = video.frame(1280, 720);
	IMPULSE_CHECK(fresh.action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(fresh.kind == LiveKitPublishKind::Camera);
}

IMPULSE_TEST(CallLogicOutgoingVideoReleaseWaitsForUnpublishConfirm) {
	LiveKitOutgoingVideo video;
	video.roomConnected();
	AttachAndConfirm(video, false);
	PublishOnFirstFrame(video);
	IMPULSE_CHECK(video.releaseCapture() == LiveKitPublishKind::Camera);

	// A screen share after the release waits for the camera to be gone.
	AttachAndConfirm(video, true);
	IMPULSE_CHECK(video.frame(1280, 720).drop == LiveKitVideoDrop::PublishInFlight);
	video.unpublishConfirmed(LiveKitPublishKind::Camera);
	const LiveKitVideoFrameDecision decision = video.frame(1280, 720);
	IMPULSE_CHECK(decision.action == LiveKitVideoFrameAction::PublishThenPush);
	IMPULSE_CHECK(decision.kind == LiveKitPublishKind::ScreenShare);
}

IMPULSE_TEST(CallLogicCaptureCountersCountDropsByReason) {
	LiveKitCaptureCounters counters;
	counters.addDrop(LiveKitVideoDrop::NoCapture);
	counters.addDrop(LiveKitVideoDrop::PendingKind);
	counters.addDrop(LiveKitVideoDrop::PendingKind);
	counters.addDrop(LiveKitVideoDrop::NotConnected);
	counters.addDrop(LiveKitVideoDrop::EmptyFrame);
	counters.addDrop(LiveKitVideoDrop::PublishInFlight);
	counters.addDrop(LiveKitVideoDrop::LibraryRejected);
	counters.addDrop(LiveKitVideoDrop::None);
	IMPULSE_CHECK(counters.droppedNoCapture == 1);
	IMPULSE_CHECK(counters.droppedPendingKind == 2);
	IMPULSE_CHECK(counters.droppedNotConnected == 1);
	IMPULSE_CHECK(counters.droppedEmptyFrame == 1);
	IMPULSE_CHECK(counters.droppedPublishInFlight == 1);
	IMPULSE_CHECK(counters.droppedLibraryRejected == 1);
	IMPULSE_CHECK(counters.pushed == 0);
}

IMPULSE_TEST(CallLogicStatsLineReportsVideoStreams) {
	using Impulse::FormatStatsLine;
	Impulse::LiveKitSessionStats stats;
	Impulse::LiveKitOutboundRtpStats outVideo;
	outVideo.kind = LiveKitTrackKind::Video;
	outVideo.rid = "h";
	outVideo.frameWidth = 1280;
	outVideo.frameHeight = 720;
	outVideo.framesPerSecond = 30.0;
	outVideo.framesEncoded = 100;
	outVideo.framesSent = 99;
	outVideo.keyFramesEncoded = 3;
	outVideo.retransmittedPacketsSent = 2;
	outVideo.targetBitrate = 1200000.0;
	outVideo.qualityLimitationReason = 1;
	outVideo.active = true;
	outVideo.pliCount = 1;
	outVideo.nackCount = 4;
	outVideo.bytesSent = 900;
	Impulse::LiveKitOutboundRtpStats lowVideo = outVideo;
	lowVideo.rid = "";
	lowVideo.qualityLimitationReason = 0;
	lowVideo.active = false;
	stats.outbound = { outVideo, lowVideo };

	Impulse::LiveKitInboundRtpStats inVideo;
	inVideo.kind = LiveKitTrackKind::Video;
	inVideo.frameWidth = 640;
	inVideo.frameHeight = 360;
	inVideo.framesPerSecond = 24.5;
	inVideo.framesDecoded = 50U;
	inVideo.framesDropped = 2;
	inVideo.keyFramesDecoded = 1;
	inVideo.freezeCount = 3;
	inVideo.pauseCount = 0;
	stats.inbound = { inVideo };

	const std::string line = FormatStatsLine(stats);
	IMPULSE_CHECK(line.find(" out_video0_rid=h") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video0_size=1280x720") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video0_fps=30.0") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video0_encoded=100") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video0_sent=99") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video0_keyframes=3") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video0_retx=2") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video0_target_bps=1200000") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video0_limit=cpu") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video0_active=1") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video0_pli=1") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video0_nack=4") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video1_rid=-") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video1_active=0") != std::string::npos);
	IMPULSE_CHECK(line.find(" out_video1_limit=none") != std::string::npos);
	IMPULSE_CHECK(line.find(" in_video0_size=640x360") != std::string::npos);
	IMPULSE_CHECK(line.find(" in_video0_fps=24.5") != std::string::npos);
	IMPULSE_CHECK(line.find(" in_video0_decoded=50") != std::string::npos);
	IMPULSE_CHECK(line.find(" in_video0_dropped=2") != std::string::npos);
	IMPULSE_CHECK(line.find(" in_video0_keyframes=1") != std::string::npos);
	IMPULSE_CHECK(line.find(" in_video0_freezes=3") != std::string::npos);
	IMPULSE_CHECK(line.find(" in_video0_pauses=0") != std::string::npos);
	IMPULSE_CHECK(line.find("video_frames_in") == std::string::npos);
}

IMPULSE_TEST(CallLogicStatsLineAppendsCaptureCounters) {
	using Impulse::FormatStatsLine;
	Impulse::LiveKitSessionStats stats;
	LiveKitCaptureCounters counters;
	counters.received = 10;
	counters.pushed = 7;
	counters.droppedPendingKind = 2;
	counters.droppedPublishInFlight = 1;
	const std::string line = FormatStatsLine(stats, counters);
	IMPULSE_CHECK(line.find(" video_frames_in=10") != std::string::npos);
	IMPULSE_CHECK(line.find(" video_frames_pushed=7") != std::string::npos);
	IMPULSE_CHECK(line.find(" video_frames_dropped=3") != std::string::npos);
	IMPULSE_CHECK(line.find(" drop_pending_kind=2") != std::string::npos);
	IMPULSE_CHECK(line.find(" drop_publish_in_flight=1") != std::string::npos);
	IMPULSE_CHECK(line.find(" drop_no_capture=0") != std::string::npos);
}
