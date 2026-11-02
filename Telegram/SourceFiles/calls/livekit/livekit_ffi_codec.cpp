#include "calls/livekit/livekit_ffi_codec.h"

#include "mtproto/impulse/core/wire.h"

#include <span>
#include <utility>

namespace Impulse {
namespace {

// FfiRequest oneof field numbers (livekit-ffi/protocol/ffi.proto).
constexpr std::uint32_t kFfiRequestDispose = 2;
constexpr std::uint32_t kFfiRequestConnect = 3;
constexpr std::uint32_t kFfiRequestDisconnect = 4;
constexpr std::uint32_t kFfiRequestPublishTrack = 5;
constexpr std::uint32_t kFfiRequestUnpublishTrack = 6;
constexpr std::uint32_t kFfiRequestGetSessionStats = 12;
constexpr std::uint32_t kFfiRequestCreateVideoTrack = 15;
constexpr std::uint32_t kFfiRequestCreateAudioTrack = 16;
constexpr std::uint32_t kFfiRequestNewVideoSource = 21;
constexpr std::uint32_t kFfiRequestNewAudioSource = 26;
constexpr std::uint32_t kFfiRequestNewPlatformAudio = 77;
constexpr std::uint32_t kFfiRequestGetAudioDevices = 78;
constexpr std::uint32_t kFfiRequestSetRecordingDevice = 79;
constexpr std::uint32_t kFfiRequestSetPlayoutDevice = 80;
constexpr std::uint32_t kFfiRequestLocalTrackMute = 17;
constexpr std::uint32_t kFfiResponseLocalTrackMute = 17;
constexpr std::uint32_t kFfiRequestNewVideoStream = 20;
constexpr std::uint32_t kFfiRequestReadyForRoomEvent = 83;
constexpr std::uint32_t kFfiRequestCaptureVideoFrame = 22;
constexpr std::uint32_t kFfiResponseCaptureVideoFrame = 22;
constexpr std::uint32_t kFfiRequestRemotePublicationQuality = 68;
constexpr std::uint32_t kFfiResponseRemotePublicationQuality = 67;

// FfiResponse fields of the responses that the codec decodes.
constexpr std::uint32_t kFfiResponseNewPlatformAudio = 76;
constexpr std::uint32_t kFfiResponseGetAudioDevices = 77;

// CaptureVideoFrameRequest fields and the rotation of an unrotated frame.
constexpr std::uint32_t kCaptureSourceHandle = 1;
constexpr std::uint32_t kCaptureBuffer = 2;
constexpr std::uint32_t kCaptureTimestampUs = 3;
constexpr std::uint32_t kCaptureRotation = 4;
constexpr std::uint64_t kVideoRotation0 = 0;

// SetRemoteTrackPublicationQualityRequest fields.
constexpr std::uint32_t kRemoteQualityPublicationHandle = 1;
constexpr std::uint32_t kRemoteQualityQuality = 2;

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

// GetSessionStatsRequest fields.
constexpr std::uint32_t kSessionStatsRoomHandle = 1;
constexpr std::uint32_t kSessionStatsRequestAsyncId = 2;

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
constexpr std::uint32_t kNewAudioSourceOptions = 2;
constexpr std::uint32_t kNewAudioSourcePlatformAudioHandle = 6;
constexpr std::uint64_t kAudioSourceTypePlatform = 1;

// AudioSourceOptions fields. Echo cancellation and gain control are on, as the
// LiveKit defaults that Android uses.
constexpr std::uint32_t kAudioOptionsEchoCancellation = 1;
constexpr std::uint32_t kAudioOptionsNoiseSuppression = 2;
constexpr std::uint32_t kAudioOptionsAutoGainControl = 3;

// GetAudioDevicesRequest, SetRecordingDeviceRequest and SetPlayoutDeviceRequest
// fields. The device lists of GetAudioDevicesResponse and the error of the set
// responses are read with the constants below.
constexpr std::uint32_t kDeviceRequestHandle = 1;
constexpr std::uint32_t kDeviceRequestDeviceId = 2;
constexpr std::uint32_t kDevicesPlaybackList = 1;
constexpr std::uint32_t kDevicesRecordingList = 2;
constexpr std::uint32_t kDevicesError = 3;
constexpr std::uint32_t kDeviceInfoIndex = 1;
constexpr std::uint32_t kDeviceInfoName = 2;
constexpr std::uint32_t kDeviceInfoGuid = 3;
constexpr std::uint32_t kSetDeviceError = 1;

// NewPlatformAudioResponse holds OwnedPlatformAudio, which holds the
// PlatformAudioInfo with the device counts.
constexpr std::uint32_t kPlatformAudioOwned = 1;
constexpr std::uint32_t kOwnedPlatformAudioInfo = 2;
constexpr std::uint32_t kPlatformAudioRecordingCount = 1;
constexpr std::uint32_t kPlatformAudioPlayoutCount = 2;

// CreateAudioTrackRequest and CreateVideoTrackRequest fields.
constexpr std::uint32_t kCreateTrackName = 1;
constexpr std::uint32_t kCreateTrackSourceHandle = 2;

// NewVideoSourceRequest fields, VideoSourceType.NATIVE and VideoSourceResolution.
constexpr std::uint32_t kNewVideoSourceType = 1;
constexpr std::uint32_t kNewVideoSourceResolution = 2;
constexpr std::uint32_t kNewVideoSourceScreencast = 3;
constexpr std::uint32_t kVideoSourceTypeNative = 0;
constexpr std::uint32_t kResolutionWidth = 1;
constexpr std::uint32_t kResolutionHeight = 2;

// PublishTrackRequest and TrackPublishOptions fields.
constexpr std::uint32_t kPublishLocalParticipantHandle = 1;
constexpr std::uint32_t kPublishTrackHandle = 2;
constexpr std::uint32_t kPublishOptions = 3;
constexpr std::uint32_t kPublishRequestAsyncId = 4;
constexpr std::uint32_t kPublishOptionsSimulcast = 6;
constexpr std::uint32_t kPublishOptionsSource = 7;

// UnpublishTrackRequest fields and the error of UnpublishTrackCallback.
constexpr std::uint32_t kUnpublishLocalParticipantHandle = 1;
constexpr std::uint32_t kUnpublishTrackSid = 2;
constexpr std::uint32_t kUnpublishStopOnUnpublish = 3;
constexpr std::uint32_t kUnpublishRequestAsyncId = 4;
constexpr std::uint32_t kUnpublishCallbackError = 2;

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
constexpr std::uint32_t kEventUnpublishTrack = 10;
constexpr std::uint32_t kEventLogs = 18;
constexpr std::uint32_t kEventSessionStats = 19;

// ConnectCallback fields.
constexpr std::uint32_t kConnectCallbackError = 2;
constexpr std::uint32_t kConnectCallbackResult = 3;
constexpr std::uint32_t kConnectResultRoom = 1;
constexpr std::uint32_t kConnectResultLocalParticipant = 2;

// PublishTrackCallback field.
constexpr std::uint32_t kPublishCallbackPublication = 3;

// GetSessionStatsCallback fields and its Result.
constexpr std::uint32_t kStatsCallbackAsyncId = 1;
constexpr std::uint32_t kStatsCallbackError = 2;
constexpr std::uint32_t kStatsCallbackResult = 3;
constexpr std::uint32_t kStatsResultPublisher = 1;
constexpr std::uint32_t kStatsResultSubscriber = 2;

// RtcStats oneof members that the codec reads, and the fields of the RTP
// stream messages (stats.proto). The stream and the kind are shared by the
// inbound and the outbound RTP stats.
constexpr std::uint32_t kRtcStatsInboundRtp = 4;
constexpr std::uint32_t kRtcStatsOutboundRtp = 5;
constexpr std::uint32_t kRtpStreamField = 2;
constexpr std::uint32_t kRtpStreamKind = 2;
constexpr std::uint32_t kOutboundSentField = 3;
constexpr std::uint32_t kSentPacketsSent = 1;
constexpr std::uint32_t kSentBytesSent = 2;
constexpr std::uint32_t kInboundReceivedField = 3;
constexpr std::uint32_t kReceivedPacketsReceived = 1;
constexpr std::uint32_t kReceivedPacketsLost = 2;
constexpr std::uint32_t kInboundStatsField = 4;
constexpr std::uint32_t kInboundFramesDecoded = 4;
constexpr std::uint32_t kInboundBytesReceived = 25;
constexpr std::uint32_t kInboundTotalSamplesReceived = 35;
constexpr std::uint32_t kInboundAudioLevel = 41;

// OutboundRtp.outbound and the OutboundRtpStreamStats fields that the codec
// reads. The quality limitation reason is a QualityLimitationReason number.
constexpr std::uint32_t kOutboundStatsField = 4;
constexpr std::uint32_t kOutboundRid = 4;
constexpr std::uint32_t kOutboundRetransmittedPacketsSent = 6;
constexpr std::uint32_t kOutboundTargetBitrate = 9;
constexpr std::uint32_t kOutboundFrameWidth = 11;
constexpr std::uint32_t kOutboundFrameHeight = 12;
constexpr std::uint32_t kOutboundFramesPerSecond = 13;
constexpr std::uint32_t kOutboundFramesSent = 14;
constexpr std::uint32_t kOutboundFramesEncoded = 16;
constexpr std::uint32_t kOutboundKeyFramesEncoded = 17;
constexpr std::uint32_t kOutboundQualityLimitationReason = 21;
constexpr std::uint32_t kOutboundNackCount = 24;
constexpr std::uint32_t kOutboundPliCount = 26;
constexpr std::uint32_t kOutboundActive = 29;

// The video fields of InboundRtpStreamStats that the codec reads.
constexpr std::uint32_t kInboundKeyFramesDecoded = 5;
constexpr std::uint32_t kInboundFramesDropped = 7;
constexpr std::uint32_t kInboundFrameWidth = 8;
constexpr std::uint32_t kInboundFrameHeight = 9;
constexpr std::uint32_t kInboundFramesPerSecond = 10;
constexpr std::uint32_t kInboundPauseCount = 15;
constexpr std::uint32_t kInboundFreezeCount = 17;

// LogBatch and LogRecord fields (livekit-ffi protocol).
constexpr std::uint32_t kLogBatchRecords = 1;
constexpr std::uint32_t kLogRecordLevel = 1;
constexpr std::uint32_t kLogRecordTarget = 2;
constexpr std::uint32_t kLogRecordMessage = 6;

// RoomEvent fields and the oneof members the adapter reads.
constexpr std::uint32_t kRoomEventHandle = 1;
constexpr std::uint32_t kRoomEventParticipantDisconnected = 3;
constexpr std::uint32_t kRoomEventTrackSubscribed = 9;
constexpr std::uint32_t kRoomEventTrackUnsubscribed = 10;
constexpr std::uint32_t kRoomEventTrackMuted = 12;
constexpr std::uint32_t kRoomEventTrackUnmuted = 13;
constexpr std::uint32_t kRoomEventDisconnected = 22;
constexpr std::uint32_t kRoomEventE2eeStateChanged = 25;
constexpr std::uint32_t kRoomEventTrackPublished = 7;
constexpr std::uint32_t kRoomEventTrackUnpublished = 8;

// ParticipantDisconnected.participant_identity.
constexpr std::uint32_t kParticipantDisconnectedIdentity = 1;

// TrackMuted and TrackUnmuted fields: the participant identity and the track sid.
constexpr std::uint32_t kTrackMuteIdentity = 1;
constexpr std::uint32_t kTrackMuteSid = 2;

// Disconnected.reason, and the DisconnectReason value of a disconnect that this
// client asked for (participant.proto).
constexpr std::uint32_t kDisconnectedReason = 1;
constexpr std::uint64_t kDisconnectReasonClientInitiated = 1;

// E2eeStateChanged fields (room.proto).
constexpr std::uint32_t kE2eeStateParticipantIdentity = 1;
constexpr std::uint32_t kE2eeStateState = 2;

// TrackSubscribed and TrackUnsubscribed fields.
constexpr std::uint32_t kTrackSubscribedIdentity = 1;
constexpr std::uint32_t kTrackSubscribedTrack = 2;
constexpr std::uint32_t kTrackUnsubscribedIdentity = 1;
constexpr std::uint32_t kTrackUnsubscribedSid = 2;

// TrackPublished fields (participant identity and OwnedTrackPublication) and
// TrackUnpublished fields (participant identity and publication sid).
constexpr std::uint32_t kTrackPublishedIdentity = 1;
constexpr std::uint32_t kTrackPublishedPublication = 2;
constexpr std::uint32_t kTrackUnpublishedIdentity = 1;
constexpr std::uint32_t kTrackUnpublishedSid = 2;

// OwnedTrack and TrackInfo fields. TrackInfo.kind is TrackKind: 1 audio, 2 video.
// TrackInfo.muted is the mute state of a subscribed track.
constexpr std::uint32_t kOwnedTrackInfo = 2;
constexpr std::uint32_t kTrackInfoSid = 1;
constexpr std::uint32_t kTrackInfoKind = 3;
constexpr std::uint32_t kTrackInfoMuted = 5;
constexpr std::uint64_t kTrackKindAudio = 1;
constexpr std::uint64_t kTrackKindVideo = 2;

// TrackPublicationInfo.source and TrackPublicationInfo.muted.
constexpr std::uint32_t kTrackPublicationInfoSource = 4;
constexpr std::uint32_t kTrackPublicationInfoMuted = 9;

// TrackSource values of the publication info (track.proto).
constexpr std::uint64_t kTrackSourceCamera = 1;
constexpr std::uint64_t kTrackSourceMicrophone = 2;
constexpr std::uint64_t kTrackSourceScreenShare = 3;
constexpr std::uint64_t kTrackSourceScreenShareAudio = 4;

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
constexpr std::uint32_t kVideoBufferInfoDataPtr = 4;
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

[[nodiscard]] std::optional<double> FindDouble(
		std::span<const std::uint8_t> data,
		std::uint32_t field) {
	try {
		ProtoReader reader(data);
		while (reader.next()) {
			if (reader.field() == field
				&& reader.wireType() == ProtoWireType::Fixed64) {
				return reader.readDouble();
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

// Reads the sid of an OwnedTrackPublication from its TrackPublicationInfo. The
// sid is empty when the publication carries no info.
[[nodiscard]] std::string PublicationSid(
		std::span<const std::uint8_t> publication) {
	const std::optional<std::span<const std::uint8_t>> info = FindMessage(
		publication,
		kOwnedTrackInfo);
	if (!info) {
		return std::string();
	}
	return FindString(*info, kTrackInfoSid).value_or(std::string());
}

// Reads a 32-bit count or size field, zero when the field is absent.
[[nodiscard]] std::uint32_t FindUint32(
		std::span<const std::uint8_t> data,
		std::uint32_t field) {
	return static_cast<std::uint32_t>(FindVarint(data, field).value_or(0));
}

[[nodiscard]] LiveKitTrackKind TrackKindOf(std::uint64_t kind) {
	if (kind == kTrackKindAudio) {
		return LiveKitTrackKind::Audio;
	} else if (kind == kTrackKindVideo) {
		return LiveKitTrackKind::Video;
	}
	return LiveKitTrackKind::Unknown;
}

[[nodiscard]] LiveKitTrackSource TrackSourceOf(std::uint64_t source) {
	if (source == kTrackSourceCamera) {
		return LiveKitTrackSource::Camera;
	} else if (source == kTrackSourceMicrophone) {
		return LiveKitTrackSource::Microphone;
	} else if (source == kTrackSourceScreenShare) {
		return LiveKitTrackSource::ScreenShare;
	} else if (source == kTrackSourceScreenShareAudio) {
		return LiveKitTrackSource::ScreenShareAudio;
	}
	return LiveKitTrackSource::Unknown;
}

// The RTP stream kind of the stats is the MediaKind string, "audio" or "video".
[[nodiscard]] LiveKitTrackKind StatsKindOf(const std::string &kind) {
	if (kind == "audio") {
		return LiveKitTrackKind::Audio;
	} else if (kind == "video") {
		return LiveKitTrackKind::Video;
	}
	return LiveKitTrackKind::Unknown;
}

[[nodiscard]] LiveKitLogLevel LogLevelOf(std::optional<std::uint64_t> level) {
	if (!level) {
		return LiveKitLogLevel::Unknown;
	} else if (*level == 0) {
		return LiveKitLogLevel::Error;
	} else if (*level == 1) {
		return LiveKitLogLevel::Warn;
	} else if (*level == 2) {
		return LiveKitLogLevel::Info;
	} else if (*level == 3) {
		return LiveKitLogLevel::Debug;
	} else if (*level == 4) {
		return LiveKitLogLevel::Trace;
	}
	return LiveKitLogLevel::Unknown;
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

[[nodiscard]] LiveKitEncryptionState EncryptionStateOf(std::uint64_t state) {
	switch (state) {
	case 0:
		return LiveKitEncryptionState::New;
	case 1:
		return LiveKitEncryptionState::Ok;
	case 2:
		return LiveKitEncryptionState::EncryptionFailed;
	case 3:
		return LiveKitEncryptionState::DecryptionFailed;
	case 4:
		return LiveKitEncryptionState::MissingKey;
	case 5:
		return LiveKitEncryptionState::KeyRatcheted;
	case 6:
		return LiveKitEncryptionState::InternalError;
	default:
		return LiveKitEncryptionState::Unknown;
	}
}

// Reads the TrackPublished of a remote participant. The publication handle is
// owned by the adapter, the sid, the kind, the source and the muted state name
// the track of the publication.
void DecodeTrackPublished(
		std::span<const std::uint8_t> payload,
		LiveKitEvent &result) {
	result.kind = LiveKitEventKind::TrackPublished;
	const std::optional<std::string> identity = FindString(
		payload,
		kTrackPublishedIdentity);
	if (identity) {
		result.participantIdentity = *identity;
	}
	const std::optional<std::span<const std::uint8_t>> publication =
		FindMessage(payload, kTrackPublishedPublication);
	if (!publication) {
		return;
	}
	result.publicationHandle = OwnedHandleId(*publication);
	const std::optional<std::span<const std::uint8_t>> info =
		FindMessage(*publication, kOwnedTrackInfo);
	if (info) {
		const std::optional<std::string> sid = FindString(*info, kTrackInfoSid);
		if (sid) {
			result.trackSid = *sid;
		}
		const std::uint64_t kind = FindVarint(*info, kTrackInfoKind).value_or(0);
		result.trackKind = TrackKindOf(kind);
		const std::uint64_t source = FindVarint(*info, kTrackPublicationInfoSource).value_or(0);
		result.trackSource = TrackSourceOf(source);
		result.trackMuted = FindVarint(*info, kTrackPublicationInfoMuted).value_or(0) != 0;
	}
}

// Reads a TrackUnpublished of a remote participant, which names the publication
// by its sid.
void DecodeTrackUnpublished(
		std::span<const std::uint8_t> payload,
		LiveKitEvent &result) {
	result.kind = LiveKitEventKind::TrackUnpublished;
	const std::optional<std::string> identity = FindString(
		payload,
		kTrackUnpublishedIdentity);
	if (identity) {
		result.participantIdentity = *identity;
	}
	const std::optional<std::string> sid = FindString(payload, kTrackUnpublishedSid);
	if (sid) {
		result.trackSid = *sid;
	}
}

// Reads a TrackMuted or a TrackUnmuted, which both name the track by its sid
// and carry the participant identity.
void DecodeTrackMuteEvent(
		std::span<const std::uint8_t> payload,
		LiveKitEventKind kind,
		LiveKitEvent &result) {
	result.kind = kind;
	const std::optional<std::string> identity = FindString(payload, kTrackMuteIdentity);
	if (identity) {
		result.participantIdentity = *identity;
	}
	const std::optional<std::string> sid = FindString(payload, kTrackMuteSid);
	if (sid) {
		result.trackSid = *sid;
	}
}

// Reads the RoomEvent of a room event envelope: the room handle, the track
// subscription or unsubscription, the track mute state, the participant that
// left, the room drop and the E2EE state the adapter needs.
void DecodeRoomEvent(
		std::span<const std::uint8_t> payload,
		LiveKitEvent &result) {
	const std::optional<std::uint64_t> roomHandle = FindVarint(
		payload,
		kRoomEventHandle);
	if (roomHandle) {
		result.roomHandle = *roomHandle;
	}
	const std::optional<std::span<const std::uint8_t>> disconnected =
		FindMessage(payload, kRoomEventDisconnected);
	if (disconnected) {
		result.kind = LiveKitEventKind::RoomDisconnected;
		const std::uint64_t reason = FindVarint(*disconnected, kDisconnectedReason)
			.value_or(0);
		result.disconnectReason = reason;
		result.clientInitiated = reason == kDisconnectReasonClientInitiated;
		return;
	}
	const std::optional<std::span<const std::uint8_t>> encryption =
		FindMessage(payload, kRoomEventE2eeStateChanged);
	if (encryption) {
		result.kind = LiveKitEventKind::EncryptionStateChanged;
		const std::optional<std::string> identity = FindString(
			*encryption,
			kE2eeStateParticipantIdentity);
		if (identity) {
			result.participantIdentity = *identity;
		}
		result.encryptionState = EncryptionStateOf(
			FindVarint(*encryption, kE2eeStateState).value_or(0));
		return;
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
				result.trackMuted = FindVarint(*info, kTrackInfoMuted).value_or(0) != 0;
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
		return;
	}
	const std::optional<std::span<const std::uint8_t>> published =
		FindMessage(payload, kRoomEventTrackPublished);
	if (published) {
		DecodeTrackPublished(*published, result);
		return;
	}
	const std::optional<std::span<const std::uint8_t>> unpublished =
		FindMessage(payload, kRoomEventTrackUnpublished);
	if (unpublished) {
		DecodeTrackUnpublished(*unpublished, result);
		return;
	}
	const std::optional<std::span<const std::uint8_t>> muted =
		FindMessage(payload, kRoomEventTrackMuted);
	if (muted) {
		DecodeTrackMuteEvent(*muted, LiveKitEventKind::TrackMuted, result);
		return;
	}
	const std::optional<std::span<const std::uint8_t>> unmuted =
		FindMessage(payload, kRoomEventTrackUnmuted);
	if (unmuted) {
		DecodeTrackMuteEvent(*unmuted, LiveKitEventKind::TrackUnmuted, result);
		return;
	}
	const std::optional<std::span<const std::uint8_t>> participant =
		FindMessage(payload, kRoomEventParticipantDisconnected);
	if (participant) {
		result.kind = LiveKitEventKind::ParticipantDisconnected;
		const std::optional<std::string> identity = FindString(
			*participant,
			kParticipantDisconnectedIdentity);
		if (identity) {
			result.participantIdentity = *identity;
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

// Reads one audio device of a device list.
[[nodiscard]] LiveKitAudioDevice DecodeAudioDevice(
		std::span<const std::uint8_t> device) {
	LiveKitAudioDevice result;
	result.index = static_cast<std::uint32_t>(
		FindVarint(device, kDeviceInfoIndex).value_or(0));
	result.name = FindString(device, kDeviceInfoName).value_or(std::string());
	result.guid = FindString(device, kDeviceInfoGuid).value_or(std::string());
	return result;
}

// Reads the sent counters and the video fields of an outbound RTP stream.
[[nodiscard]] LiveKitOutboundRtpStats DecodeOutboundRtp(
		std::span<const std::uint8_t> body) {
	LiveKitOutboundRtpStats result;
	const std::optional<std::span<const std::uint8_t>> stream =
		FindMessage(body, kRtpStreamField);
	if (stream) {
		result.kind = StatsKindOf(
			FindString(*stream, kRtpStreamKind).value_or(std::string()));
	}
	const std::optional<std::span<const std::uint8_t>> sent =
		FindMessage(body, kOutboundSentField);
	if (sent) {
		result.packetsSent = FindVarint(*sent, kSentPacketsSent).value_or(0);
		result.bytesSent = FindVarint(*sent, kSentBytesSent).value_or(0);
	}
	const std::optional<std::span<const std::uint8_t>> outbound =
		FindMessage(body, kOutboundStatsField);
	if (outbound) {
		result.rid = FindString(*outbound, kOutboundRid).value_or(std::string());
		result.frameWidth = FindUint32(*outbound, kOutboundFrameWidth);
		result.frameHeight = FindUint32(*outbound, kOutboundFrameHeight);
		result.framesPerSecond = FindDouble(
			*outbound,
			kOutboundFramesPerSecond).value_or(0.);
		result.framesSent = FindUint32(*outbound, kOutboundFramesSent);
		result.framesEncoded = FindUint32(*outbound, kOutboundFramesEncoded);
		result.keyFramesEncoded = FindUint32(*outbound, kOutboundKeyFramesEncoded);
		result.retransmittedPacketsSent = FindVarint(
			*outbound,
			kOutboundRetransmittedPacketsSent).value_or(0);
		result.targetBitrate = FindDouble(*outbound, kOutboundTargetBitrate).value_or(0.);
		result.qualityLimitationReason = FindUint32(
			*outbound,
			kOutboundQualityLimitationReason);
		result.active = FindVarint(*outbound, kOutboundActive).value_or(0) != 0;
		result.pliCount = FindUint32(*outbound, kOutboundPliCount);
		result.nackCount = FindUint32(*outbound, kOutboundNackCount);
	}
	return result;
}

// Reads the received counters of an inbound RTP stream. The audio fields are
// read for audio streams, and the frame fields for video streams.
[[nodiscard]] LiveKitInboundRtpStats DecodeInboundRtp(
		std::span<const std::uint8_t> body) {
	LiveKitInboundRtpStats result;
	const std::optional<std::span<const std::uint8_t>> stream =
		FindMessage(body, kRtpStreamField);
	if (stream) {
		result.kind = StatsKindOf(
			FindString(*stream, kRtpStreamKind).value_or(std::string()));
	}
	const std::optional<std::span<const std::uint8_t>> received =
		FindMessage(body, kInboundReceivedField);
	if (received) {
		result.packetsReceived = FindVarint(*received, kReceivedPacketsReceived).value_or(0);
		result.packetsLost = static_cast<std::int64_t>(
			FindVarint(*received, kReceivedPacketsLost).value_or(0));
	}
	const std::optional<std::span<const std::uint8_t>> inbound =
		FindMessage(body, kInboundStatsField);
	if (inbound) {
		result.bytesReceived = FindVarint(*inbound, kInboundBytesReceived).value_or(0);
		if (result.kind == LiveKitTrackKind::Audio) {
			result.totalSamplesReceived = FindVarint(
				*inbound,
				kInboundTotalSamplesReceived).value_or(0);
			result.audioLevel = FindDouble(*inbound, kInboundAudioLevel).value_or(0.);
		} else if (result.kind == LiveKitTrackKind::Video) {
			const std::optional<std::uint64_t> frames = FindVarint(
				*inbound,
				kInboundFramesDecoded);
			if (frames) {
				result.framesDecoded = static_cast<std::uint32_t>(*frames);
			}
			result.keyFramesDecoded = FindUint32(*inbound, kInboundKeyFramesDecoded);
			result.framesDropped = FindUint32(*inbound, kInboundFramesDropped);
			result.frameWidth = FindUint32(*inbound, kInboundFrameWidth);
			result.frameHeight = FindUint32(*inbound, kInboundFrameHeight);
			result.framesPerSecond = FindDouble(
				*inbound,
				kInboundFramesPerSecond).value_or(0.);
			result.pauseCount = FindUint32(*inbound, kInboundPauseCount);
			result.freezeCount = FindUint32(*inbound, kInboundFreezeCount);
		}
	}
	return result;
}

// Adds one RtcStats to the stats. Only the outbound and the inbound RTP members
// are kept; every other member is skipped.
void DecodeRtcStats(
		std::span<const std::uint8_t> rtc,
		LiveKitSessionStats &stats) {
	const std::optional<std::span<const std::uint8_t>> outbound =
		FindMessage(rtc, kRtcStatsOutboundRtp);
	if (outbound) {
		stats.outbound.push_back(DecodeOutboundRtp(*outbound));
		return;
	}
	const std::optional<std::span<const std::uint8_t>> inbound =
		FindMessage(rtc, kRtcStatsInboundRtp);
	if (inbound) {
		stats.inbound.push_back(DecodeInboundRtp(*inbound));
	}
}

// Reads a GetSessionStatsCallback. An error, or a callback with no result, gives
// stats with the error flag and no text of the library.
void DecodeSessionStatsCallback(
		std::span<const std::uint8_t> payload,
		LiveKitEvent &result) {
	result.kind = LiveKitEventKind::SessionStats;
	result.asyncId = FindVarint(payload, kStatsCallbackAsyncId).value_or(0);
	LiveKitSessionStats stats;
	const std::optional<std::span<const std::uint8_t>> callbackResult =
		FindMessage(payload, kStatsCallbackResult);
	if (FindString(payload, kStatsCallbackError)) {
		stats.error = true;
	} else if (callbackResult) {
		for (const std::span<const std::uint8_t> rtc
				: FindMessages(*callbackResult, kStatsResultPublisher)) {
			DecodeRtcStats(rtc, stats);
		}
		for (const std::span<const std::uint8_t> rtc
				: FindMessages(*callbackResult, kStatsResultSubscriber)) {
			DecodeRtcStats(rtc, stats);
		}
	} else {
		stats.error = true;
	}
	result.sessionStats = std::move(stats);
}

// Reads the records of a LogBatch.
[[nodiscard]] std::vector<LiveKitLogRecord> DecodeLogBatch(
		std::span<const std::uint8_t> payload) {
	std::vector<LiveKitLogRecord> result;
	for (const std::span<const std::uint8_t> record
			: FindMessages(payload, kLogBatchRecords)) {
		LiveKitLogRecord log;
		log.level = LogLevelOf(FindVarint(record, kLogRecordLevel));
		log.target = FindString(record, kLogRecordTarget).value_or(std::string());
		log.message = FindString(record, kLogRecordMessage).value_or(std::string());
		result.push_back(std::move(log));
	}
	return result;
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

std::vector<std::uint8_t> EncodeLiveKitGetSessionStatsRequest(
		std::uint64_t roomHandle,
		std::optional<std::uint64_t> requestAsyncId) {
	ProtoWriter stats;
	stats.writeVarintField(kSessionStatsRoomHandle, roomHandle);
	if (requestAsyncId) {
		stats.writeVarintField(kSessionStatsRequestAsyncId, *requestAsyncId);
	}
	return WrapFfiRequest(kFfiRequestGetSessionStats, stats);
}

std::vector<std::uint8_t> EncodeLiveKitNewPlatformAudioRequest() {
	const ProtoWriter empty;
	return WrapFfiRequest(kFfiRequestNewPlatformAudio, empty);
}

std::vector<std::uint8_t> EncodeLiveKitNewPlatformAudioSourceRequest(
		std::uint64_t platformAudioHandle,
		bool noiseSuppression) {
	ProtoWriter options;
	options.writeVarintField(
		kAudioOptionsEchoCancellation,
		BoolValue(true));
	options.writeVarintField(
		kAudioOptionsNoiseSuppression,
		BoolValue(noiseSuppression));
	options.writeVarintField(
		kAudioOptionsAutoGainControl,
		BoolValue(true));

	ProtoWriter source;
	source.writeVarintField(kNewAudioSourceType, kAudioSourceTypePlatform);
	source.writeMessageField(kNewAudioSourceOptions, options);
	source.writeVarintField(
		kNewAudioSourcePlatformAudioHandle,
		platformAudioHandle);
	return WrapFfiRequest(kFfiRequestNewAudioSource, source);
}

std::vector<std::uint8_t> EncodeLiveKitGetAudioDevicesRequest(
		std::uint64_t platformAudioHandle) {
	ProtoWriter devices;
	devices.writeVarintField(kDeviceRequestHandle, platformAudioHandle);
	return WrapFfiRequest(kFfiRequestGetAudioDevices, devices);
}

std::vector<std::uint8_t> EncodeLiveKitSetRecordingDeviceRequest(
		std::uint64_t platformAudioHandle,
		const std::string &deviceId) {
	ProtoWriter device;
	device.writeVarintField(kDeviceRequestHandle, platformAudioHandle);
	device.writeStringField(kDeviceRequestDeviceId, deviceId);
	return WrapFfiRequest(kFfiRequestSetRecordingDevice, device);
}

std::vector<std::uint8_t> EncodeLiveKitSetPlayoutDeviceRequest(
		std::uint64_t platformAudioHandle,
		const std::string &deviceId) {
	ProtoWriter device;
	device.writeVarintField(kDeviceRequestHandle, platformAudioHandle);
	device.writeStringField(kDeviceRequestDeviceId, deviceId);
	return WrapFfiRequest(kFfiRequestSetPlayoutDevice, device);
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
		std::uint32_t height,
		bool screencast) {
	ProtoWriter resolution;
	resolution.writeVarintField(kResolutionWidth, width);
	resolution.writeVarintField(kResolutionHeight, height);

	ProtoWriter source;
	source.writeVarintField(kNewVideoSourceType, kVideoSourceTypeNative);
	source.writeMessageField(kNewVideoSourceResolution, resolution);
	if (screencast) {
		source.writeVarintField(kNewVideoSourceScreencast, BoolValue(true));
	}
	return WrapFfiRequest(kFfiRequestNewVideoSource, source);
}

std::vector<std::uint8_t> EncodeLiveKitCaptureVideoFrameRequest(
		std::uint64_t sourceHandle,
		const LiveKitCaptureFrame &frame) {
	ProtoWriter buffer;
	buffer.writeVarintField(kVideoBufferInfoType, kVideoBufferTypeI420);
	buffer.writeVarintField(kVideoBufferInfoWidth, frame.width);
	buffer.writeVarintField(kVideoBufferInfoHeight, frame.height);
	// The planar formats are read through the components; the first plane is
	// the data pointer of the whole buffer.
	const std::uint64_t firstPlane = frame.components.empty()
		? 0
		: frame.components.front().dataPtr;
	buffer.writeVarintField(kVideoBufferInfoDataPtr, firstPlane);
	for (const LiveKitVideoComponent &component : frame.components) {
		ProtoWriter plane;
		plane.writeVarintField(kComponentDataPtr, component.dataPtr);
		plane.writeVarintField(kComponentStride, component.stride);
		plane.writeVarintField(kComponentSize, component.size);
		buffer.writeMessageField(kVideoBufferInfoComponents, plane);
	}

	ProtoWriter capture;
	capture.writeVarintField(kCaptureSourceHandle, sourceHandle);
	capture.writeMessageField(kCaptureBuffer, buffer);
	capture.writeVarintField(
		kCaptureTimestampUs,
		static_cast<std::uint64_t>(frame.timestampUs));
	capture.writeVarintField(kCaptureRotation, kVideoRotation0);
	return WrapFfiRequest(kFfiRequestCaptureVideoFrame, capture);
}

bool DecodeLiveKitCaptureVideoFrameResponse(
		const std::vector<std::uint8_t> &response) {
	return FindMessage(response, kFfiResponseCaptureVideoFrame).has_value();
}

std::vector<std::uint8_t> EncodeLiveKitRemotePublicationQualityRequest(
		std::uint64_t publicationHandle,
		LiveKitVideoQuality quality) {
	ProtoWriter request;
	request.writeVarintField(kRemoteQualityPublicationHandle, publicationHandle);
	request.writeVarintField(
		kRemoteQualityQuality,
		static_cast<std::uint64_t>(quality));
	return WrapFfiRequest(kFfiRequestRemotePublicationQuality, request);
}

bool DecodeLiveKitRemotePublicationQualityResponse(
		const std::vector<std::uint8_t> &response) {
	return FindMessage(response, kFfiResponseRemotePublicationQuality).has_value();
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
		std::optional<std::uint64_t> requestAsyncId,
		std::optional<bool> simulcast) {
	ProtoWriter options;
	if (simulcast) {
		options.writeVarintField(
			kPublishOptionsSimulcast,
			BoolValue(*simulcast));
	}
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

std::vector<std::uint8_t> EncodeLiveKitUnpublishTrackRequest(
		std::uint64_t localParticipantHandle,
		const std::string &trackSid,
		bool stopOnUnpublish,
		std::optional<std::uint64_t> requestAsyncId) {
	ProtoWriter unpublish;
	unpublish.writeVarintField(
		kUnpublishLocalParticipantHandle,
		localParticipantHandle);
	unpublish.writeStringField(kUnpublishTrackSid, trackSid);
	unpublish.writeVarintField(
		kUnpublishStopOnUnpublish,
		BoolValue(stopOnUnpublish));
	if (requestAsyncId) {
		unpublish.writeVarintField(kUnpublishRequestAsyncId, *requestAsyncId);
	}
	return WrapFfiRequest(kFfiRequestUnpublishTrack, unpublish);
}

std::string LiveKitQualityLimitationName(std::uint32_t reason) {
	if (reason == 0) {
		return "none";
	} else if (reason == 1) {
		return "cpu";
	} else if (reason == 2) {
		return "bandwidth";
	}
	return "other";
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

std::optional<LiveKitPlatformAudioInfo> DecodePlatformAudioInfo(
		const std::vector<std::uint8_t> &response) {
	const std::optional<std::span<const std::uint8_t>> message =
		FindMessage(response, kFfiResponseNewPlatformAudio);
	if (!message) {
		return std::nullopt;
	}
	const std::optional<std::span<const std::uint8_t>> owned =
		FindMessage(*message, kPlatformAudioOwned);
	if (!owned) {
		return std::nullopt;
	}
	const std::optional<std::span<const std::uint8_t>> info =
		FindMessage(*owned, kOwnedPlatformAudioInfo);
	if (!info) {
		return std::nullopt;
	}
	LiveKitPlatformAudioInfo result;
	result.recordingDeviceCount = static_cast<std::int32_t>(
		FindVarint(*info, kPlatformAudioRecordingCount).value_or(0));
	result.playoutDeviceCount = static_cast<std::int32_t>(
		FindVarint(*info, kPlatformAudioPlayoutCount).value_or(0));
	return result;
}

std::optional<LiveKitAudioDeviceList> DecodeLiveKitAudioDevices(
		const std::vector<std::uint8_t> &response) {
	const std::optional<std::span<const std::uint8_t>> message =
		FindMessage(response, kFfiResponseGetAudioDevices);
	if (!message) {
		return std::nullopt;
	}
	LiveKitAudioDeviceList result;
	for (const std::span<const std::uint8_t> device
			: FindMessages(*message, kDevicesPlaybackList)) {
		result.playout.push_back(DecodeAudioDevice(device));
	}
	for (const std::span<const std::uint8_t> device
			: FindMessages(*message, kDevicesRecordingList)) {
		result.recording.push_back(DecodeAudioDevice(device));
	}
	result.error = FindString(*message, kDevicesError).value_or(std::string());
	return result;
}

std::optional<std::string> DecodeLiveKitSetDeviceResponse(
		const std::vector<std::uint8_t> &response,
		std::uint32_t responseField) {
	const std::optional<std::span<const std::uint8_t>> message =
		FindMessage(response, responseField);
	if (!message) {
		return std::nullopt;
	}
	return FindString(*message, kSetDeviceError).value_or(std::string());
}

std::optional<std::string> FormatLiveKitLogLine(const LiveKitLogRecord &record) {
	const LiveKitLogLevel level = record.level;
	if (level == LiveKitLogLevel::Unknown) {
		return std::nullopt;
	}
	// Error and Warn are kept for every target. Info is kept for the livekit
	// targets only, and Debug and Trace are dropped.
	const bool severe = level == LiveKitLogLevel::Error
		|| level == LiveKitLogLevel::Warn;
	const bool livekitInfo = record.target.starts_with("livekit")
		&& level == LiveKitLogLevel::Info;
	if (!severe && !livekitInfo) {
		return std::nullopt;
	}
	// The message is never written. SDK text can carry URLs, tokens, key bytes
	// and addresses, and no pattern covers all of them.
	const char *levelName = "info";
	if (level == LiveKitLogLevel::Error) {
		levelName = "error";
	} else if (level == LiveKitLogLevel::Warn) {
		levelName = "warn";
	}
	return record.target + ": " + levelName;
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
				|| field == kEventPublishTrack
				|| field == kEventUnpublishTrack
				|| field == kEventLogs
				|| field == kEventSessionStats;
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
			} else if (field == kEventLogs) {
				result.kind = LiveKitEventKind::Log;
				result.logRecords = DecodeLogBatch(payload);
				return result;
			} else if (field == kEventSessionStats) {
				DecodeSessionStatsCallback(payload, result);
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
					result.trackSid = PublicationSid(*publication);
				}
			} else if (field == kEventUnpublishTrack) {
				result.kind = LiveKitEventKind::UnpublishTrack;
				if (const std::optional<std::string> error = FindString(
						payload,
						kUnpublishCallbackError)) {
					result.error = *error;
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
