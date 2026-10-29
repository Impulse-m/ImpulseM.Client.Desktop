// Loader for libXray.dll, the Xray core built as a Go c-shared library.
// The DLL is loaded by full path and its two exports, CGoInvoke and CGoFree,
// are resolved with GetProcAddress. Every call runs on one worker thread,
// because the Go runtime is entered from one thread at a time.
// The Go runtime cannot be unloaded, so the DLL stays mapped for the process
// lifetime.
#pragma once

#include <future>
#include <memory>
#include <string>

namespace Impulse {

class XrayLibrary final {
public:
	explicit XrayLibrary(const std::wstring &dllPath);
	XrayLibrary(const XrayLibrary &other) = delete;
	XrayLibrary &operator=(const XrayLibrary &other) = delete;
	~XrayLibrary();

	// True when the DLL is loaded and both symbols are resolved.
	[[nodiscard]] bool loaded() const;

	// Sends {"apiVersion":3,"method":<method>,"payload":<payloadJson>} to
	// CGoInvoke. The method name is written as is, so it must not need JSON
	// escaping; payloadJson must be a complete JSON value. The future gets the
	// response text, or an empty string when the library is not loaded or the
	// call returned nothing. Every non-null response is released with CGoFree.
	[[nodiscard]] std::future<std::string> invoke(
		const std::string &method,
		const std::string &payloadJson);

private:
	struct Impl;

	std::unique_ptr<Impl> impl;
};

} // namespace Impulse
