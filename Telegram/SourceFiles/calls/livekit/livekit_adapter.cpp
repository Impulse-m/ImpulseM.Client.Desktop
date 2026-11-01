#include "calls/livekit/livekit_adapter.h"

#include "calls/livekit/livekit_event_router.h"
#include "calls/livekit/livekit_ffi_codec.h"
#include "calls/livekit/livekit_ffi_loader.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
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

// A remote publication of the room, kept by its sid. The handle is owned by the
// adapter until the publication is unpublished or the room is released.
struct LiveKitRemotePublication final {
	std::string participantIdentity;
	LiveKitTrackKind kind = LiveKitTrackKind::Unknown;
	std::uint64_t handle = 0;
};

namespace {

// FfiResponse field numbers of the requests the adapter sends (livekit-ffi
// protocol, FfiResponse oneof).
constexpr std::uint32_t kResponseConnect = 3;
constexpr std::uint32_t kResponseDisconnect = 4;
constexpr std::uint32_t kResponsePublishTrack = 5;
constexpr std::uint32_t kResponseGetSessionStats = 12;
constexpr std::uint32_t kResponseCreateVideoTrack = 15;
constexpr std::uint32_t kResponseCreateAudioTrack = 16;
constexpr std::uint32_t kResponseNewVideoStream = 20;
constexpr std::uint32_t kResponseNewVideoSource = 21;
constexpr std::uint32_t kResponseNewAudioSource = 26;
constexpr std::uint32_t kResponseNewPlatformAudio = 76;
constexpr std::uint32_t kResponseSetRecordingDevice = 78;
constexpr std::uint32_t kResponseSetPlayoutDevice = 79;

constexpr const char *kMicrophoneName = "microphone";
constexpr const char *kCameraName = "camera";
constexpr const char *kScreenShareName = "screen";

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

// The names that the diagnostic lines use. They carry no SDK text.
[[nodiscard]] const char *PublishKindName(LiveKitPublishKind kind) {
	if (kind == LiveKitPublishKind::Microphone) {
		return "microphone";
	} else if (kind == LiveKitPublishKind::Camera) {
		return "camera";
	}
	return "screen share";
}

[[nodiscard]] const char *KindName(LiveKitTrackKind kind) {
	if (kind == LiveKitTrackKind::Audio) {
		return "audio";
	} else if (kind == LiveKitTrackKind::Video) {
		return "video";
	}
	return "unknown";
}

[[nodiscard]] const char *SourceName(LiveKitTrackSource source) {
	if (source == LiveKitTrackSource::Camera) {
		return "camera";
	} else if (source == LiveKitTrackSource::Microphone) {
		return "microphone";
	} else if (source == LiveKitTrackSource::ScreenShare) {
		return "screen share";
	} else if (source == LiveKitTrackSource::ScreenShareAudio) {
		return "screen share audio";
	}
	return "unknown";
}

[[nodiscard]] const char *EncryptionStateName(LiveKitEncryptionState state) {
	if (state == LiveKitEncryptionState::New) {
		return "new";
	} else if (state == LiveKitEncryptionState::Ok) {
		return "ok";
	} else if (state == LiveKitEncryptionState::EncryptionFailed) {
		return "encryption failed";
	} else if (state == LiveKitEncryptionState::DecryptionFailed) {
		return "decryption failed";
	} else if (state == LiveKitEncryptionState::MissingKey) {
		return "missing key";
	} else if (state == LiveKitEncryptionState::KeyRatcheted) {
		return "key ratcheted";
	} else if (state == LiveKitEncryptionState::InternalError) {
		return "internal error";
	}
	return "unknown";
}

// The endpoint ids of the platform audio are compared without the case.
[[nodiscard]] bool EqualsIgnoreCase(
		const std::string &left,
		const std::string &right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); ++index) {
		const int leftSymbol = std::tolower(static_cast<unsigned char>(left[index]));
		const int rightSymbol = std::tolower(static_cast<unsigned char>(right[index]));
		if (leftSymbol != rightSymbol) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::string DescribeMedia(const LiveKitRemoteMedia &media) {
	return media.participantIdentity
		+ ", "
		+ KindName(media.kind)
		+ ", "
		+ SourceName(media.source);
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

void SendLog(LiveKitAdapterShared &state, const std::string &line);

void NotifyRemoteMedia(LiveKitAdapterShared &state);

void ReleaseAll(LiveKitAdapterShared &state);

void ReleaseRemoteTrack(
		LiveKitAdapterShared &state,
		const LiveKitRemoteTrack &track);

void RetireIfDestroyed(LiveKitAdapterShared &state);

void FailWith(LiveKitAdapterShared &state, const char *message);

void FinishDisconnected(LiveKitAdapterShared &state);

void SendDisconnect(LiveKitAdapterShared &state);

bool CreatePlatformAudio(LiveKitAdapterShared &state);

bool ApplyAudioDevice(
		LiveKitAdapterShared &state,
		const std::string &deviceId,
		bool playout);

void ApplyStoredAudioDevices(LiveKitAdapterShared &state);

void RefreshShownStream(LiveKitAdapterShared &state);

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

bool IsShownStream(
		const LiveKitAdapterShared &state,
		std::uint64_t streamHandle);

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

void DispatchTrackPublished(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event);

void DispatchTrackUnpublished(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event);

void DispatchTrackMute(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event,
		bool muted);

void DispatchParticipantDisconnected(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event);

void DispatchSessionStats(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event);

bool RequestRemoteQuality(
		LiveKitAdapterShared &state,
		std::uint64_t publicationHandle,
		LiveKitVideoQuality quality);

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
	bool screenRequested = false;
	bool microphoneMuted = true;
	bool cameraEnabled = true;
	bool screenShareEnabled = true;
	bool noiseSuppression = true;
	std::vector<std::uint8_t> sharedKey;
	std::string playoutDeviceId;
	std::string recordingDeviceId;
	// A device is selected when a non-empty id was applied, so an empty id after
	// a selection can switch back to the default device.
	bool playoutDeviceSelected = false;
	bool recordingDeviceSelected = false;
	LiveKitVideoSink *videoSink = nullptr;
	// The shown track of the video frames. The owner thread sets the sid, and the
	// stream handle of its stream is published to the library thread, which
	// drops the frames of any other stream before they are copied.
	bool shownFilterSet = false;
	std::string shownTrackSid;
	std::atomic<bool> videoFilterOn{false};
	std::atomic<std::uint64_t> shownStreamHandle{0};
	std::optional<std::uint64_t> connectAsyncId;
	std::optional<std::uint64_t> disconnectAsyncId;
	std::map<std::uint64_t, LiveKitPublishKind> publishAsyncIds;
	std::set<std::uint64_t> statsAsyncIds;
	std::map<std::string, LiveKitRemoteTrack> remoteTracks;
	std::map<std::string, LiveKitRemotePublication> remotePublications;
	std::map<std::string, LiveKitRemoteMedia> remoteMedia;
	std::map<std::string, LiveKitVideoQuality> videoQualities;
	std::optional<std::uint64_t> roomHandle;
	std::optional<std::uint64_t> localParticipantHandle;
	std::optional<std::uint64_t> platformAudioHandle;
	std::optional<std::uint64_t> audioSourceHandle;
	std::optional<std::uint64_t> audioTrackHandle;
	std::optional<std::uint64_t> videoSourceHandle;
	std::optional<std::uint64_t> videoTrackHandle;
	std::optional<std::uint64_t> screenSourceHandle;
	std::optional<std::uint64_t> screenTrackHandle;
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

void LiveKitAdapter::setShownVideoTrack(const std::string &trackSid) {
	LiveKitAdapterShared &state = *shared;
	state.shownFilterSet = true;
	state.shownTrackSid = trackSid;
	RefreshShownStream(state);
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
	SendLog(state, "connect requested");
	const std::uint64_t asyncId = NextRequestAsyncId();
	AddKey(state, asyncId);
	const std::optional<std::vector<std::uint8_t>> response = state.library->request(
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
	if (!state.platformAudioHandle && !CreatePlatformAudio(state)) {
		FailWith(state, kPublishFailed);
		return false;
	}
	state.audioSourceHandle = RequestHandle(
		state,
		EncodeLiveKitNewPlatformAudioSourceRequest(
			*state.platformAudioHandle,
			state.noiseSuppression),
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
		EncodeLiveKitNewVideoSourceRequest(width, height, false),
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

bool LiveKitAdapter::publishScreenShare(
		std::uint32_t width,
		std::uint32_t height) {
	LiveKitAdapterShared &state = *shared;
	if (state.phase != Phase::Connected || state.screenRequested) {
		return false;
	}
	state.screenRequested = true;
	state.screenSourceHandle = RequestHandle(
		state,
		EncodeLiveKitNewVideoSourceRequest(width, height, true),
		kResponseNewVideoSource);
	if (!state.screenSourceHandle) {
		FailWith(state, kPublishFailed);
		return false;
	}
	state.screenTrackHandle = RequestHandle(
		state,
		EncodeLiveKitCreateVideoTrackRequest(
			kScreenShareName,
			*state.screenSourceHandle),
		kResponseCreateVideoTrack);
	if (!state.screenTrackHandle) {
		FailWith(state, kPublishFailed);
		return false;
	}
	// A track starts enabled, so only a screen share that was disabled before
	// the publish is muted before it is published.
	if (!state.screenShareEnabled
		&& !SetTrackMuted(state, *state.screenTrackHandle, true)) {
		return false;
	}
	return PublishTrack(
		state,
		*state.screenTrackHandle,
		LiveKitTrackSource::ScreenShare,
		LiveKitPublishKind::ScreenShare);
}

bool LiveKitAdapter::captureVideoFrame(
		LiveKitPublishKind kind,
		const LiveKitCapturedFrame &frame) {
	LiveKitAdapterShared &state = *shared;
	if (state.phase != Phase::Connected) {
		return false;
	}
	std::optional<std::uint64_t> source;
	if (kind == LiveKitPublishKind::Camera) {
		source = state.videoSourceHandle;
	} else if (kind == LiveKitPublishKind::ScreenShare) {
		source = state.screenSourceHandle;
	}
	if (!source || !frame.width || !frame.height) {
		return false;
	}
	LiveKitCaptureFrame request;
	request.width = frame.width;
	request.height = frame.height;
	request.timestampUs = frame.timestampUs;
	for (const LiveKitVideoPlane &plane : frame.planes) {
		if (plane.bytes.empty() || plane.bytes.size() > kMaxPlaneBytes) {
			return false;
		}
		LiveKitVideoComponent component;
		component.dataPtr = static_cast<std::uint64_t>(
			reinterpret_cast<std::uintptr_t>(plane.bytes.data()));
		component.stride = plane.stride;
		component.size = static_cast<std::uint32_t>(plane.bytes.size());
		request.components.push_back(component);
	}
	// The request is synchronous, so the planes are read while the frame is
	// still alive.
	const std::optional<std::vector<std::uint8_t>> response = state.library->request(
		EncodeLiveKitCaptureVideoFrameRequest(*source, request));
	return response && DecodeLiveKitCaptureVideoFrameResponse(*response);
}

bool LiveKitAdapter::setMicrophoneMuted(bool muted) {
	LiveKitAdapterShared &state = *shared;
	if (state.microphoneMuted != muted) {
		SendLog(state, muted ? "local mute: microphone muted" : "local mute: microphone unmuted");
	}
	state.microphoneMuted = muted;
	if (!state.audioTrackHandle) {
		// Applied when the track is created and published.
		return true;
	}
	return SetTrackMuted(state, *state.audioTrackHandle, muted);
}

bool LiveKitAdapter::setCameraEnabled(bool enabled) {
	LiveKitAdapterShared &state = *shared;
	if (state.cameraEnabled != enabled) {
		SendLog(state, enabled ? "local mute: camera unmuted" : "local mute: camera muted");
	}
	state.cameraEnabled = enabled;
	if (!state.videoTrackHandle) {
		// Applied when the camera track is published.
		return true;
	}
	return SetTrackMuted(state, *state.videoTrackHandle, !enabled);
}

bool LiveKitAdapter::setScreenShareEnabled(bool enabled) {
	LiveKitAdapterShared &state = *shared;
	if (state.screenShareEnabled != enabled) {
		SendLog(state, enabled ? "local mute: screen share unmuted" : "local mute: screen share muted");
	}
	state.screenShareEnabled = enabled;
	if (!state.screenTrackHandle) {
		// Applied when the screen share track is published.
		return true;
	}
	return SetTrackMuted(state, *state.screenTrackHandle, !enabled);
}

bool LiveKitAdapter::setNoiseSuppression(bool enabled) {
	LiveKitAdapterShared &state = *shared;
	// Without a microphone source the value is taken when it is created. With one,
	// the library has no request that changes it, so the value is kept for the
	// next microphone and the caller is told that it did not apply.
	const bool applied = !state.audioSourceHandle
		|| state.noiseSuppression == enabled;
	state.noiseSuppression = enabled;
	return applied;
}

bool LiveKitAdapter::setPlayoutDevice(const std::string &deviceId) {
	LiveKitAdapterShared &state = *shared;
	state.playoutDeviceId = deviceId;
	if (!state.platformAudioHandle) {
		// Applied when PlatformAudio is created.
		return true;
	}
	return ApplyAudioDevice(state, deviceId, true);
}

bool LiveKitAdapter::setRecordingDevice(const std::string &deviceId) {
	LiveKitAdapterShared &state = *shared;
	state.recordingDeviceId = deviceId;
	if (!state.platformAudioHandle) {
		// Applied when PlatformAudio is created.
		return true;
	}
	return ApplyAudioDevice(state, deviceId, false);
}

bool LiveKitAdapter::setRemoteVideoQuality(
		const std::string &participantIdentity,
		LiveKitVideoQuality quality) {
	LiveKitAdapterShared &state = *shared;
	state.videoQualities[participantIdentity] = quality;
	if (state.phase != Phase::Connected) {
		// Applied to each video publication when it is published.
		return true;
	}
	bool confirmed = true;
	for (const std::pair<const std::string, LiveKitRemotePublication> &entry
			: state.remotePublications) {
		const LiveKitRemotePublication &publication = entry.second;
		if (publication.kind != LiveKitTrackKind::Video
			|| publication.participantIdentity != participantIdentity) {
			continue;
		}
		confirmed = RequestRemoteQuality(
			state,
			publication.handle,
			quality) && confirmed;
	}
	return confirmed;
}

bool LiveKitAdapter::requestSessionStats() {
	LiveKitAdapterShared &state = *shared;
	if (state.phase != Phase::Connected || !state.roomHandle) {
		return false;
	}
	const std::uint64_t asyncId = NextRequestAsyncId();
	AddKey(state, asyncId);
	const std::optional<std::vector<std::uint8_t>> response = state.library->request(
		EncodeLiveKitGetSessionStatsRequest(*state.roomHandle, asyncId));
	if (!RequestAccepted(response, kResponseGetSessionStats, asyncId)) {
		RemoveKey(state, asyncId);
		return false;
	}
	state.statsAsyncIds.insert(asyncId);
	return true;
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

// Passes one diagnostic line to the delegate, on the owner thread.
void SendLog(LiveKitAdapterShared &state, const std::string &line) {
	if (state.delegate) {
		state.delegate->liveKitLog(line);
	}
}

// Hands the full snapshot of the remote tracks to the delegate.
void NotifyRemoteMedia(LiveKitAdapterShared &state) {
	if (!state.delegate) {
		return;
	}
	std::vector<LiveKitRemoteMedia> snapshot;
	snapshot.reserve(state.remoteMedia.size());
	for (const std::pair<const std::string, LiveKitRemoteMedia> &entry
			: state.remoteMedia) {
		snapshot.push_back(entry.second);
	}
	state.delegate->liveKitRemoteMediaChanged(snapshot);
}

void ReleaseAll(LiveKitAdapterShared &state) {
	LiveKitFfi &library = *state.library;
	for (const std::pair<const std::string, LiveKitRemoteTrack> &entry
			: state.remoteTracks) {
		ReleaseRemoteTrack(state, entry.second);
	}
	state.remoteTracks.clear();
	for (const std::pair<const std::string, LiveKitRemotePublication> &entry
			: state.remotePublications) {
		library.dropHandle(entry.second.handle);
	}
	state.remotePublications.clear();
	if (!state.remoteMedia.empty()) {
		state.remoteMedia.clear();
		NotifyRemoteMedia(state);
	}
	for (const std::uint64_t handle : state.publicationHandles) {
		library.dropHandle(handle);
	}
	state.publicationHandles.clear();
	DropHandle(library, state.screenTrackHandle);
	DropHandle(library, state.screenSourceHandle);
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
	state.statsAsyncIds.clear();
	state.connectAsyncId.reset();
	state.disconnectAsyncId.reset();
	state.disconnectRequested = false;
	state.microphoneRequested = false;
	state.cameraRequested = false;
	state.screenRequested = false;
	// The platform audio is gone with its handle, so no device is selected any more.
	state.playoutDeviceSelected = false;
	state.recordingDeviceSelected = false;
	RefreshShownStream(state);
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

// Publishes the stream handle of the shown track to the library thread. With no
// shown track set, no stream is shown, so every frame is dropped; before the
// first setShownVideoTrack call the filter is off and every frame is copied.
void RefreshShownStream(LiveKitAdapterShared &state) {
	std::uint64_t stream = 0;
	if (state.shownFilterSet && !state.shownTrackSid.empty()) {
		const std::map<std::string, LiveKitRemoteTrack>::iterator found =
			state.remoteTracks.find(state.shownTrackSid);
		if (found != state.remoteTracks.end() && found->second.streamHandle) {
			stream = *found->second.streamHandle;
		}
	}
	state.shownStreamHandle.store(stream);
	state.videoFilterOn.store(state.shownFilterSet);
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
	SendLog(state, std::string("failed: ") + message);
	if (state.delegate) {
		state.delegate->liveKitFailed(message);
	}
	RetireIfDestroyed(state);
}

void FinishDisconnected(LiveKitAdapterShared &state) {
	ReleaseAll(state);
	state.phase = Phase::Idle;
	SendLog(state, "disconnected");
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

// Creates the PlatformAudio, which plays the remote audio and captures the
// microphone. The device counts of the library are logged, and the selected
// devices are applied. Returns false when the library creates no handle.
bool CreatePlatformAudio(LiveKitAdapterShared &state) {
	const std::optional<std::vector<std::uint8_t>> response =
		state.library->request(EncodeLiveKitNewPlatformAudioRequest());
	if (!response) {
		return false;
	}
	state.platformAudioHandle = DecodeLiveKitHandleResponse(
		*response,
		kResponseNewPlatformAudio).handle;
	if (!state.platformAudioHandle) {
		return false;
	}
	const std::optional<LiveKitPlatformAudioInfo> info =
		DecodePlatformAudioInfo(*response);
	if (info) {
		SendLog(
			state,
			"PlatformAudio created, recording devices "
				+ std::to_string(info->recordingDeviceCount)
				+ ", playout devices "
				+ std::to_string(info->playoutDeviceCount));
	} else {
		SendLog(state, "PlatformAudio created");
	}
	ApplyStoredAudioDevices(state);
	return true;
}

// Applies the stored device ids. A device problem never fails the room, so
// the results are only logged. An empty id sends no request at all.
void ApplyStoredAudioDevices(LiveKitAdapterShared &state) {
	ApplyAudioDevice(state, state.playoutDeviceId, true);
	ApplyAudioDevice(state, state.recordingDeviceId, false);
}

// Selects the playout or the recording device by its endpoint id. The library
// lists the devices, the device whose guid equals the id (without the case) is
// selected with its own guid. An id that the library does not list is logged as
// a mismatch and changes nothing. An empty id sends nothing when no device was
// selected before; after a selection it selects the first listed device, the
// protocol having no request for the default one. Returns false when the library
// reports an error.
bool ApplyAudioDevice(
		LiveKitAdapterShared &state,
		const std::string &deviceId,
		bool playout) {
	const char *kind = playout ? "playout" : "recording";
	bool &selected = playout
		? state.playoutDeviceSelected
		: state.recordingDeviceSelected;
	if (deviceId.empty() && !selected) {
		SendLog(state, std::string(kind) + " device: default");
		return true;
	}
	if (!state.platformAudioHandle) {
		return false;
	}
	const std::optional<std::vector<std::uint8_t>> listResponse =
		state.library->request(
			EncodeLiveKitGetAudioDevicesRequest(*state.platformAudioHandle));
	const std::optional<LiveKitAudioDeviceList> list = listResponse
		? DecodeLiveKitAudioDevices(*listResponse)
		: std::nullopt;
	if (!list || !list->error.empty()) {
		SendLog(state, std::string("LiveKit ") + kind + " device list failed");
		return false;
	}
	const std::vector<LiveKitAudioDevice> &devices = playout
		? list->playout
		: list->recording;
	const LiveKitAudioDevice *match = nullptr;
	if (deviceId.empty()) {
		// The protocol has no request for the default device. The first listed
		// device stands in for it, and the library default is not confirmed.
		if (!devices.empty()) {
			match = &devices.front();
		}
	} else {
		for (const LiveKitAudioDevice &device : devices) {
			if (EqualsIgnoreCase(device.guid, deviceId)) {
				match = &device;
				break;
			}
		}
	}
	if (!match) {
		if (deviceId.empty()) {
			SendLog(state, std::string(kind) + " device: default, no device is listed, the previous device is kept");
		} else {
			// The library guid of Windows is a device interface path, so the
			// endpoint id of the settings may never equal it.
			SendLog(state, std::string(kind) + " device not listed, the id format of the setting may not match the library guid");
		}
		return true;
	}
	const std::optional<std::vector<std::uint8_t>> response =
		state.library->request(
			playout
				? EncodeLiveKitSetPlayoutDeviceRequest(
					*state.platformAudioHandle,
					match->guid)
				: EncodeLiveKitSetRecordingDeviceRequest(
					*state.platformAudioHandle,
					match->guid));
	const std::optional<std::string> error = response
		? DecodeLiveKitSetDeviceResponse(
			*response,
			playout ? kResponseSetPlayoutDevice : kResponseSetRecordingDevice)
		: std::nullopt;
	if (!error || !error->empty()) {
		SendLog(state, std::string("LiveKit ") + kind + " device selection failed");
		return false;
	}
	selected = !deviceId.empty();
	SendLog(
		state,
		std::string(kind)
			+ " device: "
			+ (match->name.empty() ? std::string("default") : match->name));
	return true;
}

// Runs once the room is connected. PlatformAudio plays the remote audio, so it
// is created before the delegate hears of the connection. The library is then
// told that the room events are read: it holds them, the track subscriptions
// included, until this request. Returns false when a step fails, and the room
// is then failed.
bool StartSession(LiveKitAdapterShared &state) {
	if (!state.platformAudioHandle && !CreatePlatformAudio(state)) {
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
	SendLog(state, std::string("publish requested: ") + PublishKindName(kind));
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
		SendLog(state, std::string("publish failed: ") + PublishKindName(kind));
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

// True when frames of the stream are copied and delivered. The stream of the
// shown track is the only one while the filter is on, and a frame without a
// shown stream is never delivered.
bool IsShownStream(
		const LiveKitAdapterShared &state,
		std::uint64_t streamHandle) {
	if (!state.videoFilterOn.load()) {
		return true;
	}
	const std::uint64_t shown = state.shownStreamHandle.load();
	return shown != 0 && shown == streamHandle;
}

// Hands a frame of a video stream to the owner thread. It runs on the library
// thread, because the frame data is valid only there. A frame of a stream that
// is not shown is released without a copy.
void RouteVideoFrame(
		const std::shared_ptr<LiveKitAdapterShared> &owner,
		const LiveKitEvent &event) {
	if (!event.videoBuffer) {
		return;
	}
	if (!event.streamHandle || !IsShownStream(*owner, *event.streamHandle)) {
		const std::uint64_t bufferHandle = event.videoBuffer->bufferHandle;
		if (bufferHandle) {
			owner->library->dropHandle(bufferHandle);
		}
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
	SendLog(state, "connected");
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
		SendLog(state, std::string("publish failed: ") + PublishKindName(kind));
		FailWith(state, kPublishFailed);
		return;
	}
	AddKey(state, *event.publicationHandle);
	state.publicationHandles.push_back(*event.publicationHandle);
	SendLog(state, std::string("publish confirmed: ") + PublishKindName(kind));
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
	SendLog(
		state,
		"room disconnected, reason "
			+ std::to_string(event.disconnectReason)
			+ (event.clientInitiated ? ", client initiated" : ", server dropped"));
	if (state.phase != Phase::Connected || event.clientInitiated) {
		return;
	}
	FailWith(state, kConnectionLost);
}

// Any E2EE state change is logged. A failure state of a track ends the call, as
// Android does.
void DispatchEncryptionState(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	SendLog(
		state,
		std::string("encryption state: ")
			+ event.participantIdentity
			+ ", "
			+ EncryptionStateName(event.encryptionState));
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

	// The media entry keeps the publication info of the track. A track that was
	// published earlier keeps its muted state, which the mute events update; a
	// track that was not seen published takes the muted state of its info.
	const bool known = state.remoteMedia.find(event.trackSid) != state.remoteMedia.end();
	LiveKitRemoteMedia &media = state.remoteMedia[event.trackSid];
	if (!known) {
		media.participantIdentity = event.participantIdentity;
		media.trackSid = event.trackSid;
		media.kind = event.trackKind;
		media.muted = event.trackMuted;
	}
	media.subscribed = true;
	SendLog(state, "remote track subscribed: " + DescribeMedia(media));
	NotifyRemoteMedia(state);
}

void DispatchTrackUnsubscribed(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	const std::map<std::string, LiveKitRemoteMedia>::iterator media =
		state.remoteMedia.find(event.trackSid);
	if (media != state.remoteMedia.end()) {
		media->second.subscribed = false;
		SendLog(state, "remote track unsubscribed: " + DescribeMedia(media->second));
		NotifyRemoteMedia(state);
	}
	const std::map<std::string, LiveKitRemoteTrack>::iterator found =
		state.remoteTracks.find(event.trackSid);
	if (found == state.remoteTracks.end()) {
		return;
	}
	ReleaseRemoteTrack(state, found->second);
	state.remoteTracks.erase(found);
}

// A remote publication is kept by its sid. A video publication of a participant
// with a quality set gets that quality at once, as Android applies the quality
// to each publication it refreshes.
void DispatchTrackPublished(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	if (!event.publicationHandle) {
		return;
	}
	// A publication that arrives outside a connected room is released at once.
	if (state.phase != Phase::Connected || event.trackSid.empty()) {
		state.library->dropHandle(*event.publicationHandle);
		return;
	}
	const std::map<std::string, LiveKitRemotePublication>::iterator previous =
		state.remotePublications.find(event.trackSid);
	if (previous != state.remotePublications.end()) {
		state.library->dropHandle(previous->second.handle);
		state.remotePublications.erase(previous);
	}
	LiveKitRemotePublication publication;
	publication.participantIdentity = event.participantIdentity;
	publication.kind = event.trackKind;
	publication.handle = *event.publicationHandle;
	state.remotePublications.emplace(event.trackSid, publication);

	// The media entry starts unsubscribed; a subscription that came first keeps
	// its state.
	LiveKitRemoteMedia media;
	media.participantIdentity = event.participantIdentity;
	media.trackSid = event.trackSid;
	media.kind = event.trackKind;
	media.source = event.trackSource;
	media.muted = event.trackMuted;
	const std::map<std::string, LiveKitRemoteMedia>::iterator previousMedia =
		state.remoteMedia.find(event.trackSid);
	if (previousMedia != state.remoteMedia.end()) {
		media.subscribed = previousMedia->second.subscribed;
	}
	state.remoteMedia[event.trackSid] = media;
	SendLog(state, "remote track published: " + DescribeMedia(media));
	NotifyRemoteMedia(state);

	if (publication.kind != LiveKitTrackKind::Video) {
		return;
	}
	const std::map<std::string, LiveKitVideoQuality>::iterator quality =
		state.videoQualities.find(publication.participantIdentity);
	if (quality != state.videoQualities.end()) {
		RequestRemoteQuality(state, publication.handle, quality->second);
	}
}

void DispatchTrackUnpublished(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	const std::map<std::string, LiveKitRemoteMedia>::iterator media =
		state.remoteMedia.find(event.trackSid);
	if (media != state.remoteMedia.end()) {
		SendLog(state, "remote track unpublished: " + DescribeMedia(media->second));
		state.remoteMedia.erase(media);
		NotifyRemoteMedia(state);
	}
	const std::map<std::string, LiveKitRemotePublication>::iterator found =
		state.remotePublications.find(event.trackSid);
	if (found == state.remotePublications.end()) {
		return;
	}
	state.library->dropHandle(found->second.handle);
	state.remotePublications.erase(found);
}

// A mute event of a remote track updates its muted state, and the delegate hears
// of the change. A track that is not known is ignored.
void DispatchTrackMute(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event,
		bool muted) {
	if (state.phase != Phase::Connected) {
		return;
	}
	const std::map<std::string, LiveKitRemoteMedia>::iterator found =
		state.remoteMedia.find(event.trackSid);
	if (found == state.remoteMedia.end() || found->second.muted == muted) {
		return;
	}
	found->second.muted = muted;
	SendLog(
		state,
		std::string(muted ? "remote track muted: " : "remote track unmuted: ")
			+ DescribeMedia(found->second));
	NotifyRemoteMedia(state);
}

// A participant that leaves takes its remote tracks with it. Its media entries
// are dropped, and its subscriptions and publications are released, so no frame
// of it is delivered afterwards.
void DispatchParticipantDisconnected(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	if (state.phase != Phase::Connected) {
		return;
	}
	std::vector<std::string> mediaSids;
	for (const std::pair<const std::string, LiveKitRemoteMedia> &entry
			: state.remoteMedia) {
		if (entry.second.participantIdentity == event.participantIdentity) {
			mediaSids.push_back(entry.first);
		}
	}
	for (const std::string &sid : mediaSids) {
		state.remoteMedia.erase(sid);
	}
	std::vector<std::string> trackSids;
	for (const std::pair<const std::string, LiveKitRemoteTrack> &entry
			: state.remoteTracks) {
		if (entry.second.participantIdentity == event.participantIdentity) {
			trackSids.push_back(entry.first);
		}
	}
	for (const std::string &sid : trackSids) {
		const std::map<std::string, LiveKitRemoteTrack>::iterator found =
			state.remoteTracks.find(sid);
		ReleaseRemoteTrack(state, found->second);
		state.remoteTracks.erase(found);
	}
	std::vector<std::string> publicationSids;
	for (const std::pair<const std::string, LiveKitRemotePublication> &entry
			: state.remotePublications) {
		if (entry.second.participantIdentity == event.participantIdentity) {
			publicationSids.push_back(entry.first);
		}
	}
	for (const std::string &sid : publicationSids) {
		const std::map<std::string, LiveKitRemotePublication>::iterator found =
			state.remotePublications.find(sid);
		state.library->dropHandle(found->second.handle);
		state.remotePublications.erase(found);
	}
	if (!mediaSids.empty()) {
		NotifyRemoteMedia(state);
	}
}

// The stats of a request that the adapter sent are passed to the delegate. A
// callback with an error is delivered as stats with the error flag.
void DispatchSessionStats(
		LiveKitAdapterShared &state,
		const LiveKitEvent &event) {
	const std::set<std::uint64_t>::iterator found =
		state.statsAsyncIds.find(event.asyncId);
	if (found == state.statsAsyncIds.end()) {
		return;
	}
	state.statsAsyncIds.erase(found);
	LiveKitSessionStats stats;
	if (event.sessionStats) {
		stats = *event.sessionStats;
	} else {
		stats.error = true;
	}
	SendLog(state, stats.error ? "session stats failed" : "session stats received");
	if (state.delegate) {
		state.delegate->liveKitSessionStats(stats);
	}
}

bool RequestRemoteQuality(
		LiveKitAdapterShared &state,
		std::uint64_t publicationHandle,
		LiveKitVideoQuality quality) {
	const std::optional<std::vector<std::uint8_t>> response =
		state.library->request(
			EncodeLiveKitRemotePublicationQualityRequest(
				publicationHandle,
				quality));
	return response
		&& DecodeLiveKitRemotePublicationQualityResponse(*response);
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
			frame.trackSid = entry.first;
			const std::map<std::string, LiveKitRemoteMedia>::const_iterator media =
				state.remoteMedia.find(entry.first);
			frame.source = media != state.remoteMedia.end()
				? media->second.source
				: LiveKitTrackSource::Unknown;
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
	} else if (event.kind == LiveKitEventKind::TrackPublished) {
		DispatchTrackPublished(state, event);
	} else if (event.kind == LiveKitEventKind::TrackUnpublished) {
		DispatchTrackUnpublished(state, event);
	} else if (event.kind == LiveKitEventKind::TrackMuted) {
		DispatchTrackMute(state, event, true);
	} else if (event.kind == LiveKitEventKind::TrackUnmuted) {
		DispatchTrackMute(state, event, false);
	} else if (event.kind == LiveKitEventKind::ParticipantDisconnected) {
		DispatchParticipantDisconnected(state, event);
	} else if (event.kind == LiveKitEventKind::SessionStats) {
		DispatchSessionStats(state, event);
	}
	// A track that came or went may be the shown one, so the stream is refreshed.
	RefreshShownStream(state);
}

} // namespace
} // namespace Impulse
