// LiveKit room adapter for 1:1 calls. It owns one room on the library, connects
// the room, publishes the microphone (through PlatformAudio) and a camera
// track, mutes the tracks, and disconnects. Library events reach the adapter
// through the event router of the library, so two adapters on one library each
// get only the events of their own room. Conferences, RTMP and broadcast are
// not part of this adapter.
//
// All public methods run on the owner thread. Library callbacks are decoded on
// the library thread and handed to the owner thread through the poster.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Impulse {

class LiveKitFfi;
struct LiveKitAdapterShared;

enum class LiveKitPublishKind : std::uint8_t {
	Microphone,
	Camera,
};

class LiveKitAdapterDelegate {
public:
	virtual void liveKitConnected() = 0;
	virtual void liveKitPublished(LiveKitPublishKind kind) = 0;
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

	// Starts the room connection. Returns false when the adapter is not idle.
	// The result arrives through liveKitConnected or liveKitFailed.
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
