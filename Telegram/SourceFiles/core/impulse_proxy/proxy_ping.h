// Ping of the proxy servers. The libXray pingBatch refuses to run while a
// managed Xray core runs, so a check stops the core, measures the servers in
// chunks, and then starts the core again with the saved state. A check blocks
// and must not run on the UI thread.
#pragma once

#include "core/impulse_proxy/xray_runtime.h"
#include "mtproto/impulse/core/proxy_state.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Impulse::Xray {

struct PingResult final {
	// Milliseconds, or -1 when the server failed or the call gave no result.
	std::int64_t delay = -1;

	// The failure text, when the call or the server reported one.
	std::optional<std::string> error;
};

// Receives the results of one chunk, which start at the index offset of the
// servers. Returning false stops the check; the rest keep the -1 result.
using PingChunkListener = std::function<bool(
	std::size_t offset,
	const std::vector<PingResult> &results)>;

class PingHelper final {
public:
	PingHelper(Runtime &runtime, Invoker invoker);

	PingHelper(const PingHelper &other) = delete;
	PingHelper &operator=(const PingHelper &other) = delete;

	// Stops the core, pings the servers with the ping settings of the state,
	// and starts the core again with the same state. Returns one result per
	// server, in the order of the servers. Concurrent checks run one after
	// another.
	[[nodiscard]] std::vector<PingResult> check(
		const ProxyState::State &state,
		const std::vector<ProxyState::Server> &servers,
		const PingChunkListener &listener = nullptr);

private:
	Runtime &runtime;
	Invoker invoker;
	std::mutex checkMutex;
};

} // namespace Impulse::Xray
