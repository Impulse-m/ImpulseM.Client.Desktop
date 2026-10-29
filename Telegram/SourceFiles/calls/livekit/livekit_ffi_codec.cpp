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
constexpr std::uint32_t kFfiRequestNewVideoStream = 20;
constexpr std::uint32_t kFfiRequestReadyForRoomEvent = 83;

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
constexpr std::uint32_t kRoomEncryption = 7;

// E2eeOptions and KeyProviderOptions fields (livekit-ffi protocol, e2ee).
constexpr std::uint32_t kE2eeEncryptionType = 1;
constexpr std::uint32_t kE2eeKeyProviderOptions = 2;
constexpr std::uint32_t kKeyProviderSharedKey = 1;
constexpr std::uint32_t kKeyProviderRatchetWindowSize = 2;
constexpr std::uint32_t kKeyProviderRatchetSalt = 3;
constexpr std::uint32_t kKeyProviderFailureTolerance = 4;
constexpr std::uint32_t kKeyProviderKeyRingSize = 5;
constexpr std::uint32_t kKeyProviderKeyDerivationFunction = 6;

// The shared-key E2EE values of the 1:1 call, the ones Android sets. Ratchet
// window 0, failure tolerance -1 and key ring 16 are written as the library
// reads them. The magic bytes and the discard-while-not-ready flag have no
// field in this request, so the library defaults apply to them and parity
// with Android is not claimed for either one.
constexpr std::uint64_t kEncryptionTypeGcm = 1;
constexpr std::uint64_t kKeyDerivationHkdf = 1;
constexpr std::uint64_t kRatchetWindowSize = 0;
constexpr std::int64_t kFailureTolerance = -1;
constexpr std::uint64_t kKeyRingSize = 16;
constexpr const char *kRatchetSalt = "LKFrameEncryptionKey";

// DisconnectRequest fields.
constexpr std::uint32_t kDisconnectRoomHandle = 1;
constexpr std::uint32_t kDisconnectRequestAsyncId = 2;

// ReadyForRoomEventRequest field.
constexpr std::uint32_t kReadyRoomHandle = 1;

// NewVideoStreamRequest fields, VideoStreamType.NATIVE and the I420 format.
constexpr std::uint32_t kNewVideoStreamTrackHandle = 1;
constexpr std::uint32_t kNewVideoStreamType = 2;
constexpr std::uint32_t kNewVideoStreamFormat = 3;
constexpr std::uint32_t kNewVideoStreamNormalizeStride = 4;
constexpr std::uint64_t kVideoStreamTypeNative = 0;
constexpr std::uint64_t kVideoBufferTypeI420 = 5;

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
constexpr std::uint32_t kEventRoom = 1;
constexpr std::uint32_t kEventVideoStream = 3;
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

// RoomEvent fields and the oneof members the adapter reads.
constexpr std::uint32_t kRoomEventHandle = 1;
constexpr std::uint32_t kRoomEventTrackSubscribed = 9;
constexpr std::uint32_t kRoomEventTrackUnsubscribed = 10;

// TrackSubscribed and TrackUnsubscribed fields.
constexpr std::uint32_t kTrackSubscribedIdentity = 1;
constexpr std::uint32_t kTrackSubscribedTrack = 2;
constexpr std::uint32_t kTrackUnsubscribedIdentity = 1;
constexpr std::uint32_t kTrackUnsubscribedSid = 2;

// OwnedTrack and TrackInfo fields. TrackInfo.kind is TrackKind: 1 audio, 2 video.
constexpr std::uint32_t kOwnedTrackInfo = 2;
constexpr std::uint32_t kTrackInfoSid = 1;
constexpr std::uint32_t kTrackInfoKind = 3;
constexpr std::uint64_t kTrackKindAudio = 1;
constexpr std::uint64_t kTrackKindVideo = 2;

