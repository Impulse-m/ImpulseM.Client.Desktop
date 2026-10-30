// Transport owner of one account. It creates the update and pool socket
// facades, the RPC carrier, the media fleet, the connection state host, the
// token and identity managers, the realtime decode path and the lanes
// observer, and it is the only owner of them. Nothing dials before start().
// Every method and every callback runs on the Qt main thread: the sessions of
// the account post their requests here, and the completions come back on the
// main thread.
//
// The sockets follow the proxy route. A route change recreates the facades,
// because the Rust client takes its proxy URL only at create time; a change of
// the URL alone would keep dialling the old route.
#pragma once

#include "base/weak_ptr.h"
#include "mtproto/core_types.h"
#include "mtproto/impulse/core/connection_declaration.h"
#include "mtproto/impulse/core/connection_declarer.h"
#include "mtproto/impulse/glue/centrifuge_client.h"
#include "mtproto/impulse/glue/connection_state_host.h"
#include "mtproto/impulse/glue/identity_manager.h"
#include "mtproto/impulse/glue/lanes_observer.h"
#include "mtproto/impulse/glue/login_flow.h"
#include "mtproto/impulse/glue/media_fleet.h"
#include "mtproto/impulse/glue/realtime.h"
#include "mtproto/impulse/glue/rpc_carrier.h"
#include "mtproto/impulse/glue/token_manager.h"
#include "mtproto/impulse/glue/token_store.h"
#include "mtproto/mtproto_response.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Impulse {

// The route of the sockets: blocked, or the proxy URL of the sockets (empty
// when the traffic goes direct).
struct TransportRoute {
	bool blocked = true;
	std::string proxyUrl;
};

// What the account hands its transport. The session pair lives in the account
// blob, so the account supplies its load, save and clear. The identity token
// is kept in its own file. The user id is the account user at start.
struct ImpulseAccountStorage {
	std::function<std::optional<SessionTokens>()> loadSession;
	std::function<void(const SessionTokens &tokens)> saveSession;
	std::function<void()> clearSession;
	std::filesystem::path identityPath;
	std::int64_t userId = 0;

	// The initConnection declaration of the app, read when the connection
	// starts. The values are the ones the classic session sends.
	std::function<ConnectionDeclaration()> declarationSource;
};

class ImpulseConnection final : public base::has_weak_ptr {
public:
	// Receives the decoded updates of the realtime lanes and the synthetic
	// difference triggers. It runs on the main thread, and it is handed the
	// response as a push of the main session would be.
	using UpdatesHandler = std::function<void(const MTP::Response &response)>;

	explicit ImpulseConnection(ImpulseAccountStorage storage);
	ImpulseConnection(const ImpulseConnection &other) = delete;
	ImpulseConnection &operator=(const ImpulseConnection &other) = delete;

	// Cancels every pending completion, closes the carrier and the media pool,
	// then the pool and the update sockets, in that order.
	~ImpulseConnection();

	// Sets the handler of the realtime updates. Without it the updates are
	// dropped.
	void setUpdatesHandler(UpdatesHandler handler);

	// Connects the update and the pool sockets, once, and binds the account
	// user id. The route is read again here. A route that blocks the traffic
	// (the blackhole endpoint of the proxy, or an unknown route) never connects.
	// A second start only binds the user id again.
	void start(std::int64_t userId);

	// The account user id changed. A login releases the calls that wait for
	// it.
	void setUserId(std::int64_t userId);

	// Fails the calls that need a login with -1000, and clears the tokens when
	// resetKeys is set or a user was bound. The media pool is retired then, as
	// Android cleanup does. The account lanes are dropped with the user. With
	// resetKeys the connection identity is discarded as well, in memory and on
	// disk.
	void cleanup(bool resetKeys);

	// Publishes the media endpoint of thisDc from the media_only options of the
	// account config. Without such an option the media fleet has no endpoint,
	// and its calls fail as network failures.
	void setMediaOptions(
		const std::vector<MediaDcOption> &options,
		int thisDc);

	// Sets the dialog candidates of the channel lanes and the opened channel
	// (0 when none). The lanes observer recomputes the channel lanes from them.
	void setLanes(
		std::vector<LaneCandidate> candidates,
		std::int64_t openedChannelId);

	// First step of a proxy change, called once for all accounts before any
	// account's proxyChanged(): advances the proxy epoch. A call that fails
	// after the advance and started before it is a proxy-change failure.
	static void beginProxyChange();

