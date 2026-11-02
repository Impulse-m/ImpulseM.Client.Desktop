// Std-only logic of the 1:1 LiveKit call instance: the remote media state of the
// room, the remote video that is shown, the outgoing video of the call (the
// camera or the screen share that is published), the summary of the session stats,
// and the debug JSON of a call. It has no Qt or webrtc dependency, so the std-only
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

// What the instance does with one captured frame of the call.
enum class LiveKitVideoFrameAction : std::uint8_t {
	Drop,
	Push,
	PublishThenPush,
};

// Why a captured frame is dropped. The reason is None for a frame that is pushed.
enum class LiveKitVideoDrop : std::uint8_t {
	None,
	NoCapture,
	PendingKind,
	NotConnected,
	EmptyFrame,
	PublishInFlight,
	LibraryRejected,
};

struct LiveKitVideoFrameDecision final {
	LiveKitVideoFrameAction action = LiveKitVideoFrameAction::Drop;
	LiveKitPublishKind kind = LiveKitPublishKind::Camera;
	LiveKitVideoDrop drop = LiveKitVideoDrop::NoCapture;
};

// The answer to a kind check of the capture. A check is accepted only while its
// generation is the current one. The unpublish is set when the confirmed kind
// replaces the kind of the published track.
struct LiveKitVideoKindResult final {
	bool accepted = false;
	std::optional<LiveKitPublishKind> unpublish;
};

// The captured frames of the call, counted for the debug stats line. Every
// received frame is either pushed or dropped with one reason.
struct LiveKitCaptureCounters final {
	std::uint64_t received = 0;
	std::uint64_t pushed = 0;
	std::uint64_t droppedNoCapture = 0;
	std::uint64_t droppedPendingKind = 0;
	std::uint64_t droppedNotConnected = 0;
	std::uint64_t droppedEmptyFrame = 0;
	std::uint64_t droppedPublishInFlight = 0;
	std::uint64_t droppedLibraryRejected = 0;

	void addDrop(LiveKitVideoDrop reason);
};

// The outgoing video of a 1:1 call. The call has one capture at a time, and the
// capture is either the camera or a screen share. The track of the room follows
// the kind of the capture: at most one video track is published or being
// published, it is unpublished when the kind changes or the capture is released,
// and the next frame of the kind publishes a fresh one.
//
// A published track stays published until the adapter confirms its unpublish,
// through unpublishConfirmed. While it waits, the frames of another kind are
// dropped, and a frame of the same kind asks for a publish, which cancels a
// queued unpublish.
//
// The owner thread drives it. A kind check is started with a generation when the
// capture is attached or switched to another device, and the answer is given back
// with the same generation, so an answer for an old device is ignored.
class LiveKitOutgoingVideo final {
private:
	std::uint64_t generation = 0;
	bool attached = false;
	bool kindPending = false;
	bool connected = false;
	// The published kind is being unpublished. Only the published kind is ever
	// unpublished, so the flag is set exactly while it is requested.
	bool unpublishPending = false;
	std::optional<LiveKitPublishKind> confirmedKind;
	std::optional<LiveKitPublishKind> publishedKind;

public:
	// Attaches a capture and starts its kind check. The owner releases the
	// previous capture before it attaches another one.
	[[nodiscard]] std::uint64_t attachCapture();

	// Starts a new kind check when a capture is attached, after its device was
	// switched. Returns nothing when no capture is attached.
	[[nodiscard]] std::optional<std::uint64_t> deviceSwitched();

	// Gives the kind check answer. A stale generation, or an answer without a
	// pending check, is ignored.
	[[nodiscard]] LiveKitVideoKindResult kindConfirmed(
		std::uint64_t checkGeneration,
		bool isScreen);

	// Releases the capture. Returns the kind of the published track, which the
	// owner unpublishes, if there is one and no unpublish of it is requested.
	[[nodiscard]] std::optional<LiveKitPublishKind> releaseCapture();

	// The adapter confirmed the unpublish of the kind, so the track is gone from
	// the room and the next frame of the kind publishes a fresh one.
	void unpublishConfirmed(LiveKitPublishKind kind);

	void roomConnected();

	// Decides what to do with a captured frame of the given size.
	[[nodiscard]] LiveKitVideoFrameDecision frame(
		std::uint32_t width,
		std::uint32_t height);

	// Records the answer of the adapter to a publish that the decision asked for.
	// A refused publish leaves the state as it was, so the next frame asks again.
	// An accepted publish of the kind that was being unpublished cancelled that
	// unpublish, so the track is kept.
	void publishResult(LiveKitPublishKind kind, bool started);
};

// One line of the session stats for the log. The line has no URL, token or key.
// The video streams are listed with their own fields, and the capture counters
// are appended when they are given.
[[nodiscard]] std::string FormatStatsLine(
	const LiveKitSessionStats &stats,
	const std::optional<LiveKitCaptureCounters> &capture = std::nullopt);

} // namespace Impulse
