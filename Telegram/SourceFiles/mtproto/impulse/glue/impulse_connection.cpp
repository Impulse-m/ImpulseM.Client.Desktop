#include "mtproto/impulse/glue/impulse_connection.h"

#include "base/call_delayed.h"
#include "config.h"
#include "core/application.h"
#include "logs.h"
#include "mtproto/impulse/core/hash.h"
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

// The digits of the percent-encoding of the userinfo part of a URL.
constexpr std::string_view kPercentHexDigits = "0123456789ABCDEF";

// Percent-encodes a proxy credential for the userinfo part of a URL (RFC 3986).
// Only the unreserved characters stay as they are.
[[nodiscard]] std::string PercentEncodeUserinfo(const std::string &value) {
	std::string result;
	result.reserve(value.size());
	for (const char symbol : value) {
		const unsigned char code = static_cast<unsigned char>(symbol);
		const bool unreserved = (code >= 'A' && code <= 'Z')
			|| (code >= 'a' && code <= 'z')
			|| (code >= '0' && code <= '9')
			|| code == '-'
			|| code == '.'
			|| code == '_'
			|| code == '~';
		if (unreserved) {
			result.push_back(symbol);
		} else {
			result.push_back('%');
			result.push_back(kPercentHexDigits[static_cast<std::size_t>(code >> 4)]);
			result.push_back(kPercentHexDigits[static_cast<std::size_t>(code & 0x0F)]);
		}
	}
	return result;
}

