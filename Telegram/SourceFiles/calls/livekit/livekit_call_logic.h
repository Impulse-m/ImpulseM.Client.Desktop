// Std-only logic of the 1:1 LiveKit call instance: the remote media state of the
// room, the remote video that is shown, the summary of the session stats, and
// the debug JSON of a call. It has no Qt or webrtc dependency, so the std-only
// test target builds it with recording inputs.
//
// In a 1:1 room the peer is the only remote participant. The functions still
// look at every remote track, so a second identity is seen and reported.
#pragma once

#include "calls/livekit/livekit_adapter_interface.h"
#include "calls/livekit/livekit_ffi_codec.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Impulse {

// The remote media state of a 1:1 call, as the call controller reports it.
struct LiveKitRemoteState final {
	bool audioActive = false;
	bool videoActive = false;
};

// The totals of the session stats that the call logs and counts as traffic.
struct LiveKitStatsSummary final {
	std::uint64_t outboundAudioBytes = 0;
	std::uint64_t outboundAudioPackets = 0;
	std::uint64_t outboundVideoBytes = 0;
	std::uint64_t outboundVideoPackets = 0;
	std::uint64_t inboundAudioBytes = 0;
	std::uint64_t inboundAudioPackets = 0;
	std::int64_t inboundAudioLost = 0;
	std::uint64_t inboundAudioSamples = 0;
	double inboundAudioLevel = 0.;
	std::uint64_t inboundVideoBytes = 0;
	std::uint64_t inboundVideoPackets = 0;
	std::uint64_t inboundVideoFrames = 0;
};

// The remote video shown in a 1:1 call: the screen share of the remote tracks
// when it is subscribed and unmuted, otherwise the camera in the same state.
// Empty when neither is. A video track whose source is still Unknown, because it
// was subscribed before its publication arrived, is not a camera and is not
// chosen; it is chosen once the publication names it as a camera or a screen
// share.
[[nodiscard]] std::optional<LiveKitRemoteMedia> ChooseRemoteVideo(
	const std::vector<LiveKitRemoteMedia> &media);

// Audio is active when a remote audio track is subscribed and unmuted. Video is
// active when ChooseRemoteVideo finds a video to show.
[[nodiscard]] LiveKitRemoteState ComputeRemoteState(
	const std::vector<LiveKitRemoteMedia> &media);

// The number of distinct participant identities in the snapshot.
[[nodiscard]] std::size_t CountRemoteIdentities(
	const std::vector<LiveKitRemoteMedia> &media);

// The debug log of a call, a JSON object. The error text is escaped, so the
// server gets valid JSON for any text.
[[nodiscard]] std::string BuildLiveKitDebugJson(
	bool connected,
	const std::string &error);

// The totals of a session stats answer. The error flag is not counted here.
[[nodiscard]] LiveKitStatsSummary SummarizeStats(
	const LiveKitSessionStats &stats);

// One line of the session stats for the log. The line has no URL, token or key.
[[nodiscard]] std::string FormatStatsLine(const LiveKitSessionStats &stats);

} // namespace Impulse
