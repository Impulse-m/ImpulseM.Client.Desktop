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

// The TrackSource of a track (livekit-ffi track protocol). The publish request
// writes the values 1 to 3 as the source of the publish options. Unknown holds
// every value the library does not define.
enum class LiveKitTrackSource : std::uint8_t {
	Unknown = 0,
	Camera = 1,
	Microphone = 2,
	ScreenShare = 3,
	ScreenShareAudio = 4,
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
	UnpublishTrack,
	Disconnect,
	TrackSubscribed,
	TrackUnsubscribed,
	VideoFrame,
	RoomDisconnected,
	EncryptionStateChanged,
	TrackPublished,
	TrackUnpublished,
	TrackMuted,
	TrackUnmuted,
	ParticipantDisconnected,
	SessionStats,
	Log,
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

// The LogLevel of a library log record (livekit-ffi ffi.proto). The values are
// in the order of the protocol, from the most severe. Unknown holds every value
// the library does not define.
enum class LiveKitLogLevel : std::uint8_t {
	Error,
	Warn,
	Info,
	Debug,
	Trace,
	Unknown,
};

// One record of the library log, as a LogBatch of the library carries it. The
// module path, the file and the line are not kept. The message is decoded for
// the router sinks, but FormatLiveKitLogLine never writes it.
struct LiveKitLogRecord final {
	LiveKitLogLevel level = LiveKitLogLevel::Unknown;
	std::string target;
	std::string message;
};

// One audio device of the platform audio, as the library lists it. The guid is
// the id the device is selected with; it is empty on mobile.
struct LiveKitAudioDevice final {
	std::uint32_t index = 0;
	std::string name;
	std::string guid;
};

// The devices of the platform audio. The error is empty when the listing
// succeeded.
struct LiveKitAudioDeviceList final {
	std::vector<LiveKitAudioDevice> playout;
	std::vector<LiveKitAudioDevice> recording;
	std::string error;
};

// The device counts that the library reports when the platform audio is created.
struct LiveKitPlatformAudioInfo final {
	std::int32_t recordingDeviceCount = 0;
	std::int32_t playoutDeviceCount = 0;
};

// An outbound RTP stream of the session, from the publisher stats. The rid, the
// frame and the encoder fields describe a video stream. The quality limitation
// reason is the QualityLimitationReason number of the library, named by
// LiveKitQualityLimitationName.
struct LiveKitOutboundRtpStats final {
	LiveKitTrackKind kind = LiveKitTrackKind::Unknown;
	std::uint64_t packetsSent = 0;
	std::uint64_t bytesSent = 0;
	std::string rid;
	std::uint32_t frameWidth = 0;
	std::uint32_t frameHeight = 0;
	double framesPerSecond = 0.;
	std::uint32_t framesSent = 0;
	std::uint32_t framesEncoded = 0;
	std::uint32_t keyFramesEncoded = 0;
	std::uint64_t retransmittedPacketsSent = 0;
	double targetBitrate = 0.;
	std::uint32_t qualityLimitationReason = 0;
	bool active = false;
	std::uint32_t pliCount = 0;
	std::uint32_t nackCount = 0;
};

// An inbound RTP stream of the session, from the subscriber stats. The audio
// fields are filled for audio streams only. The framesDecoded and the other
// video fields are filled for video streams only.
struct LiveKitInboundRtpStats final {
	LiveKitTrackKind kind = LiveKitTrackKind::Unknown;
	std::uint64_t packetsReceived = 0;
	std::uint64_t bytesReceived = 0;
	std::int64_t packetsLost = 0;
	std::uint64_t totalSamplesReceived = 0;
	double audioLevel = 0.;
	std::optional<std::uint32_t> framesDecoded;
	std::uint32_t keyFramesDecoded = 0;
	std::uint32_t framesDropped = 0;
	std::uint32_t frameWidth = 0;
	std::uint32_t frameHeight = 0;
	double framesPerSecond = 0.;
	std::uint32_t freezeCount = 0;
	std::uint32_t pauseCount = 0;
};

// The RTP stats of the session of a room. The error is set when the library
// failed the stats request; the lists are then empty and no text of the library
// is kept.
struct LiveKitSessionStats final {
	bool error = false;
	std::vector<LiveKitOutboundRtpStats> outbound;
	std::vector<LiveKitInboundRtpStats> inbound;
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
// of connect, publish, disconnect and session stats requests, the track
// subscriptions and publications of the room, the room drop and E2EE state of
// the room, the frames of a video stream, and the records of the library log.
struct LiveKitEvent final {
	LiveKitEventKind kind = LiveKitEventKind::Unknown;
	std::uint64_t asyncId = 0;
	std::string error;
	std::optional<std::uint64_t> roomHandle;
	std::optional<std::uint64_t> localParticipantHandle;

	// TrackPublished of a remote participant. The handle is set on a publish
	// only and is owned by the adapter; the sid, the kind, the source and the
	// muted state come from the publication info. TrackUnpublished names the
	// publication by trackSid. PublishTrack, the callback of a local publish,
	// sets the handle of the publication and its sid in trackSid.
	std::optional<std::uint64_t> publicationHandle;
	LiveKitTrackSource trackSource = LiveKitTrackSource::Unknown;
	bool trackMuted = false;

	// TrackSubscribed and TrackUnsubscribed. The handle is set on a subscribe
	// only; the sid identifies the track on both events. TrackMuted and
	// TrackUnmuted name the track by its sid. EncryptionStateChanged names the
	// participant whose state changed in participantIdentity, and so does
	// ParticipantDisconnected.
	std::string participantIdentity;
	std::string trackSid;
	std::optional<std::uint64_t> trackHandle;
	LiveKitTrackKind trackKind = LiveKitTrackKind::Unknown;

