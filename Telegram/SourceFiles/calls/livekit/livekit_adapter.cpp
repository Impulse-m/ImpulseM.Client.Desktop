#include "calls/livekit/livekit_adapter.h"

#include "calls/livekit/livekit_event_router.h"
#include "calls/livekit/livekit_ffi_codec.h"
#include "calls/livekit/livekit_ffi_loader.h"

#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace Impulse {
namespace {

// FfiResponse field numbers of the requests the adapter sends (livekit-ffi
// protocol, FfiResponse oneof).
constexpr std::uint32_t kResponseConnect = 3;
constexpr std::uint32_t kResponseDisconnect = 4;
constexpr std::uint32_t kResponsePublishTrack = 5;
constexpr std::uint32_t kResponseCreateVideoTrack = 15;
constexpr std::uint32_t kResponseCreateAudioTrack = 16;
constexpr std::uint32_t kResponseNewVideoSource = 21;
constexpr std::uint32_t kResponseNewAudioSource = 26;
constexpr std::uint32_t kResponseNewPlatformAudio = 76;

constexpr const char *kMicrophoneName = "microphone";
constexpr const char *kCameraName = "camera";

// Error texts are fixed. SDK messages can contain the authenticated WebSocket
// URL and are never forwarded.
constexpr const char *kConnectFailed = "LiveKit connection failed";
constexpr const char *kPublishFailed = "LiveKit publish failed";
constexpr const char *kMuteFailed = "LiveKit mute failed";

// Auto-subscribe stays on, as in Android's room defaults. Subscription and
// E2EE wiring are added by the next task.
[[nodiscard]] LiveKitRoomOptions DefaultRoomOptions() {
	LiveKitRoomOptions options;
	options.autoSubscribe = true;
	return options;
}

enum class Phase : std::uint8_t {
	Idle,
	Connecting,
	Connected,
	Disconnecting,
	Failed,
};

void DropHandle(
		LiveKitFfi &library,
		std::optional<std::uint64_t> &handle) {
	if (handle) {
		library.dropHandle(*handle);
		handle.reset();
	}
}

std::optional<std::uint64_t> RequestHandle(
		LiveKitAdapterShared &state,
		const std::vector<std::uint8_t> &request,
		std::uint32_t responseField);

void AddKey(LiveKitAdapterShared &state, std::uint64_t key);

void ReleaseAll(LiveKitAdapterShared &state);

void RetireIfDestroyed(LiveKitAdapterShared &state);

void FailWith(LiveKitAdapterShared &state, const char *message);

void FinishDisconnected(LiveKitAdapterShared &state);

void SendDisconnect(LiveKitAdapterShared &state);

bool PublishTrack(
		LiveKitAdapterShared &state,
		std::uint64_t trackHandle,
		LiveKitTrackSource source,
		LiveKitPublishKind kind);

bool SetTrackMuted(
		LiveKitAdapterShared &state,
		std::uint64_t trackHandle,
		bool muted);

void DispatchConnect(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event);

void DispatchPublish(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event);

void DispatchDisconnect(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event);

void Dispatch(LiveKitAdapterShared &state, const LiveKitEvent &event);

} // namespace

struct LiveKitAdapterShared final {
	LiveKitAdapterDelegate *delegate = nullptr;
	LiveKitFfi *library = nullptr;
	LiveKitAdapter::Poster post;
	LiveKitEventRouter::RouteId routeId = 0;
	Phase phase = Phase::Idle;
	bool alive = true;
	bool disconnectRequested = false;
	bool microphoneRequested = false;
	bool cameraRequested = false;
	bool microphoneMuted = true;
	bool cameraEnabled = true;
	std::optional<std::uint64_t> connectAsyncId;
	std::optional<std::uint64_t> disconnectAsyncId;
	std::map<std::uint64_t, LiveKitPublishKind> publishAsyncIds;
	std::optional<std::uint64_t> roomHandle;
	std::optional<std::uint64_t> localParticipantHandle;
	std::optional<std::uint64_t> platformAudioHandle;
	std::optional<std::uint64_t> audioSourceHandle;
	std::optional<std::uint64_t> audioTrackHandle;
	std::optional<std::uint64_t> videoSourceHandle;
	std::optional<std::uint64_t> videoTrackHandle;
	std::vector<std::uint64_t> publicationHandles;
};

LiveKitAdapter::LiveKitAdapter(
		std::shared_ptr<LiveKitFfi> library,
		LiveKitAdapterDelegate &delegate,
		Poster post)