// The endpoint of the proxy as the Rust client takes it: http://host:port
// for HTTP CONNECT, with the percent-encoded credentials in the userinfo part
// when the endpoint has them. Empty when the traffic goes direct.
[[nodiscard]] std::string ProxyUrlOf(
		const std::optional<ProxyRouting::HttpEndpoint> &endpoint) {
	if (!endpoint) {
		return std::string();
	}
	std::string url = "http://";
	if (endpoint->hasCredentials) {
		url += PercentEncodeUserinfo(endpoint->user);
		url += ":";
		url += PercentEncodeUserinfo(endpoint->password);
		url += "@";
	}
	url += endpoint->host;
	url += ":";
	url += std::to_string(endpoint->port);
	return url;
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

// The serial of each connection instance of the process, for the diagnostic
// lines. The first instance is 1.
std::atomic<std::uint64_t> connectionSerialCounter{ 0 };

// The constructors that the diagnostic lines look for in a realtime frame.
constexpr std::uint32_t kUpdateShortConstructor = 0x78d4dec1;
constexpr std::uint32_t kUpdatesContainerConstructor = 0x74ae4240;
constexpr std::uint32_t kVectorConstructor = 0x1cb5c415;
constexpr std::uint32_t kUpdateLoginTokenConstructor = 0x564fe691;

// The word at the offset of TL bytes, little-endian. Zero when the bytes end
// before the word.
[[nodiscard]] std::uint32_t ReadWordAt(
		const std::vector<std::uint8_t> &bytes,
		std::size_t offset) {
	if (bytes.size() < offset + 4) {
		return 0;
	}
	return static_cast<std::uint32_t>(bytes[offset])
		| (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
		| (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
		| (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

// True when the first update of a realtime frame is updateLoginToken. The
// frame is an updateShort, or an updates container whose vector starts with
// that update. Only the first update is looked at.
[[nodiscard]] bool StartsWithLoginTokenUpdate(const std::vector<std::uint8_t> &updates) {
	const std::uint32_t constructor = ReadWordAt(updates, 0);
	if (constructor == kUpdateShortConstructor) {
		return ReadWordAt(updates, 4) == kUpdateLoginTokenConstructor;
	}
	if (constructor == kUpdatesContainerConstructor) {
		return ReadWordAt(updates, 4) == kVectorConstructor
			&& ReadWordAt(updates, 12) == kUpdateLoginTokenConstructor;
	}
	return false;
}

[[nodiscard]] QString YesNo(bool value) {
	return QString::fromLatin1(value ? "yes" : "no");
}

[[nodiscard]] QString OutcomeKindName(CarrierOutcomeKind kind) {
	switch (kind) {
	case CarrierOutcomeKind::Reply:
		return QString::fromLatin1("reply");
	case CarrierOutcomeKind::RpcError:
		return QString::fromLatin1("rpc_error");
	case CarrierOutcomeKind::NetworkFailure:
		return QString::fromLatin1("network_failure");
	}
	return QString::fromLatin1("unknown");
}

// The name of the socket or the fleet that carries a call of the kind.
[[nodiscard]] QString SocketNameOf(CarrierKind carrier) {
	switch (carrier) {
	case CarrierKind::Update:
		return QString::fromLatin1("update socket");
	case CarrierKind::Pool:
		return QString::fromLatin1("pool socket");
	case CarrierKind::Media:
		return QString::fromLatin1("media fleet");
	}
	return QString::fromLatin1("unknown");
}

// The carrier of a call of the method as its route has it now. The update
// socket is the identity socket while signed out.
[[nodiscard]] QString CarrierNameOf(const std::string &method, bool signedIn) {
	RouteRequest route;
	route.method = method;
	route.signedIn = signedIn;
	const RouteDecision decision = routeMethod(route);
	if (decision.refused) {
		return QString::fromLatin1("refused locally");
	}
	if (decision.carrier == CarrierKind::Update && !signedIn) {
		return QString::fromLatin1("identity socket");
	}
	return SocketNameOf(decision.carrier);
}

// The text of an error, or none when the error has no text.
[[nodiscard]] QString TextOrNone(const std::string &text) {
	return text.empty()
		? QString::fromLatin1("none")
		: QString::fromStdString(text);
}

// A short tag of an identity token for the diagnostic lines: the first four
// bytes of the SHA-256 of its subject, in hex, and its orig_iat. The token and
// the subject itself are never written.
[[nodiscard]] QString IdentityTagOf(const std::string &identityToken) {
	if (identityToken.empty()) {
		return QString::fromLatin1("none");
	}
	const std::optional<TokenClaims> claims = readTokenClaims(identityToken);
	if (!claims) {
		return QString::fromLatin1("unreadable");
	}
	const Sha256Digest digest = Sha256(claims->sub);
	QString hex;
	for (std::size_t index = 0; index < 4; ++index) {
		hex += QString::number(digest[index] >> 4, 16);
		hex += QString::number(digest[index] & 0x0F, 16);
	}
	return QString::fromLatin1("%1 orig_iat %2").arg(hex).arg(
		QString::number(claims->origIat));
}

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
	serial = connectionSerialCounter.fetch_add(1) + 1;

	// The session pair goes to the account blob, the identity token to its own
	// file. Loading them does not dial.
	sessionStore = std::make_unique<AccountSessionStore>(
		std::move(storage.loadSession),
		std::move(storage.saveSession),
		std::move(storage.clearSession));
	identityStore = std::make_unique<DpapiTokenStore>(storage.identityPath);
	declarationSource = std::move(storage.declarationSource);

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
		noteLoginReady();
		declarer->credentialChanged();
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

	// The declarer sends initConnection over the update socket, once per
	// credential and declaration. Its sends go through send() like any call.
	declarer = std::make_unique<ConnectionDeclarer>(
		[this](
				const ConnectionDeclaration &declaration,
				DeclarationSendDone done) {
			RpcCarrierRequest request;
			request.route.method = kInitConnectionMethod;
			request.data = EncodeConnectionDeclaration(declaration);
			send(
				NextConnectionRequestId(),
				std::move(request),
				[done = std::move(done)](RpcCarrierReply reply) {
					DeclarationReply answer;
					answer.acknowledged
						= (reply.outcome.kind == CarrierOutcomeKind::Reply);
					done(answer);
				});
		},
		MakeScheduler(base::make_weak(this)),
		[this]() -> std::string {
			return currentCredential();
		});

	DEBUG_LOG(("Impulse Info: connection %1 created, session %2, account user %3."
		).arg(QString::number(serial)
		).arg(YesNo(tokens->hasSession())
		).arg(YesNo(storage.userId != 0)));

	buildTransport(ReadRoute());
}

ImpulseConnection::~ImpulseConnection() {
	DEBUG_LOG(("Impulse Info: connection %1 destroyed, session %2, user %3."
		).arg(QString::number(serial)
		).arg(YesNo(tokens && tokens->hasSession())
		).arg(YesNo(userId != 0)));

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
	// The declaration is read before the sockets dial, so the first declare
	// after the connect carries the app values.
	if (declarationSource) {
		declarer->setDeclaration(ClampConnectionDeclaration(declarationSource()));
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
	if (resetKeys) {
		// A sign-out ends the connection identity too: it is forgotten in
		// memory and its file is removed.
		identity->discard();
		refreshIdentityLane();
	}
	declarer->credentialChanged();
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
	pending->method = method;
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
	return !rekeying && (userId != 0) && tokens->hasSession();
}

void ImpulseConnection::noteLoginReady() {
	const bool ready = loginReady();
	if (ready && !loginReadyNoted) {
		DEBUG_LOG(("Impulse Info: connection %1 is login ready, user %2."
			).arg(QString::number(serial)
			).arg(QString::number(userId)));
	}
	loginReadyNoted = ready;
}

void ImpulseConnection::bindUser(std::int64_t userIdValue) {
	userId = userIdValue;
	laneIdentity.userId = userIdValue;
	if (lanes) {
		lanes->setIdentity(laneIdentity);
	}
	noteLoginReady();
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

	const RouteDecision decision = routeMethod(request.route);
	if (decision.refused) {
		DEBUG_LOG(("Impulse Info: connection %1 refused %2 locally as %3, signed in %4."
			).arg(QString::number(serial)
			).arg(QString::fromStdString(request.route.method)
			).arg(QString::fromStdString(decision.refusalType)
			).arg(YesNo(request.route.signedIn)));
	} else if (!request.route.signedIn
		&& isIdentitySocketMethod(request.route.method)
		&& !isPoolOnlyMethod(request.route.method)) {
		DEBUG_LOG(("Impulse Info: connection %1 sends %2 on the identity socket, identity %3."
			).arg(QString::number(serial)
			).arg(QString::fromStdString(request.route.method)
			).arg(IdentityTagOf(identity->current())));
	}

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
	if (pending->capturesLogin) {
		DEBUG_LOG(("Impulse Info: connection %1 settled login call %2 as %3."
			).arg(QString::number(serial)
			).arg(QString::fromStdString(pending->method)
			).arg(OutcomeKindName(reply.outcome.kind)));
	}
	if (reply.outcome.kind != CarrierOutcomeKind::Reply) {
		DEBUG_LOG(("Impulse Info: connection %1 request %2 %3 settled as %4, code %5, text %6, carrier %7, cancelled %8."
			).arg(QString::number(serial)
			).arg(QString::number(requestId)
			).arg(QString::fromStdString(pending->method)
			).arg(OutcomeKindName(reply.outcome.kind)
			).arg(QString::number(reply.outcome.error.code)
			).arg(TextOrNone(reply.outcome.error.text)
			).arg(CarrierNameOf(pending->method, loginReady())
			).arg(YesNo(pending->cancelled)));
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
	// A failed socket call is logged here, since it never reaches the settle
	// path. The serial is copied: a socket callback can run after this
	// connection is gone.
	const std::uint64_t connectionSerial = serial;
	const QString socketName = SocketNameOf(carrier);
	SocketCallDone settled = [connectionSerial, socketName, method, done = std::move(done)](
			CarrierOutcome outcome,
			std::vector<std::uint8_t> data) {
		if (outcome.kind != CarrierOutcomeKind::Reply) {
			DEBUG_LOG(("Impulse Info: connection %1 socket call %2 on %3 settled as %4, code %5, text %6."
				).arg(QString::number(connectionSerial)
				).arg(QString::fromStdString(method)
				).arg(socketName
				).arg(OutcomeKindName(outcome.kind)
				).arg(QString::number(outcome.error.code)
				).arg(TextOrNone(outcome.error.text)));
		}
		done(std::move(outcome), std::move(data));
	};

	if (!socket) {
		// No socket is built for a blocked route: the call fails as a network
		// failure, and nothing dials.
		CarrierOutcome outcome;
		outcome.kind = CarrierOutcomeKind::NetworkFailure;
		settled(std::move(outcome), {});
		return;
	}

	CentrifugoRpcRequest request;
	request.method = method;
	request.data = std::move(payload);
	request.timeoutMs = static_cast<std::uint32_t>(timeoutMs);

	const std::int32_t status = socket->rpc(
		request,
		[carrier, settled](const CentrifugoResult &result) {
			CarrierOutcome outcome = mapCarrierResult(result, carrier);
			std::vector<std::uint8_t> data;
			if (outcome.kind == CarrierOutcomeKind::Reply) {
				data = result.data;
			}
			settled(std::move(outcome), std::move(data));
		});
	if (status != kCentrifugoStatusOk) {
		// The completion does not run for a call that could not start.
		CentrifugoResult failed;
		failed.status = status;
		settled(mapCarrierResult(failed, carrier), {});
	}
}

bool ImpulseConnection::tokenAnswerStale(
		std::uint64_t requestId,
		std::uint64_t generation) const {
	if (generation == transportGeneration) {
		return false;
	}
	DEBUG_LOG(("Impulse Info: connection %1 ignored the token answer %2 of an earlier transport %3."
		).arg(QString::number(serial)
		).arg(QString::number(requestId)
		).arg(QString::number(generation)));
	return true;
}

void ImpulseConnection::answerConnectionToken(
		std::uint64_t requestId,
		std::uint64_t generation) {
	// The declaration follows the credential the socket now presents. It is
	// triggered after the token is answered, so the socket connects with it.
	if (tokens->hasSession()) {
		tokens->connectionToken([this, requestId, generation](ConnectionTokenResult result) {
			// The session token was asked for by a socket that a rebuild has since
			// replaced: its answer must not reach the sockets of the new transport.
			if (tokenAnswerStale(requestId, generation)) {
				return;
			}
			if (result.status == RefreshStatus::Fresh && !result.token.empty()) {
				resolveUpdateToken(
					requestId,
					CentrifugoTokenOutcome::Token,
					result.token);
				declarer->credentialChanged();
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
		declarer->credentialChanged();
		return;
	}
	identity->obtain([this, requestId, generation](IdentityObtainResult result) {
		if (!result.token.empty()) {
			// The identity lane of the minted identity is subscribed from now,
			// also when the socket that asked was replaced by a rebuild.
			refreshIdentityLane();
		}
		// The identity was minted for a socket that a rebuild has since replaced.
		if (tokenAnswerStale(requestId, generation)) {
			return;
		}
		if (!result.token.empty()) {
			resolveUpdateToken(requestId, CentrifugoTokenOutcome::Token, result.token);
			declarer->credentialChanged();
			return;
		}
		resolveUpdateToken(requestId, CentrifugoTokenOutcome::Failed, std::string());
	});
}

std::string ImpulseConnection::currentCredential() {
	// While the sockets are rebuilt for a new session, the old ones still carry
	// the identity: nothing is declared until the rebuilt sockets present it.
	if (rekeying) {
		return std::string();
	}
	if (tokens->hasSession()) {
		return tokens->bearer();
	}
	return identity->current();
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
	DEBUG_LOG(("Impulse Info: connection %1 adopts a login of user %2."
		).arg(QString::number(serial)
		).arg(QString::number(pair.userId)));

	// Rekeying starts before the pair is stored: the session change runs inside
	// onLoginTokens and must not release the waiting calls on the old sockets.
	rekeying = true;

	// The pair ends the connection identity: the session change discards it
	// and retires the media pool.
	tokens->onLoginTokens(pair);
	refreshIdentityLane();

	// The sockets are keyed to the identity token, and a reconnect of a live
	// socket keeps that token. Every socket is built again instead, so each new
	// client has no cached token and the update socket asks for the session
	// token. The rebuild destroys the carrier that delivers this reply, so it
	// runs once the reply is settled. Until then no login call is sent.
	bindUser(pair.userId);
	crl::on_main(this, [=] {
		if (started) {
			connectionState.onNetworkLost();
		}
		rebuildTransport(currentRoute);
		if (started) {
			connectSockets();
		}
		DEBUG_LOG(("Impulse Info: connection %1 rebuilt the transport for the session of user %2."
			).arg(QString::number(serial)
			).arg(QString::number(userId)));
		rekeying = false;

		// The calls that wait for the login are released once the sockets are
		// keyed. The account lanes already follow the user.
		noteLoginReady();
		releaseLoginWaiters();

		// The rebuilt sockets are declared again with the session token.
		declarer->credentialChanged();
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
		// The request ids of a client restart from 1, so an answer is resolved only
		// on the client that asked: the generation of this client is captured.
		const std::uint64_t generation = transportGeneration;
		static_cast<void>(updateClient->setTokenCallback(
			[this, generation](const CentrifugoTokenRequest &request) {
				if (tokenAnswerStale(request.requestId, generation)) {
					return;
				}
				if (request.kind != CentrifugoTokenKind::Connection) {
					// Channel tokens are not used: subscriptions carry none.
					resolveUpdateToken(
						request.requestId,
						CentrifugoTokenOutcome::Anonymous,
						std::string());
					return;
				}
				answerConnectionToken(request.requestId, generation);
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

	// A call that asks for the declaration waits for the declarer. The
	// carrier's own declaration wait bounds the wait.
	rpcCarrier->setDeclarationGate([this](std::function<void()> declared) {
		declarer->awaitDeclared(std::move(declared));
	});
}

void ImpulseConnection::dropTransport() {
	// Every token request of the clients dropped here is stale from now on. The
	// answers are ignored, so they never resolve on the clients of the next build.
	++transportGeneration;

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
	if (StartsWithLoginTokenUpdate(updates)) {
		DEBUG_LOG(("Impulse Info: connection %1 received updateLoginToken on the update socket."
			).arg(QString::number(serial)));
	}
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
