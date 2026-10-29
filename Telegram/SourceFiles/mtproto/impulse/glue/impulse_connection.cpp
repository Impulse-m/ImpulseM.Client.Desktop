#include "mtproto/impulse/glue/impulse_connection.h"

#include "base/call_delayed.h"
#include "core/application.h"
#include "logs.h"
#include "mtproto/impulse/core/proxy_routing.h"
#include "mtproto/impulse/glue/media_pool_rust.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace Impulse {
namespace {

// The endpoint of the proxy as the Rust client takes it: http://host:port
// for HTTP CONNECT, empty when the traffic goes direct.
[[nodiscard]] std::string ProxyUrlOf(
		const std::optional<ProxyRouting::HttpEndpoint> &endpoint) {
	if (!endpoint) {
		return std::string();
	}
	return "http://" + endpoint->host + ":" + std::to_string(endpoint->port);
}

// The route is blocked: the endpoint is the discard port of the loopback.
[[nodiscard]] bool IsBlackhole(
		const std::optional<ProxyRouting::HttpEndpoint> &endpoint) {
	return endpoint
		&& endpoint->port == ProxyRouting::kBlackholePort
		&& endpoint->host == ProxyRouting::kLoopbackHost;
}

} // namespace

ImpulseConnection::ImpulseConnection() {
	const std::optional<ProxyRouting::HttpEndpoint> endpoint
		= Core::App().impulseHttpEndpoint();
	dialAllowed = !IsBlackhole(endpoint);
	const std::string proxyUrl = ProxyUrlOf(endpoint);

	// Socket callbacks come from the Rust runtime threads: they are handed to
	// the main thread, and they do nothing once this connection is gone.
	const base::weak_ptr<ImpulseConnection> weak = base::make_weak(this);
	const CentrifugoRust::Poster poster = [weak](std::function<void()> task) {
		crl::on_main(weak, [task = std::move(task)] {
			task();
		});
	};
	const RpcCarrierScheduler schedule = [weak](
			std::int64_t delayMs,
			std::function<void()> task) {
		base::call_delayed(
			static_cast<crl::time>(delayMs),
			weak,
			std::move(task));
	};

	CentrifugoConfig updateConfig;
	updateConfig.proxyUrl = proxyUrl;
	const std::int32_t updateStatus = updateClient.create(
		CentrifugeFleet::Update,
		updateConfig,
		poster,
		&connectionState);
	if (updateStatus != kCentrifugoStatusOk) {
		LOG(("ImpulseM Error: update client was not created, status %1.").arg(updateStatus));
	}

	CentrifugoConfig poolConfig;
	poolConfig.proxyUrl = proxyUrl;
	const std::int32_t poolStatus = poolClient.create(
		CentrifugeFleet::Pool,
		poolConfig,
		poster,
		nullptr);
	if (poolStatus != kCentrifugoStatusOk) {
		LOG(("ImpulseM Error: pool client was not created, status %1.").arg(poolStatus));
	}

	// The media pool is built on the update client, which owns the socket it
	// shares its token with. Without that client there is no media fleet.
	CentrifugoRust *const updateRust = updateClient.rustClient();
	if (updateRust) {
		MediaFleetDependencies mediaDependencies;
		mediaDependencies.createPool = makeRustMediaPoolFactory(updateRust);
		mediaDependencies.schedule = schedule;
		mediaFleet = std::make_unique<MediaFleet>(std::move(mediaDependencies));
	}

	RpcCarrierDependencies carrierDependencies;
	carrierDependencies.update = &updateClient;
	carrierDependencies.pool = &poolClient;
	carrierDependencies.media = mediaFleet.get();
	carrierDependencies.schedule = schedule;
	rpcCarrier = std::make_unique<RpcCarrier>(std::move(carrierDependencies));
}

ImpulseConnection::~ImpulseConnection() {
	// Destruction cancels every pending completion. The carrier settles the
	// open calls with a network failure while it closes, and those done
	// callbacks do not run. The media pool goes before the update client it is
	// built on.
	for (const std::pair<const mtpRequestId, std::shared_ptr<PendingRequest>> &entry
			: pendingRequests) {
		entry.second->cancelled = true;
	}
	pendingRequests.clear();
	rpcCarrier->close();
	if (mediaFleet) {
		mediaFleet->close();
	}
	poolClient.close();
	updateClient.close();
}

void ImpulseConnection::start() {
	if (started) {
		return;
	}
	started = true;
	if (!dialAllowed) {
		return;
	}
	static_cast<void>(updateClient.connect());
	static_cast<void>(poolClient.connect());
}

void ImpulseConnection::send(
		mtpRequestId requestId,
		RpcCarrierRequest request,
		RpcCarrierDone done) {
	const std::shared_ptr<PendingRequest> pending
		= std::make_shared<PendingRequest>();
	pending->done = std::move(done);
	pendingRequests[requestId] = pending;

	// The carrier may settle the call before send() returns, for a refusal.
	const std::uint64_t callId = rpcCarrier->send(
		std::move(request),
		[this, requestId, pending](RpcCarrierReply reply) {
			settle(requestId, pending, std::move(reply));
		});
	if (!pending->settled) {
		pending->carrierCallId = callId;
	}
}

void ImpulseConnection::cancel(mtpRequestId requestId) {
	const std::map<mtpRequestId, std::shared_ptr<PendingRequest>>::iterator found
		= pendingRequests.find(requestId);
	if (found == pendingRequests.end()) {
		return;
	}
	const std::shared_ptr<PendingRequest> pending = found->second;
	pendingRequests.erase(found);
	pending->cancelled = true;
	if (!pending->settled) {
		// The carrier settles the call with a network failure, which is
		// dropped here because the call is cancelled.
		rpcCarrier->cancel(pending->carrierCallId);
	}
}

void ImpulseConnection::settle(
		mtpRequestId requestId,
		const std::shared_ptr<PendingRequest> &pending,
		RpcCarrierReply reply) {
	pending->settled = true;
	const std::map<mtpRequestId, std::shared_ptr<PendingRequest>>::iterator found
		= pendingRequests.find(requestId);
	if (found != pendingRequests.end() && found->second == pending) {
		pendingRequests.erase(found);
	}
	RpcCarrierDone done = std::move(pending->done);
	pending->done = nullptr;
	if (done && !pending->cancelled) {
		done(std::move(reply));
	}
}

} // namespace Impulse
