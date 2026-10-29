#include "mtproto/impulse/glue/impulse_connection.h"

#include "base/call_delayed.h"
#include "config.h"
#include "core/application.h"
#include "logs.h"
#include "mtproto/impulse/core/mapping.h"
#include "mtproto/impulse/core/method_classes.h"
#include "mtproto/impulse/core/proxy_routing.h"
#include "mtproto/impulse/core/transcoder.h"
#include "mtproto/impulse/glue/carrier_errors.h"
#include "mtproto/impulse/glue/login_flow.h"
#include "mtproto/impulse/glue/media_pool_rust.h"
#include "mtproto/impulse/glue/mtp_buffer_bridge.h"
#include "settings.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Impulse {
namespace {

// The method names of the identity and refresh calls. Both are pool-only
// methods of the router, so the socket calls below name the socket themselves.
const std::string kConnectionIdentityMethod = "connectionIdentity";
const std::string kRefreshSessionMethod = "refreshSession";
const std::string kInitConnectionMethod = "initConnection";

// The declaration after a login is sent up to three times, one second apart,
// as Android does when the update socket is not up yet.
constexpr std::uint32_t kDeclarationAttempts = 3;
constexpr std::int64_t kDeclarationRetryMs = 1000;

// The code Android fails the login-only calls with when the login goes away.
constexpr int kLoginLostErrorCode = -1000;

// The reason of the difference request of a resubscribe without recovery.
// Realtime names it the same way.
constexpr std::string_view kSubscribeNotRecoveredReason = "subscribe_not_recovered";

// Constructors of the TL objects that the realtime path hands to the updates
// pipeline: updatesTooLong is a difference trigger there, and newSessionCreated
// runs mtpNewSessionCreated.
constexpr std::uint32_t kUpdatesTooLongConstructor = 0xe317af7e;
constexpr std::uint32_t kNewSessionCreatedConstructor = 0x9ec20908;

// The three int64 fields of newSessionCreated. The updates pipeline reads only
// the constructor of that object, so the fields are written as zeros.
constexpr std::size_t kNewSessionCreatedTailBytes = 24;

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

// Milliseconds since the unix epoch.
[[nodiscard]] std::int64_t NowMs() {
	return static_cast<std::int64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count());
}

// The proxy epoch of the process. It advances once per proxy change.
std::atomic<std::uint64_t> proxyEpochCounter{ 0 };

// A TL object of the given constructor followed by zero bytes of its fields.
[[nodiscard]] std::vector<std::uint8_t> ConstructorBytes(
		std::uint32_t constructor,
		std::size_t tailBytes) {
	std::vector<std::uint8_t> bytes(4 + tailBytes, 0);
	bytes[0] = static_cast<std::uint8_t>(constructor & 0xff);
	bytes[1] = static_cast<std::uint8_t>((constructor >> 8) & 0xff);
	bytes[2] = static_cast<std::uint8_t>((constructor >> 16) & 0xff);
	bytes[3] = static_cast<std::uint8_t>((constructor >> 24) & 0xff);
	return bytes;
}

// The route of the sockets as of now. A routing state that cannot be read,
// because the core is not up, counts as blocked: nothing dials then.
[[nodiscard]] TransportRoute ReadRoute() {
	TransportRoute route;
	if (!Core::IsAppLaunched()) {
		return route;
	}
	const std::optional<ProxyRouting::HttpEndpoint> endpoint
		= Core::App().impulseHttpEndpoint();
	route.blocked = IsBlackhole(endpoint);
	if (!route.blocked) {
		route.proxyUrl = ProxyUrlOf(endpoint);
	}
	return route;
}

[[nodiscard]] bool SameRoute(
		const TransportRoute &first,
		const TransportRoute &second) {
	return first.blocked == second.blocked
		&& first.proxyUrl == second.proxyUrl;
}

// The mapping codec of the realtime decode path. The session glue has its own
// codec; this one is loaded once per process from the same file.
struct ConnectionCodec final {
	explicit ConnectionCodec(MappingTable source)
	: table(std::move(source))
	, transcoder(table) {
	}