// VideoStreamEvent, VideoFrameReceived, OwnedVideoBuffer, VideoBufferInfo and
// its ComponentInfo fields.
constexpr std::uint32_t kVideoStreamEventHandle = 1;
constexpr std::uint32_t kVideoStreamEventFrame = 2;
constexpr std::uint32_t kVideoFrameBuffer = 1;
constexpr std::uint32_t kVideoFrameTimestamp = 2;
constexpr std::uint32_t kOwnedVideoBufferInfo = 2;
constexpr std::uint32_t kVideoBufferInfoType = 1;
constexpr std::uint32_t kVideoBufferInfoWidth = 2;
constexpr std::uint32_t kVideoBufferInfoHeight = 3;
constexpr std::uint32_t kVideoBufferInfoComponents = 7;
constexpr std::uint32_t kComponentDataPtr = 1;
constexpr std::uint32_t kComponentStride = 2;
constexpr std::uint32_t kComponentSize = 3;

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
	const std::optional<std::span<const std::uint8_t>> bytes = FindMessage(data, field);
	if (!bytes) {
		return std::nullopt;
	}
	return std::string(
		reinterpret_cast<const char *>(bytes->data()),
		bytes->size());
}

// Returns the payloads of every field with the given number. A repeated
// message field has one payload per element.
[[nodiscard]] std::vector<std::span<const std::uint8_t>> FindMessages(
		std::span<const std::uint8_t> data,
		std::uint32_t field) {
	std::vector<std::span<const std::uint8_t>> result;
	try {
		ProtoReader reader(data);
		while (reader.next()) {
			if (reader.field() == field
				&& reader.wireType() == ProtoWireType::LengthDelimited) {
				result.push_back(reader.readBytes());
			} else {
				reader.skip();
			}
		}
	} catch (const ProtoWireError &) {
		return {};
	}
	return result;
}

// Reads FfiOwnedHandle.id from an owned message that wraps the handle in its
// field 1, for example OwnedTrack or OwnedVideoSource.
[[nodiscard]] std::optional<std::uint64_t> OwnedHandleId(
		std::span<const std::uint8_t> owned) {
	const std::optional<std::span<const std::uint8_t>> handle = FindMessage(owned, kOwnedHandleMessage);
	if (!handle) {
		return std::nullopt;
	}
	return FindVarint(*handle, kOwnedHandleValue);
}

[[nodiscard]] LiveKitTrackKind TrackKindOf(std::uint64_t kind) {
	if (kind == kTrackKindAudio) {
		return LiveKitTrackKind::Audio;
	} else if (kind == kTrackKindVideo) {
		return LiveKitTrackKind::Video;
	}
	return LiveKitTrackKind::Unknown;
}

// Builds the E2eeOptions message of the connect request.
[[nodiscard]] ProtoWriter EncodeE2eeOptions(
		const LiveKitEncryptionOptions &encryption) {
	ProtoWriter keyProvider;
	if (!encryption.sharedKey.empty()) {
		keyProvider.writeBytesField(
			kKeyProviderSharedKey,
			encryption.sharedKey);
	}
	keyProvider.writeVarintField(
		kKeyProviderRatchetWindowSize,
		kRatchetWindowSize);
	keyProvider.writeStringField(kKeyProviderRatchetSalt, kRatchetSalt);
	keyProvider.writeSignedVarintField(
		kKeyProviderFailureTolerance,
		kFailureTolerance);
	keyProvider.writeVarintField(kKeyProviderKeyRingSize, kKeyRingSize);
	keyProvider.writeVarintField(
		kKeyProviderKeyDerivationFunction,
		kKeyDerivationHkdf);

	ProtoWriter e2ee;
	e2ee.writeVarintField(kE2eeEncryptionType, kEncryptionTypeGcm);
	e2ee.writeMessageField(kE2eeKeyProviderOptions, keyProvider);
	return e2ee;
}

