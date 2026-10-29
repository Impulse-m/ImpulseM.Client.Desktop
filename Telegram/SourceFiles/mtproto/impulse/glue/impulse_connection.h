// Transport owner of one account. It creates the update and pool socket
// facades, the RPC carrier, the media fleet and the connection state host, and
// it is the only owner of them. Nothing dials before start(). Every method and
// every callback runs on the Qt main thread: the sessions of the account post
// their requests here, and the completions come back on the main thread.
#pragma once

#include "base/weak_ptr.h"
#include "mtproto/core_types.h"
#include "mtproto/impulse/glue/centrifuge_client.h"
#include "mtproto/impulse/glue/connection_state_host.h"
#include "mtproto/impulse/glue/media_fleet.h"
#include "mtproto/impulse/glue/rpc_carrier.h"

#include <cstdint>
#include <map>
#include <memory>

namespace Impulse {

class ImpulseConnection final : public base::has_weak_ptr {
public:
	ImpulseConnection();
	ImpulseConnection(const ImpulseConnection &other) = delete;
	ImpulseConnection &operator=(const ImpulseConnection &other) = delete;

	// Settles every pending call with a network failure, closes the media
	// pool, then the pool and the update sockets, in that order.
	~ImpulseConnection();

	// Connects the update and the pool sockets, once. A route that blocks the
	// traffic (the blackhole endpoint of the proxy) never connects.
	void start();

	// Sends one call. The done callback runs once on the main thread with the
	// reply or the error. A call that is cancelled does not run it.
	void send(
		mtpRequestId requestId,
		RpcCarrierRequest request,
		RpcCarrierDone done);

	// Cancels the call sent with requestId: it is cut off on its socket and
	// its done callback does not run. Unknown ids are ignored.
	void cancel(mtpRequestId requestId);

private:
	struct PendingRequest {
		RpcCarrierDone done;
		std::uint64_t carrierCallId = 0;
		bool settled = false;
		bool cancelled = false;
	};

	void settle(
		mtpRequestId requestId,
		const std::shared_ptr<PendingRequest> &pending,
		RpcCarrierReply reply);

	ConnectionStateHost connectionState;
	CentrifugeClient updateClient;
	CentrifugeClient poolClient;
	std::unique_ptr<MediaFleet> mediaFleet;
	std::unique_ptr<RpcCarrier> rpcCarrier;
	std::map<mtpRequestId, std::shared_ptr<PendingRequest>> pendingRequests;
	bool started = false;
	bool dialAllowed = false;
};

} // namespace Impulse