	// The current proxy epoch; see beginProxyChange().
	[[nodiscard]] static std::uint64_t proxyEpoch();

	// Per-account part of a proxy change. The route is read again. When the
	// proxy URL or the blocked state changed, the media pool and the sockets
	// are destroyed and created again, and the lanes are subscribed again. When
	// they did not change, the realtime socket is reconnected now. The waiting
	// calls are released either way.
	void proxyChanged();

	// The network changed. Lanes are recomputed soon. A loss moves the state
	// host to Disconnected; a return after a loss reconnects the realtime
	// socket now and releases the waiting calls, as Android networkChanged.
	void networkChanged(bool online);

	// The app resumes: the network state is checked and the realtime socket
	// connects now, as Android resume.
	void resume(bool online);

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
		std::string method;
		std::uint64_t carrierCallId = 0;
		std::uint32_t methodId = 0;
		bool needsLogin = false;
		bool waitingLogin = false;
		bool settled = false;
		bool cancelled = false;
		bool capturesLogin = false;
	};

	using SocketCallDone = std::function<void(
		CarrierOutcome outcome,
		std::vector<std::uint8_t> data)>;

	[[nodiscard]] bool loginReady() const;
	void noteLoginReady();
	void bindUser(std::int64_t userId);
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
		CentrifugeClient *socket,
		CarrierKind carrier,
		const std::string &method,
		std::vector<std::uint8_t> payload,
		std::int64_t timeoutMs,
		SocketCallDone done);
	void answerConnectionToken(
		std::uint64_t requestId,
		std::uint64_t generation);
	[[nodiscard]] bool tokenAnswerStale(
		std::uint64_t requestId,
		std::uint64_t generation) const;
	void refreshIdentityLane();
	void adoptLogin(const SessionTokens &pair);
	[[nodiscard]] std::string currentCredential();
	void resolveUpdateToken(
		std::uint64_t requestId,
		CentrifugoTokenOutcome outcome,
		const std::string &token);

	// Builds the facades, the carrier and the media fleet for the route. A
	// blocked route builds none: the carrier then fails every call at once.
	void buildTransport(const TransportRoute &route);
	void dropTransport();
	void rebuildTransport(const TransportRoute &route);
	void connectSockets();
	void rebuildLanes();
	[[nodiscard]] std::unique_ptr<LanesObserver> makeLanes();
	void deliverUpdates(const std::vector<std::uint8_t> &updates);
	void requestDifference(const std::string &reason);

	ConnectionStateHost connectionState;
	std::unique_ptr<CentrifugeClient> updateClient;
	std::unique_ptr<CentrifugeClient> poolClient;
	std::unique_ptr<MediaFleet> mediaFleet;
	std::unique_ptr<RpcCarrier> rpcCarrier;
	std::unique_ptr<SessionStore> sessionStore;
	std::unique_ptr<TokenStore> identityStore;
	std::unique_ptr<TokenManager> tokens;
	std::unique_ptr<IdentityManager> identity;
	std::unique_ptr<LoginFlow> loginFlow;
	std::unique_ptr<Realtime> realtime;
	std::unique_ptr<LanesObserver> lanes;
	std::unique_ptr<ConnectionDeclarer> declarer;
	std::function<ConnectionDeclaration()> declarationSource;
	UpdatesHandler updatesHandler;
	std::map<mtpRequestId, std::shared_ptr<PendingRequest>> pendingRequests;
	TransportRoute currentRoute;
	std::vector<LaneCandidate> laneCandidates;
	LaneIdentity laneIdentity;
	std::int64_t openedChannelId = 0;
	std::optional<std::string> mediaEndpoint;
	std::int64_t userId = 0;
	bool started = false;
	bool mediaPublished = false;
	bool networkLost = false;

	// The serial of this instance in the process, and whether its login was
	// seen ready, for the diagnostic lines.
	std::uint64_t serial = 0;
	bool loginReadyNoted = false;

	// Set from a sign-in until the transport is rebuilt for the session. The
	// sockets are keyed to the identity until then, so no login call is sent.
	bool rekeying = false;

	// Raised by every drop of the update and pool sockets. A token request is
	// answered only while the generation of its client is still current, since
	// the request ids of a client restart from 1 on each new client.
	std::uint64_t transportGeneration = 0;
};

} // namespace Impulse
