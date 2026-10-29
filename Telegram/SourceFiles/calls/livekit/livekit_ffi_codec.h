// Request encoding for the LiveKit FFI library. Each request is a protobuf
// FfiRequest message (livekit-ffi protocol, rust-sdks v0.13.2) written with the
// hand-written wire codec of core/wire, so no protobuf library is linked.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Impulse {

struct LiveKitRoomOptions final {
	bool autoSubscribe = false;
	bool adaptiveStream = false;
	bool dynacast = false;
};

enum class LiveKitTrackSource : std::uint8_t {
	Camera = 1,
	Microphone = 2,
	ScreenShare = 3,
};

// The VideoQuality of a remote video publication (livekit-ffi room protocol).
enum class LiveKitVideoQuality : std::uint8_t {
	Low = 0,
	Medium = 1,
	High = 2,
};

// E2EE options of a room, as the shared-key mode of the library takes them.
// The encoder writes the fixed values of the 1:1 call, so the caller passes
// only the key.
struct LiveKitEncryptionOptions final {
	std::vector<std::uint8_t> sharedKey;
};

enum class LiveKitTrackKind : std::uint8_t {
	Unknown = 0,
	Audio = 1,
	Video = 2,
};

enum class LiveKitEventKind : std::uint8_t {
	Unknown,
	Connect,
	PublishTrack,
	Disconnect,
	TrackSubscribed,
	TrackUnsubscribed,
	VideoFrame,
	RoomDisconnected,
	EncryptionStateChanged,
	TrackPublished,
	TrackUnpublished,
};

// The EncryptionState of an E2EE state change (livekit-ffi e2ee.proto). Unknown
// holds every value the library does not define.
enum class LiveKitEncryptionState : std::uint8_t {
	Unknown,
	New,
	Ok,
	EncryptionFailed,
	DecryptionFailed,
	MissingKey,
	KeyRatcheted,
	InternalError,
};

// One plane of an I420 buffer that the library decoded for a video frame. The
// data pointer is valid only until the buffer handle is dropped.
struct LiveKitVideoComponent final {
	std::uint64_t dataPtr = 0;
	std::uint32_t stride = 0;
	std::uint32_t size = 0;
};

// One I420 frame that the adapter pushes into a native video source. The data
// pointers of the components are read by the library during the synchronous
// capture request only.
struct LiveKitCaptureFrame final {
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::int64_t timestampUs = 0;
	std::vector<LiveKitVideoComponent> components;
};

// The buffer of a received video frame, as the library reports it. The type is
// the VideoBufferType value; the adapter accepts I420 only.
struct LiveKitVideoBuffer final {
	std::uint64_t bufferHandle = 0;
	std::uint32_t type = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::int64_t timestampUs = 0;
	std::vector<LiveKitVideoComponent> components;
};

// The decoded part of an FfiEvent that the adapter needs: the async callbacks
// of connect, publish and disconnect requests, the track subscriptions of the
// room, the room drop and E2EE state of the room, and the frames of a video
// stream.
struct LiveKitEvent final {
	LiveKitEventKind kind = LiveKitEventKind::Unknown;
	std::uint64_t asyncId = 0;
	std::string error;
	std::optional<std::uint64_t> roomHandle;
	std::optional<std::uint64_t> localParticipantHandle;

	// TrackPublished of a remote participant. The handle is set on a publish
	// only and is owned by the adapter; the sid and the kind come from the
	// publication info. TrackUnpublished names the publication by trackSid.
	std::optional<std::uint64_t> publicationHandle;

	// TrackSubscribed and TrackUnsubscribed. The handle is set on a subscribe
	// only; the sid identifies the track on both events. EncryptionStateChanged
	// names the participant whose state changed in participantIdentity.
	std::string participantIdentity;
	std::string trackSid;
	std::optional<std::uint64_t> trackHandle;
	LiveKitTrackKind trackKind = LiveKitTrackKind::Unknown;

	// RoomDisconnected: true when this client left the room, which is the
	// disconnect the adapter asked for. Any other reason is a server-side drop.
	bool clientInitiated = false;

	// EncryptionStateChanged: the state the library reports for the track.
	LiveKitEncryptionState encryptionState = LiveKitEncryptionState::Unknown;

	// VideoFrame: the video stream that delivered the frame and its buffer.
	std::optional<std::uint64_t> streamHandle;
	std::optional<LiveKitVideoBuffer> videoBuffer;
};

