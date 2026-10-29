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
};

enum class LiveKitEventKind : std::uint8_t {
	Unknown,
	Connect,
	PublishTrack,
	Disconnect,
};

// The decoded part of an FfiEvent that the adapter needs: the async callbacks
// of connect, publish and disconnect requests.
struct LiveKitEvent final {
	LiveKitEventKind kind = LiveKitEventKind::Unknown;
	std::uint64_t asyncId = 0;
	std::string error;
	std::optional<std::uint64_t> roomHandle;
	std::optional<std::uint64_t> localParticipantHandle;
	std::optional<std::uint64_t> publicationHandle;
};

// A synchronous response that creates one owned handle (PlatformAudio, audio
// source, audio track, video source or video track).
struct LiveKitHandleResponse final {
	std::optional<std::uint64_t> handle;
	std::string error;
};

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitDisposeRequest(
	bool async);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitConnectRequest(
	const std::string &url,
	const std::string &token,
	const LiveKitRoomOptions &options,
	std::optional<std::uint64_t> requestAsyncId);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitDisconnectRequest(
	std::uint64_t roomHandle,
	std::optional<std::uint64_t> requestAsyncId);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitNewPlatformAudioRequest();

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitNewPlatformAudioSourceRequest(
	std::uint64_t platformAudioHandle);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitCreateAudioTrackRequest(
	const std::string &name,
	std::uint64_t sourceHandle);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitNewVideoSourceRequest(
	std::uint32_t width,
	std::uint32_t height);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitCreateVideoTrackRequest(
	const std::string &name,
	std::uint64_t sourceHandle);

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