: ffi(std::move(library))
, shared(std::make_shared<LiveKitAdapterShared>()) {
	LiveKitAdapterShared &state = *shared;
	state.delegate = &delegate;
	state.library = ffi.get();
	state.post = std::move(post);
	if (!ffi->loaded()) {
		return;
	}
	// The route keeps the state alive until the room is retired, so events
	// that arrive after the adapter is destroyed still finish the disconnect.
	const std::shared_ptr<LiveKitAdapterShared> owner = shared;
	state.routeId = ffi->eventRouter().addRoute(
		[owner](const LiveKitEvent &event) {
			owner->post([owner, event]() {
				Dispatch(*owner, event);
			});
		});
}

LiveKitAdapter::~LiveKitAdapter() {
	LiveKitAdapterShared &state = *shared;
	state.alive = false;
	state.delegate = nullptr;
	if (!state.routeId) {
		return;
	}
	if (state.phase == Phase::Connecting) {
		// The room exists only when the library reports the connect, so the
		// disconnect waits for that event.
		state.disconnectRequested = true;
	} else if (state.phase == Phase::Connected) {
		SendDisconnect(state);
	} else if (state.phase == Phase::Idle || state.phase == Phase::Failed) {
		ReleaseAll(state);
		state.phase = Phase::Idle;
	}
	RetireIfDestroyed(state);
}

bool LiveKitAdapter::connect(const std::string &url, const std::string &token) {
	LiveKitAdapterShared &state = *shared;
	if (state.phase != Phase::Idle) {
		return false;
	}
	if (!state.routeId) {
		FailWith(state, kConnectFailed);
		return false;
	}
	const auto response = ffi->request(EncodeLiveKitConnectRequest(
		url,
		token,
		DefaultRoomOptions(),
		std::nullopt));
	std::optional<std::uint64_t> asyncId;
	if (response) {
		asyncId = DecodeLiveKitAsyncId(*response, kResponseConnect);
	}
	if (!asyncId) {
		FailWith(state, kConnectFailed);
		return false;
	}
	state.connectAsyncId = asyncId;
	AddKey(state, *asyncId);
	state.phase = Phase::Connecting;
	return true;
}

bool LiveKitAdapter::publishMicrophone() {
	LiveKitAdapterShared &state = *shared;
	if (state.phase != Phase::Connected || state.microphoneRequested) {
		return false;
	}
	state.microphoneRequested = true;

	// PlatformAudio is created first, so the ADM records the microphone.
	if (!state.platformAudioHandle) {
		state.platformAudioHandle = RequestHandle(
			state,
			EncodeLiveKitNewPlatformAudioRequest(),
			kResponseNewPlatformAudio);
	}
	if (!state.platformAudioHandle) {
		FailWith(state, kPublishFailed);
		return false;
	}
	state.audioSourceHandle = RequestHandle(
		state,
		EncodeLiveKitNewPlatformAudioSourceRequest(
			*state.platformAudioHandle),
		kResponseNewAudioSource);
	if (!state.audioSourceHandle) {
		FailWith(state, kPublishFailed);
		return false;
	}
	state.audioTrackHandle = RequestHandle(
		state,
		EncodeLiveKitCreateAudioTrackRequest(
			kMicrophoneName,
			*state.audioSourceHandle),
		kResponseCreateAudioTrack);
	if (!state.audioTrackHandle) {
		FailWith(state, kPublishFailed);
		return false;
	}
	// The track is muted before it is published, so no audio is sent while the
	// call starts muted.
	if (!SetTrackMuted(
			state,
			*state.audioTrackHandle,
			state.microphoneMuted)) {
		return false;
	}
	return PublishTrack(
		state,
		*state.audioTrackHandle,
		LiveKitTrackSource::Microphone,
		LiveKitPublishKind::Microphone);
}

bool LiveKitAdapter::publishCamera(std::uint32_t width, std::uint32_t height) {
	LiveKitAdapterShared &state = *shared;
	if (state.phase != Phase::Connected || state.cameraRequested) {
		return false;
	}
	state.cameraRequested = true;
	state.videoSourceHandle = RequestHandle(
		state,
		EncodeLiveKitNewVideoSourceRequest(width, height),
		kResponseNewVideoSource);
	if (!state.videoSourceHandle) {
		FailWith(state, kPublishFailed);
		return false;
	}
	state.videoTrackHandle = RequestHandle(
		state,
		EncodeLiveKitCreateVideoTrackRequest(
			kCameraName,
			*state.videoSourceHandle),
		kResponseCreateVideoTrack);
	if (!state.videoTrackHandle) {
		FailWith(state, kPublishFailed);
		return false;
	}
	if (!SetTrackMuted(
			state,
			*state.videoTrackHandle,
			!state.cameraEnabled)) {
		return false;
	}
	return PublishTrack(
		state,
		*state.videoTrackHandle,
		LiveKitTrackSource::Camera,
		LiveKitPublishKind::Camera);
}

