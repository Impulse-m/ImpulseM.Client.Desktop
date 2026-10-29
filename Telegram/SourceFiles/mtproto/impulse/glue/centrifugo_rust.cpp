#include "mtproto/impulse/glue/centrifugo_rust.h"

#include "impulse_centrifugo.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Impulse {

static_assert(kCentrifugoStatusOk == IMPULSE_CENTRIFUGO_STATUS_OK);
static_assert(kCentrifugoStatusInvalidArgument == IMPULSE_CENTRIFUGO_STATUS_INVALID_ARGUMENT);
static_assert(kCentrifugoStatusNotInitialized == IMPULSE_CENTRIFUGO_STATUS_NOT_INITIALIZED);
static_assert(kCentrifugoStatusAbiMismatch == IMPULSE_CENTRIFUGO_STATUS_ABI_MISMATCH);
static_assert(kCentrifugoStatusPanic == IMPULSE_CENTRIFUGO_STATUS_PANIC);
static_assert(kCentrifugoStatusClientClosed == IMPULSE_CENTRIFUGO_STATUS_CLIENT_CLOSED);
static_assert(kCentrifugoStatusTransportWriteError == IMPULSE_CENTRIFUGO_STATUS_TRANSPORT_WRITE_ERROR);
static_assert(kCentrifugoStatusConnectionClosed == IMPULSE_CENTRIFUGO_STATUS_CONNECTION_CLOSED);
static_assert(kCentrifugoStatusConfigurationError == IMPULSE_CENTRIFUGO_STATUS_CONFIGURATION_ERROR);
static_assert(kCentrifugoStatusTimeout == IMPULSE_CENTRIFUGO_STATUS_TIMEOUT);
static_assert(kCentrifugoStatusClientDisconnected == IMPULSE_CENTRIFUGO_STATUS_CLIENT_DISCONNECTED);
static_assert(kCentrifugoStatusUnsupported == IMPULSE_CENTRIFUGO_STATUS_UNSUPPORTED);
static_assert(kCentrifugoStatusLimit == IMPULSE_CENTRIFUGO_STATUS_LIMIT);
static_assert(kCentrifugoStatusCancelled == IMPULSE_CENTRIFUGO_STATUS_CANCELLED);
static_assert(kCentrifugoStatusRedirectFailed == IMPULSE_CENTRIFUGO_STATUS_REDIRECT_FAILED);
static_assert(kCentrifugoStatusServerError == IMPULSE_CENTRIFUGO_STATUS_SERVER_ERROR);

static_assert(static_cast<std::int32_t>(CentrifugoEventKind::Connected)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_EVENT_KIND_CONNECTED));
static_assert(static_cast<std::int32_t>(CentrifugoEventKind::Disconnected)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_EVENT_KIND_DISCONNECTED));
static_assert(static_cast<std::int32_t>(CentrifugoEventKind::Publication)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_EVENT_KIND_PUBLICATION));
static_assert(static_cast<std::int32_t>(CentrifugoEventKind::Error)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_EVENT_KIND_ERROR));
static_assert(static_cast<std::int32_t>(CentrifugoEventKind::Subscribing)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_EVENT_KIND_SUBSCRIBING));
static_assert(static_cast<std::int32_t>(CentrifugoEventKind::Subscribed)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_EVENT_KIND_SUBSCRIBED));
static_assert(static_cast<std::int32_t>(CentrifugoEventKind::Unsubscribed)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_EVENT_KIND_UNSUBSCRIBED));
static_assert(static_cast<std::int32_t>(CentrifugoEventKind::SubscriptionError)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_EVENT_KIND_SUBSCRIPTION_ERROR));

static_assert(static_cast<std::int32_t>(CentrifugoTokenKind::Connection)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_TOKEN_KIND_CONNECTION));
static_assert(static_cast<std::int32_t>(CentrifugoTokenKind::Subscription)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_TOKEN_KIND_SUBSCRIPTION));

static_assert(static_cast<std::int32_t>(CentrifugoTokenOutcome::Token)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_TOKEN_OUTCOME_TOKEN));
static_assert(static_cast<std::int32_t>(CentrifugoTokenOutcome::Anonymous)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_TOKEN_OUTCOME_ANONYMOUS));
static_assert(static_cast<std::int32_t>(CentrifugoTokenOutcome::Unauthorized)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_TOKEN_OUTCOME_UNAUTHORIZED));
static_assert(static_cast<std::int32_t>(CentrifugoTokenOutcome::Failed)
	== static_cast<std::int32_t>(IMPULSE_CENTRIFUGO_TOKEN_OUTCOME_FAILED));

