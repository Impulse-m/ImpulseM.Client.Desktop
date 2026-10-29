// C++ wrapper over the Rust Centrifugo client (impulse_centrifugo C ABI).
// Callbacks from the Rust runtime threads copy what they receive and hand it
// to the poster; the user callbacks run through the poster on the thread that
// owns the wrapper. Every other method is called from that thread only.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Impulse {

// Status values returned by every call. Zero is success, positive values are
// client error codes, negative values are ABI errors. A server error has the
// status kCentrifugoStatusServerError and its code in serverCode.
inline constexpr std::int32_t kCentrifugoStatusOk = 0;
inline constexpr std::int32_t kCentrifugoStatusInvalidArgument = -1;
inline constexpr std::int32_t kCentrifugoStatusNotInitialized = -2;
inline constexpr std::int32_t kCentrifugoStatusAbiMismatch = -3;
inline constexpr std::int32_t kCentrifugoStatusPanic = -4;
inline constexpr std::int32_t kCentrifugoStatusClientClosed = -5;
inline constexpr std::int32_t kCentrifugoStatusTransportWriteError = 10;
inline constexpr std::int32_t kCentrifugoStatusConnectionClosed = 11;
inline constexpr std::int32_t kCentrifugoStatusConfigurationError = 12;
inline constexpr std::int32_t kCentrifugoStatusTimeout = 13;
inline constexpr std::int32_t kCentrifugoStatusClientDisconnected = 14;
inline constexpr std::int32_t kCentrifugoStatusUnsupported = 15;
inline constexpr std::int32_t kCentrifugoStatusLimit = 16;
inline constexpr std::int32_t kCentrifugoStatusCancelled = 17;
inline constexpr std::int32_t kCentrifugoStatusRedirectFailed = 18;
inline constexpr std::int32_t kCentrifugoStatusServerError = 19;

enum class CentrifugoEventKind : std::int32_t {
	Connected = 1,
	Disconnected = 2,
	Publication = 3,
	Error = 4,
	Subscribing = 5,
	Subscribed = 6,
	Unsubscribed = 7,
	SubscriptionError = 8,
};

enum class CentrifugoTokenKind : std::int32_t {
	Connection = 1,
	Subscription = 2,
};

enum class CentrifugoTokenOutcome : std::int32_t {
	Token = 1,
	Anonymous = 2,
	Unauthorized = 3,
	Failed = 4,
};

struct CentrifugoHeader {
	std::string name;
	std::string value;
};

// The fields of the create-time configuration. An empty proxyUrl dials
// directly; an empty instance sends none. Zero numeric fields select the
// Rust defaults.
struct CentrifugoConfig {
	std::string endpointUrl;
	bool websocketSubprotocol = false;
	std::vector<CentrifugoHeader> headers;
	std::string instance;
	std::string proxyUrl;
	std::uint32_t timeoutMs = 0;
	std::uint32_t minReconnectMs = 0;
	std::uint32_t maxReconnectMs = 0;
	std::uint32_t maxServerPingDelayMs = 0;
	std::uint32_t maxPendingRpc = 0;
};

struct CentrifugoRpcRequest {
	std::string method;
	std::vector<std::uint8_t> data;
	std::vector<CentrifugoHeader> headers;
	std::uint32_t timeoutMs = 0;
};

// The outcome of a call. A server error has the status
// kCentrifugoStatusServerError, its code in serverCode and its message in
// message. Empty strings and vectors stand for the null pointers of the C ABI.
struct CentrifugoResult {
	std::int32_t status = kCentrifugoStatusOk;
	std::int32_t appCode = 0;
	std::string appType;
	std::string message;
	std::vector<std::uint8_t> data;
	std::uint32_t serverCode = 0;
};

// For Subscribed, code holds the flags: positioned 1, recoverable 2, was
// recovering 4, recovered 8, stream position 16. With the stream position
// flag, offset is the stream offset and text the epoch.
struct CentrifugoEvent {
	CentrifugoEventKind kind = CentrifugoEventKind::Error;
	std::uint32_t code = 0;
	std::string text;
	std::string channel;
	std::vector<std::uint8_t> data;
	std::uint64_t offset = 0;
	std::uint32_t serverCode = 0;
};

struct CentrifugoTokenRequest {
	std::uint64_t requestId = 0;
	CentrifugoTokenKind kind = CentrifugoTokenKind::Connection;
	std::string channel;
};

// An empty sinceEpoch means no recovery position.
struct CentrifugoSubscribeRequest {
	std::string channel;
	std::vector<std::uint8_t> data;
	bool positioned = false;
	bool recoverable = false;
	bool deltaFossil = false;
	bool tokenCallback = false;
	std::string sinceEpoch;
	std::uint64_t sinceOffset = 0;
};

// One endpoint of a pool: the key that calls name, and one URL of that key.
// Repeat the key for more URLs.
struct CentrifugoEndpoint {
	std::string key;
	std::string url;
};

