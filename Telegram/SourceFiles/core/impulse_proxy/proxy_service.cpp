#include "core/impulse_proxy/proxy_service.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/impulse_proxy/proxy_links.h"
#include "core/impulse_proxy/proxy_subscription.h"
#include "core/impulse_proxy/xray_libxray.h"
#include "crl/crl_on_main.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Impulse {
namespace {

// The core key of the state, as the restart of the core compares it. Ping
// settings and the toggles are not part of it.
[[nodiscard]] std::string ImpulseCoreKey(
		const ProxyState::State &state) {
	std::string key = state.enabled ? "on" : "off";
	key += '|';
	key += ProxyState::CoreKey(state.advanced);
	if (!state.selectedId) {
		return key;
	}
	const std::string &selected = *state.selectedId;
	key += '|';
	key += selected;
	for (const ProxyState::Server &server : state.manual) {
		if (server.id == selected) {
			key += '|';
			key += server.outbound;
		}
	}
	for (const ProxyState::Subscription &subscription
			: state.subscriptions) {
		for (const ProxyState::Server &server : subscription.servers) {
			if (server.id == selected) {
				key += '|';
				key += server.outbound;
			}
		}
	}
	return key;
}

// The invoker before the core exists. Every call gives an empty response.
[[nodiscard]] Xray::Invoker EmptyInvoker() {
	return [](const std::string &, const std::string &) {
		return std::string();
	};
}

// Writes the outcome of a timed refresh into the stored subscription. A
// failed download keeps the servers and records the error, as a manual
// refresh does. Runs on the main thread.
void StoreTimedRefresh(
		const std::string &id,
		const SubscriptionRefresh &result) {
	ProxyState::State state
		= Core::App().settings().proxy().impulseProxyState();
	std::vector<ProxyState::Subscription>::iterator found
		= std::find_if(
			state.subscriptions.begin(),
			state.subscriptions.end(),
			[&](const ProxyState::Subscription &subscription) {
				return subscription.id == id;
			});
	if (found == state.subscriptions.end()) {
		return;
	}
	const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
	if (!result.error.empty()) {
		found->lastError = result.error;
		found->updatedAt = now;
	} else {
		found->meta.title = result.meta.title;
		found->meta.upload = result.meta.upload;
		found->meta.download = result.meta.download;
		found->meta.total = result.meta.total;
		found->meta.expire = result.meta.expire;
		found->meta.updateIntervalHours = result.meta.updateIntervalHours;
		found->lastError.reset();
		found->updatedAt = now;
		found->skipped = result.skipped;
		found->servers = result.servers;
	}
	Core::App().settings().proxy().setImpulseProxyState(std::move(state));
	Core::App().saveSettingsDelayed();
	Core::App().refreshGlobalProxy();
}

} // namespace

// Guarded by the mutex, except where a member says otherwise. The worker and
// the posted lambdas hold a reference to it, so it outlives the service.
struct ProxyServiceHost final {
	std::mutex mutex;
	std::condition_variable condition;
	std::deque<std::function<void()>> tasks;
	bool stopping = false;

	std::wstring libraryPath;
	std::shared_ptr<XrayLibrary> library;
	std::shared_ptr<Xray::Runtime> runtime;
	std::shared_ptr<Xray::PingHelper> ping;
	Xray::Invoker invoker = EmptyInvoker();

	// The state the last restart was requested with, and its core key.
	std::optional<std::string> requestedKey;
	std::optional<ProxyState::State> requested;

	// True while a restart task is queued or running.
	bool busy = false;

	// The core does not yet run the requested state, so traffic that the state
	// routes through the core is blocked until the restart ends.
	bool stale = false;

	// True while a ping check stops the core.
	bool checking = false;
};

