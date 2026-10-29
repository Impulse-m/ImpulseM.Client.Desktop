// LiveKit room adapter for 1:1 calls. It owns the FFI loader of one room,
// connects the room, publishes the microphone (through PlatformAudio) and a
// camera track, and disconnects. Subscription and E2EE are wired by the next
// task. Conferences, RTMP and broadcast are not part of this adapter.
//
// All public methods run on the owner thread. Library callbacks are decoded on
// the library thread and handed to the owner thread through the poster.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Impulse {

class LiveKitFfiLoader;
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

	LiveKitAdapter(
		const std::wstring &dllPath,
		LiveKitAdapterDelegate &delegate,
		Poster post);
	LiveKitAdapter(const LiveKitAdapter &other) = delete;
	LiveKitAdapter &operator=(const LiveKitAdapter &other) = delete;
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

	// Leaves the room and releases every owned handle. liveKitDisconnected
	// fires once the library confirms the disconnect.
	void disconnect();

	[[nodiscard]] bool connected() const;

private:
	std::unique_ptr<LiveKitFfiLoader> loader;
	std::shared_ptr<LiveKitAdapterShared> shared;
};

} // namespace Impulse
