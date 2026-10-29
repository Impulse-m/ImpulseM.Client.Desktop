// The interface of a LiveKit room adapter, and the types its callers share. The
// group and call instances drive a room through this interface, so a fake room
// can stand in for LiveKitAdapter in tests. LiveKitAdapter is the implementation
// on the library.
//
// All methods run on the owner thread. Library callbacks are decoded on the
// library thread and handed to the owner thread through the poster.
#pragma once

#include "calls/livekit/livekit_ffi_codec.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Impulse {

enum class LiveKitPublishKind : std::uint8_t {
	Microphone,
	Camera,
	ScreenShare,
};

// One plane of an I420 frame. The bytes are a copy, so the frame outlives the
// library buffer it came from.
struct LiveKitVideoPlane final {
	std::vector<std::uint8_t> bytes;
	std::uint32_t stride = 0;
};

// One decoded frame of a remote video track. The planes are Y, U and V.
struct LiveKitVideoFrame final {
	std::string participantIdentity;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::int64_t timestampUs = 0;
	std::array<LiveKitVideoPlane, 3> planes;
};

// One I420 frame to push into the native video source of a camera or of a screen
// share. The planes are Y, U and V; the adapter copies nothing, the library reads
// the bytes during the capture call.
struct LiveKitCapturedFrame final {
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::int64_t timestampUs = 0;
	std::array<LiveKitVideoPlane, 3> planes;
};

// Receives the frames of every subscribed remote video track. It is called on
// the owner thread. The sink must outlive its registration, or be removed with
// setVideoSink before it is destroyed.
class LiveKitVideoSink {
public:
	virtual void liveKitVideoFrame(const LiveKitVideoFrame &frame) = 0;

	virtual ~LiveKitVideoSink() = default;
};

class LiveKitAdapterDelegate {
public:
	virtual void liveKitConnected() = 0;
	virtual void liveKitPublished(LiveKitPublishKind kind) = 0;

	// Fires when the room fails: a connect or publish fails, the room is
	// dropped by the server, or a track reports an E2EE failure state. The
	// room is released before the call.
	virtual void liveKitFailed(const std::string &message) = 0;
	virtual void liveKitDisconnected() = 0;

	virtual ~LiveKitAdapterDelegate() = default;
};

class LiveKitAdapterInterface {
public:
	using Poster = std::function<void(std::function<void()>)>;

	// Sets the E2EE shared key (key index 0) for the next connect. The key is the
	// 256-byte media key of the call; the caller derives and checks it. It is
	// sent in the connect request, so it is set before the room connects and
	// before any track is published. An empty key connects without E2EE. The
	// key is not logged.
	virtual void setSharedKey(std::vector<std::uint8_t> key) = 0;

	// Sets the sink of the remote video frames. nullptr stops delivery. Frames
	// of a track that is subscribed before the sink is set are delivered too.
	virtual void setVideoSink(LiveKitVideoSink *sink) = 0;

	// Starts the room connection. Returns false when the adapter is not idle.
	// The result arrives through liveKitConnected or liveKitFailed. A connected
	// room subscribes to the remote tracks of the room automatically.
	virtual bool connect(const std::string &url, const std::string &token) = 0;

	// Publishes the microphone. PlatformAudio is created first. Returns false
	// when the room is not connected or the microphone is already requested.
	virtual bool publishMicrophone() = 0;

	// Publishes a camera track from a native video source of the given size.
	// Frames are pushed with captureVideoFrame.
	virtual bool publishCamera(std::uint32_t width, std::uint32_t height) = 0;

	// Publishes a screen share from a native screencast source of the given
	// size, as the SCREEN_SHARE source of the room. Frames are pushed with
	// captureVideoFrame and the kind ScreenShare. Returns false when the room is
	// not connected or the screen share is already requested.
	virtual bool publishScreenShare(
		std::uint32_t width,
		std::uint32_t height) = 0;

	// Pushes one I420 frame into the source of the camera or of the screen share.
	// Returns false when the room is not connected, the source of the kind is
	// not published, the frame has no usable planes, or the library rejects the
	// frame. A dropped frame does not end the call.
	virtual bool captureVideoFrame(
		LiveKitPublishKind kind,
		const LiveKitCapturedFrame &frame) = 0;

	// Sets the microphone mute. A muted track sends no audio: the library
	// mute request is sent to the track, and a track created while muted is
	// muted before it is published. The state is kept until the room is
	// released, and it starts muted. Returns false when the library did not
	// confirm the request; the room is then failed, so no audio is left
	// live.
	virtual bool setMicrophoneMuted(bool muted) = 0;

	// Enables or disables the camera track. A disabled track sends no video.
	// The state starts enabled and applies to a camera published later.
	// Returns false when the library did not confirm the request; the room is
	// then failed.
	virtual bool setCameraEnabled(bool enabled) = 0;

	// Sets the noise suppression of the microphone, on by default as on Android.
	// The library takes it with the microphone source, so it applies to the
	// microphone that is published next. The library has no request that changes
	// an existing source, so a change while a microphone source exists returns
	// false; the value is still kept for the next microphone.
	virtual bool setNoiseSuppression(bool enabled) = 0;

	// Sets the simulcast layer of the video of a remote participant, by the
	// participant identity. The quality is kept for the adapter lifetime and
	// applies to every video publication of the participant, both the ones that
	// are published already and the ones published later. Returns false when the
	// library rejects a request; the room is not failed for it.
	virtual bool setRemoteVideoQuality(
		const std::string &participantIdentity,
		LiveKitVideoQuality quality) = 0;

	// Leaves the room and releases every owned handle. liveKitDisconnected
	// fires once the library confirms the disconnect.
	virtual void disconnect() = 0;

	[[nodiscard]] virtual bool connected() const = 0;

	virtual ~LiveKitAdapterInterface() = default;
};

} // namespace Impulse
