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

std::string FormatStatsLine(const LiveKitSessionStats &stats) {
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
	return result;
}

} // namespace Impulse