// The fields of the pool configuration. Zero numeric fields select the Rust
// defaults; a zero timeoutMs is 30 s.
struct CentrifugoPoolConfig {
	std::vector<CentrifugoEndpoint> endpoints;
	std::vector<CentrifugoHeader> headers;
	std::uint32_t timeoutMs = 0;
	std::uint32_t minReconnectMs = 0;
	std::uint32_t maxReconnectMs = 0;
	std::uint32_t maxServerPingDelayMs = 0;
	std::uint32_t maxConnectionsPerEndpoint = 0;
	std::uint32_t maxInflightPerConnection = 0;
	std::uint32_t maxQueued = 0;
	std::uint32_t idleTimeoutMs = 0;
	std::uint32_t retries = 0;
	bool tokenFrom = false;
};

struct CentrifugoRustState;
class CentrifugoRustPool;

class CentrifugoRust {
public:
	// Runs a task on the owner thread. It is called from Rust runtime threads,
	// so it must be thread-safe, and it must not run the task inline.
	using Poster = std::function<void(std::function<void()>)>;
	using EventCallback = std::function<void(const CentrifugoEvent &event)>;
	using TokenCallback = std::function<void(const CentrifugoTokenRequest &request)>;
	using Completion = std::function<void(const CentrifugoResult &result)>;

private:
	std::shared_ptr<CentrifugoRustState> state;

public:
	CentrifugoRust() = default;
	CentrifugoRust(const CentrifugoRust &other) = delete;
	CentrifugoRust &operator=(const CentrifugoRust &other) = delete;
	~CentrifugoRust();

	// Checks IMPULSE_CENTRIFUGO_ABI_VERSION against the library, then creates
	// the client. The client does not connect until connect().
	[[nodiscard]] std::int32_t create(const CentrifugoConfig &config, Poster poster);

	[[nodiscard]] std::int32_t connect();
	[[nodiscard]] std::int32_t disconnect();

	// Reconnects without waiting for the reconnect backoff, for when the app
	// resumes. Replaces a socket that has been silent for longer than the ping
	// interval plus the max server ping delay. Does nothing when the caller
	// disconnected the client.
	[[nodiscard]] std::int32_t reconnectNow();

	// Receives the client events (connected, disconnected, publications of
	// server-side subscriptions, errors). Publications and subscription events
	// of subscribe() do not come here.
	[[nodiscard]] std::int32_t setEventCallback(EventCallback callback);

	// Receives the token requests. An empty callback clears it, and the
	// connection then goes on anonymously. Set it before connect().
	[[nodiscard]] std::int32_t setTokenCallback(TokenCallback callback);

	[[nodiscard]] std::int32_t resolveToken(
		std::uint64_t requestId,
		CentrifugoTokenOutcome outcome,
		const std::string &token);

	// The completion runs once with the reply or the failure. When callId is
	// not null it receives the id for cancelRpc(). If the call cannot start,
	// the completion never runs and the status is returned.
	[[nodiscard]] std::int32_t rpc(
		const CentrifugoRpcRequest &request,
		Completion done,
		std::uint64_t *callId = nullptr);

	[[nodiscard]] std::int32_t cancelRpc(std::uint64_t callId);

	// The completion runs once with the outcome; its data is empty.
	[[nodiscard]] std::int32_t publish(
		const std::string &channel,
		const std::vector<std::uint8_t> &data,
		Completion done);

	// Registers a subscription and starts it. Its events go to onEvent, which
	// is matched by channel.
	[[nodiscard]] std::int32_t subscribe(
		const CentrifugoSubscribeRequest &request,
		EventCallback onEvent);

	// Events of the channel stop reaching its callback once this returns.
	[[nodiscard]] std::int32_t unsubscribe(const std::string &channel);

private:
	friend class CentrifugoRustPool;

	[[nodiscard]] bool ready() const;
};

// An rpc pool over a CentrifugoRust client. The pool shares the client's state,
// so its completions run through the client's poster. The client closes every
// pool it still holds when it is destroyed, before it destroys its own handle,
// so no pool outlives its client. Owner thread only.
class CentrifugoRustPool {
public:
	using Completion = CentrifugoRust::Completion;

private:
	std::shared_ptr<CentrifugoRustState> state;

public:
	CentrifugoRustPool() = default;
	CentrifugoRustPool(const CentrifugoRustPool &other) = delete;
	CentrifugoRustPool &operator=(const CentrifugoRustPool &other) = delete;
	~CentrifugoRustPool();

	// Builds the pool over client. Fails when the client is not created or the
	// pool already exists.
	[[nodiscard]] std::int32_t create(
		const CentrifugoRust &client,
		const CentrifugoPoolConfig &config);

	// Starts a call on the endpoint key. The completion runs once with the
	// outcome when the call started. If the call cannot start, the completion
	// never runs and the status is returned. callId, when not null, receives the
	// id for cancelRpc().
	[[nodiscard]] std::int32_t rpc(
		const std::string &key,
		const CentrifugoRpcRequest &request,
		Completion done,
		std::uint64_t *callId = nullptr);

	[[nodiscard]] std::int32_t cancelRpc(std::uint64_t callId);

	// Closes the pool and frees its handle. Calls waiting or in flight fail with
	// their statuses, and their completions run later through the poster. Safe
	// to call more than once.
	void close();

private:
	[[nodiscard]] bool ready() const;
};

} // namespace Impulse
