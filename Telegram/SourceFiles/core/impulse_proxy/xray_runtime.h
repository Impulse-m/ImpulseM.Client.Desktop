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
	CoreStopped,
};

// The "err:..." text of the error, as the Android client names it.
[[nodiscard]] std::string_view ErrorCode(Error error);

// Seconds between two health checks of a running core.
inline constexpr int kHealthCheckSeconds = 30;

// What the periodic health check does next, as Android HealthPolicy decides.
enum class HealthAction {
	// The check is stale (an older generation, or the core is not running).
	// Nothing is scheduled.
	Stop,
	// The core is alive. The retry allowance is cleared and the next check is
	// scheduled.
	Recheck,
	// The core died for the first time. It is restarted once.
	Restart,
	// The core died again after an automatic restart. The runtime is FAILED
	// with Error::CoreStopped.
	Fail,
};

// The pure policy of one health check. current: the checked generation is the
// running one and the status is Running; alreadyRetried: an automatic restart
// happened since the last healthy check; alive: the core answered that it runs.
[[nodiscard]] HealthAction DecideHealth(
	bool current,
	bool alreadyRetried,
	bool alive);

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
	// restart begins. A restart is a new generation, and it clears the
	// automatic retry allowance of the health check.
	void restart(const Impulse::ProxyState::State &state);

	// Runs one health check of the core started in the given generation, on
	// the same thread as restart. Restart restarts the core with the state of
	// the last restart. The owner schedules the next check after
	// kHealthCheckSeconds when the status is still Running after a Recheck or
	// a Restart, and schedules nothing after Stop or Fail.
	[[nodiscard]] HealthAction checkHealth(int expectedGeneration);

	[[nodiscard]] Status status() const;
	[[nodiscard]] Error error() const;

	// Increases with every restart. Health checks of an older generation are
	// stale.
	[[nodiscard]] int generation() const;

	// Present only while the status is Running.
	[[nodiscard]] std::optional<Endpoints> endpoints() const;

private:
	void restartLocked(const Impulse::ProxyState::State &state);

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
	int currentGeneration = 0;

	// Guarded by restartMutex.
	std::optional<Impulse::ProxyState::State> appliedState;
	bool autoRetried = false;
};

} // namespace Impulse::Xray
