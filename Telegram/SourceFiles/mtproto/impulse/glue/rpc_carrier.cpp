#include "mtproto/impulse/glue/rpc_carrier.h"

#include "mtproto/impulse/glue/centrifuge_client.h"

#include <string_view>
#include <utility>

namespace Impulse {
namespace {

constexpr std::string_view TakeoutHeaderName = "x-takeout-id";

[[nodiscard]] RpcCarrierReply networkFailure() {
	RpcCarrierReply reply;
	reply.outcome.kind = CarrierOutcomeKind::NetworkFailure;
	return reply;
}

[[nodiscard]] RpcCarrierReply refusal(const std::string &type) {
	RpcCarrierReply reply;
	reply.outcome.kind = CarrierOutcomeKind::RpcError;
	reply.outcome.error.code = kClientRefusalCode;
	reply.outcome.error.text = type;
	return reply;
}

} // namespace

RpcCarrier::RpcCarrier(RpcCarrierDependencies dependencies)
: update(dependencies.update)
, pool(dependencies.pool)
, media(dependencies.media)
, schedule(std::move(dependencies.schedule))
, alive(std::make_shared<bool>(true)) {
}

void RpcCarrier::setDeclarationGate(RpcDeclarationGate gate) {
	declarationGate = std::move(gate);
}

std::uint64_t RpcCarrier::send(RpcCarrierRequest request, RpcCarrierDone done) {
	const std::uint64_t callId = nextId++;
	const RouteDecision decision = routeMethod(request.route);
	if (decision.refused) {
		done(refusal(decision.refusalType));
		return callId;
	}
	if (closed) {
		done(networkFailure());
		return callId;
	}
	if (decision.carrier != CarrierKind::Media
		&& pendingOn(decision.carrier) >= kMaxPendingRpc) {
		done(networkFailure());
		return callId;
	}

	const std::shared_ptr<PendingCall> call = std::make_shared<PendingCall>();
	call->carrier = decision.carrier;
	call->method = request.route.method;
	call->data = std::move(request.data);
	call->done = std::move(done);
	if (decision.forwardTakeout && request.takeoutId) {
		call->headers.push_back(CentrifugoHeader{
			std::string(TakeoutHeaderName),
			std::to_string(*request.takeoutId),
		});
	}
	active.emplace(callId, call);

	const std::weak_ptr<bool> guard = alive;
	if (request.awaitDeclaration && declarationGate) {
		declarationGate([this, guard, callId]() {
			if (guard.lock()) {
				dispatch(callId);
			}
		});
		schedule(kDeclarationWaitMs, [this, guard, callId]() {
			if (guard.lock()) {
				dispatch(callId);
			}
		});
	} else {
		dispatch(callId);
	}
	return callId;
}

void RpcCarrier::cancel(std::uint64_t callId) {
	settle(callId, networkFailure(), true);
}

void RpcCarrier::close() {
	closed = true;
	std::vector<std::uint64_t> ids;
	ids.reserve(active.size());
	for (const std::pair<const std::uint64_t, std::shared_ptr<PendingCall>> &entry : active) {
		ids.push_back(entry.first);
	}
	for (const std::uint64_t callId : ids) {
		settle(callId, networkFailure(), true);
	}
}

int RpcCarrier::pendingOn(CarrierKind carrier) const {
	int count = 0;
	for (const std::pair<const std::uint64_t, std::shared_ptr<PendingCall>> &entry : active) {
		if (entry.second->carrier == carrier) {
			++count;
		}
	}
	return count;
}

void RpcCarrier::dispatch(std::uint64_t callId) {
	const PendingMap::iterator it = active.find(callId);
	if (it == active.end() || it->second->dispatched) {
		return;
	}
	const std::shared_ptr<PendingCall> call = it->second;
	call->dispatched = true;
	if (call->carrier == CarrierKind::Media) {
		dispatchMedia(callId, call);
	} else {
		dispatchRpc(callId, call);
	}
}

void RpcCarrier::dispatchRpc(
		std::uint64_t callId,
		const std::shared_ptr<PendingCall> &call) {
	CentrifugeClient *socket = (call->carrier == CarrierKind::Pool) ? pool : update;
	if (!socket) {
		settle(callId, networkFailure(), false);
		return;
	}
	CentrifugoRpcRequest request;
	request.method = call->method;
	request.data = std::move(call->data);
	request.headers = call->headers;
	request.timeoutMs = static_cast<std::uint32_t>(kRpcCallTimeoutMs);

	const std::weak_ptr<bool> guard = alive;
	std::uint64_t carrierCallId = 0;
	const std::int32_t status = socket->rpc(
		request,
		[this, guard, callId](const CentrifugoResult &result) {
			if (guard.lock()) {
				onRpcResult(callId, result);
			}
		},
		&carrierCallId);
	if (status != kCentrifugoStatusOk) {
		// The completion does not run for a call that could not start.
		CentrifugoResult failed;
		failed.status = status;
		onRpcResult(callId, failed);
		return;
	}
	if (active.find(callId) == active.end()) {
		return;
	}
	call->carrierCallId = carrierCallId;
	armGrace(callId, kRpcCallTimeoutMs);
}

void RpcCarrier::dispatchMedia(
		std::uint64_t callId,
		const std::shared_ptr<PendingCall> &call) {
	if (!media) {
		settle(callId, networkFailure(), false);
		return;
	}
	const std::weak_ptr<bool> guard = alive;
	const std::uint64_t carrierCallId = media->call(
		call->method,
		std::move(call->data),
		kMediaCallTimeoutMs,
		[this, guard, callId](MediaReply reply) {
			if (guard.lock()) {
				onMediaReply(callId, std::move(reply));
			}
		});
	// The media fleet may settle a call that cannot start before call() returns.
	if (active.find(callId) == active.end()) {
		return;
	}
	call->carrierCallId = carrierCallId;
	armGrace(callId, kMediaCallTimeoutMs);
}

void RpcCarrier::armGrace(std::uint64_t callId, std::int64_t timeoutMs) {
	const std::weak_ptr<bool> guard = alive;
	schedule(timeoutMs + kSettleGraceMs, [this, guard, callId]() {
		if (guard.lock()) {
			onGraceExpired(callId);
		}
	});
}

void RpcCarrier::onGraceExpired(std::uint64_t callId) {
	// No answer within the timeout and the grace: the call is cut off.
	settle(callId, networkFailure(), true);
}

void RpcCarrier::onRpcResult(std::uint64_t callId, const CentrifugoResult &result) {
	const PendingMap::iterator it = active.find(callId);
	if (it == active.end()) {
		return;
	}
	RpcCarrierReply reply;
	reply.outcome = mapCarrierResult(result, it->second->carrier);
	if (reply.outcome.kind == CarrierOutcomeKind::Reply) {
		reply.data = result.data;
	}
	settle(callId, std::move(reply), false);
}

void RpcCarrier::onMediaReply(std::uint64_t callId, MediaReply reply) {
	RpcCarrierReply settled;
	settled.outcome = std::move(reply.outcome);
	settled.data = std::move(reply.data);
	settle(callId, std::move(settled), false);
}

void RpcCarrier::settle(
		std::uint64_t callId,
		RpcCarrierReply reply,
		bool cancelSent) {
	const PendingMap::iterator it = active.find(callId);
	if (it == active.end()) {
		return;
	}
	// Removed before any callback so that a re-entrant call sees it settled.
	const std::shared_ptr<PendingCall> call = it->second;
	active.erase(it);
	if (cancelSent && call->carrierCallId != 0) {
		cancelOnSocket(*call);
	}
	if (call->done) {
		call->done(std::move(reply));
	}
}

void RpcCarrier::cancelOnSocket(const PendingCall &call) {
	switch (call.carrier) {
	case CarrierKind::Update:
		if (update) {
			static_cast<void>(update->cancelRpc(call.carrierCallId));
		}
		break;

	case CarrierKind::Pool:
		if (pool) {
			static_cast<void>(pool->cancelRpc(call.carrierCallId));
		}
		break;

	case CarrierKind::Media:
		if (media) {
			media->cancel(call.carrierCallId);
		}
		break;
	}
}

} // namespace Impulse