// Wrapper state, shared with the Rust side. Every posted task keeps it alive,
// so a task that runs after the wrapper is gone finds alive false and does
// nothing. The callbacks and the subscription map are touched only by the
// thread that runs the posted tasks, which is the thread owning the wrapper.
struct CentrifugoRustState : std::enable_shared_from_this<CentrifugoRustState> {
	CentrifugoRust::Poster poster;
	CentrifugoRust::EventCallback eventCallback;
	CentrifugoRust::TokenCallback tokenCallback;
	std::map<std::string, CentrifugoRust::EventCallback> subscriptionCallbacks;
	impulse_centrifugo_client_t *client = nullptr;
	bool alive = true;
};

namespace {

// Passed as the user pointer of a rpc or publish call. The Rust side frees
// nothing: the trampoline takes ownership back when the completion fires.
struct CompletionContext {
	std::shared_ptr<CentrifugoRustState> state;
	CentrifugoRust::Completion done;
};

std::string copyText(const char *text, std::uintptr_t length) {
	if (!text || length == 0) {
		return std::string();
	}
	return std::string(text, static_cast<std::size_t>(length));
}

std::vector<std::uint8_t> copyBytes(const std::uint8_t *data, std::uintptr_t length) {
	if (!data || length == 0) {
		return std::vector<std::uint8_t>();
	}
	return std::vector<std::uint8_t>(data, data + static_cast<std::size_t>(length));
}

CentrifugoResult copyResult(const impulse_centrifugo_result_t *result) {
	CentrifugoResult copy;
	if (!result) {
		copy.status = kCentrifugoStatusInvalidArgument;
		return copy;
	}
	copy.status = result->status;
	copy.appCode = result->app_code;
	copy.appType = copyText(result->app_type, result->app_type_len);
	copy.message = copyText(result->message, result->message_len);
	copy.data = copyBytes(result->data, result->data_len);
	copy.serverCode = result->server_code;
	return copy;
}

CentrifugoEvent copyEvent(const impulse_centrifugo_event_t *event) {
	CentrifugoEvent copy;
	if (!event) {
		return copy;
	}
	copy.kind = static_cast<CentrifugoEventKind>(static_cast<std::int32_t>(event->kind));
	copy.code = event->code;
	copy.text = copyText(event->text, event->text_len);
	copy.channel = copyText(event->channel, event->channel_len);
	copy.data = copyBytes(event->data, event->data_len);
	copy.offset = event->offset;
	copy.serverCode = event->server_code;
	return copy;
}

CentrifugoTokenRequest copyTokenRequest(const impulse_centrifugo_token_request_t *request) {
	CentrifugoTokenRequest copy;
	if (!request) {
		return copy;
	}
	copy.requestId = request->request_id;
	copy.kind = static_cast<CentrifugoTokenKind>(static_cast<std::int32_t>(request->kind));
	copy.channel = copyText(request->channel, request->channel_len);
	return copy;
}

std::vector<impulse_centrifugo_header_t> toCHeaders(const std::vector<CentrifugoHeader> &headers) {
	std::vector<impulse_centrifugo_header_t> result;
	result.reserve(headers.size());
	for (const CentrifugoHeader &header : headers) {
		impulse_centrifugo_header_t raw{};
		raw.name = header.name.c_str();
		raw.value = header.value.c_str();
		result.push_back(raw);
	}
	return result;
}

std::uint8_t toFlag(bool value) {
	return value ? std::uint8_t(1) : std::uint8_t(0);
}

// Rust thread: copies the outcome, then posts the completion to the owner.
// Callbacks called from the Rust side are noexcept: an exception must not
// unwind through the C ABI frames.
extern "C" void onCompletion(void *user, const impulse_centrifugo_result_t *result) noexcept {
	std::shared_ptr<CompletionContext> context(static_cast<CompletionContext *>(user));
	CentrifugoResult copy = copyResult(result);
	context->state->poster([context, copy = std::move(copy)]() {
		if (context->state->alive && context->done) {
			context->done(copy);
		}
	});
}

// Rust thread: the user pointer is the owner state of the client.
extern "C" void onClientEvent(void *user, const impulse_centrifugo_event_t *event) noexcept {
	std::shared_ptr<CentrifugoRustState> state = static_cast<CentrifugoRustState *>(user)->shared_from_this();
	CentrifugoEvent copy = copyEvent(event);
	state->poster([state, copy = std::move(copy)]() {
		if (!state->alive || !state->eventCallback) {
			return;
		}
		// A copy: the callback may replace itself while it runs.
		const CentrifugoRust::EventCallback callback = state->eventCallback;
		callback(copy);
	});
}

// Rust thread: the user pointer is the owner state; events go to the callback
// registered for their channel.
extern "C" void onSubscriptionEvent(void *user, const impulse_centrifugo_event_t *event) noexcept {
	std::shared_ptr<CentrifugoRustState> state = static_cast<CentrifugoRustState *>(user)->shared_from_this();
	CentrifugoEvent copy = copyEvent(event);
	state->poster([state, copy = std::move(copy)]() {
		if (!state->alive) {
			return;
		}
		std::map<std::string, CentrifugoRust::EventCallback>::iterator found
			= state->subscriptionCallbacks.find(copy.channel);
		if (found == state->subscriptionCallbacks.end() || !found->second) {
			return;
		}
		// A copy: the callback may unsubscribe its own channel while it runs,
		// which erases the entry this iterator points to.
		const CentrifugoRust::EventCallback callback = found->second;
		callback(copy);
	});
}

// Rust thread: the user pointer is the owner state.
extern "C" void onTokenRequest(void *user, const impulse_centrifugo_token_request_t *request) noexcept {
	std::shared_ptr<CentrifugoRustState> state = static_cast<CentrifugoRustState *>(user)->shared_from_this();
	CentrifugoTokenRequest copy = copyTokenRequest(request);
	state->poster([state, copy = std::move(copy)]() {
		if (!state->alive || !state->tokenCallback) {
			return;
		}
		const CentrifugoRust::TokenCallback callback = state->tokenCallback;
		callback(copy);
	});
}

} // namespace