	MappingTable table;
	Transcoder transcoder;
};

[[nodiscard]] const ConnectionCodec *SharedConnectionCodec() {
	static const std::unique_ptr<ConnectionCodec> codec = []()
			-> std::unique_ptr<ConnectionCodec> {
		std::string error;
		const std::string path
			= (cExeDir() + "impulse/tl-proto-232.json.gz").toStdString();
		std::optional<MappingTable> table = MappingTable::LoadGzipFile(
			path,
			&error);
		if (!table) {
			LOG(("ImpulseM Error: mapping was not loaded for realtime: %1."
				).arg(QString::fromStdString(error)));
			return nullptr;
		}
		return std::make_unique<ConnectionCodec>(std::move(*table));
	}();
	return codec.get();
}

[[nodiscard]] const Transcoder *SharedTranscoder() {
	const ConnectionCodec *const codec = SharedConnectionCodec();
	return codec ? &codec->transcoder : nullptr;
}

// The id of a mapped method, or zero when the method is not mapped. The reply
// of a login is decoded with it.
[[nodiscard]] std::uint32_t MappedMethodId(const std::string &method) {
	const ConnectionCodec *const codec = SharedConnectionCodec();
	if (!codec) {
		return 0;
	}
	return codec->table.methodId(method).value_or(0);
}

// Request ids of the calls that the connection makes by itself. They are far
// below the declarations of the sessions, which count from -1.
[[nodiscard]] mtpRequestId NextConnectionRequestId() {
	static std::atomic<mtpRequestId> counter = -(1 << 30);
	return counter.fetch_sub(1) - 1;
}

// The timer of the carrier and of the token and identity managers: a task runs
// on the main thread after the delay, and never once the connection is gone.
[[nodiscard]] RpcCarrierScheduler MakeScheduler(
		const base::weak_ptr<ImpulseConnection> &weak) {
	return [weak](std::int64_t delayMs, std::function<void()> task) {
		base::call_delayed(
			static_cast<crl::time>(delayMs),
			weak,
			std::move(task));
	};
}

// The session pair of the account, through the three callbacks the account
// gives its transport. It runs on the main thread only.
class AccountSessionStore final : public SessionStore {
public:
	AccountSessionStore(
		std::function<std::optional<SessionTokens>()> loadCallback,
		std::function<void(const SessionTokens &tokens)> saveCallback,
		std::function<void()> clearCallback)
	: loadFn(std::move(loadCallback))
	, saveFn(std::move(saveCallback))
	, clearFn(std::move(clearCallback)) {
	}

	[[nodiscard]] std::optional<SessionTokens> load() override {
		if (!loadFn) {
			return std::nullopt;
		}
		return loadFn();
	}

	void save(const SessionTokens &tokens) override {
		if (saveFn) {
			saveFn(tokens);
		}
	}

	void clear() override {
		if (clearFn) {
			clearFn();
		}
	}

private:
	std::function<std::optional<SessionTokens>()> loadFn;
	std::function<void(const SessionTokens &tokens)> saveFn;
	std::function<void()> clearFn;
};

} // namespace

