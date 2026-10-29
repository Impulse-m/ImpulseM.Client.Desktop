// LiveKit room adapter for 1:1 calls. It owns one room on the library, connects
// the room with the E2EE shared key, publishes the microphone (through
// PlatformAudio) and a camera track, subscribes to the remote tracks, mutes the
// tracks, and disconnects. Remote audio plays through PlatformAudio, which is
// created when the room connects. Remote video frames go to a sink. Library
// events reach the adapter through the event router of the library, so two
// adapters on one library each get only the events of their own room.
// Conferences, RTMP and broadcast are not part of this adapter.
//
// All public methods run on the owner thread. Library callbacks are decoded on
// the library thread and handed to the owner thread through the poster.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Impulse {

class LiveKitFfi;
struct LiveKitAdapterShared;

enum class LiveKitPublishKind : std::uint8_t {
	Microphone,
	Camera,
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

class LiveKitAdapter final {
public:
	using Poster = std::function<void(std::function<void()>)>;

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
	~LiveKitAdapter();

	// Sets the E2EE shared key (key index 0) for the next connect. The key is the
	// 256-byte media key of the call; the caller derives and checks it. It is
	// sent in the connect request, so it is set before the room connects and
	// before any track is published. An empty key connects without E2EE. The
	// key is not logged.
	void setSharedKey(std::vector<std::uint8_t> key);

	// Sets the sink of the remote video frames. nullptr stops delivery. Frames
	// of a track that is subscribed before the sink is set are delivered too.
	void setVideoSink(LiveKitVideoSink *sink);

	// Starts the room connection. Returns false when the adapter is not idle.
	// The result arrives through liveKitConnected or liveKitFailed. A connected
	// room subscribes to the remote tracks of the room automatically.
	bool connect(const std::string &url, const std::string &token);

	// Publishes the microphone. PlatformAudio is created first. Returns false
	// when the room is not connected or the microphone is already requested.
	bool publishMicrophone();

	// Publishes a camera track from a native video source of the given size.
	// Frames are pushed by the video wiring, not by this adapter.
	bool publishCamera(std::uint32_t width, std::uint32_t height);

	// Sets the microphone mute. A muted track sends no audio: the library
	// mute request is sent to the track, and a track created while muted is
	// muted before it is published. The state is kept until the room is
	// released, and it starts muted. Returns false when the library did not
	// confirm the request; the room is then failed, so no audio is left
	// live.
	bool setMicrophoneMuted(bool muted);

	// Enables or disables the camera track. A disabled track sends no video.
	// The state starts enabled and applies to a camera published later.
	// Returns false when the library did not confirm the request; the room is
	// then failed.
	bool setCameraEnabled(bool enabled);

	// Leaves the room and releases every owned handle. liveKitDisconnected
	// fires once the library confirms the disconnect.
	void disconnect();

	[[nodiscard]] bool connected() const;

private:
	std::shared_ptr<LiveKitFfi> ffi;
	std::shared_ptr<LiveKitAdapterShared> shared;
};

} // namespace Impulse
