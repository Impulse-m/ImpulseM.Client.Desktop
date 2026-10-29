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

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitDisposeRequest(
	bool async);

[[nodiscard]] std::vector<std::uint8_t> EncodeLiveKitConnectRequest(
	const std::string &url,
	const std::string &token,
	const LiveKitRoomOptions &options,
	std::optional<std::uint64_t> requestAsyncId);

} // namespace Impulse