ImpulseConnection::ImpulseConnection(ImpulseAccountStorage storage) {
	// The session pair goes to the account blob, the identity token to its own
	// file. Loading them does not dial.
	sessionStore = std::make_unique<AccountSessionStore>(
		std::move(storage.loadSession),
		std::move(storage.saveSession),
		std::move(storage.clearSession));
	identityStore = std::make_unique<DpapiTokenStore>(storage.identityPath);

	const std::shared_ptr<std::mt19937_64> engine
		= std::make_shared<std::mt19937_64>(std::random_device()());
	const RandomUnit random = [engine]() -> double {
		return std::uniform_real_distribution<double>(0.0, 1.0)(*engine);
	};

	TokenManagerDependencies tokenDependencies;
	tokenDependencies.store = sessionStore.get();
	tokenDependencies.schedule = MakeScheduler(base::make_weak(this));
	tokenDependencies.nowMs = NowMs;
	tokenDependencies.random = random;
	tokens = std::make_unique<TokenManager>(std::move(tokenDependencies));

	// The identity mint goes to the pool fleet, the renewal to the update
	// socket, as the router sends the method name of the mint.
	IdentityManagerDependencies identityDependencies;
	identityDependencies.store = identityStore.get();
	identityDependencies.mint = [this](
			std::vector<std::uint8_t> payload,
			std::int64_t timeoutMs,
			ConnectionIdentityDone done) {
		callSocket(
			poolClient.get(),
			CarrierKind::Pool,
			kConnectionIdentityMethod,
			std::move(payload),
			timeoutMs,
			[done = std::move(done)](
					CarrierOutcome outcome,
					std::vector<std::uint8_t> data) {
				done(ConnectionIdentityReply{
					std::move(outcome),
					std::move(data),
				});
			});
	};
	identityDependencies.schedule = MakeScheduler(base::make_weak(this));
	identityDependencies.nowMs = NowMs;
	identityDependencies.random = random;
	identity = std::make_unique<IdentityManager>(std::move(identityDependencies));
	identity->setApiId(ApiId);
	identity->setRenewalCall([this](
			std::vector<std::uint8_t> payload,
			std::int64_t timeoutMs,
			ConnectionIdentityDone done) {
		callSocket(
			updateClient.get(),
			CarrierKind::Update,
			kConnectionIdentityMethod,
			std::move(payload),
			timeoutMs,
			[done = std::move(done)](
					CarrierOutcome outcome,
					std::vector<std::uint8_t> data) {
				done(ConnectionIdentityReply{
					std::move(outcome),
					std::move(data),
				});
			});
	});

	tokens->setRefreshCarriers(
		[this](
				std::vector<std::uint8_t> payload,
				std::int64_t timeoutMs,
				RefreshCarrierDone done) {
			callSocket(
				updateClient.get(),
				CarrierKind::Update,
				kRefreshSessionMethod,
				std::move(payload),
				timeoutMs,
				[done = std::move(done)](
						CarrierOutcome outcome,
						std::vector<std::uint8_t> data) {
					done(RefreshCarrierReply{
						std::move(outcome),
						std::move(data),
					});
				});
		},
		[this](
				std::vector<std::uint8_t> payload,
				std::int64_t timeoutMs,
				RefreshCarrierDone done) {
			callSocket(
				poolClient.get(),
				CarrierKind::Pool,
				kRefreshSessionMethod,
				std::move(payload),
				timeoutMs,
				[done = std::move(done)](
						CarrierOutcome outcome,
						std::vector<std::uint8_t> data) {
					done(RefreshCarrierReply{
						std::move(outcome),
						std::move(data),
					});
				});
		});

	// A sign-in ends the identity, and the media pool is retired with it, so
	// no connection keeps another session's token. A refresh of a live session
	// keeps the identity and the pool.
	tokens->setSessionChanged([this]() {
		if (!identity->current().empty()) {
			identity->discard();
			if (mediaFleet) {
				mediaFleet->sessionChanged();
			}
		}
		refreshIdentityLane();
		releaseLoginWaiters();
	});

	// The realtime decode path of the account. Every decoded update goes to the
	// updates handler; a difference trigger is turned into the TL object that
	// the updates pipeline reads as one. The realtime path is off when the
	// mapping is not loaded, and then no lane carries updates.
	if (const Transcoder *const transcoder = SharedTranscoder()) {
		realtime = std::make_unique<Realtime>(
			*transcoder,
			[this](const std::vector<std::uint8_t> &updates) {
				deliverUpdates(updates);
			},
			[this](const std::string &reason, const std::string &channel) {
				// A channel gap falls back to the account difference, as
				// Realtime documents; the channel name does not change it.
				static_cast<void>(channel);
				requestDifference(reason);
			},
			[this]() {
				if (lanes) {
					lanes->requestRecompute();
				}
			});
	}
	lanes = makeLanes();

	// The login capture reads the replies of the identity methods. A pair it
	// reads is adopted on the main thread, like every other completion.
	loginFlow = std::make_unique<LoginFlow>(
		SharedTranscoder(),
		[this](const SessionTokens &pair) {
			adoptLogin(pair);
		});

	buildTransport(ReadRoute());
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
	lanes.reset();
	dropTransport();
}

