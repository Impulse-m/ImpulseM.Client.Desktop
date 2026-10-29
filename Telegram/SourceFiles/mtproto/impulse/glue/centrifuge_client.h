// Centrifuge client of one fleet: the update socket, which carries RPC and
// realtime, or the token-less pool socket. A facade over CentrifugoRust that
// fixes the fleet rules (endpoint, subprotocol, token policy), turns the
// client events into a connection state and forwards disconnects to the
// connection state host. Owner thread only: the poster given to create() must
// run its tasks on that thread.
#pragma once

#include "mtproto/impulse/glue/centrifugo_rust.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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

class CentrifugeClient {
public:
	using StateCallback = std::function<void(CentrifugeState state)>;
	using ErrorCallback = std::function<void(std::uint32_t kind, const std::string &text)>;

private:
	std::unique_ptr<CentrifugoRust> rust;
	CentrifugeFleet fleet = CentrifugeFleet::Update;
	CentrifugeState state = CentrifugeState::Disconnected;
	StateCallback stateChanged;
	ErrorCallback errorReported;
	ConnectionStateHost *connectionState = nullptr;

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
	[[nodiscard]] std::int32_t disconnect();

	// Reconnects without the backoff wait; see CentrifugoRust::reconnectNow.
	[[nodiscard]] std::int32_t reconnectNow();

	// Closes the client and waits for its runtime to stop, after which no
	// callback runs. The state moves to Closed and the client cannot be used
	// again.
	void close();

	[[nodiscard]] CentrifugeState currentState() const;

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

private:
	void onEvent(const CentrifugoEvent &event);
	void setState(CentrifugeState next);
};

} // namespace Impulse
