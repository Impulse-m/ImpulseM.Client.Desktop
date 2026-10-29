// Loader for livekit_ffi.dll, the LiveKit client core built as a C-ABI DLL.
// The DLL is loaded by full path and its four exports (livekit_ffi_initialize,
// livekit_ffi_request, livekit_ffi_drop_handle, livekit_ffi_dispose) are
// resolved with GetProcAddress. The Rust runtime cannot be unloaded safely, so
// the DLL stays mapped for the process lifetime.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Impulse {

class LiveKitFfiLoader final {
public:
	using EventHandler = std::function<void(std::vector<std::uint8_t>)>;

	explicit LiveKitFfiLoader(const std::wstring &dllPath);
	LiveKitFfiLoader(const LiveKitFfiLoader &other) = delete;
	LiveKitFfiLoader &operator=(const LiveKitFfiLoader &other) = delete;
	~LiveKitFfiLoader();

	// True when the DLL is loaded and all four symbols are resolved.
	[[nodiscard]] bool loaded() const;

	// Calls livekit_ffi_initialize once. The handler receives every encoded
	// FfiEvent from the library thread that produced it; it must not block.
	// Returns false when the library is not loaded or already initialized.
	bool initialize(
		EventHandler handler,
		bool captureLogs,
		const std::string &sdk,
		const std::string &sdkVersion);

	// Sends an encoded FfiRequest and returns the encoded FfiResponse. The
	// response buffer is copied out and released with livekit_ffi_drop_handle.
	// Returns nullopt for an empty request, when the library is not
	// initialized, or when the library rejects the request.
	[[nodiscard]] std::optional<std::vector<std::uint8_t>> request(
		std::span<const std::uint8_t> data);

	// Releases an owned handle that the library handed out in a response or
	// an event (room, participant, track, source or PlatformAudio handle).
	void dropHandle(std::uint64_t handle);

	// Calls livekit_ffi_dispose synchronously and allows initialize again.
	void dispose();

private:
	struct Impl;

	std::unique_ptr<Impl> impl;
};

} // namespace Impulse
