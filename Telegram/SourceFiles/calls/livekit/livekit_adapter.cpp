#include "calls/livekit/livekit_adapter.h"

#ifdef _WIN32

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

constexpr auto kSdkName = "impulsem-desktop";
constexpr auto kSdkVersion = "1.0";
constexpr auto kMicrophoneName = "microphone";
constexpr auto kCameraName = "camera";

// Error texts are fixed. SDK messages can contain the authenticated WebSocket
// URL and are never forwarded.
constexpr auto kConnectFailed = "LiveKit connection failed";
constexpr auto kPublishFailed = "LiveKit publish failed";
constexpr auto kMuteFailed = "LiveKit mute failed";

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
		LiveKitFfiLoader &loader,
		std::optional<std::uint64_t> &handle) {
	if (handle) {
		loader.dropHandle(*handle);
		handle.reset();
	}
}

std::optional<std::uint64_t> RequestHandle(
		LiveKitAdapterShared &state,
		const std::vector<std::uint8_t> &request,
		std::uint32_t responseField);

void ReleaseAll(LiveKitAdapterShared &state);

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
	LiveKitFfiLoader *loader = nullptr;
	LiveKitAdapter::Poster post;
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
		const std::wstring &dllPath,
		LiveKitAdapterDelegate &delegate,
		Poster post)
: loader(std::make_unique<LiveKitFfiLoader>(dllPath))
, shared(std::make_shared<LiveKitAdapterShared>()) {
	shared->delegate = &delegate;
	shared->loader = loader.get();
	shared->post = std::move(post);
	if (!loader->loaded()) {
		return;
	}
	std::shared_ptr<LiveKitAdapterShared> state = shared;
	loader->initialize(
		[state](std::vector<std::uint8_t> bytes) {
			LiveKitEvent event = DecodeLiveKitEvent(bytes);
			if (event.kind == LiveKitEventKind::Unknown) {
				return;
			}
			state->post([state, event = std::move(event)]() {
				Dispatch(*state, event);
			});
		},
		false,
		std::string(kSdkName),
		std::string(kSdkVersion));
}

LiveKitAdapter::~LiveKitAdapter() {
	LiveKitAdapterShared &state = *shared;
	if (state.phase == Phase::Connected && state.roomHandle) {
		[[maybe_unused]] const auto response = loader->request(
			EncodeLiveKitDisconnectRequest(*state.roomHandle, std::nullopt));
	}
	state.alive = false;
	state.delegate = nullptr;
	ReleaseAll(state);
	loader->dispose();
}

bool LiveKitAdapter::connect(const std::string &url, const std::string &token) {
	LiveKitAdapterShared &state = *shared;
	if (state.phase != Phase::Idle) {
		return false;
	}
	if (!loader->loaded()) {
		FailWith(state, kConnectFailed);
		return false;
	}
	const auto response = loader->request(EncodeLiveKitConnectRequest(
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
	const auto response = state.loader->request(request);
	if (!response) {
		return std::nullopt;
	}
	return DecodeLiveKitHandleResponse(*response, responseField).handle;
}

void ReleaseAll(LiveKitAdapterShared &state) {
	LiveKitFfiLoader &loader = *state.loader;
	for (const std::uint64_t handle : state.publicationHandles) {
		loader.dropHandle(handle);
	}
	state.publicationHandles.clear();
	DropHandle(loader, state.videoTrackHandle);
	DropHandle(loader, state.videoSourceHandle);
	DropHandle(loader, state.audioTrackHandle);
	DropHandle(loader, state.audioSourceHandle);
	DropHandle(loader, state.platformAudioHandle);
	DropHandle(loader, state.localParticipantHandle);
	DropHandle(loader, state.roomHandle);
	state.publishAsyncIds.clear();
	state.connectAsyncId.reset();
	state.disconnectAsyncId.reset();
	state.disconnectRequested = false;
	state.microphoneRequested = false;
	state.cameraRequested = false;
}

void FailWith(LiveKitAdapterShared &state, const char *message) {
	// A connected room stays online until it is disconnected, so it is left
	// before its handle is released.
	if (state.phase == Phase::Connected && state.roomHandle && state.loader) {
		[[maybe_unused]] const auto response = state.loader->request(
			EncodeLiveKitDisconnectRequest(*state.roomHandle, std::nullopt));
	}
	ReleaseAll(state);
	state.phase = Phase::Failed;
	if (state.delegate) {
		state.delegate->liveKitFailed(message);
	}
}

void FinishDisconnected(LiveKitAdapterShared &state) {
	ReleaseAll(state);
	state.phase = Phase::Idle;
	if (state.delegate) {
		state.delegate->liveKitDisconnected();
	}
}

void SendDisconnect(LiveKitAdapterShared &state) {
	const auto response = state.loader->request(
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
	state.phase = Phase::Disconnecting;
}

bool PublishTrack(
		LiveKitAdapterShared &state,
		std::uint64_t trackHandle,
		LiveKitTrackSource source,
		LiveKitPublishKind kind) {
	const auto response = state.loader->request(
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
	return true;
}

bool SetTrackMuted(
		LiveKitAdapterShared &state,
		std::uint64_t trackHandle,
		bool muted) {
	const auto response = state.loader->request(
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
	const auto it = state.publishAsyncIds.find(event.asyncId);
	if (it == state.publishAsyncIds.end()) {
		return;
	}
	const LiveKitPublishKind kind = it->second;
	state.publishAsyncIds.erase(it);
	if (!event.error.empty() || !event.publicationHandle) {
		FailWith(state, kPublishFailed);
		return;
	}
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
	if (!state.alive) {
		return;
	}
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

#endif // _WIN32
