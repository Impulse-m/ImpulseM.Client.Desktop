#include "calls/livekit/livekit_adapter.h"

#include "calls/livekit/livekit_event_router.h"
#include "calls/livekit/livekit_ffi_codec.h"
#include "calls/livekit/livekit_ffi_loader.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Impulse {

// A remote track of the room. The track handle is kept until the track is
// unsubscribed, and the stream handle is the video stream of a video track.
struct LiveKitRemoteTrack final {
	std::string participantIdentity;
	LiveKitTrackKind kind = LiveKitTrackKind::Unknown;
	std::optional<std::uint64_t> trackHandle;
	std::optional<std::uint64_t> streamHandle;
};

namespace {

// FfiResponse field numbers of the requests the adapter sends (livekit-ffi
// protocol, FfiResponse oneof).
constexpr std::uint32_t kResponseConnect = 3;
constexpr std::uint32_t kResponseDisconnect = 4;
constexpr std::uint32_t kResponsePublishTrack = 5;
constexpr std::uint32_t kResponseCreateVideoTrack = 15;
constexpr std::uint32_t kResponseCreateAudioTrack = 16;
constexpr std::uint32_t kResponseNewVideoStream = 20;
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
constexpr const char *kConnectionLost = "LiveKit disconnected";
constexpr const char *kEncryptionFailed = "LiveKit media encryption failed";

// The async ids of the requests that the adapter sends carry the top bit. The
// library counts its own ids up from 1, and the router keys handles and async
// ids in one map, so the two sets never meet.
constexpr std::uint64_t kClientAsyncIdFlag = std::uint64_t(1) << 63;

// VideoBufferType.I420 and the three planes of an I420 buffer.
constexpr std::uint32_t kBufferTypeI420 = 5;
constexpr std::size_t kI420PlaneCount = 3;

// A plane larger than this is not a frame of a call; it is dropped.
constexpr std::uint32_t kMaxPlaneBytes = 64u * 1024u * 1024u;

// Auto-subscribe stays on, as in Android's room defaults, so the library
// subscribes to every remote track and reports it as a track event.
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

// Returns the async id for the next request. The id goes into the request, so
// its route key is registered before the library can answer.
[[nodiscard]] std::uint64_t NextRequestAsyncId() {
	static std::atomic<std::uint64_t> last{0};
	return kClientAsyncIdFlag | (last.fetch_add(1) + 1);
}

// True when the library answered the request with the async id the adapter
// sent. The library uses the id of the request as the id of the response.
[[nodiscard]] bool RequestAccepted(
		const std::optional<std::vector<std::uint8_t>> &response,
		std::uint32_t responseField,
		std::uint64_t asyncId) {
	if (!response) {
		return false;
	}
	const std::optional<std::uint64_t> echoed = DecodeLiveKitAsyncId(
		*response,
		responseField);
	return echoed && *echoed == asyncId;
}

[[nodiscard]] bool IsEncryptionFailure(LiveKitEncryptionState state) {
	return state == LiveKitEncryptionState::EncryptionFailed
		|| state == LiveKitEncryptionState::DecryptionFailed
		|| state == LiveKitEncryptionState::MissingKey
		|| state == LiveKitEncryptionState::InternalError;
}

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

void RemoveKey(LiveKitAdapterShared &state, std::uint64_t key);

void ReleaseAll(LiveKitAdapterShared &state);

void ReleaseRemoteTrack(
		LiveKitAdapterShared &state,
		const LiveKitRemoteTrack &track);

void RetireIfDestroyed(LiveKitAdapterShared &state);

void FailWith(LiveKitAdapterShared &state, const char *message);

void FinishDisconnected(LiveKitAdapterShared &state);

void SendDisconnect(LiveKitAdapterShared &state);

bool StartSession(LiveKitAdapterShared &state);

bool PublishTrack(
		LiveKitAdapterShared &state,
		std::uint64_t trackHandle,
		LiveKitTrackSource source,
		LiveKitPublishKind kind);

bool SetTrackMuted(
		LiveKitAdapterShared &state,
		std::uint64_t trackHandle,
		bool muted);

std::optional<LiveKitVideoFrame> CopyVideoFrame(
		LiveKitFfi &library,
		const LiveKitVideoBuffer &buffer);

void RouteVideoFrame(
		const std::shared_ptr<LiveKitAdapterShared> &owner,
		const LiveKitEvent &event);

void DispatchConnect(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event);

void DispatchPublish(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event);

void DispatchDisconnect(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event);

void DispatchTrackSubscribed(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event);

void DispatchTrackUnsubscribed(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event);

void DispatchVideoFrame(
		LiveKitAdapterShared &state,
		std::uint64_t streamHandle,
		LiveKitVideoFrame frame);

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
	std::vector<std::uint8_t> sharedKey;
	LiveKitVideoSink *videoSink = nullptr;
	std::optional<std::uint64_t> connectAsyncId;
	std::optional<std::uint64_t> disconnectAsyncId;
	std::map<std::uint64_t, LiveKitPublishKind> publishAsyncIds;
	std::map<std::string, LiveKitRemoteTrack> remoteTracks;
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
			if (event.kind == LiveKitEventKind::VideoFrame) {
				RouteVideoFrame(owner, event);
				return;
			}
			owner->post([owner, event]() {
				Dispatch(*owner, event);
			});
		});
}

