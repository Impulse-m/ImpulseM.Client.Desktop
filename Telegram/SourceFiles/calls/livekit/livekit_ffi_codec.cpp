#include "calls/livekit/livekit_ffi_codec.h"

#include "mtproto/impulse/core/wire.h"

namespace Impulse {
namespace {

// FfiRequest oneof field numbers (livekit-ffi/protocol/ffi.proto).
constexpr std::uint32_t kFfiRequestDispose = 2;
constexpr std::uint32_t kFfiRequestConnect = 3;

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

[[nodiscard]] std::uint64_t BoolValue(bool value) {
	return value ? 1 : 0;
}

} // namespace

std::vector<std::uint8_t> EncodeLiveKitDisposeRequest(bool async) {
	ProtoWriter dispose;
	dispose.writeVarintField(kDisposeAsync, BoolValue(async));

	ProtoWriter request;
	request.writeMessageField(kFfiRequestDispose, dispose);
	return request.takeBytes();
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

	ProtoWriter request;
	request.writeMessageField(kFfiRequestConnect, connect);
	return request.takeBytes();
}

} // namespace Impulse