CentrifugoRust::~CentrifugoRust() {
	if (!state) {
		return;
	}
	state->alive = false;
	if (state->client) {
		// Waits for the runtime to stop; no callback fires after this returns.
		impulse_centrifugo_client_destroy(state->client);
		state->client = nullptr;
	}
}

std::int32_t CentrifugoRust::create(const CentrifugoConfig &config, Poster poster) {
	if (impulse_centrifugo_abi_version() != IMPULSE_CENTRIFUGO_ABI_VERSION) {
		return kCentrifugoStatusAbiMismatch;
	}
	if (!poster || ready()) {
		return kCentrifugoStatusInvalidArgument;
	}

	std::vector<impulse_centrifugo_header_t> headers = toCHeaders(config.headers);

	impulse_centrifugo_config_t raw{};
	raw.cfg_size = sizeof(impulse_centrifugo_config_t);
	raw.endpoint_url = config.endpointUrl.c_str();
	raw.websocket_subprotocol = toFlag(config.websocketSubprotocol);
	raw.headers = headers.empty() ? nullptr : headers.data();
	raw.header_count = headers.size();
	raw.instance = config.instance.empty() ? nullptr : config.instance.c_str();
	raw.proxy_url = config.proxyUrl.empty() ? nullptr : config.proxyUrl.c_str();
	raw.timeout_ms = config.timeoutMs;
	raw.min_reconnect_ms = config.minReconnectMs;
	raw.max_reconnect_ms = config.maxReconnectMs;
	raw.max_server_ping_delay_ms = config.maxServerPingDelayMs;
	raw.max_pending_rpc = config.maxPendingRpc;

	std::shared_ptr<CentrifugoRustState> created = std::make_shared<CentrifugoRustState>();
	created->poster = std::move(poster);

	impulse_centrifugo_client_t *client = nullptr;
	const std::int32_t createStatus = impulse_centrifugo_client_create(&raw, &client);
	if (createStatus != kCentrifugoStatusOk) {
		return createStatus;
	}

	const std::int32_t eventStatus = impulse_centrifugo_client_set_event_callback(
		client,
		&onClientEvent,
		created.get());
	if (eventStatus != kCentrifugoStatusOk) {
		impulse_centrifugo_client_destroy(client);
		return eventStatus;
	}

	created->client = client;
	state = std::move(created);
	return kCentrifugoStatusOk;
}

std::int32_t CentrifugoRust::connect() {
	if (!ready()) {
		return kCentrifugoStatusNotInitialized;
	}
	return impulse_centrifugo_client_connect(state->client);
}

std::int32_t CentrifugoRust::disconnect() {
	if (!ready()) {
		return kCentrifugoStatusNotInitialized;
	}
	return impulse_centrifugo_client_disconnect(state->client);
}

std::int32_t CentrifugoRust::reconnectNow() {
	if (!ready()) {
		return kCentrifugoStatusNotInitialized;
	}
	return impulse_centrifugo_client_reconnect_now(state->client);
}

std::int32_t CentrifugoRust::setEventCallback(EventCallback callback) {
	if (!ready()) {
		return kCentrifugoStatusNotInitialized;
	}
	state->eventCallback = std::move(callback);
	return kCentrifugoStatusOk;
}