// Reads the RoomEvent of a room event envelope: the room handle and the track
// subscription or unsubscription the adapter needs.
void DecodeRoomEvent(
		std::span<const std::uint8_t> payload,
		LiveKitEvent &result) {
	const std::optional<std::uint64_t> roomHandle = FindVarint(
		payload,
		kRoomEventHandle);
	if (roomHandle) {
		result.roomHandle = *roomHandle;
	}
	const std::optional<std::span<const std::uint8_t>> subscribed =
		FindMessage(payload, kRoomEventTrackSubscribed);
	if (subscribed) {
		result.kind = LiveKitEventKind::TrackSubscribed;
		const std::optional<std::string> identity = FindString(
			*subscribed,
			kTrackSubscribedIdentity);
		if (identity) {
			result.participantIdentity = *identity;
		}
		const std::optional<std::span<const std::uint8_t>> track =
			FindMessage(*subscribed, kTrackSubscribedTrack);
		if (track) {
			result.trackHandle = OwnedHandleId(*track);
			const std::optional<std::span<const std::uint8_t>> info =
				FindMessage(*track, kOwnedTrackInfo);
			if (info) {
				const std::optional<std::string> sid = FindString(
					*info,
					kTrackInfoSid);
				if (sid) {
					result.trackSid = *sid;
				}
				const std::uint64_t kind = FindVarint(*info, kTrackInfoKind)
					.value_or(0);
				result.trackKind = TrackKindOf(kind);
			}
		}
		return;
	}
	const std::optional<std::span<const std::uint8_t>> unsubscribed =
		FindMessage(payload, kRoomEventTrackUnsubscribed);
	if (unsubscribed) {
		result.kind = LiveKitEventKind::TrackUnsubscribed;
		const std::optional<std::string> identity = FindString(
			*unsubscribed,
			kTrackUnsubscribedIdentity);
		if (identity) {
			result.participantIdentity = *identity;
		}
		const std::optional<std::string> sid = FindString(
			*unsubscribed,
			kTrackUnsubscribedSid);
		if (sid) {
			result.trackSid = *sid;
		}
	}
}

