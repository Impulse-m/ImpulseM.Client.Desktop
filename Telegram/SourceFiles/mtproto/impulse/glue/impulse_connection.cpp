#include "mtproto/impulse/glue/impulse_connection.h"

#include "base/call_delayed.h"
#include "config.h"
#include "core/application.h"
#include "logs.h"
#include "mtproto/impulse/core/method_classes.h"
#include "mtproto/impulse/core/proxy_routing.h"
#include "mtproto/impulse/glue/carrier_errors.h"
#include "mtproto/impulse/glue/media_pool_rust.h"

#include <chrono>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace Impulse {
namespace {

// The method names of the identity and refresh calls. Both are pool-only
// methods of the router, so the socket calls below name the socket themselves.
const std::string kConnectionIdentityMethod = "connectionIdentity";
const std::string kRefreshSessionMethod = "refreshSession";

// The code Android fails the login-only calls with when the login goes away.
constexpr int kLoginLostErrorCode = -1000;

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
	tokenDependencies.schedule = schedule;
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
			poolClient,
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
	identityDependencies.schedule = schedule;
	identityDependencies.nowMs = NowMs;
	identityDependencies.random = random;
	identity = std::make_unique<IdentityManager>(std::move(identityDependencies));
	identity->setApiId(ApiId);
	identity->setRenewalCall([this](
			std::vector<std::uint8_t> payload,
			std::int64_t timeoutMs,
			ConnectionIdentityDone done) {
		callSocket(
			updateClient,
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
				updateClient,
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
				poolClient,
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
		releaseLoginWaiters();
	});

	// The update socket connects with the session access token, or with the
	// identity token while there is no session. The media pool presents the
	// same token (tokenFrom), so the update socket is the only one that asks.
	static_cast<void>(updateClient.setTokenCallback(
		[this](const CentrifugoTokenRequest &request) {
			if (request.kind != CentrifugoTokenKind::Connection) {
				// Channel tokens are not used: subscriptions carry none.
				static_cast<void>(updateClient.resolveToken(
					request.requestId,
					CentrifugoTokenOutcome::Anonymous,
					std::string()));
				return;
			}
			answerConnectionToken(request.requestId);
		}));
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

void ImpulseConnection::start(std::int64_t userIdValue) {
	userId = userIdValue;
	if (started) {
		releaseLoginWaiters();
		return;
	}
	started = true;
	if (!dialAllowed) {
		return;
	}
	static_cast<void>(updateClient.connect());
	static_cast<void>(poolClient.connect());
}

void ImpulseConnection::setUserId(std::int64_t userIdValue) {
	userId = userIdValue;
	releaseLoginWaiters();
}

void ImpulseConnection::cleanup(bool resetKeys) {
	const bool hadUser = (userId != 0);
	userId = 0;

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
	if (mediaFleet) {
		mediaFleet->setEndpoint(mediaEndpointFor(options, thisDc));
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
	pending->needsLogin = !isIdentitySocketMethod(request.route.method);
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
	if (!pending->settled) {
		// The carrier settles the call with a network failure, which is
		// dropped here because the call is cancelled. A call that still waits
		// for the login has no carrier call: the cancel of id 0 is ignored.
		rpcCarrier->cancel(pending->carrierCallId);
	}
}

bool ImpulseConnection::loginReady() const {
	return (userId != 0) && tokens->hasSession();
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
	if (!pending->settled && !pending->waitingLogin) {
		rpcCarrier->cancel(pending->carrierCallId);
	}
	if (done) {
		RpcCarrierReply reply;
		reply.outcome = std::move(outcome);
		done(std::move(reply));
	}
}

void ImpulseConnection::callSocket(
		CentrifugeClient &socket,
		CarrierKind carrier,
		const std::string &method,
		std::vector<std::uint8_t> payload,
		std::int64_t timeoutMs,
		SocketCallDone done) {
	CentrifugoRpcRequest request;
	request.method = method;
	request.data = std::move(payload);
	request.timeoutMs = static_cast<std::uint32_t>(timeoutMs);

	const std::int32_t status = socket.rpc(
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
				static_cast<void>(updateClient.resolveToken(
					requestId,
					CentrifugoTokenOutcome::Token,
					result.token));
				return;
			}
			const CentrifugoTokenOutcome outcome = (result.status == RefreshStatus::Ended)
				? CentrifugoTokenOutcome::Unauthorized
				: CentrifugoTokenOutcome::Failed;
			static_cast<void>(updateClient.resolveToken(
				requestId,
				outcome,
				std::string()));
		});
		return;
	}
	const std::string current = identity->current();
	if (!current.empty()) {
		static_cast<void>(updateClient.resolveToken(
			requestId,
			CentrifugoTokenOutcome::Token,
			current));
		return;
	}
	identity->obtain([this, requestId](IdentityObtainResult result) {
		if (!result.token.empty()) {
			static_cast<void>(updateClient.resolveToken(
				requestId,
				CentrifugoTokenOutcome::Token,
				result.token));
			return;
		}
		static_cast<void>(updateClient.resolveToken(
			requestId,
			CentrifugoTokenOutcome::Failed,
			std::string()));
	});
}

} // namespace Impulse
