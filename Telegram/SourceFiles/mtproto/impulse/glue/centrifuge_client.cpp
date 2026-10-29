#include "mtproto/impulse/glue/centrifuge_client.h"

#include "mtproto/impulse/core/endpoints.h"
#include "mtproto/impulse/glue/connection_state_host.h"

#include <memory>
#include <utility>

namespace Impulse {

namespace {

// Client error kinds of the C ABI that mean the socket is being dialled again.
constexpr std::uint32_t kErrorKindTransport = 1;
constexpr std::uint32_t kErrorKindConnect = 3;

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
	config.endpointUrl = std::string(
		updateFleet ? Endpoints::kUpdateUrl : Endpoints::kPoolUrl);
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
	return rust->disconnect();
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
	connectionState = nullptr;
	setState(CentrifugeState::Closed);
}

CentrifugeState CentrifugeClient::currentState() const {
	return state;
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

// Runs on the owner thread, from the poster of CentrifugoRust.
void CentrifugeClient::onEvent(const CentrifugoEvent &event) {
	switch (event.kind) {
	case CentrifugoEventKind::Connected:
		setState(CentrifugeState::Connected);
		break;

	case CentrifugoEventKind::Disconnected:
		// The client will not reconnect by itself: any phase of the
		// connection state machine goes to network loss.
		setState(CentrifugeState::Disconnected);
		if (connectionState) {
			connectionState->onNetworkLost();
		}
		break;

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
		// Subscriptions and their publications are handled by the subscribe
		// path of the facade, not by the connection state.
		break;
	}
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
