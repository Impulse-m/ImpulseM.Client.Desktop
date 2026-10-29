// RPC carrier of one account. Routes each call by its method name to the
// update socket, the token-less pool socket or the media fleet, and settles
// every call exactly once with the app result. A call waits for the
// declaration gate when it asks for it, is cut off by a settle grace after
// the carrier timeout, and is refused locally past the pending limit of a
// socket. Owner thread only: every method and every callback runs on the
// thread that owns the carrier.
#pragma once

#include "mtproto/impulse/core/errors.h"
#include "mtproto/impulse/core/method_classes.h"
#include "mtproto/impulse/glue/carrier_errors.h"
#include "mtproto/impulse/glue/centrifugo_rust.h"
#include "mtproto/impulse/glue/media_fleet.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Impulse {

class CentrifugeClient;

// Timeout of one update or pool call, as sent to the socket.
inline constexpr std::int64_t kRpcCallTimeoutMs = 30000;

// How long a call waits for the declaration gate before it is sent anyway.
inline constexpr std::int64_t kDeclarationWaitMs = 10000;

// Extra wait past a call's own timeout, so that the carrier's timeout
// normally settles the call first.
inline constexpr std::int64_t kSettleGraceMs = 5000;

// Calls pending on one socket at a time. Past it a call fails at once with a
// network failure, which is not an rpc_error.
inline constexpr int kMaxPendingRpc = 32;

struct RpcCarrierRequest {
	RouteRequest route;
	std::optional<std::int64_t> takeoutId;
	std::vector<std::uint8_t> data;

	// The call waits for the declaration gate before it is sent. Off for
	// initConnection itself, which is the declaration.
	bool awaitDeclaration = false;
};

struct RpcCarrierReply {
	CarrierOutcome outcome;
	std::vector<std::uint8_t> data;
};

using RpcCarrierDone = std::function<void(RpcCarrierReply reply)>;

// Runs task after delayMs on the owner thread.
using RpcCarrierScheduler = std::function<void(
	std::int64_t delayMs,
	std::function<void()> task)>;

// Calls declared() once the current credential is declared. It may never
// call it: the declaration wait then sends the call anyway.
using RpcDeclarationGate = std::function<void(std::function<void()> declared)>;

struct RpcCarrierDependencies {
	// Not owned. Any of them may be null; a call routed to a missing carrier
	// fails with a network failure.
	CentrifugeClient *update = nullptr;
	CentrifugeClient *pool = nullptr;
	MediaFleet *media = nullptr;

	// Required: it drives the declaration wait and the settle grace.
	RpcCarrierScheduler schedule;
};

class RpcCarrier {
private:
	struct PendingCall {
		CarrierKind carrier = CarrierKind::Update;
		std::string method;
		std::vector<std::uint8_t> data;
		std::vector<CentrifugoHeader> headers;
		RpcCarrierDone done;
		bool dispatched = false;
		std::uint64_t carrierCallId = 0;
	};

	CentrifugeClient *update = nullptr;
	CentrifugeClient *pool = nullptr;
	MediaFleet *media = nullptr;
	RpcCarrierScheduler schedule;
	RpcDeclarationGate declarationGate;

	// Scheduled tasks and carrier callbacks hold a weak copy and do nothing
	// once the carrier is destroyed.
	std::shared_ptr<bool> alive;
	using PendingMap = std::map<std::uint64_t, std::shared_ptr<PendingCall>>;

	PendingMap active;
	std::uint64_t nextId = 1;
	bool closed = false;

public:
	explicit RpcCarrier(RpcCarrierDependencies dependencies);
	RpcCarrier(const RpcCarrier &other) = delete;
	RpcCarrier &operator=(const RpcCarrier &other) = delete;

	// Callers must call close() first: the destructor drops the pending calls
	// without running their callbacks.
	~RpcCarrier() = default;

	// Null removes the gate; calls then never wait for a declaration.
	void setDeclarationGate(RpcDeclarationGate gate);

	// Sends one call and returns its id for cancel(). The done callback runs
	// exactly once, on the owner thread; a call refused at once settles
	// before send() returns.
	std::uint64_t send(RpcCarrierRequest request, RpcCarrierDone done);

	// Settles a pending call with a network failure and cancels it on its
	// socket or in the media fleet. Unknown ids are ignored.
	void cancel(std::uint64_t callId);

	// Settles every pending call with a network failure and accepts no call
	// afterwards.
	void close();

private:
	[[nodiscard]] int pendingOn(CarrierKind carrier) const;
	void dispatch(std::uint64_t callId);
	void dispatchRpc(
		std::uint64_t callId,
		const std::shared_ptr<PendingCall> &call);
	void dispatchMedia(
		std::uint64_t callId,
		const std::shared_ptr<PendingCall> &call);
	void armGrace(std::uint64_t callId, std::int64_t timeoutMs);
	void onGraceExpired(std::uint64_t callId);
	void onRpcResult(std::uint64_t callId, const CentrifugoResult &result);
	void onMediaReply(std::uint64_t callId, MediaReply reply);
	void settle(std::uint64_t callId, RpcCarrierReply reply, bool cancelSent);
	void cancelOnSocket(const PendingCall &call);
};

} // namespace Impulse