namespace {

// Runs queued tasks in order on the one worker. On shutdown the queue is
// drained first, so every posted callback is still delivered.
void RunWorker(const std::shared_ptr<ProxyServiceHost> &host) {
	while (true) {
		std::function<void()> task;
		{
			std::unique_lock<std::mutex> lock(host->mutex);
			host->condition.wait(lock, [&host] {
				return host->stopping || !host->tasks.empty();
			});
			if (host->tasks.empty()) {
				return;
			}
			task = std::move(host->tasks.front());
			host->tasks.pop_front();
		}
		task();
	}
}

void Post(
		const std::shared_ptr<ProxyServiceHost> &host,
		std::function<void()> task) {
	{
		std::lock_guard<std::mutex> lock(host->mutex);
		host->tasks.push_back(std::move(task));
	}
	host->condition.notify_one();
}

// Creates the library, the runtime and the ping helper once. The core is not
// started here. Returns false where the Xray core cannot run. Must be called
// with the mutex held.
bool EnsureCoreLocked(ProxyServiceHost &host) {
	if (host.runtime) {
		return true;
	}
#ifdef _WIN32
	host.library = std::make_shared<XrayLibrary>(host.libraryPath);
	const std::shared_ptr<XrayLibrary> library = host.library;
	host.invoker = [library](
			const std::string &method,
			const std::string &payloadJson) {
		return library->invoke(method, payloadJson).get();
	};
	host.runtime = std::make_shared<Xray::Runtime>(host.invoker);
	host.ping = std::make_shared<Xray::PingHelper>(
		*host.runtime,
		host.invoker);
	return true;
#else // _WIN32
	return false;
#endif // _WIN32
}

// The invoker that reaches the core library, created on first use.
[[nodiscard]] Xray::Invoker CurrentInvoker(ProxyServiceHost &host) {
	std::lock_guard<std::mutex> lock(host.mutex);
	EnsureCoreLocked(host);
	return host.invoker;
}

// Applies the latest requested state until no newer one arrives. Runs on the
// worker, because a restart blocks until the core is started or failed.
void RunRestartLoop(ProxyServiceHost &host) {
	while (true) {
		std::shared_ptr<Xray::Runtime> runtime;
		std::optional<ProxyState::State> next;
		{
			std::lock_guard<std::mutex> lock(host.mutex);
			next = std::move(host.requested);
			host.requested.reset();
			if (!next) {
				host.busy = false;
				host.stale = false;
				return;
			}
			runtime = host.runtime;
		}
		if (runtime) {
			runtime->restart(*next);
		}
	}
}

} // namespace

ImpulseProxyService::ImpulseProxyService(RestartsSettled settled)
: host(std::make_shared<ProxyServiceHost>())
, restartsSettled(std::move(settled)) {
#ifdef _WIN32
	host->libraryPath = QDir::toNativeSeparators(
		QCoreApplication::applicationDirPath()
		+ "/modules/x64/xray/libXray.dll").toStdWString();
#endif // _WIN32
	const std::shared_ptr<ProxyServiceHost> worker = host;
	std::thread([worker] {
		RunWorker(worker);
	}).detach();
}

ImpulseProxyService::~ImpulseProxyService() {
	{
		std::lock_guard<std::mutex> lock(host->mutex);
		host->stopping = true;
	}
	host->condition.notify_all();
}

void ImpulseProxyService::apply(const ProxyState::State &state) {
	currentState = state;
	armSubscriptionTimers(state);
	const std::string key = ImpulseCoreKey(state);
	const std::shared_ptr<ProxyServiceHost> shared = host;
	const base::weak_ptr<ImpulseProxyService> weak = base::make_weak(this);
	bool queue = false;
	{
		std::lock_guard<std::mutex> lock(host->mutex);
		const bool skip = !host->runtime
			&& (!state.enabled || !EnsureCoreLocked(*host));
		if (!skip && host->requestedKey != key) {
			host->requestedKey = key;
			host->requested = state;
			host->stale = true;
			queue = !host->busy;
			host->busy = true;
		}
	}
	if (queue) {
		Post(shared, [shared, weak] {
			RunRestartLoop(*shared);
			crl::on_main(weak, [weak] {
				if (ImpulseProxyService *const service = weak.get()) {
					service->refreshRoute();
					service->restartsSettled();
				}
			});
		});
	}
	refreshRoute();
}

void ImpulseProxyService::importLinks(
		std::string text,
		ImportCallback callback) {
	const std::shared_ptr<ProxyServiceHost> shared = host;
	const base::weak_ptr<ImpulseProxyService> weak = base::make_weak(this);
	Post(shared, [shared, weak, text, callback] {
		const Xray::Invoker invoke = CurrentInvoker(*shared);
		const std::vector<ProxyState::Server> servers
			= Xray::ConvertShareLines(invoke, text);
		crl::on_main(weak, [callback, servers] {
			if (callback) {
				callback(servers);
			}
		});
	});
}

void ImpulseProxyService::refreshSubscription(
		std::string url,
		RefreshCallback callback) {
	const std::shared_ptr<ProxyServiceHost> shared = host;
	const base::weak_ptr<ImpulseProxyService> weak = base::make_weak(this);
	Xray::FetchSubscription(
		url,
		[shared, weak, callback](Xray::SubscriptionFetch fetch) {
			if (!fetch.error.empty()) {
				SubscriptionRefresh failed;
				failed.error = fetch.error;
				crl::on_main(weak, [callback, failed] {
					if (callback) {
						callback(failed);
					}
				});
				return;
			}
			Post(shared, [shared, weak, callback, fetch] {
				const Xray::Invoker invoke = CurrentInvoker(*shared);
				const Xray::LinkConversion conversion
					= Xray::ConvertLinks(invoke, fetch.body);
				SubscriptionRefresh result;
				result.error = Xray::SubscriptionBodyError(conversion)
					.value_or(std::string());
				result.servers = conversion.servers;
				result.skipped = conversion.skipped;
				result.meta = fetch.meta;
				crl::on_main(weak, [callback, result] {
					if (callback) {
						callback(result);
					}
				});
			});
		});
}