void ImpulseConnection::setUpdatesHandler(UpdatesHandler handler) {
	updatesHandler = std::move(handler);
}

void ImpulseConnection::start(std::int64_t userIdValue) {
	bindUser(userIdValue);
	if (started) {
		releaseLoginWaiters();
		return;
	}
	started = true;

	// The route is read again: a route that changed since the connection was
	// built is applied before anything dials.
	const TransportRoute route = ReadRoute();
	if (!SameRoute(route, currentRoute)) {
		rebuildTransport(route);
	}
	lanes->start();
	connectSockets();
}

void ImpulseConnection::setUserId(std::int64_t userIdValue) {
	bindUser(userIdValue);
	releaseLoginWaiters();
}

void ImpulseConnection::cleanup(bool resetKeys) {
	const bool hadUser = (userId != 0);
	bindUser(0);

	std::vector<std::pair<mtpRequestId, std::shared_ptr<PendingRequest>>> victims;
	for (const std::pair<const mtpRequestId, std::shared_ptr<PendingRequest>> &entry
			: pendingRequests) {
		if (entry.second->needsLogin) {
			victims.emplace_back(entry.first, entry.second);
		}
	}
	for (const std::pair<mtpRequestId, std::shared_ptr<PendingRequest>> &victim
			: victims) {
		pendingRequests.erase(victim.first);
		CarrierOutcome outcome;
		outcome.kind = CarrierOutcomeKind::RpcError;
		outcome.error.code = kLoginLostErrorCode;
		failPending(victim.second, std::move(outcome));
	}

	if (resetKeys || hadUser) {
		tokens->clear();
		if (mediaFleet) {
			mediaFleet->sessionChanged();
		}
	}
}

void ImpulseConnection::setMediaOptions(
		const std::vector<MediaDcOption> &options,
		int thisDc) {
	mediaEndpoint = mediaEndpointFor(options, thisDc);
	mediaPublished = true;
	if (mediaFleet) {
		mediaFleet->setEndpoint(mediaEndpoint);
	}
}

void ImpulseConnection::setLanes(
		std::vector<LaneCandidate> candidates,
		std::int64_t openedChannelIdValue) {
	laneCandidates = std::move(candidates);
	openedChannelId = openedChannelIdValue;
	if (lanes) {
		lanes->setOpenedChannel(openedChannelId);
	}
}

void ImpulseConnection::beginProxyChange() {
	proxyEpochCounter.fetch_add(1);
}

std::uint64_t ImpulseConnection::proxyEpoch() {
	return proxyEpochCounter.load();
}

void ImpulseConnection::proxyChanged() {
	const TransportRoute route = ReadRoute();
	if (!SameRoute(route, currentRoute)) {
		// The sockets take the proxy URL only at create time, so they are
		// created again. The state moves to Disconnected until they connect.
		if (started) {
			connectionState.onNetworkLost();
		}
		rebuildTransport(route);
		if (started) {
			connectSockets();
		}
	} else if (started && updateClient) {
		static_cast<void>(updateClient->reconnectNow());
	}
	releaseLoginWaiters();
}

void ImpulseConnection::networkChanged(bool online) {
	if (lanes) {
		lanes->requestRecompute();
	}
	if (!online) {
		networkLost = true;
		connectionState.onNetworkLost();
		return;
	}
	if (!networkLost) {
		return;
	}
	networkLost = false;
	if (started && updateClient) {
		static_cast<void>(updateClient->reconnectNow());
	}
	releaseLoginWaiters();
}

