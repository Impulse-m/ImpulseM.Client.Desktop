#include "calls/livekit/livekit_call_logic.h"

#include <cstdio>
#include <set>
#include <string>

namespace Impulse {
namespace {

// A remote track that is subscribed and not muted is the one that plays.
[[nodiscard]] bool IsPlaying(const LiveKitRemoteMedia &media) {
	return media.subscribed && !media.muted;
}

// Remote audio of the call is the microphone, not the audio of a screen share.
[[nodiscard]] bool IsRemoteAudio(const LiveKitRemoteMedia &media) {
	return media.kind == LiveKitTrackKind::Audio
		&& media.source != LiveKitTrackSource::ScreenShareAudio;
}

[[nodiscard]] bool IsScreenShareVideo(const LiveKitRemoteMedia &media) {
	return media.kind == LiveKitTrackKind::Video
		&& media.source == LiveKitTrackSource::ScreenShare;
}

// Only a video that the publication names as a camera is a camera. A video
// whose publication is not known yet has the Unknown source, and it is not
// chosen until the publication arrives.
[[nodiscard]] bool IsCameraVideo(const LiveKitRemoteMedia &media) {
	return media.kind == LiveKitTrackKind::Video
		&& media.source == LiveKitTrackSource::Camera;
}

// The replacement of a byte that is not part of valid UTF-8: U+FFFD.
constexpr const char *kReplacementCharacter = "\xEF\xBF\xBD";

// Returns the length of the valid UTF-8 sequence that starts at the index, or 0
// when the bytes there are not one. Overlong forms, surrogates and code points
// above U+10FFFF are not valid.
[[nodiscard]] std::size_t Utf8SequenceLength(
		const std::string &text,
		std::size_t index) {
	const unsigned int lead = static_cast<unsigned char>(text[index]);
	if (lead < 0x80) {
		return 1;
	}
	std::size_t length = 0;
	// The second byte has a narrower range after some leads; the rest of the
	// bytes are plain continuation bytes.
	unsigned int secondLow = 0x80;
	unsigned int secondHigh = 0xBF;
	if (lead >= 0xC2 && lead <= 0xDF) {
		length = 2;
	} else if (lead >= 0xE0 && lead <= 0xEF) {
		length = 3;
		if (lead == 0xE0) {
			secondLow = 0xA0;
		} else if (lead == 0xED) {
			secondHigh = 0x9F;
		}
	} else if (lead >= 0xF0 && lead <= 0xF4) {
		length = 4;
		if (lead == 0xF0) {
			secondLow = 0x90;
		} else if (lead == 0xF4) {
			secondHigh = 0x8F;
		}
	} else {
		return 0;
	}
	if (index + length > text.size()) {
		return 0;
	}
	for (std::size_t offset = 1; offset < length; ++offset) {
		const unsigned int byte = static_cast<unsigned char>(text[index + offset]);
		const unsigned int low = (offset == 1) ? secondLow : 0x80;
		const unsigned int high = (offset == 1) ? secondHigh : 0xBF;
		if (byte < low || byte > high) {
			return 0;
		}
	}
	return length;
}

// Escapes the text for a JSON string. Control bytes are escaped, and valid UTF-8
// sequences are kept. Any other byte is replaced with U+FFFD, so the result is
// always valid JSON text.
[[nodiscard]] std::string EscapeJson(const std::string &text) {
	std::string result;
	result.reserve(text.size());
	std::size_t index = 0;
	while (index < text.size()) {
		const char symbol = text[index];
		const unsigned char code = static_cast<unsigned char>(symbol);
		if (code >= 0x80) {
			const std::size_t length = Utf8SequenceLength(text, index);
			if (length) {
				result.append(text, index, length);
				index += length;
			} else {
				result += kReplacementCharacter;
				++index;
			}
			continue;
		}
		++index;
		if (symbol == '"') {
			result += "\\\"";
		} else if (symbol == '\\') {
			result += "\\\\";
		} else if (symbol == '\n') {
			result += "\\n";
		} else if (symbol == '\r') {
			result += "\\r";
		} else if (symbol == '\t') {
			result += "\\t";
		} else if (code < 0x20) {
			char buffer[8] = {};
			std::snprintf(
				buffer,
				sizeof(buffer),
				"\\u%04x",
				static_cast<unsigned int>(code));
			result += buffer;
		} else {
			result += symbol;
		}
	}
	return result;
}

// The fields of one outbound video stream of the session stats. The index counts
// the video streams only.
[[nodiscard]] std::string FormatOutboundVideo(
		std::size_t index,
		const LiveKitOutboundRtpStats &item) {
	char fps[32] = {};
	std::snprintf(fps, sizeof(fps), "%.1f", item.framesPerSecond);
	char bitrate[32] = {};
	std::snprintf(bitrate, sizeof(bitrate), "%.0f", item.targetBitrate);
	const std::string prefix = "out_video" + std::to_string(index) + "_";
	const std::string rid = item.rid.empty() ? std::string("-") : item.rid;
	std::string result;
	result += " " + prefix + "rid=" + rid;
	result += " " + prefix + "size=" + std::to_string(item.frameWidth)
		+ "x" + std::to_string(item.frameHeight);
	result += " " + prefix + "fps=" + fps;
	result += " " + prefix + "encoded=" + std::to_string(item.framesEncoded);
	result += " " + prefix + "sent=" + std::to_string(item.framesSent);
	result += " " + prefix + "keyframes="
		+ std::to_string(item.keyFramesEncoded);
	result += " " + prefix + "retx="
		+ std::to_string(item.retransmittedPacketsSent);
	result += " " + prefix + "target_bps=" + bitrate;
	result += " " + prefix + "limit="
		+ LiveKitQualityLimitationName(item.qualityLimitationReason);
	result += " " + prefix + "active=" + (item.active ? "1" : "0");
	result += " " + prefix + "pli=" + std::to_string(item.pliCount);
	result += " " + prefix + "nack=" + std::to_string(item.nackCount);
	return result;
}

// The fields of one inbound video stream of the session stats. The index counts
// the video streams only.
[[nodiscard]] std::string FormatInboundVideo(
		std::size_t index,
		const LiveKitInboundRtpStats &item) {
	char fps[32] = {};
	std::snprintf(fps, sizeof(fps), "%.1f", item.framesPerSecond);
	const std::string prefix = "in_video" + std::to_string(index) + "_";
	std::string result;
	result += " " + prefix + "size=" + std::to_string(item.frameWidth)
		+ "x" + std::to_string(item.frameHeight);
	result += " " + prefix + "fps=" + fps;
	result += " " + prefix + "decoded="
		+ std::to_string(item.framesDecoded.value_or(0U));
	result += " " + prefix + "dropped=" + std::to_string(item.framesDropped);
	result += " " + prefix + "keyframes=" + std::to_string(item.keyFramesDecoded);
	result += " " + prefix + "freezes=" + std::to_string(item.freezeCount);
	result += " " + prefix + "pauses=" + std::to_string(item.pauseCount);
	return result;
}

// The captured frames of the call, since the previous stats line.
[[nodiscard]] std::string FormatCaptureCounters(
		const LiveKitCaptureCounters &capture) {
	std::string result;
	result += " video_frames_in=" + std::to_string(capture.received);
	result += " video_frames_pushed=" + std::to_string(capture.pushed);
	result += " video_frames_dropped="
		+ std::to_string(capture.droppedNoCapture
			+ capture.droppedPendingKind
			+ capture.droppedNotConnected
			+ capture.droppedEmptyFrame
			+ capture.droppedPublishInFlight
			+ capture.droppedLibraryRejected);
	result += " drop_no_capture=" + std::to_string(capture.droppedNoCapture);
	result += " drop_pending_kind=" + std::to_string(capture.droppedPendingKind);
	result += " drop_not_connected=" + std::to_string(capture.droppedNotConnected);
	result += " drop_empty=" + std::to_string(capture.droppedEmptyFrame);
	result += " drop_publish_in_flight="
		+ std::to_string(capture.droppedPublishInFlight);
	result += " drop_library_rejected="
		+ std::to_string(capture.droppedLibraryRejected);
	return result;
}

} // namespace

std::optional<LiveKitRemoteMedia> ChooseRemoteVideo(
		const std::vector<LiveKitRemoteMedia> &media) {
	std::optional<LiveKitRemoteMedia> camera;
	for (const LiveKitRemoteMedia &item : media) {
		if (!IsPlaying(item)) {
			continue;
		}
		if (IsScreenShareVideo(item)) {
			return item;
		}
		if (IsCameraVideo(item) && !camera) {
			camera = item;
		}
	}
	return camera;
}

LiveKitRemoteState ComputeRemoteState(
		const std::vector<LiveKitRemoteMedia> &media) {
	LiveKitRemoteState result;
	for (const LiveKitRemoteMedia &item : media) {
		if (IsRemoteAudio(item) && IsPlaying(item)) {
			result.audioActive = true;
		}
	}
	result.videoActive = ChooseRemoteVideo(media).has_value();
	return result;
}

std::size_t CountRemoteIdentities(const std::vector<LiveKitRemoteMedia> &media) {
	std::set<std::string> identities;
	for (const LiveKitRemoteMedia &item : media) {
		identities.insert(item.participantIdentity);
	}
	return identities.size();
}

std::string BuildLiveKitDebugJson(bool connected, const std::string &error) {
	std::string result = "{\"impulsem\":\"livekit\",\"connected\":";
	result += connected ? "true" : "false";
	result += ",\"error\":\"";
	result += EscapeJson(error);
	result += "\"}";
	return result;
}

LiveKitStatsSummary SummarizeStats(const LiveKitSessionStats &stats) {
	LiveKitStatsSummary result;
	for (const LiveKitOutboundRtpStats &item : stats.outbound) {
		if (item.kind == LiveKitTrackKind::Audio) {
			result.outboundAudioBytes += item.bytesSent;
			result.outboundAudioPackets += item.packetsSent;
		} else if (item.kind == LiveKitTrackKind::Video) {
			result.outboundVideoBytes += item.bytesSent;
			result.outboundVideoPackets += item.packetsSent;
		}
	}
	for (const LiveKitInboundRtpStats &item : stats.inbound) {
		if (item.kind == LiveKitTrackKind::Audio) {
			result.inboundAudioBytes += item.bytesReceived;
			result.inboundAudioPackets += item.packetsReceived;
			result.inboundAudioLost += item.packetsLost;
			result.inboundAudioSamples += item.totalSamplesReceived;
			if (item.audioLevel > result.inboundAudioLevel) {
				result.inboundAudioLevel = item.audioLevel;
			}
		} else if (item.kind == LiveKitTrackKind::Video) {
			result.inboundVideoBytes += item.bytesReceived;
			result.inboundVideoPackets += item.packetsReceived;
			if (item.framesDecoded) {
				result.inboundVideoFrames += *item.framesDecoded;
			}
		}
	}
	return result;
}

std::string FormatStatsLine(
		const LiveKitSessionStats &stats,
		const std::optional<LiveKitCaptureCounters> &capture) {
	if (stats.error) {
		return "session stats: unavailable";
	}
	const LiveKitStatsSummary summary = SummarizeStats(stats);
	char level[32] = {};
	std::snprintf(level, sizeof(level), "%.3f", summary.inboundAudioLevel);
	std::string result = "session stats:";
	result += " out_audio_bytes=" + std::to_string(summary.outboundAudioBytes);
	result += " out_audio_packets=" + std::to_string(summary.outboundAudioPackets);
	result += " out_video_bytes=" + std::to_string(summary.outboundVideoBytes);
	result += " out_video_packets=" + std::to_string(summary.outboundVideoPackets);
	result += " in_audio_packets=" + std::to_string(summary.inboundAudioPackets);
	result += " in_audio_bytes=" + std::to_string(summary.inboundAudioBytes);
	result += " in_audio_lost=" + std::to_string(summary.inboundAudioLost);
	result += " in_audio_samples=" + std::to_string(summary.inboundAudioSamples);
	result += " in_audio_level=";
	result += level;
	result += " in_video_packets=" + std::to_string(summary.inboundVideoPackets);
	result += " in_video_bytes=" + std::to_string(summary.inboundVideoBytes);
	result += " in_video_frames=" + std::to_string(summary.inboundVideoFrames);
	std::size_t outboundVideo = 0;
	for (const LiveKitOutboundRtpStats &item : stats.outbound) {
		if (item.kind == LiveKitTrackKind::Video) {
			result += FormatOutboundVideo(outboundVideo, item);
			++outboundVideo;
		}
	}
	std::size_t inboundVideo = 0;
	for (const LiveKitInboundRtpStats &item : stats.inbound) {
		if (item.kind == LiveKitTrackKind::Video) {
			result += FormatInboundVideo(inboundVideo, item);
			++inboundVideo;
		}
	}
	if (capture) {
		result += FormatCaptureCounters(*capture);
	}
	return result;
}

void LiveKitCaptureCounters::addDrop(LiveKitVideoDrop reason) {
	if (reason == LiveKitVideoDrop::NoCapture) {
		++droppedNoCapture;
	} else if (reason == LiveKitVideoDrop::PendingKind) {
		++droppedPendingKind;
	} else if (reason == LiveKitVideoDrop::NotConnected) {
		++droppedNotConnected;
	} else if (reason == LiveKitVideoDrop::EmptyFrame) {
		++droppedEmptyFrame;
	} else if (reason == LiveKitVideoDrop::PublishInFlight) {
		++droppedPublishInFlight;
	} else if (reason == LiveKitVideoDrop::LibraryRejected) {
		++droppedLibraryRejected;
	}
}

std::uint64_t LiveKitOutgoingVideo::attachCapture() {
	attached = true;
	kindPending = true;
	confirmedKind = std::nullopt;
	return ++generation;
}

std::optional<std::uint64_t> LiveKitOutgoingVideo::deviceSwitched() {
	if (!attached) {
		return std::nullopt;
	}
	kindPending = true;
	return ++generation;
}

LiveKitVideoKindResult LiveKitOutgoingVideo::kindConfirmed(
		std::uint64_t checkGeneration,
		bool isScreen) {
	LiveKitVideoKindResult result;
	if (!attached || !kindPending || checkGeneration != generation) {
		return result;
	}
	const LiveKitPublishKind kind = isScreen
		? LiveKitPublishKind::ScreenShare
		: LiveKitPublishKind::Camera;
	kindPending = false;
	confirmedKind = kind;
	result.accepted = true;
	// The published track of another kind is unpublished once. It stays published
	// until the adapter confirms the unpublish.
	if (publishedKind && *publishedKind != kind && !unpublishPending) {
		result.unpublish = *publishedKind;
		unpublishPending = true;
	}
	return result;
}

std::optional<LiveKitPublishKind> LiveKitOutgoingVideo::releaseCapture() {
	attached = false;
	kindPending = false;
	confirmedKind = std::nullopt;
	// A kind check that is still running is stale from now on.
	++generation;
	if (!publishedKind || unpublishPending) {
		return std::nullopt;
	}
	unpublishPending = true;
	return publishedKind;
}

void LiveKitOutgoingVideo::unpublishConfirmed(LiveKitPublishKind kind) {
	if (publishedKind == kind) {
		publishedKind = std::nullopt;
		unpublishPending = false;
	}
}

void LiveKitOutgoingVideo::roomConnected() {
	connected = true;
}

LiveKitVideoFrameDecision LiveKitOutgoingVideo::frame(
		std::uint32_t width,
		std::uint32_t height) {
	LiveKitVideoFrameDecision result;
	if (!attached) {
		result.drop = LiveKitVideoDrop::NoCapture;
		return result;
	}
	if (kindPending || !confirmedKind) {
		result.drop = LiveKitVideoDrop::PendingKind;
		return result;
	}
	if (!connected) {
		result.drop = LiveKitVideoDrop::NotConnected;
		return result;
	}
	if (!width || !height) {
		result.drop = LiveKitVideoDrop::EmptyFrame;
		return result;
	}
	// The published track of another kind waits until its unpublish is confirmed.
	if (unpublishPending && publishedKind != confirmedKind) {
		result.drop = LiveKitVideoDrop::PublishInFlight;
		return result;
	}
	result.kind = *confirmedKind;
	// A track of the same kind that is being unpublished is asked to publish
	// again, which cancels its unpublish when the adapter has it queued.
	result.action = (publishedKind == confirmedKind && !unpublishPending)
		? LiveKitVideoFrameAction::Push
		: LiveKitVideoFrameAction::PublishThenPush;
	result.drop = LiveKitVideoDrop::None;
	return result;
}

void LiveKitOutgoingVideo::publishResult(LiveKitPublishKind kind, bool started) {
	if (started && attached && confirmedKind == kind) {
		publishedKind = kind;
		unpublishPending = false;
	}
}

} // namespace Impulse
