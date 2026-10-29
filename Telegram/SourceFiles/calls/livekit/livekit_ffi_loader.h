// Loader for livekit_ffi.dll, the LiveKit client core built as a C-ABI DLL.
// The DLL is loaded by full path and its four exports (livekit_ffi_initialize,
// livekit_ffi_request, livekit_ffi_drop_handle, livekit_ffi_dispose) are
// resolved with GetProcAddress. The Rust runtime cannot be unloaded safely, so
// one loader stays mapped for the process lifetime and every room shares it.
// Its events reach the rooms through the process event router.
#pragma once

#include "calls/livekit/livekit_event_router.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Impulse {

// The library calls a room adapter uses. The process loader implements it, and
// the tests substitute a recording fake.
class LiveKitFfi {
public:
	// True when the library is loaded and all its symbols are resolved.
	[[nodiscard]] virtual bool loaded() const = 0;

	// Sends an encoded FfiRequest and returns the encoded FfiResponse. Returns
	// nullopt when the library rejects the request.
	[[nodiscard]] virtual std::optional<std::vector<std::uint8_t>> request(
		std::span<const std::uint8_t> data) = 0;

	// Releases an owned handle that the library handed out.
	virtual void dropHandle(std::uint64_t handle) = 0;

	// The router that delivers the library events to the rooms.
	[[nodiscard]] virtual LiveKitEventRouter &eventRouter() = 0;

	virtual ~LiveKitFfi() = default;
};

class LiveKitFfiLoader final : public LiveKitFfi {
public:
	explicit LiveKitFfiLoader(const std::wstring &dllPath);
	LiveKitFfiLoader(const LiveKitFfiLoader &other) = delete;
	LiveKitFfiLoader &operator=(const LiveKitFfiLoader &other) = delete;
	~LiveKitFfiLoader() override;

	// Returns the process loader. The first call loads dllPath and initializes
	// the library; later calls return the same loader and ignore dllPath.
	[[nodiscard]] static std::shared_ptr<LiveKitFfiLoader> Process(
		const std::wstring &dllPath);

	[[nodiscard]] bool loaded() const override;
	[[nodiscard]] std::optional<std::vector<std::uint8_t>> request(
		std::span<const std::uint8_t> data) override;
	void dropHandle(std::uint64_t handle) override;
	[[nodiscard]] LiveKitEventRouter &eventRouter() override;

	// Calls livekit_ffi_dispose synchronously and allows initialize again. It
	// frees every room of the process, so room adapters never call it.
	void dispose();

private:
	bool initialize(
		bool captureLogs,
		const std::string &sdk,
		const std::string &sdkVersion);

	struct Impl;

	std::unique_ptr<Impl> impl;
};

} // namespace Impulse
