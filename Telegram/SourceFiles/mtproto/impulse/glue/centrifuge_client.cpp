#include "mtproto/impulse/glue/centrifuge_client.h"

#include "mtproto/impulse/core/build_config.h"
#include "mtproto/impulse/core/endpoints.h"
#include "mtproto/impulse/glue/connection_state_host.h"

#include <memory>
#include <string_view>
#include <utility>

namespace Impulse {

namespace {

// Client error kinds of the C ABI that mean the socket is being dialled again.
constexpr std::uint32_t kErrorKindTransport = 1;
constexpr std::uint32_t kErrorKindConnect = 3;

// Subscription error kind of the C ABI for a refused subscribe.
constexpr std::uint32_t kSubscriptionErrorKindSubscribe = 1;

// Subscribed flags of the C ABI: the server replayed the missed publications
// on this subscribe and the subscribe asked for recovery.
constexpr std::uint32_t kSubscribedFlagWasRecovering = 4;
constexpr std::uint32_t kSubscribedFlagRecovered = 8;

// State invalidated: the stored position is dropped, so the next subscribe is
// not a recovery.
constexpr std::uint32_t kPositionDroppedCode = 2502;

// Subscribe refusals: 4403 is permanent unless it carries a budget
// FLOOD_WAIT_<s> text; 4500 is temporary.
constexpr std::uint32_t kRefusedPermanentCode = 4403;
constexpr std::string_view kFloodWaitPrefix = "FLOOD_WAIT_";

} // namespace

std::int32_t CentrifugeClient::create(
		CentrifugeFleet fleetKind,
		CentrifugoConfig config,
		CentrifugoRust::Poster poster,
		ConnectionStateHost *connection) {
	if (rust) {
		return kCentrifugoStatusInvalidArgument;
	}

	const bool updateFleet = (fleetKind == CentrifugeFleet::Update);
	const std::string_view rpcBase = BuildConfig::RpcUrl();
	config.endpointUrl = updateFleet
		? Endpoints::UpdateUrl(rpcBase)
		: Endpoints::PoolUrl(rpcBase);
	if (config.endpointUrl.empty()) {
		// No transport host is configured: the socket is never created.
		return kCentrifugoStatusConfigurationError;
	}
	config.websocketSubprotocol = updateFleet;

	std::unique_ptr<CentrifugoRust> client = std::make_unique<CentrifugoRust>();
	const std::int32_t createStatus = client->create(config, std::move(poster));
	if (createStatus != kCentrifugoStatusOk) {
		return createStatus;
	}

	// The callback captures this object, which is not movable: the client is
	// created here and used only through this facade.
	const std::int32_t eventStatus = client->setEventCallback(
		[this](const CentrifugoEvent &event) {
			onEvent(event);
		});
	if (eventStatus != kCentrifugoStatusOk) {
		return eventStatus;
	}

	fleet = fleetKind;
	connectionState = connection;
	rust = std::move(client);
	return kCentrifugoStatusOk;
}

std::int32_t CentrifugeClient::connect() {
	if (!rust) {
		return kCentrifugoStatusNotInitialized;
	}
	deliberateDisconnect = false;
	const std::int32_t status = rust->connect();
	if (status == kCentrifugoStatusOk && state == CentrifugeState::Disconnected) {
		setState(CentrifugeState::Connecting);
	}
	return status;
}

std::int32_t CentrifugeClient::disconnect() {
	if (!rust) {
		return kCentrifugoStatusNotInitialized;
	}
	const std::int32_t status = rust->disconnect();
	if (status == kCentrifugoStatusOk) {
		deliberateDisconnect = true;
	}
	return status;
}

std::int32_t CentrifugeClient::reconnectNow() {
	if (!rust) {
		return kCentrifugoStatusNotInitialized;
	}
	return rust->reconnectNow();
}

void CentrifugeClient::close() {
	if (!rust) {
		return;
	}
	// Waits for the runtime to stop. Posted tasks that run later see the
	// wrapper as closed and do nothing.
	rust.reset();
	subscriptionCallbacks.clear();
	positionDropped.clear();
	connectionState = nullptr;
	setState(CentrifugeState::Closed);
}

CentrifugeState CentrifugeClient::currentState() const {
	return state;
}

CentrifugoRust *CentrifugeClient::rustClient() const {
	return rust.get();
}

void CentrifugeClient::setStateCallback(StateCallback callback) {
	stateChanged = std::move(callback);
}

void CentrifugeClient::setErrorCallback(ErrorCallback callback) {
	errorReported = std::move(callback);
}

std::int32_t CentrifugeClient::setTokenCallback(CentrifugoRust::TokenCallback callback) {
	if (!rust) {
		return kCentrifugoStatusNotInitialized;
	}
	if (fleet == CentrifugeFleet::Pool && callback) {
		return kCentrifugoStatusInvalidArgument;
	}
	return rust->setTokenCallback(std::move(callback));
}

std::int32_t CentrifugeClient::resolveToken(
		std::uint64_t requestId,
		CentrifugoTokenOutcome outcome,
		const std::string &token) {
	if (!rust) {
		return kCentrifugoStatusNotInitialized;
	}
	return rust->resolveToken(requestId, outcome, token);
}

std::int32_t CentrifugeClient::rpc(
		const CentrifugoRpcRequest &request,
		CentrifugoRust::Completion done,
		std::uint64_t *callId) {
	if (!rust) {
		return kCentrifugoStatusNotInitialized;
	}
	return rust->rpc(request, std::move(done), callId);
}

std::int32_t CentrifugeClient::cancelRpc(std::uint64_t callId) {
	if (!rust) {
		return kCentrifugoStatusNotInitialized;
	}
	return rust->cancelRpc(callId);
}

std::int32_t CentrifugeClient::subscribe(
		const std::string &channel,
		SubscriptionCallback onEvent) {
	if (!rust) {
		return kCentrifugoStatusNotInitialized;
	}
	if (fleet == CentrifugeFleet::Pool) {
		return kCentrifugoStatusInvalidArgument;
	}
	if (subscriptionCallbacks.find(channel) != subscriptionCallbacks.end()) {
		return kCentrifugoStatusInvalidArgument;
	}

	CentrifugoSubscribeRequest request;
	request.channel = channel;
	request.positioned = true;
	request.recoverable = true;
	request.tokenCallback = false;

	subscriptionCallbacks.emplace(channel, std::move(onEvent));
	const std::int32_t status = rust->subscribe(
		request,
		[this, channel](const CentrifugoEvent &event) {
			onSubscriptionEvent(channel, event);
		});
	if (status != kCentrifugoStatusOk) {
		subscriptionCallbacks.erase(channel);
	}
	return status;
}

std::int32_t CentrifugeClient::unsubscribe(const std::string &channel) {
	if (!rust) {
		return kCentrifugoStatusNotInitialized;
	}
	subscriptionCallbacks.erase(channel);
	positionDropped.erase(channel);
	return rust->unsubscribe(channel);
}

std::int32_t CentrifugeClient::publish(
		const std::string &channel,
		const std::vector<std::uint8_t> &data,
		CentrifugoRust::Completion done) {
	if (!rust) {
		return kCentrifugoStatusNotInitialized;
	}
	if (fleet == CentrifugeFleet::Pool) {
		return kCentrifugoStatusInvalidArgument;
	}
	return rust->publish(channel, data, std::move(done));
}

// Runs on the owner thread, from the poster of CentrifugoRust.
void CentrifugeClient::onEvent(const CentrifugoEvent &event) {
	switch (event.kind) {
	case CentrifugoEventKind::Connected:
		setState(CentrifugeState::Connected);
		break;

	case CentrifugoEventKind::Disconnected: {
		// Only terminal disconnects reach this event: the client redials by
		// itself after the reconnect codes, 4001 session revoked and 4002
		// membership escalation among them. A terminal close of the 4500 range
		// goes to network loss: any phase of the connection state machine goes
		// to Disconnected. The close asked for by disconnect() does not.
		const bool requested = deliberateDisconnect;
		deliberateDisconnect = false;
		setState(CentrifugeState::Disconnected);
		if (connectionState && !requested) {
			connectionState->onNetworkLost();
		}
		break;
	}

	case CentrifugoEventKind::Error:
		// A transport or connect error while connected means the client
		// dials again on its own; it reports Connecting until it connects.
		if (state == CentrifugeState::Connected
			&& (event.code == kErrorKindTransport || event.code == kErrorKindConnect)) {
			setState(CentrifugeState::Connecting);
		}
		if (errorReported) {
			const ErrorCallback reported = errorReported;
			reported(event.code, event.text);
		}
		break;

	case CentrifugoEventKind::Publication:
	case CentrifugoEventKind::Subscribing:
	case CentrifugoEventKind::Subscribed:
	case CentrifugoEventKind::Unsubscribed:
	case CentrifugoEventKind::SubscriptionError:
		// Subscriptions and their publications arrive on their own callbacks
		// (see subscribe()), not on the client callback, and do not change the
		// connection state.
		break;
	}
}

// Runs on the owner thread, from the subscription callback of CentrifugoRust.
void CentrifugeClient::onSubscriptionEvent(
		const std::string &channel,
		const CentrifugoEvent &event) {
	const std::map<std::string, SubscriptionCallback>::iterator found
		= subscriptionCallbacks.find(channel);
	if (found == subscriptionCallbacks.end()) {
		return;
	}
	// A copy: the callback may unsubscribe or resubscribe its own channel.
	const SubscriptionCallback callback = found->second;

	CentrifugeSubscriptionEvent translated;
	switch (event.kind) {
	case CentrifugoEventKind::Subscribing:
		if (event.code == kPositionDroppedCode) {
			positionDropped.insert(channel);
		}
		translated.kind = CentrifugeSubscriptionKind::Subscribing;
		translated.code = event.code;
		translated.text = event.text;
		break;

	case CentrifugoEventKind::Subscribed:
		translated.kind = CentrifugeSubscriptionKind::Subscribed;
		translated.recovered = (event.code & kSubscribedFlagRecovered) != 0;
		translated.wasRecovering = (event.code & kSubscribedFlagWasRecovering) != 0
			&& positionDropped.find(channel) == positionDropped.end();
		translated.text = event.text;
		translated.offset = event.offset;
		positionDropped.erase(channel);
		break;

	case CentrifugoEventKind::Publication:
		translated.kind = CentrifugeSubscriptionKind::Publication;
		translated.data = event.data;
		translated.offset = event.offset;
		break;

	case CentrifugoEventKind::Unsubscribed:
		// The Rust client resubscribes by itself on the codes from 2500 up
		// (reported as Subscribing), so an Unsubscribed event is always
		// terminal for the channel.
		translated.kind = CentrifugeSubscriptionKind::Unsubscribed;
		translated.code = event.code;
		translated.text = event.text;
		translated.terminal = true;
		break;

	case CentrifugoEventKind::SubscriptionError: {
		if (event.code != kSubscriptionErrorKindSubscribe || event.serverCode == 0) {
			// Token and state errors of the subscription are not refusals;
			// the Rust client reports and retries them.
			return;
		}
		const bool floodWait = std::string_view(event.text).starts_with(kFloodWaitPrefix);
		translated.kind = CentrifugeSubscriptionKind::Refused;
		translated.code = event.serverCode;
		translated.text = event.text;
		translated.terminal = (event.serverCode == kRefusedPermanentCode && !floodWait);
		break;
	}

	case CentrifugoEventKind::Connected:
	case CentrifugoEventKind::Disconnected:
	case CentrifugoEventKind::Error:
		return;
	}

	if (translated.terminal) {
		// The channel is gone for good: the client forgets it on both sides
		// before the callback runs.
		subscriptionCallbacks.erase(channel);
		positionDropped.erase(channel);
		if (rust) {
			// The status does not matter here: the channel is forgotten anyway.
			static_cast<void>(rust->unsubscribe(channel));
		}
	}
	callback(translated);
}

void CentrifugeClient::setState(CentrifugeState next) {
	if (state == next) {
		return;
	}
	state = next;
	if (stateChanged) {
		// A copy: the callback may replace itself while it runs.
		const StateCallback callback = stateChanged;
		callback(state);
	}
}

} // namespace Impulse
