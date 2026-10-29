// Transport owner of one account. It creates the update and pool socket
// facades, the RPC carrier, the media fleet, the connection state host and
// the token and identity managers, and it is the only owner of them. Nothing
// dials before start(). Every method and every callback runs on the Qt main
// thread: the sessions of the account post their requests here, and the
// completions come back on the main thread.
#pragma once

#include "base/weak_ptr.h"
#include "mtproto/core_types.h"
#include "mtproto/impulse/glue/centrifuge_client.h"
#include "mtproto/impulse/glue/connection_state_host.h"
#include "mtproto/impulse/glue/identity_manager.h"
#include "mtproto/impulse/glue/media_fleet.h"
#include "mtproto/impulse/glue/rpc_carrier.h"
#include "mtproto/impulse/glue/token_manager.h"
#include "mtproto/impulse/glue/token_store.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace Impulse {

// What the account hands its transport. The session pair lives in the account
// blob, so the account supplies its load, save and clear. The identity token
// is kept in its own file. The user id is the account user at start.
struct ImpulseAccountStorage {
	std::function<std::optional<SessionTokens>()> loadSession;
	std::function<void(const SessionTokens &tokens)> saveSession;
	std::function<void()> clearSession;
	std::filesystem::path identityPath;
	std::int64_t userId = 0;
};

class ImpulseConnection final : public base::has_weak_ptr {
public:
	explicit ImpulseConnection(ImpulseAccountStorage storage);
	ImpulseConnection(const ImpulseConnection &other) = delete;
	ImpulseConnection &operator=(const ImpulseConnection &other) = delete;

	// Cancels every pending completion, closes the carrier and the media pool,
	// then the pool and the update sockets, in that order.
	~ImpulseConnection();

	// Connects the update and the pool sockets, once, and binds the account
	// user id. A route that blocks the traffic (the blackhole endpoint of the
	// proxy) never connects. A second start only binds the user id again.
	void start(std::int64_t userId);

	// The account user id changed. A login releases the calls that wait for
	// it.
	void setUserId(std::int64_t userId);

	// Fails the calls that need a login with -1000, and clears the tokens when
	// resetKeys is set or a user was bound. The media pool is retired then, as
	// Android cleanup does.
	void cleanup(bool resetKeys);

	// Publishes the media endpoint of thisDc from the media_only options of the
	// account config. Without such an option the media fleet has no endpoint,
	// and its calls fail as network failures.
	void setMediaOptions(
		const std::vector<MediaDcOption> &options,
		int thisDc);

	// Sends one call. The done callback runs once on the main thread with the
	// reply or the error. A call that needs a login waits for it. A call that
	// is cancelled does not run it.
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
		RpcCarrierRequest deferred;
		std::uint64_t carrierCallId = 0;
		bool needsLogin = false;
		bool waitingLogin = false;
		bool settled = false;
		bool cancelled = false;
	};

	using SocketCallDone = std::function<void(
		CarrierOutcome outcome,
		std::vector<std::uint8_t> data)>;

	[[nodiscard]] bool loginReady() const;
	void releaseLoginWaiters();
	void dispatch(
		mtpRequestId requestId,
		const std::shared_ptr<PendingRequest> &pending,
		RpcCarrierRequest request);
	void settle(
		mtpRequestId requestId,
		const std::shared_ptr<PendingRequest> &pending,
		RpcCarrierReply reply);
	void failPending(
		const std::shared_ptr<PendingRequest> &pending,
		CarrierOutcome outcome);
	void callSocket(
		CentrifugeClient &socket,
		CarrierKind carrier,
		const std::string &method,
		std::vector<std::uint8_t> payload,
		std::int64_t timeoutMs,
		SocketCallDone done);
	void answerConnectionToken(std::uint64_t requestId);

	ConnectionStateHost connectionState;
	CentrifugeClient updateClient;
	CentrifugeClient poolClient;
	std::unique_ptr<MediaFleet> mediaFleet;
	std::unique_ptr<RpcCarrier> rpcCarrier;
	std::unique_ptr<SessionStore> sessionStore;
	std::unique_ptr<TokenStore> identityStore;
	std::unique_ptr<TokenManager> tokens;
	std::unique_ptr<IdentityManager> identity;
	std::map<mtpRequestId, std::shared_ptr<PendingRequest>> pendingRequests;
	std::int64_t userId = 0;
	bool started = false;
	bool dialAllowed = false;
};

} // namespace Impulse
