// LiveKit room adapter for 1:1 calls and group calls. It owns one room on the
// library, connects the room with the E2EE shared key, publishes the microphone
// (through PlatformAudio), a camera track and a screen share, subscribes to the
// remote tracks, mutes the tracks, sets the video quality of the remote
// participants, and disconnects. Remote audio plays through PlatformAudio, which
// is created when the room connects. Remote video frames go to a sink. Library
// events reach the adapter through the event router of the library, so two
// adapters on one library each get only the events of their own room.
// The remote audio volume is not set here: the pinned protocol has no request
// for it.
//
// The public interface is LiveKitAdapterInterface. All public methods run on the
// owner thread. Library callbacks are decoded on the library thread and handed
// to the owner thread through the poster.
#pragma once

#include "calls/livekit/livekit_adapter_interface.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Impulse {

class LiveKitFfi;
struct LiveKitAdapterShared;

class LiveKitAdapter final : public LiveKitAdapterInterface {
public:
	// The library must outlive the adapter. Use LiveKitFfiLoader::Process for
	// the process library.
	LiveKitAdapter(
		std::shared_ptr<LiveKitFfi> library,
		LiveKitAdapterDelegate &delegate,
		Poster post);
	LiveKitAdapter(const LiveKitAdapter &other) = delete;
	LiveKitAdapter &operator=(const LiveKitAdapter &other) = delete;

	// A connected room gets an explicit disconnect request, and a room that is
	// still connecting is disconnected as soon as the library reports it. The
	// handles of the room are released when the library confirms the
	// disconnect. The library itself is not disposed.
	~LiveKitAdapter() override;

	void setSharedKey(std::vector<std::uint8_t> key) override;
	void setVideoSink(LiveKitVideoSink *sink) override;
	bool connect(const std::string &url, const std::string &token) override;
	bool publishMicrophone() override;
	bool publishCamera(std::uint32_t width, std::uint32_t height) override;
	bool publishScreenShare(std::uint32_t width, std::uint32_t height) override;
	bool captureVideoFrame(
		LiveKitPublishKind kind,
		const LiveKitCapturedFrame &frame) override;
	bool setMicrophoneMuted(bool muted) override;
	bool setCameraEnabled(bool enabled) override;
	bool setNoiseSuppression(bool enabled) override;
	bool setRemoteVideoQuality(
		const std::string &participantIdentity,
		LiveKitVideoQuality quality) override;
	void disconnect() override;
	[[nodiscard]] bool connected() const override;

private:
	std::shared_ptr<LiveKitFfi> ffi;
	std::shared_ptr<LiveKitAdapterShared> shared;
};

} // namespace Impulse