void ImpulseConnection::resume(bool online) {
	networkChanged(online);
	if (started && updateClient) {
		static_cast<void>(updateClient->reconnectNow());
	}
}

void ImpulseConnection::send(
		mtpRequestId requestId,
		RpcCarrierRequest request,
		RpcCarrierDone done) {
	const std::shared_ptr<PendingRequest> pending
		= std::make_shared<PendingRequest>();
	pending->done = std::move(done);
	// A method outside the identity set needs a login, as Android waits for a
	// request without FLAG_WITHOUT_LOGIN. route.signedIn is the state of the
	// socket and is set at dispatch, so it is not read here.
	const std::string &method = request.route.method;
	pending->needsLogin = !isIdentitySocketMethod(method);
	// The replies of the login methods may carry the session pair. The
	// declarations and the identity calls never do.
	pending->capturesLogin = isIdentitySocketMethod(method)
		&& method != kInitConnectionMethod
		&& method != kConnectionIdentityMethod
		&& method != kRefreshSessionMethod;
	if (pending->capturesLogin) {
		pending->methodId = MappedMethodId(method);
	}
	if (method == kInitConnectionMethod) {
		lastDeclaration = request.data;
	}
	pendingRequests[requestId] = pending;

	// A call that needs a login waits for it, as Android does. The waiting
	// call is sent by releaseLoginWaiters() once the login is ready.
	if (pending->needsLogin && !loginReady()) {
		pending->waitingLogin = true;
		pending->deferred = std::move(request);
		return;
	}
	dispatch(requestId, pending, std::move(request));
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
	if (!pending->settled && rpcCarrier) {
		// The carrier settles the call with a network failure, which is
		// dropped here because the call is cancelled. A call that still waits
		// for the login has no carrier call: the cancel of id 0 is ignored.
		rpcCarrier->cancel(pending->carrierCallId);
	}
}

bool ImpulseConnection::loginReady() const {
	return (userId != 0) && tokens->hasSession();
}

void ImpulseConnection::bindUser(std::int64_t userIdValue) {
	userId = userIdValue;
	laneIdentity.userId = userIdValue;
	if (lanes) {
		lanes->setIdentity(laneIdentity);
	}
}

void ImpulseConnection::releaseLoginWaiters() {
	if (!loginReady()) {
		return;
	}
	std::vector<std::pair<mtpRequestId, std::shared_ptr<PendingRequest>>> ready;
	for (const std::pair<const mtpRequestId, std::shared_ptr<PendingRequest>> &entry
			: pendingRequests) {
		if (entry.second->waitingLogin) {
			ready.emplace_back(entry.first, entry.second);
		}
	}
	for (const std::pair<mtpRequestId, std::shared_ptr<PendingRequest>> &waiter
			: ready) {
		const std::shared_ptr<PendingRequest> pending = waiter.second;
		if (!pending->waitingLogin || pending->cancelled) {
			continue;
		}
		pending->waitingLogin = false;
		RpcCarrierRequest request = std::move(pending->deferred);
		dispatch(waiter.first, pending, std::move(request));
	}
}