void ImpulseProxyService::checkServers(
		const ProxyState::State &state,
		std::vector<ProxyState::Server> servers,
		CheckCallback callback) {
	const std::shared_ptr<ProxyServiceHost> shared = host;
	const base::weak_ptr<ImpulseProxyService> weak = base::make_weak(this);
	Post(shared, [shared, weak, state, servers, callback] {
		std::shared_ptr<Xray::PingHelper> ping;
		{
			std::lock_guard<std::mutex> lock(shared->mutex);
			if (EnsureCoreLocked(*shared)) {
				ping = shared->ping;
			}
			shared->checking = true;
		}
		crl::on_main(weak, [weak] {
			if (ImpulseProxyService *const service = weak.get()) {
				service->refreshRoute();
			}
		});

		std::vector<Xray::PingResult> results;
		if (ping) {
			results = ping->check(state, servers);
		} else {
			results.resize(servers.size());
			for (Xray::PingResult &result : results) {
				result.error = std::string(
					Xray::ErrorCode(Xray::Error::CoreStart));
			}
		}
		{
			std::lock_guard<std::mutex> lock(shared->mutex);
			shared->checking = false;
		}
		crl::on_main(weak, [weak, results, callback] {
			if (ImpulseProxyService *const service = weak.get()) {
				service->refreshRoute();
			}
			if (callback) {
				callback(results);
			}
		});
	});
}

bool ImpulseProxyService::running() const {
	std::lock_guard<std::mutex> lock(host->mutex);
	return !host->stale
		&& !host->checking
		&& host->runtime
		&& host->runtime->status() == Xray::Status::Running;
}

std::optional<ProxyRouting::Runtime>
ImpulseProxyService::runningRuntime() const {
	std::lock_guard<std::mutex> lock(host->mutex);
	if (host->stale || host->checking || !host->runtime) {
		return std::nullopt;
	}
	const std::optional<Xray::Endpoints> endpoints
		= host->runtime->endpoints();
	if (!endpoints) {
		return std::nullopt;
	}
	return ProxyRouting::Runtime{
		endpoints->httpPort,
		endpoints->user,
		endpoints->password,
	};
}

rpl::producer<Xray::Status> ImpulseProxyService::stateValue() const {
	return stateVariable.value();
}

rpl::producer<ProxyRouting::Route> ImpulseProxyService::routeValue() const {
	return routeVariable.value();
}

void ImpulseProxyService::refreshRoute() {
	Xray::Status status = Xray::Status::Off;
	bool isRunning = false;
	{
		std::lock_guard<std::mutex> lock(host->mutex);
		if (host->runtime) {
			status = host->runtime->status();
		}
		isRunning = !host->stale
			&& !host->checking
			&& status == Xray::Status::Running;
	}
	const ProxyRouting::Route route = ProxyRouting::Decide(
		currentState.enabled,
		currentState.useForTransport,
		isRunning);
	if (stateVariable.current() != status) {
		stateVariable = status;
	}
	if (routeVariable.current() != route) {
		routeVariable = route;
	}
}

void ImpulseProxyService::armSubscriptionTimers(
		const ProxyState::State &state) {
	constexpr crl::time kMillisInHour = crl::time(60) * 60 * 1000;
	// The interval comes from the subscription server, and base::Timer
	// accepts at most INT_MAX milliseconds.
	constexpr int kMaxTimerHours
		= int(std::numeric_limits<int>::max() / kMillisInHour);
	std::map<std::string, SubscriptionTimer> next;
	for (const ProxyState::Subscription &subscription
			: state.subscriptions) {
		const int hours = std::min(
			subscription.meta.updateIntervalHours,
			kMaxTimerHours);
		if (hours < 1) {
			continue;
		}
		std::map<std::string, SubscriptionTimer>::iterator existing
			= refreshTimers.find(subscription.id);
		if (existing != refreshTimers.end()
				&& existing->second.url == subscription.url
				&& existing->second.hours == hours) {
			next.emplace(subscription.id, std::move(existing->second));
			refreshTimers.erase(existing);
			continue;
		}
		SubscriptionTimer armed;
		armed.url = subscription.url;
		armed.hours = hours;
		const std::string id = subscription.id;
		armed.timer = std::make_unique<base::Timer>([this, id] {
			refreshTimed(id);
		});
		armed.timer->callEach(kMillisInHour * hours);
		next.emplace(subscription.id, std::move(armed));
	}
	refreshTimers = std::move(next);
}

void ImpulseProxyService::refreshTimed(const std::string &id) {
	const ProxyState::State state
		= Core::App().settings().proxy().impulseProxyState();
	std::vector<ProxyState::Subscription>::const_iterator found
		= std::find_if(
			state.subscriptions.begin(),
			state.subscriptions.end(),
			[&](const ProxyState::Subscription &subscription) {
				return subscription.id == id;
			});
	if (found == state.subscriptions.end()) {
		return;
	}
	refreshSubscription(
		found->url,
		[id](SubscriptionRefresh result) {
			StoreTimedRefresh(id, result);
		});
}

} // namespace Impulse