LiveKitAdapter::~LiveKitAdapter() {
	LiveKitAdapterShared &state = *shared;
	state.alive = false;
	state.delegate = nullptr;
	state.videoSink = nullptr;
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

void LiveKitAdapter::setSharedKey(std::vector<std::uint8_t> key) {
	shared->sharedKey = std::move(key);
}

void LiveKitAdapter::setVideoSink(LiveKitVideoSink *sink) {
	shared->videoSink = sink;
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
	std::optional<LiveKitEncryptionOptions> encryption;
	if (!state.sharedKey.empty()) {
		LiveKitEncryptionOptions options;
		options.sharedKey = state.sharedKey;
		encryption = std::move(options);
	}
	const std::uint64_t asyncId = NextRequestAsyncId();
	AddKey(state, asyncId);
	const std::optional<std::vector<std::uint8_t>> response = ffi->request(
		EncodeLiveKitConnectRequest(
			url,
			token,
			DefaultRoomOptions(),
			asyncId,
			encryption));
	if (!RequestAccepted(response, kResponseConnect, asyncId)) {
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

	// PlatformAudio is created when the room connects, so this reuses it. It is
	// created here only when the connect step did not leave one.
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
	const std::optional<std::vector<std::uint8_t>> response =
		state.library->request(request);
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

void RemoveKey(LiveKitAdapterShared &state, std::uint64_t key) {
	if (state.routeId) {
		state.library->eventRouter().removeKey(state.routeId, key);
	}
}

void ReleaseAll(LiveKitAdapterShared &state) {
	LiveKitFfi &library = *state.library;
	for (const std::pair<const std::string, LiveKitRemoteTrack> &entry
			: state.remoteTracks) {
		ReleaseRemoteTrack(state, entry.second);
	}
	state.remoteTracks.clear();
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

// Releases the stream and the handle of a remote track. The stream key is
// removed first, so no frame of the stream is routed after its handle is gone.
void ReleaseRemoteTrack(
		LiveKitAdapterShared &state,
		const LiveKitRemoteTrack &track) {
	if (track.streamHandle) {
		RemoveKey(state, *track.streamHandle);
		state.library->dropHandle(*track.streamHandle);
	}
	if (track.trackHandle) {
		state.library->dropHandle(*track.trackHandle);
	}
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
		[[maybe_unused]] const std::optional<std::vector<std::uint8_t>> response =
			state.library->request(
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
	const std::uint64_t asyncId = NextRequestAsyncId();
	AddKey(state, asyncId);
	const std::optional<std::vector<std::uint8_t>> response =
		state.library->request(
			EncodeLiveKitDisconnectRequest(*state.roomHandle, asyncId));
	if (!RequestAccepted(response, kResponseDisconnect, asyncId)) {
		FinishDisconnected(state);
		return;
	}
	state.disconnectAsyncId = asyncId;
	state.phase = Phase::Disconnecting;
}

// Runs once the room is connected. PlatformAudio plays the remote audio, so it
// is created before the delegate hears of the connection. The library is then
// told that the room events are read: it holds them, the track subscriptions
// included, until this request. Returns false when a step fails, and the room
// is then failed.
bool StartSession(LiveKitAdapterShared &state) {
	if (!state.platformAudioHandle) {
		state.platformAudioHandle = RequestHandle(
			state,
			EncodeLiveKitNewPlatformAudioRequest(),
			kResponseNewPlatformAudio);
	}
	if (!state.platformAudioHandle) {
		FailWith(state, kConnectFailed);
		return false;
	}
	const std::optional<std::vector<std::uint8_t>> response =
		state.library->request(
			EncodeLiveKitReadyForRoomEventRequest(*state.roomHandle));
	if (!response) {
		FailWith(state, kConnectFailed);
		return false;
	}
	return true;
}

bool PublishTrack(
		LiveKitAdapterShared &state,
		std::uint64_t trackHandle,
		LiveKitTrackSource source,
		LiveKitPublishKind kind) {
	const std::uint64_t asyncId = NextRequestAsyncId();
	AddKey(state, asyncId);
	const std::optional<std::vector<std::uint8_t>> response =
		state.library->request(
			EncodeLiveKitPublishTrackRequest(
				*state.localParticipantHandle,
				trackHandle,
				source,
				asyncId));
	if (!RequestAccepted(response, kResponsePublishTrack, asyncId)) {
		FailWith(state, kPublishFailed);
		return false;
	}
	state.publishAsyncIds.emplace(asyncId, kind);
	return true;
}

bool SetTrackMuted(
		LiveKitAdapterShared &state,
		std::uint64_t trackHandle,
		bool muted) {
	const std::optional<std::vector<std::uint8_t>> response =
		state.library->request(
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

// Copies the planes of an I420 buffer and releases the buffer handle. The
// copy is made on the library thread, where the buffer is valid. A buffer of
// another type or with a plane that cannot be read gives no frame.
std::optional<LiveKitVideoFrame> CopyVideoFrame(
		LiveKitFfi &library,
		const LiveKitVideoBuffer &buffer) {
	std::optional<LiveKitVideoFrame> result;
	const bool shaped = buffer.type == kBufferTypeI420
		&& buffer.components.size() == kI420PlaneCount
		&& buffer.width > 0
		&& buffer.height > 0;
	if (shaped) {
		LiveKitVideoFrame frame;
		frame.width = buffer.width;
		frame.height = buffer.height;
		frame.timestampUs = buffer.timestampUs;
		bool readable = true;
		for (std::size_t index = 0; index < kI420PlaneCount; ++index) {
			const LiveKitVideoComponent &component = buffer.components[index];
			if (!component.dataPtr
				|| !component.size
				|| component.size > kMaxPlaneBytes) {
				readable = false;
				break;
			}
			LiveKitVideoPlane &plane = frame.planes[index];
			plane.stride = component.stride;
			plane.bytes.resize(component.size);
			std::memcpy(
				plane.bytes.data(),
				reinterpret_cast<const void *>(
					static_cast<std::uintptr_t>(component.dataPtr)),
				component.size);
		}
		if (readable) {
			result = std::move(frame);
		}
	}
	if (buffer.bufferHandle) {
		library.dropHandle(buffer.bufferHandle);
	}
	return result;
}

// Hands a frame of a video stream to the owner thread. It runs on the library
// thread, because the frame data is valid only there.
void RouteVideoFrame(
		const std::shared_ptr<LiveKitAdapterShared> &owner,
		const LiveKitEvent &event) {
	if (!event.videoBuffer) {
		return;
	}
	std::optional<LiveKitVideoFrame> frame = CopyVideoFrame(
		*owner->library,
		*event.videoBuffer);
	if (!frame || !event.streamHandle) {
		return;
	}
	const std::uint64_t streamHandle = *event.streamHandle;
	LiveKitVideoFrame copy = std::move(*frame);
	owner->post([owner, streamHandle, copy]() mutable {
		DispatchVideoFrame(*owner, streamHandle, std::move(copy));
	});
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
	if (!StartSession(state)) {
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
	// A publish that ends while the room leaves is dropped with the room. Its
	// publication handle is owned by the library, so it is released here.
	if (state.phase != Phase::Connected) {
		if (event.publicationHandle) {
			state.library->dropHandle(*event.publicationHandle);
		}
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

// A room that the library drops for any reason other than this client's own
// leave ends the call, as Android does. The room event of the disconnect that
// the adapter asked for is ignored; its disconnect callback finishes the leave.
void DispatchRoomDisconnected(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	if (state.phase != Phase::Connected || event.clientInitiated) {
		return;
	}
	FailWith(state, kConnectionLost);
}

// Any E2EE failure state of a track ends the call, as Android does.
void DispatchEncryptionState(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	if (state.phase != Phase::Connected
		|| !IsEncryptionFailure(event.encryptionState)) {
		return;
	}
	FailWith(state, kEncryptionFailed);
}

// A remote track is subscribed by the library. Audio is played by
// PlatformAudio, so only its handle is kept. A video track opens a native
// video stream whose frames go to the sink.
void DispatchTrackSubscribed(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	if (!event.trackHandle) {
		return;
	}
	// A track that arrives outside a connected room is released at once, as
	// the room that owns it is leaving.
	if (state.phase != Phase::Connected) {
		state.library->dropHandle(*event.trackHandle);
		return;
	}
	const std::map<std::string, LiveKitRemoteTrack>::iterator previous =
		state.remoteTracks.find(event.trackSid);
	if (previous != state.remoteTracks.end()) {
		ReleaseRemoteTrack(state, previous->second);
		state.remoteTracks.erase(previous);
	}
	LiveKitRemoteTrack track;
	track.participantIdentity = event.participantIdentity;
	track.kind = event.trackKind;
	track.trackHandle = event.trackHandle;
	if (event.trackKind == LiveKitTrackKind::Video) {
		track.streamHandle = RequestHandle(
			state,
			EncodeLiveKitNewVideoStreamRequest(*event.trackHandle),
			kResponseNewVideoStream);
		if (track.streamHandle) {
			AddKey(state, *track.streamHandle);
		}
	}
	state.remoteTracks.emplace(event.trackSid, std::move(track));
}

void DispatchTrackUnsubscribed(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	const std::map<std::string, LiveKitRemoteTrack>::iterator found =
		state.remoteTracks.find(event.trackSid);
	if (found == state.remoteTracks.end()) {
		return;
	}
	ReleaseRemoteTrack(state, found->second);
	state.remoteTracks.erase(found);
}

void DispatchVideoFrame(
		LiveKitAdapterShared &state,
		std::uint64_t streamHandle,
		LiveKitVideoFrame frame) {
	if (state.phase != Phase::Connected || !state.videoSink) {
		return;
	}
	for (const std::pair<const std::string, LiveKitRemoteTrack> &entry
			: state.remoteTracks) {
		const LiveKitRemoteTrack &track = entry.second;
		if (track.streamHandle && *track.streamHandle == streamHandle) {
			frame.participantIdentity = track.participantIdentity;
			state.videoSink->liveKitVideoFrame(frame);
			return;
		}
	}
}

void Dispatch(LiveKitAdapterShared &state, const LiveKitEvent &event) {
	if (event.kind == LiveKitEventKind::Connect) {
		DispatchConnect(state, event);
	} else if (event.kind == LiveKitEventKind::PublishTrack) {
		DispatchPublish(state, event);
	} else if (event.kind == LiveKitEventKind::Disconnect) {
		DispatchDisconnect(state, event);
	} else if (event.kind == LiveKitEventKind::TrackSubscribed) {
		DispatchTrackSubscribed(state, event);
	} else if (event.kind == LiveKitEventKind::TrackUnsubscribed) {
		DispatchTrackUnsubscribed(state, event);
	} else if (event.kind == LiveKitEventKind::RoomDisconnected) {
		DispatchRoomDisconnected(state, event);
	} else if (event.kind == LiveKitEventKind::EncryptionStateChanged) {
		DispatchEncryptionState(state, event);
	}
}

} // namespace
} // namespace Impulse