void ImpulseConnection::dispatch(
		mtpRequestId requestId,
		const std::shared_ptr<PendingRequest> &pending,
		RpcCarrierRequest request) {
	// The socket is signed in when the account has a user and a session. The
	// router refuses the login-only methods of a signed-in socket, and every
	// method outside the identity set on a signed-out one.
	request.route.signedIn = loginReady();

	if (!rpcCarrier) {
		// No carrier while the transport is being rebuilt or the route is
		// blocked: the call fails as a network failure at once.
		RpcCarrierReply reply;
		settle(requestId, pending, std::move(reply));
		return;
	}

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

void ImpulseConnection::settle(
		mtpRequestId requestId,
		const std::shared_ptr<PendingRequest> &pending,
		RpcCarrierReply reply) {
	pending->settled = true;
	// The pair of a login is stored before the reply goes on, so the calls
	// that follow find the session. A cancelled login may still have stored it.
	if (pending->capturesLogin
		&& reply.outcome.kind == CarrierOutcomeKind::Reply
		&& loginFlow) {
		static_cast<void>(loginFlow->onReply(pending->methodId, reply.data));
	}
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

void ImpulseConnection::failPending(
		const std::shared_ptr<PendingRequest> &pending,
		CarrierOutcome outcome) {
	// The carrier's own network failure of the cancel is dropped: the done
	// callback below gets the outcome of the failure.
	pending->cancelled = true;
	RpcCarrierDone done = std::move(pending->done);
	pending->done = nullptr;
	if (!pending->settled && !pending->waitingLogin && rpcCarrier) {
		rpcCarrier->cancel(pending->carrierCallId);
	}
	if (done) {
		RpcCarrierReply reply;
		reply.outcome = std::move(outcome);
		done(std::move(reply));
	}
}

void ImpulseConnection::callSocket(
		CentrifugeClient *socket,
		CarrierKind carrier,
		const std::string &method,
		std::vector<std::uint8_t> payload,
		std::int64_t timeoutMs,
		SocketCallDone done) {
	if (!socket) {
		// No socket is built for a blocked route: the call fails as a network
		// failure, and nothing dials.
		CarrierOutcome outcome;
		outcome.kind = CarrierOutcomeKind::NetworkFailure;
		done(std::move(outcome), {});
		return;
	}

	CentrifugoRpcRequest request;
	request.method = method;
	request.data = std::move(payload);
	request.timeoutMs = static_cast<std::uint32_t>(timeoutMs);

	const std::int32_t status = socket->rpc(
		request,
		[carrier, done](const CentrifugoResult &result) {
			CarrierOutcome outcome = mapCarrierResult(result, carrier);
			std::vector<std::uint8_t> data;
			if (outcome.kind == CarrierOutcomeKind::Reply) {
				data = result.data;
			}
			done(std::move(outcome), std::move(data));
		});
	if (status != kCentrifugoStatusOk) {
		// The completion does not run for a call that could not start.
		CentrifugoResult failed;
		failed.status = status;
		done(mapCarrierResult(failed, carrier), {});
	}
}

void ImpulseConnection::answerConnectionToken(std::uint64_t requestId) {
	if (tokens->hasSession()) {
		tokens->connectionToken([this, requestId](ConnectionTokenResult result) {
			if (result.status == RefreshStatus::Fresh && !result.token.empty()) {
				resolveUpdateToken(
					requestId,
					CentrifugoTokenOutcome::Token,
					result.token);
				return;
			}
			const CentrifugoTokenOutcome outcome = (result.status == RefreshStatus::Ended)
				? CentrifugoTokenOutcome::Unauthorized
				: CentrifugoTokenOutcome::Failed;
			resolveUpdateToken(requestId, outcome, std::string());
		});
		return;
	}
	const std::string current = identity->current();
	if (!current.empty()) {
		resolveUpdateToken(requestId, CentrifugoTokenOutcome::Token, current);
		return;
	}
	identity->obtain([this, requestId](IdentityObtainResult result) {
		if (!result.token.empty()) {
			// The identity lane of the minted identity is subscribed from now.
			refreshIdentityLane();
			resolveUpdateToken(requestId, CentrifugoTokenOutcome::Token, result.token);
			return;
		}
		resolveUpdateToken(requestId, CentrifugoTokenOutcome::Failed, std::string());
	});
}

void ImpulseConnection::resolveUpdateToken(
		std::uint64_t requestId,
		CentrifugoTokenOutcome outcome,
		const std::string &token) {
	if (updateClient) {
		static_cast<void>(updateClient->resolveToken(requestId, outcome, token));
	}
}

void ImpulseConnection::refreshIdentityLane() {
	// QR login is answered on the identity lane of the connection identity.
	// Without a usable identity the lane is dropped.
	std::string identityId;
	if (identity) {
		identityId = IdentityLaneId(identity->current());
	}
	laneIdentity.identityId = identityId;
	if (lanes) {
		lanes->setIdentity(laneIdentity);
	}
}

void ImpulseConnection::adoptLogin(const SessionTokens &pair) {
	// The pair ends the connection identity: the session change discards it
	// and retires the media pool.
	tokens->onLoginTokens(pair);
	refreshIdentityLane();

	// The update socket connected with the identity token. It reconnects now
	// and asks for the access token.
	if (updateClient) {
		static_cast<void>(updateClient->reconnectNow());
	}

	// The user is bound last. The calls that wait for the login are released
	// here, and the account lanes follow the user.
	bindUser(pair.userId);
	releaseLoginWaiters();
	redeclare(1);
}

void ImpulseConnection::redeclare(std::uint32_t attempt) {
	if (lastDeclaration.empty()) {
		return;
	}
	RpcCarrierRequest request;
	request.route.method = kInitConnectionMethod;
	request.data = lastDeclaration;
	const base::weak_ptr<ImpulseConnection> weak = base::make_weak(this);
	send(
		NextConnectionRequestId(),
		std::move(request),
		[weak, attempt](RpcCarrierReply reply) {
			if (reply.outcome.kind == CarrierOutcomeKind::Reply
				|| attempt >= kDeclarationAttempts) {
				return;
			}
			base::call_delayed(
				static_cast<crl::time>(kDeclarationRetryMs),
				weak,
				[weak, attempt]() {
					ImpulseConnection *const connection = weak.get();
					if (connection) {
						connection->redeclare(attempt + 1);
					}
				});
		});
}

void ImpulseConnection::buildTransport(const TransportRoute &route) {
	currentRoute = route;
	const base::weak_ptr<ImpulseConnection> weak = base::make_weak(this);
	const RpcCarrierScheduler schedule = MakeScheduler(weak);

	if (!route.blocked) {
		// Socket callbacks come from the Rust runtime threads: they are handed
		// to the main thread, and they do nothing once this connection is gone.
		const CentrifugoRust::Poster poster = [weak](std::function<void()> task) {
			crl::on_main(weak, [task = std::move(task)] {
				task();
			});
		};

		CentrifugoConfig updateConfig;
		updateConfig.proxyUrl = route.proxyUrl;
		updateClient = std::make_unique<CentrifugeClient>();
		const std::int32_t updateStatus = updateClient->create(
			CentrifugeFleet::Update,
			updateConfig,
			poster,
			&connectionState);
		if (updateStatus != kCentrifugoStatusOk) {
			LOG(("ImpulseM Error: update client was not created, status %1.").arg(updateStatus));
		}

		CentrifugoConfig poolConfig;
		poolConfig.proxyUrl = route.proxyUrl;
		poolClient = std::make_unique<CentrifugeClient>();
		const std::int32_t poolStatus = poolClient->create(
			CentrifugeFleet::Pool,
			poolConfig,
			poster,
			nullptr);
		if (poolStatus != kCentrifugoStatusOk) {
			LOG(("ImpulseM Error: pool client was not created, status %1.").arg(poolStatus));
		}

		// The update socket connects with the session access token, or with the
		// identity token while there is no session. The media pool presents the
		// same token (tokenFrom), so the update socket is the only one that asks.
		static_cast<void>(updateClient->setTokenCallback(
			[this](const CentrifugoTokenRequest &request) {
				if (request.kind != CentrifugoTokenKind::Connection) {
					// Channel tokens are not used: subscriptions carry none.
					resolveUpdateToken(
						request.requestId,
						CentrifugoTokenOutcome::Anonymous,
						std::string());
					return;
				}
				answerConnectionToken(request.requestId);
			}));

		// The media pool is built on the update client, which owns the socket it
		// shares its token with. Without that client there is no media fleet.
		CentrifugoRust *const updateRust = updateClient->rustClient();
		if (updateRust) {
			MediaFleetDependencies mediaDependencies;
			mediaDependencies.createPool = makeRustMediaPoolFactory(updateRust);
			mediaDependencies.schedule = schedule;
			mediaFleet = std::make_unique<MediaFleet>(std::move(mediaDependencies));
			if (mediaPublished) {
				mediaFleet->setEndpoint(mediaEndpoint);
			}
		}
	}

	RpcCarrierDependencies carrierDependencies;
	carrierDependencies.update = updateClient.get();
	carrierDependencies.pool = poolClient.get();
	carrierDependencies.media = mediaFleet.get();
	carrierDependencies.schedule = schedule;
	rpcCarrier = std::make_unique<RpcCarrier>(std::move(carrierDependencies));
}

void ImpulseConnection::dropTransport() {
	// The carrier settles its open calls with network failures while it closes.
	// A call sent from one of those callbacks finds no carrier and fails the
	// same way, so the carrier is taken out before it is closed.
	const std::unique_ptr<RpcCarrier> carrier = std::move(rpcCarrier);
	if (carrier) {
		carrier->close();
	}

	const std::unique_ptr<MediaFleet> media = std::move(mediaFleet);
	if (media) {
		media->close();
	}

	// The pool goes before the update client, as the destructor does.
	const std::unique_ptr<CentrifugeClient> pool = std::move(poolClient);
	if (pool) {
		pool->close();
	}

	const std::unique_ptr<CentrifugeClient> update = std::move(updateClient);
	if (update) {
		update->close();
	}
}

void ImpulseConnection::rebuildTransport(const TransportRoute &route) {
	dropTransport();
	buildTransport(route);
	rebuildLanes();
}

void ImpulseConnection::connectSockets() {
	if (currentRoute.blocked) {
		return;
	}
	if (updateClient) {
		static_cast<void>(updateClient->connect());
	}
	if (poolClient) {
		static_cast<void>(poolClient->connect());
	}
}

void ImpulseConnection::rebuildLanes() {
	// The lanes observer keeps the lanes it subscribed on the client it had.
	// A new client has none, so the observer is built again and started again
	// when it was running.
	const bool wasActive = lanes && lanes->isActive();
	lanes = makeLanes();
	lanes->setIdentity(laneIdentity);
	if (wasActive) {
		lanes->start();
	}
}

std::unique_ptr<LanesObserver> ImpulseConnection::makeLanes() {
	const base::weak_ptr<ImpulseConnection> weak = base::make_weak(this);
	return std::make_unique<LanesObserver>(
		[this]() -> std::vector<LaneCandidate> {
			return laneCandidates;
		},
		[this](const std::string &lane) {
			if (updateClient && realtime) {
				static_cast<void>(realtime->subscribe(*updateClient, lane));
			}
		},
		[this](const std::string &lane) {
			if (updateClient && realtime) {
				static_cast<void>(realtime->unsubscribe(*updateClient, lane));
			}
		},
		[weak](std::chrono::milliseconds delay, std::function<void()> task) {
			base::call_delayed(
				static_cast<crl::time>(delay.count()),
				weak,
				std::move(task));
		});
}

void ImpulseConnection::deliverUpdates(const std::vector<std::uint8_t> &updates) {
	if (!updatesHandler) {
		return;
	}
	MTP::Response response;
	try {
		response.reply = TlBytesToBuffer<mtpBuffer>(updates);
	} catch (const MtpBufferBridgeError &) {
		// A stream that is not whole words cannot be read. It is replaced by a
		// difference trigger, which the pipeline reads as a whole object.
		deliverUpdates(ConstructorBytes(kUpdatesTooLongConstructor, 0));
		return;
	}
	updatesHandler(response);
}

void ImpulseConnection::requestDifference(const std::string &reason) {
	if (reason == kSubscribeNotRecoveredReason) {
		deliverUpdates(ConstructorBytes(
			kNewSessionCreatedConstructor,
			kNewSessionCreatedTailBytes));
		return;
	}
	deliverUpdates(ConstructorBytes(kUpdatesTooLongConstructor, 0));
}

} // namespace Impulse