// A synchronous response that creates one owned handle (PlatformAudio, audio
// source, audio track, video source or video track).
struct LiveKitHandleResponse final {
	std::optional<std::uint64_t> handle;
	std::string error;
};

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitDisposeRequest(
	bool async);

// The encryption, when set, is sent in the connect request, so the key is set
// on the key provider before the room connects.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitConnectRequest(
	const std::string &url,
	const std::string &token,
	const LiveKitRoomOptions &options,
	std::optional<std::uint64_t> requestAsyncId,
	const std::optional<LiveKitEncryptionOptions> &encryption);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitDisconnectRequest(
	std::uint64_t roomHandle,
	std::optional<std::uint64_t> requestAsyncId);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitNewPlatformAudioRequest();

// The audio processing of the platform audio is set with the source: echo
// cancellation and gain control are on, noise suppression follows the argument.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitNewPlatformAudioSourceRequest(
	std::uint64_t platformAudioHandle,
	bool noiseSuppression);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitCreateAudioTrackRequest(
	const std::string &name,
	std::uint64_t sourceHandle);

// A screencast source is marked as screen content, so the encoder favours
// detail over frame rate.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitNewVideoSourceRequest(
	std::uint32_t width,
	std::uint32_t height,
	bool screencast);

// Pushes one I420 frame into a native video source. The request is synchronous;
// the library copies the planes before it returns.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitCaptureVideoFrameRequest(
	std::uint64_t sourceHandle,
	const LiveKitCaptureFrame &frame);

// Returns true when the response carries the CaptureVideoFrameResponse.
[[nodiscard]] bool DecodeLiveKitCaptureVideoFrameResponse(
	const std::vector<std::uint8_t> &response);

// Sets the simulcast layer that the library subscribes to for a remote video
// publication.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitRemotePublicationQualityRequest(
	std::uint64_t publicationHandle,
	LiveKitVideoQuality quality);

// Returns true when the response carries the SetRemoteTrackPublicationQualityResponse.
[[nodiscard]] bool DecodeLiveKitRemotePublicationQualityResponse(
	const std::vector<std::uint8_t> &response);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitCreateVideoTrackRequest(
	const std::string &name,
	std::uint64_t sourceHandle);

// Tells the library that the room event listener of the room is ready. The
// library holds the room events, the track subscriptions included, until this
// request is sent after the connect callback.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitReadyForRoomEventRequest(
	std::uint64_t roomHandle);

// Opens a native video stream of a remote video track. Frames arrive as
// VideoStreamEvent events in I420 with the stride normalised to the width.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitNewVideoStreamRequest(
	std::uint64_t trackHandle);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitPublishTrackRequest(
	std::uint64_t localParticipantHandle,
	std::uint64_t trackHandle,
	LiveKitTrackSource source,
	std::optional<std::uint64_t> requestAsyncId);

// Sets the mute state of a local audio or video track. The request is
// synchronous: the response reports the state the library has now.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitLocalTrackMuteRequest(
	std::uint64_t trackHandle,
	bool mute);

// Reads the muted field of a LocalTrackMuteResponse. Returns nullopt when the
// response carries no LocalTrackMuteResponse.
[[nodiscard]] std::optional<bool> DecodeLiveKitLocalTrackMuteResponse(
	const std::vector<std::uint8_t> &response);

// Reads the async id of a response that carries one (ConnectResponse,
// PublishTrackResponse or DisconnectResponse) from the FfiResponse field
// responseField.
[[nodiscard]] std::optional<std::uint64_t> DecodeLiveKitAsyncId(
	const std::vector<std::uint8_t> &response,
	std::uint32_t responseField);

// Reads the owned handle of a synchronous response (NewPlatformAudioResponse,
// NewAudioSourceResponse, CreateAudioTrackResponse, NewVideoSourceResponse,
// CreateVideoTrackResponse) from the FfiResponse field responseField.
[[nodiscard]] LiveKitHandleResponse DecodeLiveKitHandleResponse(
	const std::vector<std::uint8_t> &response,
	std::uint32_t responseField);

// Decodes one encoded FfiEvent. Returns a LiveKitEvent of kind Unknown for any
// event the adapter does not use.
[[nodiscard]] LiveKitEvent DecodeLiveKitEvent(
	const std::vector<std::uint8_t> &event);

} // namespace Impulse