std::int32_t CentrifugoRust::setTokenCallback(TokenCallback callback) {
	if (!ready()) {
		return kCentrifugoStatusNotInitialized;
	}
	const bool enabled = static_cast<bool>(callback);
	state->tokenCallback = std::move(callback);
	return impulse_centrifugo_client_set_token_callback(
		state->client,
		enabled ? &onTokenRequest : nullptr,
		state.get());
}

std::int32_t CentrifugoRust::resolveToken(
		std::uint64_t requestId,
		CentrifugoTokenOutcome outcome,
		const std::string &token) {
	if (!ready()) {
		return kCentrifugoStatusNotInitialized;
	}
	return impulse_centrifugo_token_resolve(
		state->client,
		requestId,
		static_cast<std::int32_t>(outcome),
		token.empty() ? nullptr : token.c_str());
}

std::int32_t CentrifugoRust::rpc(
		const CentrifugoRpcRequest &request,
		Completion done,
		std::uint64_t *callId) {
	if (!ready()) {
		return kCentrifugoStatusNotInitialized;
	}

	std::vector<impulse_centrifugo_header_t> headers = toCHeaders(request.headers);

	impulse_centrifugo_rpc_request_t raw{};
	raw.method = request.method.c_str();
	raw.data = request.data.data();
	raw.data_len = request.data.size();
	raw.headers = headers.empty() ? nullptr : headers.data();
	raw.header_count = headers.size();
	raw.timeout_ms = request.timeoutMs;
	raw.on_done = &onCompletion;

	CompletionContext *context = new CompletionContext();
	context->state = state;
	context->done = std::move(done);
	raw.user = context;

	const std::int32_t status = impulse_centrifugo_rpc(state->client, &raw, callId);
	if (status != kCentrifugoStatusOk) {
		// A call that did not start never runs its completion.
		delete context;
	}
	return status;
}

std::int32_t CentrifugoRust::cancelRpc(std::uint64_t callId) {
	if (!ready()) {
		return kCentrifugoStatusNotInitialized;
	}
	return impulse_centrifugo_rpc_cancel(state->client, callId);
}

std::int32_t CentrifugoRust::publish(
		const std::string &channel,
		const std::vector<std::uint8_t> &data,
		Completion done) {
	if (!ready()) {
		return kCentrifugoStatusNotInitialized;
	}

	CompletionContext *context = new CompletionContext();
	context->state = state;
	context->done = std::move(done);

	const std::int32_t status = impulse_centrifugo_publish(
		state->client,
		channel.c_str(),
		data.data(),
		data.size(),
		&onCompletion,
		context);
	if (status != kCentrifugoStatusOk) {
		delete context;
	}
	return status;
}

std::int32_t CentrifugoRust::subscribe(
		const CentrifugoSubscribeRequest &request,
		EventCallback onEvent) {
	if (!ready()) {
		return kCentrifugoStatusNotInitialized;
	}
	if (state->subscriptionCallbacks.find(request.channel) != state->subscriptionCallbacks.end()) {
		return kCentrifugoStatusInvalidArgument;
	}

	impulse_centrifugo_subscribe_request_t raw{};
	raw.channel = request.channel.c_str();
	raw.data = request.data.data();
	raw.data_len = request.data.size();
	raw.positioned = toFlag(request.positioned);
	raw.recoverable = toFlag(request.recoverable);
	raw.delta_fossil = toFlag(request.deltaFossil);
	raw.token_callback = toFlag(request.tokenCallback);
	raw.since_epoch = request.sinceEpoch.empty() ? nullptr : request.sinceEpoch.c_str();
	raw.since_offset = request.sinceOffset;
	raw.on_event = &onSubscriptionEvent;
	raw.user = state.get();

	// The callback is registered first: events can arrive as soon as the
	// subscription starts.
	state->subscriptionCallbacks[request.channel] = std::move(onEvent);

	const std::int32_t status = impulse_centrifugo_subscribe(state->client, &raw);
	if (status != kCentrifugoStatusOk) {
		state->subscriptionCallbacks.erase(request.channel);
	}
	return status;
}

std::int32_t CentrifugoRust::unsubscribe(const std::string &channel) {
	if (!ready()) {
		return kCentrifugoStatusNotInitialized;
	}
	const std::int32_t status = impulse_centrifugo_unsubscribe(state->client, channel.c_str());
	state->subscriptionCallbacks.erase(channel);
	return status;
}

bool CentrifugoRust::ready() const {
	return state != nullptr && state->client != nullptr;
}

} // namespace Impulse
