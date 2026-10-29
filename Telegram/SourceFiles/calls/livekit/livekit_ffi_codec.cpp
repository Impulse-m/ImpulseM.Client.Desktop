#include "calls/livekit/livekit_ffi_codec.h"

#include "mtproto/impulse/core/wire.h"

#include <span>

namespace Impulse {
namespace {

// FfiRequest oneof field numbers (livekit-ffi/protocol/ffi.proto).
constexpr std::uint32_t kFfiRequestDispose = 2;
constexpr std::uint32_t kFfiRequestConnect = 3;
constexpr std::uint32_t kFfiRequestDisconnect = 4;
constexpr std::uint32_t kFfiRequestPublishTrack = 5;
constexpr std::uint32_t kFfiRequestCreateVideoTrack = 15;
constexpr std::uint32_t kFfiRequestCreateAudioTrack = 16;
constexpr std::uint32_t kFfiRequestNewVideoSource = 21;
constexpr std::uint32_t kFfiRequestNewAudioSource = 26;
constexpr std::uint32_t kFfiRequestNewPlatformAudio = 77;
constexpr std::uint32_t kFfiRequestLocalTrackMute = 17;
constexpr std::uint32_t kFfiResponseLocalTrackMute = 17;

// LocalTrackMuteRequest and LocalTrackMuteResponse fields (livekit-ffi
// protocol, track messages).
constexpr std::uint32_t kLocalTrackMuteTrackHandle = 1;
constexpr std::uint32_t kLocalTrackMuteMute = 2;
constexpr std::uint32_t kLocalTrackMuteMuted = 1;

// DisposeRequest.async.
constexpr std::uint32_t kDisposeAsync = 1;

// ConnectRequest fields.
constexpr std::uint32_t kConnectUrl = 1;
constexpr std::uint32_t kConnectToken = 2;
constexpr std::uint32_t kConnectOptions = 3;
constexpr std::uint32_t kConnectRequestAsyncId = 4;

// RoomOptions fields.
constexpr std::uint32_t kRoomAutoSubscribe = 1;
constexpr std::uint32_t kRoomAdaptiveStream = 2;
constexpr std::uint32_t kRoomDynacast = 3;

// DisconnectRequest fields.
constexpr std::uint32_t kDisconnectRoomHandle = 1;
constexpr std::uint32_t kDisconnectRequestAsyncId = 2;

// NewAudioSourceRequest fields and AudioSourceType.PLATFORM.
constexpr std::uint32_t kNewAudioSourceType = 1;
constexpr std::uint32_t kNewAudioSourcePlatformAudioHandle = 6;
constexpr std::uint64_t kAudioSourceTypePlatform = 1;

// CreateAudioTrackRequest and CreateVideoTrackRequest fields.
constexpr std::uint32_t kCreateTrackName = 1;
constexpr std::uint32_t kCreateTrackSourceHandle = 2;

// NewVideoSourceRequest fields, VideoSourceType.NATIVE and VideoSourceResolution.
constexpr std::uint32_t kNewVideoSourceType = 1;
constexpr std::uint32_t kNewVideoSourceResolution = 2;
constexpr std::uint32_t kVideoSourceTypeNative = 0;
constexpr std::uint32_t kResolutionWidth = 1;
constexpr std::uint32_t kResolutionHeight = 2;

// PublishTrackRequest and TrackPublishOptions fields.
constexpr std::uint32_t kPublishLocalParticipantHandle = 1;
constexpr std::uint32_t kPublishTrackHandle = 2;
constexpr std::uint32_t kPublishOptions = 3;
constexpr std::uint32_t kPublishRequestAsyncId = 4;
constexpr std::uint32_t kPublishOptionsSource = 7;

// Response and event fields. Every async response starts with async_id = 1.
constexpr std::uint32_t kAsyncId = 1;
constexpr std::uint32_t kOwnedHandleValue = 1;
constexpr std::uint32_t kOwnedMessage = 1;
constexpr std::uint32_t kErrorString = 2;
constexpr std::uint32_t kOwnedHandleMessage = 1;

// FfiEvent oneof field numbers.
constexpr std::uint32_t kEventConnect = 5;
constexpr std::uint32_t kEventDisconnect = 7;
constexpr std::uint32_t kEventPublishTrack = 9;

// ConnectCallback fields.
constexpr std::uint32_t kConnectCallbackError = 2;
constexpr std::uint32_t kConnectCallbackResult = 3;
constexpr std::uint32_t kConnectResultRoom = 1;
constexpr std::uint32_t kConnectResultLocalParticipant = 2;

// PublishTrackCallback field.
constexpr std::uint32_t kPublishCallbackPublication = 3;

[[nodiscard]] std::uint64_t BoolValue(bool value) {
	return value ? 1 : 0;
}

[[nodiscard]] std::vector<std::uint8_t> WrapFfiRequest(
		std::uint32_t field,
		const ProtoWriter &message) {
	ProtoWriter request;
	request.writeMessageField(field, message);
	return request.takeBytes();
}

// Finds the first field of the message with the given number and wire type
// length-delimited and returns its payload.
[[nodiscard]] std::optional<std::span<const std::uint8_t>> FindMessage(
		std::span<const std::uint8_t> data,
		std::uint32_t field) {
	try {
		ProtoReader reader(data);
		while (reader.next()) {
			if (reader.field() == field
				&& reader.wireType() == ProtoWireType::LengthDelimited) {
				return reader.readBytes();
			}
			reader.skip();
		}
	} catch (const ProtoWireError &) {
		return std::nullopt;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> FindVarint(
		std::span<const std::uint8_t> data,
		std::uint32_t field) {
	try {
		ProtoReader reader(data);
		while (reader.next()) {
			if (reader.field() == field
				&& reader.wireType() == ProtoWireType::Varint) {
				return reader.readVarint();
			}
			reader.skip();
		}
	} catch (const ProtoWireError &) {
		return std::nullopt;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> FindString(
		std::span<const std::uint8_t> data,
		std::uint32_t field) {
	const auto bytes = FindMessage(data, field);
	if (!bytes) {
		return std::nullopt;
	}
	return std::string(
		reinterpret_cast<const char *>(bytes->data()),
		bytes->size());
}

// Reads FfiOwnedHandle.id from an owned message that wraps the handle in its
// field 1, for example OwnedTrack or OwnedVideoSource.
[[nodiscard]] std::optional<std::uint64_t> OwnedHandleId(
		std::span<const std::uint8_t> owned) {
	const auto handle = FindMessage(owned, kOwnedHandleMessage);
	if (!handle) {
		return std::nullopt;
	}
	return FindVarint(*handle, kOwnedHandleValue);
}

} // namespace

std::vector<std::uint8_t> EncodeLiveKitDisposeRequest(bool async) {
	ProtoWriter dispose;
	dispose.writeVarintField(kDisposeAsync, BoolValue(async));
	return WrapFfiRequest(kFfiRequestDispose, dispose);
}

std::vector<std::uint8_t> EncodeLiveKitConnectRequest(
		const std::string &url,
		const std::string &token,
		const LiveKitRoomOptions &options,
		std::optional<std::uint64_t> requestAsyncId) {
	ProtoWriter roomOptions;
	roomOptions.writeVarintField(
		kRoomAutoSubscribe,
		BoolValue(options.autoSubscribe));
	roomOptions.writeVarintField(
		kRoomAdaptiveStream,
		BoolValue(options.adaptiveStream));
	roomOptions.writeVarintField(
		kRoomDynacast,
		BoolValue(options.dynacast));

	ProtoWriter connect;
	connect.writeStringField(kConnectUrl, url);
	connect.writeStringField(kConnectToken, token);
	connect.writeMessageField(kConnectOptions, roomOptions);
	if (requestAsyncId) {
		connect.writeVarintField(kConnectRequestAsyncId, *requestAsyncId);
	}
	return WrapFfiRequest(kFfiRequestConnect, connect);
}

std::vector<std::uint8_t> EncodeLiveKitDisconnectRequest(
		std::uint64_t roomHandle,
		std::optional<std::uint64_t> requestAsyncId) {
	ProtoWriter disconnect;
	disconnect.writeVarintField(kDisconnectRoomHandle, roomHandle);
	if (requestAsyncId) {
		disconnect.writeVarintField(
			kDisconnectRequestAsyncId,
			*requestAsyncId);
	}
	return WrapFfiRequest(kFfiRequestDisconnect, disconnect);
}

std::vector<std::uint8_t> EncodeLiveKitNewPlatformAudioRequest() {
	const ProtoWriter empty;
	return WrapFfiRequest(kFfiRequestNewPlatformAudio, empty);
}

std::vector<std::uint8_t> EncodeLiveKitNewPlatformAudioSourceRequest(
		std::uint64_t platformAudioHandle) {
	ProtoWriter source;
	source.writeVarintField(kNewAudioSourceType, kAudioSourceTypePlatform);
	source.writeVarintField(
		kNewAudioSourcePlatformAudioHandle,
		platformAudioHandle);
	return WrapFfiRequest(kFfiRequestNewAudioSource, source);
}

std::vector<std::uint8_t> EncodeLiveKitCreateAudioTrackRequest(
		const std::string &name,
		std::uint64_t sourceHandle) {
	ProtoWriter track;
	track.writeStringField(kCreateTrackName, name);
	track.writeVarintField(kCreateTrackSourceHandle, sourceHandle);
	return WrapFfiRequest(kFfiRequestCreateAudioTrack, track);
}

std::vector<std::uint8_t> EncodeLiveKitNewVideoSourceRequest(
		std::uint32_t width,
		std::uint32_t height) {
	ProtoWriter resolution;
	resolution.writeVarintField(kResolutionWidth, width);
	resolution.writeVarintField(kResolutionHeight, height);

	ProtoWriter source;
	source.writeVarintField(kNewVideoSourceType, kVideoSourceTypeNative);
	source.writeMessageField(kNewVideoSourceResolution, resolution);
	return WrapFfiRequest(kFfiRequestNewVideoSource, source);
}

std::vector<std::uint8_t> EncodeLiveKitCreateVideoTrackRequest(
		const std::string &name,
		std::uint64_t sourceHandle) {
	ProtoWriter track;
	track.writeStringField(kCreateTrackName, name);
	track.writeVarintField(kCreateTrackSourceHandle, sourceHandle);
	return WrapFfiRequest(kFfiRequestCreateVideoTrack, track);
}

std::vector<std::uint8_t> EncodeLiveKitPublishTrackRequest(
		std::uint64_t localParticipantHandle,
		std::uint64_t trackHandle,
		LiveKitTrackSource source,
		std::optional<std::uint64_t> requestAsyncId) {
	ProtoWriter options;
	options.writeVarintField(
		kPublishOptionsSource,
		static_cast<std::uint64_t>(source));

	ProtoWriter publish;
	publish.writeVarintField(
		kPublishLocalParticipantHandle,
		localParticipantHandle);
	publish.writeVarintField(kPublishTrackHandle, trackHandle);
	publish.writeMessageField(kPublishOptions, options);
	if (requestAsyncId) {
		publish.writeVarintField(kPublishRequestAsyncId, *requestAsyncId);
	}
	return WrapFfiRequest(kFfiRequestPublishTrack, publish);
}

std::vector<std::uint8_t> EncodeLiveKitLocalTrackMuteRequest(
		std::uint64_t trackHandle,
		bool mute) {
	ProtoWriter muteRequest;
	muteRequest.writeVarintField(kLocalTrackMuteTrackHandle, trackHandle);
	muteRequest.writeVarintField(kLocalTrackMuteMute, BoolValue(mute));
	return WrapFfiRequest(kFfiRequestLocalTrackMute, muteRequest);
}

std::optional<bool> DecodeLiveKitLocalTrackMuteResponse(
		const std::vector<std::uint8_t> &response) {
	const auto message = FindMessage(response, kFfiResponseLocalTrackMute);
	if (!message) {
		return std::nullopt;
	}
	return FindVarint(*message, kLocalTrackMuteMuted).value_or(0) != 0;
}

std::optional<std::uint64_t> DecodeLiveKitAsyncId(
		const std::vector<std::uint8_t> &response,
		std::uint32_t responseField) {
	const auto message = FindMessage(response, responseField);
	if (!message) {
		return std::nullopt;
	}
	return FindVarint(*message, kAsyncId);
}

LiveKitHandleResponse DecodeLiveKitHandleResponse(
		const std::vector<std::uint8_t> &response,
		std::uint32_t responseField) {
	LiveKitHandleResponse result;
	const auto message = FindMessage(response, responseField);
	if (!message) {
		return result;
	}
	if (const auto error = FindString(*message, kErrorString)) {
		result.error = *error;
	}
	const auto owned = FindMessage(*message, kOwnedMessage);
	if (owned) {
		result.handle = OwnedHandleId(*owned);
	}
	return result;
}

LiveKitEvent DecodeLiveKitEvent(const std::vector<std::uint8_t> &event) {
	LiveKitEvent result;
	try {
		ProtoReader reader(event);
		while (reader.next()) {
			const auto field = reader.field();
			if (field != kEventConnect
				&& field != kEventDisconnect
				&& field != kEventPublishTrack) {
				reader.skip();
				continue;
			}
			if (reader.wireType() != ProtoWireType::LengthDelimited) {
				reader.skip();
				continue;
			}
			const auto payload = reader.readBytes();
			if (const auto asyncId = FindVarint(payload, kAsyncId)) {
				result.asyncId = *asyncId;
			}
			if (field == kEventDisconnect) {
				result.kind = LiveKitEventKind::Disconnect;
			} else if (field == kEventPublishTrack) {
				result.kind = LiveKitEventKind::PublishTrack;
				if (const auto error = FindString(payload, kErrorString)) {
					result.error = *error;
				}
				const auto publication = FindMessage(
					payload,
					kPublishCallbackPublication);
				if (publication) {
					result.publicationHandle = OwnedHandleId(*publication);
				}
			} else {
				result.kind = LiveKitEventKind::Connect;
				if (const auto error = FindString(
						payload,
						kConnectCallbackError)) {
					result.error = *error;
				}
				const auto callbackResult = FindMessage(
					payload,
					kConnectCallbackResult);
				if (callbackResult) {
					const auto room = FindMessage(
						*callbackResult,
						kConnectResultRoom);
					if (room) {
						result.roomHandle = OwnedHandleId(*room);
					}
					const auto participant = FindMessage(
						*callbackResult,
						kConnectResultLocalParticipant);
					if (participant) {
						result.localParticipantHandle =
							OwnedHandleId(*participant);
					}
				}
			}
			return result;
		}
	} catch (const ProtoWireError &) {
		return LiveKitEvent();
	}
	return result;
}

} // namespace Impulse
