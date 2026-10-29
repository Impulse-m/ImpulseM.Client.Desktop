// The one owner of the ImpulseM proxy runtime in the process. It owns the Xray
// runtime, the ping helper and the worker that runs every blocking call into
// libXray: restarts, ping checks, share link conversions and subscription
// conversions. The worker runs them one at a time in the order they were
// queued, so a ping check never overlaps a restart. The route and the core
// status are published on the main thread.
#pragma once

#include "base/weak_ptr.h"
#include "core/impulse_proxy/proxy_ping.h"
#include "core/impulse_proxy/xray_runtime.h"
#include "mtproto/impulse/core/proxy_input.h"
#include "mtproto/impulse/core/proxy_routing.h"
#include "mtproto/impulse/core/proxy_state.h"
#include "rpl/variable.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
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

class ImpulseProxyService final : public base::has_weak_ptr {
public:
	using RestartsSettled = std::function<void()>;
	using ImportCallback = std::function<void(
		std::vector<ProxyState::Server> servers)>;
	using RefreshCallback = std::function<void(SubscriptionRefresh result)>;
	using CheckCallback = std::function<void(
		std::vector<Xray::PingResult> results)>;

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

	// Main thread. Stops the core, pings the servers, and starts the core again
	// with the given state after the queued restarts have finished. The
	// callback runs on the main thread with one result per server.
	void checkServers(
		const ProxyState::State &state,
		std::vector<ProxyState::Server> servers,
		CheckCallback callback);

	// True while the core runs the applied state and no check is running.
	[[nodiscard]] bool running() const;

	// The endpoints and credentials, present only while running().
	[[nodiscard]] std::optional<ProxyRouting::Runtime> runningRuntime() const;

	// Core status changes, emitted on the main thread.
	[[nodiscard]] rpl::producer<Xray::Status> stateValue() const;

	// Route changes of the main traffic, emitted on the main thread.
	[[nodiscard]] rpl::producer<ProxyRouting::Route> routeValue() const;

private:
	void refreshRoute();

	const std::shared_ptr<ProxyServiceHost> host;
	RestartsSettled restartsSettled;
	ProxyState::State currentState;
	rpl::variable<Xray::Status> stateVariable;
	rpl::variable<ProxyRouting::Route> routeVariable;
};

} // namespace Impulse
