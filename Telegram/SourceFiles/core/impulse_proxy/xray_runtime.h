// State machine of the in-process Xray core. A restart stops the running core,
// picks two free loopback ports, generates the inbound credentials, builds the
// run config, tests it and starts it. The runtime is exposed only while the
// core is RUNNING, so a failed or stopped core fails closed.
#pragma once

#include "mtproto/impulse/core/proxy_state.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace Impulse::Xray {

enum class Status {
	Off,
	Starting,
	Running,
	Failed,
};

enum class Error {
	None,
	NoServer,
	InvalidConfig,
	CoreStart,
};

// The "err:..." text of the error, as the Android client names it.
[[nodiscard]] std::string_view ErrorCode(Error error);

// Loopback inbounds of the running core. Both inbounds use the same
// credentials. The user is 32 hex characters and the password 48.
struct Endpoints {
	int httpPort = 0;
	int socksPort = 0;
	std::string user;
	std::string password;
};

// Sends one libXray call: the method name and a complete JSON payload object.
// Returns the raw JSON response envelope, or an empty string when the call
// produced nothing. The libXray glue binds this to XrayLibrary::invoke.
using Invoker = std::function<std::string(
	const std::string &method,
	const std::string &payloadJson)>;

class Runtime final {
public:
	explicit Runtime(Invoker invoker);

	Runtime(const Runtime &other) = delete;
	Runtime &operator=(const Runtime &other) = delete;

	// Applies the proxy state: stops the running core, then starts the
	// selected server when the proxy is enabled. Blocks until the core is
	// started or failed, so it must not run on the UI thread. Concurrent
	// restarts run one after another. The endpoints are cleared before the
	// restart begins.
	void restart(const Impulse::ProxyState::State &state);

	[[nodiscard]] Status status() const;
	[[nodiscard]] Error error() const;

	// Present only while the status is Running.
	[[nodiscard]] std::optional<Endpoints> endpoints() const;

private:
	void setState(
		Status newStatus,
		Error newError,
		std::optional<Endpoints> newEndpoints);

	Invoker invoker;

	std::mutex restartMutex;
	mutable std::mutex stateMutex;
	Status currentStatus = Status::Off;
	Error currentError = Error::None;
	std::optional<Endpoints> currentEndpoints;
};

} // namespace Impulse::Xray
