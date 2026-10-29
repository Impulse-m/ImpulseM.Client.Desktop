// Centrifuge client of one fleet: the update socket, which carries RPC and
// realtime, or the token-less pool socket. A facade over CentrifugoRust that
// fixes the fleet rules (endpoint, subprotocol, token policy), turns the
// client events into a connection state, forwards disconnects to the
// connection state host and carries the channel subscriptions. Owner thread
// only: the poster given to create() must run its tasks on that thread.
#pragma once

#include "mtproto/impulse/glue/centrifugo_rust.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Impulse {

class ConnectionStateHost;

enum class CentrifugeFleet {
	Update,
	Pool,
};

enum class CentrifugeState {
	Disconnected,
	Connecting,
	Connected,
	Closed,
};

enum class CentrifugeSubscriptionKind {
	Subscribing,
	Subscribed,
	Publication,
	Unsubscribed,
	Refused,
};

// One event of a channel subscription. Subscribing and Unsubscribed carry the
// server code and reason in code and text. Subscribed carries the recovery
// flags: recovered is the server flag, wasRecovering is cleared when the
// stored position was dropped (code 2502); text is the stream epoch and
// offset the stream offset. Publication carries data and offset. Refused
// carries the server refusal in code and text; terminal tells whether the
// channel is gone for good.
struct CentrifugeSubscriptionEvent {
	CentrifugeSubscriptionKind kind = CentrifugeSubscriptionKind::Subscribing;
	std::uint32_t code = 0;
	std::string text;
	std::vector<std::uint8_t> data;
	std::uint64_t offset = 0;
	bool recovered = false;
	bool wasRecovering = false;
	bool terminal = false;
};

class CentrifugeClient {
public:
	using StateCallback = std::function<void(CentrifugeState state)>;
	using ErrorCallback = std::function<void(std::uint32_t kind, const std::string &text)>;
	using SubscriptionCallback = std::function<void(const CentrifugeSubscriptionEvent &event)>;

private:
	std::unique_ptr<CentrifugoRust> rust;
	CentrifugeFleet fleet = CentrifugeFleet::Update;
	CentrifugeState state = CentrifugeState::Disconnected;
	StateCallback stateChanged;
	ErrorCallback errorReported;
	ConnectionStateHost *connectionState = nullptr;
	std::map<std::string, SubscriptionCallback> subscriptionCallbacks;
	std::set<std::string> positionDropped;
	bool deliberateDisconnect = false;

public:
	CentrifugeClient() = default;
	CentrifugeClient(const CentrifugeClient &other) = delete;
	CentrifugeClient &operator=(const CentrifugeClient &other) = delete;
	~CentrifugeClient() = default;

	// Sets the endpoint and the subprotocol of the fleet, then creates the
	// client. The update fleet uses the centrifuge-protobuf subprotocol; the
	// pool fleet does not. The client does not connect until connect().
	// connection may be null; when set, the owner keeps it alive.
	[[nodiscard]] std::int32_t create(
		CentrifugeFleet fleetKind,
		CentrifugoConfig config,
		CentrifugoRust::Poster poster,
		ConnectionStateHost *connection);

	// Starts connecting from the disconnected state. The state moves to
	// Connecting now and to Connected on the connected event.
	[[nodiscard]] std::int32_t connect();

	// The disconnect the caller asks for is not network loss: the owner is not
	// told about the Disconnected event that follows it.
	[[nodiscard]] std::int32_t disconnect();

	// Reconnects without the backoff wait; see CentrifugoRust::reconnectNow.
	[[nodiscard]] std::int32_t reconnectNow();

	// Closes the client and waits for its runtime to stop, after which no
	// callback runs. The state moves to Closed and the client cannot be used
	// again.
	void close();

	[[nodiscard]] CentrifugeState currentState() const;

	// The Rust client behind this facade, null before create(). It is owned by
	// the facade; the media pool of the fleet is built on it.
	[[nodiscard]] CentrifugoRust *rustClient() const;

	void setStateCallback(StateCallback callback);
	void setErrorCallback(ErrorCallback callback);

	// The pool fleet is token-less: a token callback is refused there.
	[[nodiscard]] std::int32_t setTokenCallback(CentrifugoRust::TokenCallback callback);

	[[nodiscard]] std::int32_t resolveToken(
		std::uint64_t requestId,
		CentrifugoTokenOutcome outcome,
		const std::string &token);

	// Passes through to CentrifugoRust; see there for the completion rules.
	[[nodiscard]] std::int32_t rpc(
		const CentrifugoRpcRequest &request,
		CentrifugoRust::Completion done,
		std::uint64_t *callId = nullptr);

	[[nodiscard]] std::int32_t cancelRpc(std::uint64_t callId);

	// Subscribes to a channel with positioned and recoverable set and no
	// subscription token. The callback gets the events of the channel until
	// unsubscribe(), a terminal Unsubscribed or a terminal Refused event. The
	// pool fleet has no subscriptions and refuses the call.
	[[nodiscard]] std::int32_t subscribe(
		const std::string &channel,
		SubscriptionCallback onEvent);

	[[nodiscard]] std::int32_t unsubscribe(const std::string &channel);

	// Publishes into a channel through the update fleet. The completion runs
	// once with the outcome. The pool fleet refuses the call.
	[[nodiscard]] std::int32_t publish(
		const std::string &channel,
		const std::vector<std::uint8_t> &data,
		CentrifugoRust::Completion done);

private:
	void onEvent(const CentrifugoEvent &event);
	void onSubscriptionEvent(const std::string &channel, const CentrifugoEvent &event);
	void setState(CentrifugeState next);
};

} // namespace Impulse
