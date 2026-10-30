// The one owner of the ImpulseM proxy runtime in the process. It owns the Xray
// runtime, the ping helper and the worker that runs every blocking call into
// libXray: restarts, ping checks, share link conversions and subscription
// conversions. The worker runs them one at a time in the order they were
// queued, so a ping check never overlaps a restart. The route and the core
// status are published on the main thread.
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/impulse_proxy/proxy_ping.h"
#include "core/impulse_proxy/xray_runtime.h"
#include "mtproto/impulse/core/proxy_input.h"
#include "mtproto/impulse/core/proxy_routing.h"
#include "mtproto/impulse/core/proxy_state.h"
#include "rpl/event_stream.h"
#include "rpl/variable.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Impulse {

struct ProxyServiceHost;

// The outcome of one subscription refresh. An empty error means that the
// servers and the meta are the ones of the subscription.
struct SubscriptionRefresh final {
	std::string error;
	std::vector<ProxyState::Server> servers;
	int skipped = 0;
	ProxyInput::SubscriptionMeta meta;
};

// The refresh timer of one subscription, armed for its update interval.
struct SubscriptionTimer final {
	std::string url;
	int hours = 0;
	std::unique_ptr<base::Timer> timer;
};

class ImpulseProxyService final : public base::has_weak_ptr {
public:
	using RestartsSettled = std::function<void()>;
	using ImportCallback = std::function<void(
		std::vector<ProxyState::Server> servers)>;
	using RefreshCallback = std::function<void(SubscriptionRefresh result)>;

	// The callback runs on the main thread after a queued restart has finished
	// and the worker has gone idle.
	explicit ImpulseProxyService(RestartsSettled restartsSettled);
	~ImpulseProxyService();

	ImpulseProxyService(const ImpulseProxyService &other) = delete;
	ImpulseProxyService &operator=(const ImpulseProxyService &other) = delete;

	// Main thread. Stores the state and publishes the route. When the core
	// settings changed, the core is restarted by the worker; a running core
	// keeps the traffic blocked until the restart ends. Nothing starts while
	// the proxy is disabled.
	void apply(const ProxyState::State &state);

	// Main thread. Converts pasted share lines one by one, as the manual
	// add box does. The callback runs on the main thread.
	void importLinks(std::string text, ImportCallback callback);

	// Main thread. Downloads a subscription on the Qt network and converts its
	// body on the worker. The callback runs on the main thread.
	void refreshSubscription(std::string url, RefreshCallback callback);

	// Main thread. Starts a check round of all servers, unless a round runs: then
	// the request is remembered, and one more round runs when the round finishes.
	// An automatic request is skipped while the core runs and the setting asks
	// for it. Returns true when a round started now.
	bool requestCheck(bool manual);

	// Main thread. Emits when a check round has finished and its results are
	// stored, including the round that a request during a round started.
	[[nodiscard]] rpl::producer<> checkFinishedValue() const;

	// The sort key of the server with the id, as the last manual check left it.
	// The group orders the rows: 0 for available servers, by ping, 1 for servers
	// that were never checked, 2 for unavailable ones. A server that the last
	// manual check did not list has the key of a never checked one.
	[[nodiscard]] std::pair<int, std::int64_t> sortKey(
		const std::string &id) const;

	// True while the core runs the applied state and no check is running.
	[[nodiscard]] bool running() const;

	// The endpoints and credentials, present only while running().
	[[nodiscard]] std::optional<ProxyRouting::Runtime> runningRuntime() const;

	// Core status changes, emitted on the main thread.
	[[nodiscard]] rpl::producer<Xray::Status> stateValue() const;

	// Route changes of the main traffic, emitted on the main thread.
	[[nodiscard]] rpl::producer<ProxyRouting::Route> routeValue() const;

	// The error of the core as the runtime holds it now. Error::None unless the
	// core failed.
	[[nodiscard]] Xray::Error error() const;

	// Core error changes, emitted on the main thread with the state changes.
	[[nodiscard]] rpl::producer<Xray::Error> errorValue() const;

	// The last ping of the server with the id, in milliseconds; negative when
	// the server did not answer. Empty when the server was not checked.
	[[nodiscard]] std::optional<std::int64_t> pingResult(
		const std::string &id) const;

	// True while a check round runs and measures the server with the id.
	[[nodiscard]] bool checkingServer(const std::string &id) const;

	// When the last check round finished, in milliseconds since the epoch. Zero
	// when no round has finished.
	[[nodiscard]] std::int64_t lastCheckTime() const;

private:
	void refreshRoute();

	// Stops the core, pings the servers, and starts the core again with the
	// given state after the queued restarts have finished.
	void startCheck(
		const ProxyState::State &state,
		std::vector<ProxyState::Server> servers,
		bool manual);

	// Stores the pings of a finished round for the servers that the current
	// state still lists, records when the round finished, and starts the round
	// that was requested during it. A manual round also fixes the sort keys.
	void finishCheck(
		const std::vector<ProxyState::Server> &servers,
		const std::vector<Xray::PingResult> &results,
		bool manual);

	// Fixes the sort keys of every server of the current state from the pings.
	void rememberSortKeys();

	// Drops the pings and the sort keys of the servers that the state does not
	// list any more.
	void prunePingResults(const ProxyState::State &state);

	// Arms one timer per subscription with a positive update interval and
	// drops the timers of the subscriptions that are gone or changed.
	void armSubscriptionTimers(const ProxyState::State &state);

	// Timer callback: downloads the subscription with the id and stores it.
	void refreshTimed(const std::string &id);

	const std::shared_ptr<ProxyServiceHost> host;
	RestartsSettled restartsSettled;
	ProxyState::State currentState;
	rpl::variable<Xray::Status> stateVariable;
	rpl::variable<ProxyRouting::Route> routeVariable;
	rpl::variable<Xray::Error> errorVariable;
	std::map<std::string, SubscriptionTimer> refreshTimers;

	// Main thread only. Survive the box, so the rows show the last round.
	std::map<std::string, std::int64_t> pingResults;
	std::map<std::string, std::pair<int, std::int64_t>> sortKeys;
	std::int64_t lastCheckAt = 0;
	bool roundRunning = false;
	std::vector<std::string> roundIds;

	// A request that came during a round, run when the round finishes.
	bool checkAgain = false;
	bool checkAgainManual = false;

	rpl::event_stream<> checkFinishedStream;
};

} // namespace Impulse