// Reads a VideoStreamEvent. Only a frame is kept; end of stream is not used by
// the adapter, which drops the stream when the track is unsubscribed.
void DecodeVideoStreamEvent(
		std::span<const std::uint8_t> payload,
		LiveKitEvent &result) {
	const std::optional<std::span<const std::uint8_t>> frame =
		FindMessage(payload, kVideoStreamEventFrame);
	if (!frame) {
		return;
	}
	result.kind = LiveKitEventKind::VideoFrame;
	result.streamHandle = FindVarint(payload, kVideoStreamEventHandle);

	LiveKitVideoBuffer buffer;
	buffer.timestampUs = static_cast<std::int64_t>(
		FindVarint(*frame, kVideoFrameTimestamp).value_or(0));
	const std::optional<std::span<const std::uint8_t>> owned =
		FindMessage(*frame, kVideoFrameBuffer);
	if (!owned) {
		return;
	}
	// The handle is kept even when the info is unusable, so the adapter can
	// release it.
	buffer.bufferHandle = OwnedHandleId(*owned).value_or(0);
	const std::optional<std::span<const std::uint8_t>> info =
		FindMessage(*owned, kOwnedVideoBufferInfo);
	if (!info) {
		result.videoBuffer = std::move(buffer);
		return;
	}
	buffer.type = static_cast<std::uint32_t>(
		FindVarint(*info, kVideoBufferInfoType).value_or(0));
	buffer.width = static_cast<std::uint32_t>(
		FindVarint(*info, kVideoBufferInfoWidth).value_or(0));
	buffer.height = static_cast<std::uint32_t>(
		FindVarint(*info, kVideoBufferInfoHeight).value_or(0));
	for (const std::span<const std::uint8_t> component
			: FindMessages(*info, kVideoBufferInfoComponents)) {
		LiveKitVideoComponent plane;
		plane.dataPtr = FindVarint(component, kComponentDataPtr).value_or(0);
		plane.stride = static_cast<std::uint32_t>(
			FindVarint(component, kComponentStride).value_or(0));
		plane.size = static_cast<std::uint32_t>(
			FindVarint(component, kComponentSize).value_or(0));
		buffer.components.push_back(plane);
	}
	result.videoBuffer = std::move(buffer);
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
		std::optional<std::uint64_t> requestAsyncId,
		const std::optional<LiveKitEncryptionOptions> &encryption) {
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
	if (encryption) {
		roomOptions.writeMessageField(
			kRoomEncryption,
			EncodeE2eeOptions(*encryption));
	}

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

std::vector<std::uint8_t> EncodeLiveKitReadyForRoomEventRequest(
		std::uint64_t roomHandle) {
	ProtoWriter ready;
	ready.writeVarintField(kReadyRoomHandle, roomHandle);
	return WrapFfiRequest(kFfiRequestReadyForRoomEvent, ready);
}

std::vector<std::uint8_t> EncodeLiveKitNewVideoStreamRequest(
		std::uint64_t trackHandle) {
	ProtoWriter stream;
	stream.writeVarintField(kNewVideoStreamTrackHandle, trackHandle);
	stream.writeVarintField(kNewVideoStreamType, kVideoStreamTypeNative);
	stream.writeVarintField(kNewVideoStreamFormat, kVideoBufferTypeI420);
	stream.writeVarintField(
		kNewVideoStreamNormalizeStride,
		BoolValue(true));
	return WrapFfiRequest(kFfiRequestNewVideoStream, stream);
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
	const std::optional<std::span<const std::uint8_t>> message = FindMessage(response, kFfiResponseLocalTrackMute);
	if (!message) {
		return std::nullopt;
	}
	return FindVarint(*message, kLocalTrackMuteMuted).value_or(0) != 0;
}

std::optional<std::uint64_t> DecodeLiveKitAsyncId(
		const std::vector<std::uint8_t> &response,
		std::uint32_t responseField) {
	const std::optional<std::span<const std::uint8_t>> message = FindMessage(response, responseField);
	if (!message) {
		return std::nullopt;
	}
	return FindVarint(*message, kAsyncId);
}

LiveKitHandleResponse DecodeLiveKitHandleResponse(
		const std::vector<std::uint8_t> &response,
		std::uint32_t responseField) {
	LiveKitHandleResponse result;
	const std::optional<std::span<const std::uint8_t>> message = FindMessage(response, responseField);
	if (!message) {
		return result;
	}
	if (const std::optional<std::string> error = FindString(*message, kErrorString)) {
		result.error = *error;
	}
	const std::optional<std::span<const std::uint8_t>> owned = FindMessage(*message, kOwnedMessage);
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
			const std::uint32_t field = reader.field();
			const bool used = field == kEventRoom
				|| field == kEventVideoStream
				|| field == kEventConnect
				|| field == kEventDisconnect
				|| field == kEventPublishTrack;
			if (!used
				|| reader.wireType() != ProtoWireType::LengthDelimited) {
				reader.skip();
				continue;
			}
			const std::span<const std::uint8_t> payload = reader.readBytes();
			if (field == kEventRoom) {
				DecodeRoomEvent(payload, result);
				return result;
			} else if (field == kEventVideoStream) {
				DecodeVideoStreamEvent(payload, result);
				return result;
			}
			if (const std::optional<std::uint64_t> asyncId = FindVarint(payload, kAsyncId)) {
				result.asyncId = *asyncId;
			}
			if (field == kEventDisconnect) {
				result.kind = LiveKitEventKind::Disconnect;
			} else if (field == kEventPublishTrack) {
				result.kind = LiveKitEventKind::PublishTrack;
				if (const std::optional<std::string> error = FindString(payload, kErrorString)) {
					result.error = *error;
				}
				const std::optional<std::span<const std::uint8_t>> publication = FindMessage(
					payload,
					kPublishCallbackPublication);
				if (publication) {
					result.publicationHandle = OwnedHandleId(*publication);
				}
			} else {
				result.kind = LiveKitEventKind::Connect;
				if (const std::optional<std::string> error = FindString(
						payload,
						kConnectCallbackError)) {
					result.error = *error;
				}
				const std::optional<std::span<const std::uint8_t>> callbackResult = FindMessage(
					payload,
					kConnectCallbackResult);
				if (callbackResult) {
					const std::optional<std::span<const std::uint8_t>> room = FindMessage(
						*callbackResult,
						kConnectResultRoom);
					if (room) {
						result.roomHandle = OwnedHandleId(*room);
					}
					const std::optional<std::span<const std::uint8_t>> participant = FindMessage(
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
