#include "mtproto/impulse/glue/media_pool_rust.h"

#include "mtproto/impulse/glue/carrier_errors.h"
#include "mtproto/impulse/glue/centrifugo_rust.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Impulse {
namespace {

// The endpoint key of the media pool. Its endpoint table has one URL.
constexpr char kMediaEndpointKey[] = "media";

// One call of the pool. It settles once: through its completion, or at once
// when the pool closes or the call cannot start.
struct PendingCall {
	MediaDone done;
	std::uint64_t callId = 0;
	bool settled = false;
};

// The calls of one pool that have not settled, by their pool call id. Shared
// with the completions, so a completion that runs after the pool is gone finds
// its call already settled.
struct PendingCalls {
	std::map<std::uint64_t, std::shared_ptr<PendingCall>> calls;
};

[[nodiscard]] MediaReply replyOf(const CentrifugoResult &result) {
	MediaReply reply;
	reply.outcome = mapCarrierResult(result, CarrierKind::Media);
	if (result.status == kCentrifugoStatusClientDisconnected) {
		// The main client is not connected: a retryable network failure.
		reply.outcome.temporary = true;
	}
	if (reply.outcome.kind == CarrierOutcomeKind::Reply) {
		reply.data = result.data;
	}
	return reply;
}

[[nodiscard]] MediaReply closedReply() {
	MediaReply reply;
	reply.outcome.kind = CarrierOutcomeKind::NetworkFailure;
	reply.outcome.error.text = "media pool is closed";
	return reply;
}

// Settles pending once. A call that already settled does nothing, so a
// completion that arrives after close() is ignored.
void settleCall(
		const std::shared_ptr<PendingCalls> &registry,
		const std::shared_ptr<PendingCall> &pending,
		MediaReply reply) {
	if (pending->settled) {
		return;
	}
	pending->settled = true;
	const std::map<std::uint64_t, std::shared_ptr<PendingCall>>::iterator found
		= registry->calls.find(pending->callId);
	if (found != registry->calls.end() && found->second == pending) {
		registry->calls.erase(found);
	}
	MediaDone done = std::move(pending->done);
	pending->done = nullptr;
	if (done) {
		done(std::move(reply));
	}
}

class RustMediaPool final : public MediaPool {
private:
	std::shared_ptr<PendingCalls> registry;
	CentrifugoRustPool pool;
	std::int32_t createStatus = kCentrifugoStatusOk;

public:
	RustMediaPool(
		CentrifugoRust &client,
		const std::string &endpointUrl,
		int connections,
		int inflightPerConnection,
		int maxQueued)
	: registry(std::make_shared<PendingCalls>()) {
		CentrifugoEndpoint endpoint;
		endpoint.key = kMediaEndpointKey;
		endpoint.url = endpointUrl;

		CentrifugoPoolConfig config;
		config.endpoints.push_back(endpoint);
		config.timeoutMs = static_cast<std::uint32_t>(kMediaCallTimeoutMs);
		config.maxConnectionsPerEndpoint = static_cast<std::uint32_t>(connections);
		config.maxInflightPerConnection = static_cast<std::uint32_t>(inflightPerConnection);
		config.maxQueued = static_cast<std::uint32_t>(maxQueued);
		config.tokenFrom = true;

		// A failed create leaves the pool closed: every call then fails with
		// this status, as a network failure.
		createStatus = pool.create(client, config);
	}

	~RustMediaPool() override {
		close();
	}

	std::uint64_t send(
			const std::string &method,
			std::vector<std::uint8_t> data,
			std::int64_t timeoutMs,
			MediaDone done) override {
		const std::shared_ptr<PendingCall> pending = std::make_shared<PendingCall>();
		pending->done = std::move(done);

		CentrifugoRpcRequest request;
		request.method = method;
		request.data = std::move(data);
		request.timeoutMs = static_cast<std::uint32_t>(timeoutMs);

		std::uint64_t callId = 0;
		std::int32_t status = createStatus;
		if (status == kCentrifugoStatusOk) {
			const std::shared_ptr<PendingCalls> sharedRegistry = registry;
			status = pool.rpc(
				kMediaEndpointKey,
				request,
				[sharedRegistry, pending](const CentrifugoResult &result) {
					settleCall(sharedRegistry, pending, replyOf(result));
				},
				&callId);
		}
		if (status != kCentrifugoStatusOk) {
			// A call that cannot start never runs its completion, so it settles
			// here with the status of the start.
			CentrifugoResult failed;
			failed.status = status;
			settleCall(registry, pending, replyOf(failed));
			return 0;
		}
		pending->callId = callId;
		if (!pending->settled) {
			registry->calls.emplace(callId, pending);
		}
		return callId;
	}

	void cancel(std::uint64_t callId) override {
		if (callId == 0 || registry->calls.find(callId) == registry->calls.end()) {
			return;
		}
		// The completion of the cancelled call settles it with the cancelled
		// status. A failed cancel means the pool is closed, which settled every
		// call already.
		if (pool.cancelRpc(callId) != kCentrifugoStatusOk) {
			return;
		}
	}

	void close() override {
		// Settle every waiting and in-flight call now, as MediaPool::close
		// requires. Their completions of the closed pool run later through the
		// poster and find them settled.
		const std::map<std::uint64_t, std::shared_ptr<PendingCall>> open = registry->calls;
		registry->calls.clear();
		for (const std::pair<const std::uint64_t, std::shared_ptr<PendingCall>> &entry : open) {
			settleCall(registry, entry.second, closedReply());
		}
		pool.close();
	}
};

} // namespace

MediaPoolFactory makeRustMediaPoolFactory(CentrifugoRust *client) {
	return [client](
			const std::string &endpointUrl,
			int connections,
			int inflightPerConnection,
			int maxQueued) -> std::unique_ptr<MediaPool> {
		return std::make_unique<RustMediaPool>(
			*client,
			endpointUrl,
			connections,
			inflightPerConnection,
			maxQueued);
	};
}

} // namespace Impulse
