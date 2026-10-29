#include "calls/livekit/livekit_ffi_loader.h"

#ifdef _WIN32

#include "base/platform/win/base_windows_safe_library.h"
#include "calls/livekit/livekit_ffi_codec.h"

#include <cstddef>
#include <utility>

namespace Impulse {
namespace {

// Handle value the library returns for a failed request (INVALID_HANDLE in
// livekit-ffi).
constexpr std::uint64_t kInvalidHandle = 0;

// Identifies this client to the library and its logs.
constexpr const char *kSdkName = "impulsem-desktop";
constexpr const char *kSdkVersion = "1.0";

using FfiCallbackFunction = void (*)(const std::uint8_t *, std::size_t);

LiveKitEventRouter &ProcessEventRouter() {
	static LiveKitEventRouter router;
	return router;
}

extern "C" void LiveKitFfiEventCallback(
		const std::uint8_t *data,
		std::size_t length) {
	if (!data || !length) {
		return;
	}
	try {
		const LiveKitEvent event = DecodeLiveKitEvent(
			std::vector<std::uint8_t>(data, data + length));
		if (event.kind != LiveKitEventKind::Unknown) {
			ProcessEventRouter().dispatch(event);
		}
	} catch (...) {
		// An exception must not unwind into the library thread.
	}
}

} // namespace

struct LiveKitFfiLoader::Impl {
	using InitializeFunction = void (*)(
		FfiCallbackFunction,
		bool,
		const char *,
		const char *);
	using RequestFunction = std::uint64_t (*)(
		const std::uint8_t *,
		std::size_t,
		const std::uint8_t **,
		std::size_t *);
	using DropHandleFunction = bool (*)(std::uint64_t);
	using DisposeFunction = void (*)();

	HINSTANCE module = nullptr;
	InitializeFunction initializeCall = nullptr;
	RequestFunction requestCall = nullptr;
	DropHandleFunction dropHandleCall = nullptr;
	DisposeFunction disposeCall = nullptr;
	bool initialized = false;

	void clearSymbols() {
		module = nullptr;
		initializeCall = nullptr;
		requestCall = nullptr;
		dropHandleCall = nullptr;
		disposeCall = nullptr;
	}
};

LiveKitFfiLoader::LiveKitFfiLoader(const std::wstring &dllPath)
: impl(std::make_unique<Impl>()) {
	base::Platform::InitDynamicLibraries();

	impl->module = ::LoadLibraryExW(
		dllPath.c_str(),
		nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
	if (!impl->module) {
		return;
	}

	const bool resolved = base::Platform::LoadMethod(
			impl->module,
			"livekit_ffi_initialize",
			impl->initializeCall)
		&& base::Platform::LoadMethod(
			impl->module,
			"livekit_ffi_request",
			impl->requestCall)
		&& base::Platform::LoadMethod(
			impl->module,
			"livekit_ffi_drop_handle",
			impl->dropHandleCall)
		&& base::Platform::LoadMethod(
			impl->module,
			"livekit_ffi_dispose",
			impl->disposeCall);
	if (!resolved) {
		impl->clearSymbols();
	}
}

LiveKitFfiLoader::~LiveKitFfiLoader() = default;

std::shared_ptr<LiveKitFfiLoader> LiveKitFfiLoader::Process(
		const std::wstring &dllPath) {
	static const std::shared_ptr<LiveKitFfiLoader> loader = [&dllPath]() {
		std::shared_ptr<LiveKitFfiLoader> created =
			std::make_shared<LiveKitFfiLoader>(dllPath);
		if (created->loaded()) {
			created->initialize(false, kSdkName, kSdkVersion);
		}
		return created;
	}();
	return loader;
}

bool LiveKitFfiLoader::loaded() const {
	return impl->module != nullptr;
}

bool LiveKitFfiLoader::initialize(
		bool captureLogs,
		const std::string &sdk,
		const std::string &sdkVersion) {
	if (!loaded() || impl->initialized) {
		return false;
	}
	impl->initializeCall(
		&LiveKitFfiEventCallback,
		captureLogs,
		sdk.c_str(),
		sdkVersion.c_str());
	impl->initialized = true;
	return true;
}

std::optional<std::vector<std::uint8_t>> LiveKitFfiLoader::request(
		std::span<const std::uint8_t> data) {
	if (!loaded() || !impl->initialized || data.empty()) {
		return std::nullopt;
	}

	const std::uint8_t *responseData = nullptr;
	std::size_t responseLength = 0;
	const std::uint64_t handle = impl->requestCall(
		data.data(),
		data.size(),
		&responseData,
		&responseLength);
	if (handle == kInvalidHandle || !responseData) {
		return std::nullopt;
	}

	std::vector<std::uint8_t> response(
		responseData,
		responseData + responseLength);
	impl->dropHandleCall(handle);
	return response;
}

void LiveKitFfiLoader::dropHandle(std::uint64_t handle) {
	if (!loaded() || !impl->initialized || handle == kInvalidHandle) {
		return;
	}
	impl->dropHandleCall(handle);
}

LiveKitEventRouter &LiveKitFfiLoader::eventRouter() {
	return ProcessEventRouter();
}

void LiveKitFfiLoader::dispose() {
	if (!loaded() || !impl->initialized) {
		return;
	}
	impl->disposeCall();
	impl->initialized = false;
}

} // namespace Impulse

#endif // _WIN32