bool LiveKitAdapter::setMicrophoneMuted(bool muted) {
	LiveKitAdapterShared &state = *shared;
	state.microphoneMuted = muted;
	if (!state.audioTrackHandle) {
		// Applied when the track is created and published.
		return true;
	}
	return SetTrackMuted(state, *state.audioTrackHandle, muted);
}

bool LiveKitAdapter::setCameraEnabled(bool enabled) {
	LiveKitAdapterShared &state = *shared;
	state.cameraEnabled = enabled;
	if (!state.videoTrackHandle) {
		// Applied when the camera track is published.
		return true;
	}
	return SetTrackMuted(state, *state.videoTrackHandle, !enabled);
}

void LiveKitAdapter::disconnect() {
	LiveKitAdapterShared &state = *shared;
	if (state.phase == Phase::Connecting) {
		state.disconnectRequested = true;
		return;
	}
	if (state.phase == Phase::Connected && state.roomHandle) {
		SendDisconnect(state);
		return;
	}
	if (state.phase == Phase::Idle || state.phase == Phase::Failed) {
		ReleaseAll(state);
		state.phase = Phase::Idle;
	}
}

bool LiveKitAdapter::connected() const {
	return shared->phase == Phase::Connected;
}

namespace {

std::optional<std::uint64_t> RequestHandle(
		LiveKitAdapterShared &state,
		const std::vector<std::uint8_t> &request,
		std::uint32_t responseField) {
	const auto response = state.library->request(request);
	if (!response) {
		return std::nullopt;
	}
	return DecodeLiveKitHandleResponse(*response, responseField).handle;
}

// Routes the library events of the key to this room. The keys are cleared
// when the room is released, so a closed room gets no events.
void AddKey(LiveKitAdapterShared &state, std::uint64_t key) {
	if (state.routeId) {
		state.library->eventRouter().addKey(state.routeId, key);
	}
}

void ReleaseAll(LiveKitAdapterShared &state) {
	LiveKitFfi &library = *state.library;
	for (const std::uint64_t handle : state.publicationHandles) {
		library.dropHandle(handle);
	}
	state.publicationHandles.clear();
	DropHandle(library, state.videoTrackHandle);
	DropHandle(library, state.videoSourceHandle);
	DropHandle(library, state.audioTrackHandle);
	DropHandle(library, state.audioSourceHandle);
	DropHandle(library, state.platformAudioHandle);
	DropHandle(library, state.localParticipantHandle);
	DropHandle(library, state.roomHandle);
	if (state.routeId) {
		library.eventRouter().clearKeys(state.routeId);
	}
	state.publishAsyncIds.clear();
	state.connectAsyncId.reset();
	state.disconnectAsyncId.reset();
	state.disconnectRequested = false;
	state.microphoneRequested = false;
	state.cameraRequested = false;
}

// Removes the route of a destroyed adapter once its room is gone, so the
// router keeps no state for it. A room that still waits for a connect or a
// disconnect keeps its route.
void RetireIfDestroyed(LiveKitAdapterShared &state) {
	if (state.alive
		|| !state.routeId
		|| state.connectAsyncId
		|| state.disconnectAsyncId) {
		return;
	}
	state.library->eventRouter().removeRoute(state.routeId);
	state.routeId = 0;
}

void FailWith(LiveKitAdapterShared &state, const char *message) {
	// A connected room stays online until it is disconnected, so it is left
	// before its handle is released.
	if (state.phase == Phase::Connected && state.roomHandle) {
		[[maybe_unused]] const auto response = state.library->request(
			EncodeLiveKitDisconnectRequest(*state.roomHandle, std::nullopt));
	}
	ReleaseAll(state);
	state.phase = Phase::Failed;
	if (state.delegate) {
		state.delegate->liveKitFailed(message);
	}
	RetireIfDestroyed(state);
}

void FinishDisconnected(LiveKitAdapterShared &state) {
	ReleaseAll(state);
	state.phase = Phase::Idle;
	if (state.delegate) {
		state.delegate->liveKitDisconnected();
	}
	RetireIfDestroyed(state);
}

void SendDisconnect(LiveKitAdapterShared &state) {
	const auto response = state.library->request(
		EncodeLiveKitDisconnectRequest(*state.roomHandle, std::nullopt));
	std::optional<std::uint64_t> asyncId;
	if (response) {
		asyncId = DecodeLiveKitAsyncId(*response, kResponseDisconnect);
	}
	if (!asyncId) {
		FinishDisconnected(state);
		return;
	}
	state.disconnectAsyncId = asyncId;
	AddKey(state, *asyncId);
	state.phase = Phase::Disconnecting;
}

bool PublishTrack(
		LiveKitAdapterShared &state,
		std::uint64_t trackHandle,
		LiveKitTrackSource source,
		LiveKitPublishKind kind) {
	const auto response = state.library->request(
		EncodeLiveKitPublishTrackRequest(
			*state.localParticipantHandle,
			trackHandle,
			source,
			std::nullopt));
	std::optional<std::uint64_t> asyncId;
	if (response) {
		asyncId = DecodeLiveKitAsyncId(*response, kResponsePublishTrack);
	}
	if (!asyncId) {
		FailWith(state, kPublishFailed);
		return false;
	}
	state.publishAsyncIds.emplace(*asyncId, kind);
	AddKey(state, *asyncId);
	return true;
}

bool SetTrackMuted(
		LiveKitAdapterShared &state,
		std::uint64_t trackHandle,
		bool muted) {
	const auto response = state.library->request(
		EncodeLiveKitLocalTrackMuteRequest(trackHandle, muted));
	const std::optional<bool> confirmed = response
		? DecodeLiveKitLocalTrackMuteResponse(*response)
		: std::nullopt;
	if (!confirmed || *confirmed != muted) {
		// The track may still send, so the room is left instead of kept live.
		FailWith(state, kMuteFailed);
		return false;
	}
	return true;
}

void DispatchConnect(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	if (!state.connectAsyncId || *state.connectAsyncId != event.asyncId) {
		return;
	}
	state.connectAsyncId.reset();
	// Handles are stored before the check so that a failed connect still
	// releases the room the library created.
	state.roomHandle = event.roomHandle;
	state.localParticipantHandle = event.localParticipantHandle;
	if (!event.error.empty()
		|| !state.roomHandle
		|| !state.localParticipantHandle) {
		if (state.disconnectRequested) {
			FinishDisconnected(state);
		} else {
			FailWith(state, kConnectFailed);
		}
		return;
	}
	AddKey(state, *state.roomHandle);
	AddKey(state, *state.localParticipantHandle);
	state.phase = Phase::Connected;
	if (state.disconnectRequested) {
		state.disconnectRequested = false;
		SendDisconnect(state);
		return;
	}
	if (state.delegate) {
		state.delegate->liveKitConnected();
	}
}

void DispatchPublish(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	const std::map<std::uint64_t, LiveKitPublishKind>::iterator it =
		state.publishAsyncIds.find(event.asyncId);
	if (it == state.publishAsyncIds.end()) {
		return;
	}
	const LiveKitPublishKind kind = it->second;
	state.publishAsyncIds.erase(it);
	// A publish that ends while the room leaves is dropped with the room.
	if (state.phase != Phase::Connected) {
		return;
	}
	if (!event.error.empty() || !event.publicationHandle) {
		FailWith(state, kPublishFailed);
		return;
	}
	AddKey(state, *event.publicationHandle);
	state.publicationHandles.push_back(*event.publicationHandle);
	if (state.delegate) {
		state.delegate->liveKitPublished(kind);
	}
}

void DispatchDisconnect(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	if (!state.disconnectAsyncId || *state.disconnectAsyncId != event.asyncId) {
		return;
	}
	state.disconnectAsyncId.reset();
	FinishDisconnected(state);
}

void Dispatch(LiveKitAdapterShared &state, const LiveKitEvent &event) {
	if (event.kind == LiveKitEventKind::Connect) {
		DispatchConnect(state, event);
	} else if (event.kind == LiveKitEventKind::PublishTrack) {
		DispatchPublish(state, event);
	} else if (event.kind == LiveKitEventKind::Disconnect) {
		DispatchDisconnect(state, event);
	}
}

} // namespace
} // namespace Impulse