	// RoomDisconnected: true when this client left the room, which is the
	// disconnect the adapter asked for. Any other reason is a server-side drop.
	// The reason is the DisconnectReason code of the library.
	bool clientInitiated = false;
	std::uint64_t disconnectReason = 0;

	// EncryptionStateChanged: the state the library reports for the track.
	LiveKitEncryptionState encryptionState = LiveKitEncryptionState::Unknown;

	// VideoFrame: the video stream that delivered the frame and its buffer.
	std::optional<std::uint64_t> streamHandle;
	std::optional<LiveKitVideoBuffer> videoBuffer;

	// Log: the records of one LogBatch.
	std::vector<LiveKitLogRecord> logRecords;

	// SessionStats: the stats of the callback, routed by asyncId.
	std::optional<LiveKitSessionStats> sessionStats;
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

// Requests the RTP stats of the session of a room. The callback arrives as a
// SessionStats event with the same async id.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitGetSessionStatsRequest(
	std::uint64_t roomHandle,
	std::optional<std::uint64_t> requestAsyncId);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitNewPlatformAudioRequest();

// The audio processing of the platform audio is set with the source: echo
// cancellation and gain control are on, noise suppression follows the argument.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitNewPlatformAudioSourceRequest(
	std::uint64_t platformAudioHandle,
	bool noiseSuppression);

// Lists the playout and recording devices of a platform audio. The request is
// synchronous.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitGetAudioDevicesRequest(
	std::uint64_t platformAudioHandle);

// Selects the recording or the playout device by its guid, as listed by
// EncodeLiveKitGetAudioDevicesRequest. The requests are synchronous.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitSetRecordingDeviceRequest(
	std::uint64_t platformAudioHandle,
	const std::string &deviceId);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitSetPlayoutDeviceRequest(
	std::uint64_t platformAudioHandle,
	const std::string &deviceId);

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

// The simulcast is written to the publish options only when it is given, so a
// request without it carries the same bytes as before the option existed.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitPublishTrackRequest(
	std::uint64_t localParticipantHandle,
	std::uint64_t trackHandle,
	LiveKitTrackSource source,
	std::optional<std::uint64_t> requestAsyncId,
	std::optional<bool> simulcast = std::nullopt);

// Unpublishes the local track of the publication sid. The answer is an
// UnpublishTrackCallback with the request async id. With stopOnUnpublish the
// library stops the track too.
[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitUnpublishTrackRequest(
	std::uint64_t localParticipantHandle,
	const std::string &trackSid,
	bool stopOnUnpublish,
	std::optional<std::uint64_t> requestAsyncId);

// The name of a QualityLimitationReason number of an outbound video stream:
// "none", "cpu" or "bandwidth". The OTHER reason and every number the library
// does not define are named "other".
[[nodiscard]] std::string LiveKitQualityLimitationName(std::uint32_t reason);

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
// PublishTrackResponse, DisconnectResponse or GetSessionStatsResponse) from the
// FfiResponse field responseField.
[[nodiscard]] std::optional<std::uint64_t> DecodeLiveKitAsyncId(
	const std::vector<std::uint8_t> &response,
	std::uint32_t responseField);

// Reads the owned handle of a synchronous response (NewPlatformAudioResponse,
// NewAudioSourceResponse, CreateAudioTrackResponse, NewVideoSourceResponse,
// CreateVideoTrackResponse) from the FfiResponse field responseField.
[[nodiscard]] LiveKitHandleResponse DecodeLiveKitHandleResponse(
	const std::vector<std::uint8_t> &response,
	std::uint32_t responseField);

// Reads the device counts of the PlatformAudioInfo of a NewPlatformAudioResponse.
// Returns nullopt when the response carries no info.
[[nodiscard]] std::optional<LiveKitPlatformAudioInfo> DecodePlatformAudioInfo(
	const std::vector<std::uint8_t> &response);

// Reads the device lists of a GetAudioDevicesResponse. Returns nullopt when the
// response carries no GetAudioDevicesResponse.
[[nodiscard]] std::optional<LiveKitAudioDeviceList> DecodeLiveKitAudioDevices(
	const std::vector<std::uint8_t> &response);

// Reads the error of a SetRecordingDeviceResponse or a SetPlayoutDeviceResponse
// from the FfiResponse field responseField. Returns nullopt when the response
// carries no such message, and the error text otherwise, which is empty on
// success.
[[nodiscard]] std::optional<std::string> DecodeLiveKitSetDeviceResponse(
	const std::vector<std::uint8_t> &response,
	std::uint32_t responseField);

// Turns a library log record into a line that is safe to write. The line is
// "<target>: <level>", for example "livekit_room: info", and the message text of
// the record is never part of it: SDK text can carry the authenticated URL, the
// token, key bytes and addresses, and no redaction covers all of them. Records
// of a target that starts with livekit are kept at Info and above, and records
// of any other target at Warn and above; the rest give nullopt.
[[nodiscard]] std::optional<std::string> FormatLiveKitLogLine(
	const LiveKitLogRecord &record);

// Decodes one encoded FfiEvent. Returns a LiveKitEvent of kind Unknown for any
// event the adapter does not use.
[[nodiscard]] LiveKitEvent DecodeLiveKitEvent(
	const std::vector<std::uint8_t> &event);

} // namespace Impulse
