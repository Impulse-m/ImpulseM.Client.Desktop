#include "mtproto/impulse/glue/impulse_session_private.h"

#include "base/call_delayed.h"
#include "base/invoke_queued.h"
#include "base/unixtime.h"
#include "crl/crl_on_main.h"
#include "logs.h"
#include "mtproto/impulse/core/connection_declaration.h"
#include "mtproto/impulse/core/mapping.h"
#include "mtproto/impulse/core/rpc_response.h"
#include "mtproto/impulse/core/transcoder.h"
#include "mtproto/impulse/glue/impulse_connection.h"
#include "mtproto/impulse/glue/mtp_buffer_bridge.h"
#include "mtproto/impulse/glue/rpc_carrier.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_response.h"
#include "mtproto/session.h"
#include "settings.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Impulse {
namespace {

using SerializedRequest = MTP::details::SerializedRequest;
using RequestMap = base::flat_map<mtpRequestId, SerializedRequest>;

// The rpc_error object of the TL schema: error_code and error_message.
constexpr std::int32_t kRpcErrorConstructor = 0x2144ca19;

// The mapping of the layer 232 schema, next to the executable.
[[nodiscard]] std::string MappingFilePath() {
	return (cExeDir() + "impulse/tl-proto-232.json.gz").toStdString();
}

// The transcoder refers to the table it was built on, so both live in one
// object that is created once and never moved.
struct RequestCodec final {
	explicit RequestCodec(MappingTable source)
	: table(std::move(source))
	, transcoder(table) {
	}

	MappingTable table;
	Transcoder transcoder;
};

[[nodiscard]] const RequestCodec *SharedRequestCodec() {
	static const std::unique_ptr<RequestCodec> codec = []()
			-> std::unique_ptr<RequestCodec> {
		std::string error;
		std::optional<MappingTable> table = MappingTable::LoadGzipFile(
			MappingFilePath(),
			&error);
		if (!table) {
			LOG(("ImpulseM Error: mapping was not loaded: %1."
				).arg(QString::fromStdString(error)));
			return nullptr;
		}
		return std::make_unique<RequestCodec>(std::move(*table));
	}();
	return codec.get();
}

// The TL bytes of a request: the body after the message header, whose length
// in bytes is the word at the length position.
[[nodiscard]] std::span<const std::uint8_t> BodyBytes(
		const SerializedRequest &request) {
	const mtpPrime *const words = request->constData()
		+ SerializedRequest::kMessageBodyPosition;
	const std::size_t size = static_cast<std::size_t>(
		(*request)[SerializedRequest::kMessageLengthPosition]);
	return std::span<const std::uint8_t>(
		reinterpret_cast<const std::uint8_t *>(words),
		size);
}

// Declaration ids are negative, so they never meet a request id of the
// session, which are positive.
[[nodiscard]] mtpRequestId NextDeclarationRequestId() {
	static std::atomic<mtpRequestId> counter = 0;
	return counter.fetch_sub(1) - 1;
}

// The retry schedule of a session in milliseconds: 1, 2, 3, then 1000 and
// doubling up to 64000.
[[nodiscard]] crl::time NextRetryTimeout(crl::time current) {
	if (current < 3) {
		return current + 1;
	} else if (current == 3) {
		return 1000;
	} else if (current < 64000) {
		return current * 2;
	}
	return current;
}

// The TL rpc_error object of a code and a message.
[[nodiscard]] mtpBuffer ErrorBuffer(
		std::int32_t code,
		std::string_view message) {
	TlWriter writer;
	writer.writeInt32(kRpcErrorConstructor);
	writer.writeInt32(code);
	writer.writeBytes(std::span<const std::uint8_t>(
		reinterpret_cast<const std::uint8_t *>(message.data()),
		message.size()));
	return TlBytesToBuffer<mtpBuffer>(writer.bytes());
}

// The TL result of a server reply. A reply that does not decode is a local
// permanent failure, so it becomes an rpc_error of the CLIENT_* class.
[[nodiscard]] mtpBuffer ResultBuffer(
		std::uint32_t methodId,
		const std::vector<std::uint8_t> &proto) {
	const RequestCodec *const codec = SharedRequestCodec();
	if (!codec) {
		const RpcErrorPayload payload = MakeClientError(
			kTranscodeFailedType,
			"The mapping was not loaded.");
		return ErrorBuffer(payload.code, RpcErrorText(
			payload.type,
			payload.description));
	}
	try {
		const std::vector<std::uint8_t> tl = codec->transcoder.decodeResult(
			methodId,
			proto,
			TranscodeCaptureSink());
		return TlBytesToBuffer<mtpBuffer>(tl);
	} catch (const TranscodeError &error) {
		const RpcErrorPayload payload = MakeClientError(
			error.type(),
			error.what());
		return ErrorBuffer(payload.code, RpcErrorText(
			payload.type,
			payload.description));
	} catch (const MtpBufferBridgeError &error) {
		const RpcErrorPayload payload = MakeClientError(
			kResponseParseFailedType,
			error.what());
		return ErrorBuffer(payload.code, RpcErrorText(
			payload.type,
			payload.description));
	}
}

} // namespace

ImpulseSessionPrivate::ImpulseSessionPrivate(
		not_null<MTP::Instance*> instance,
		not_null<QThread*> thread,
		std::shared_ptr<MTP::details::SessionData> data,
		MTP::ShiftedDcId shiftedDcId)
: instance(instance)
, shiftedDcId(shiftedDcId)
, sessionData(std::move(data))
, tracking(std::make_shared<Tracking>()) {
	moveToThread(thread);
}

ImpulseSessionPrivate::~ImpulseSessionPrivate() = default;

MTP::ShiftedDcId ImpulseSessionPrivate::getShiftedDcId() const {
	return shiftedDcId;
}

void ImpulseSessionPrivate::dcOptionsChanged() {
	// Every DC is the account connection, so there are no DC options to follow.
}

void ImpulseSessionPrivate::cdnConfigChanged() {
	// There is no CDN key to follow: media goes through the media fleet.
}

int32 ImpulseSessionPrivate::getState() const {
	QMutexLocker locker(&tracking->mutex);
	return tracking->state;
}

QString ImpulseSessionPrivate::transport() const {
	return QStringLiteral("impulse");
}

void ImpulseSessionPrivate::updateAuthKey() {
	// There is no auth key: the connection identity replaces it.
}

void ImpulseSessionPrivate::restartNow() {
	// The sockets reconnect on their own, and the proxy changes are passed to
	// the account connection.
}

void ImpulseSessionPrivate::sendPingForce() {
	// The sockets keep their own liveness through the client configuration.
}

void ImpulseSessionPrivate::tryToSend() {
	QWriteLocker locker(sessionData->toSendMutex());
	RequestMap &toSend = sessionData->toSendMap();
	std::vector<mtpRequestId> dropped;
	for (RequestMap::iterator it = toSend.begin(); it != toSend.end(); ++it) {
		if (!isReady(it->first, it->second)) {
			continue;
		}
		if (dispatch(it->first, it->second) == Dispatch::Drop) {
			dropped.push_back(it->first);
		}
	}
	for (const mtpRequestId requestId : dropped) {
		toSend.remove(requestId);
	}
}

bool ImpulseSessionPrivate::isReady(
		mtpRequestId requestId,
		const SerializedRequest &request) {
	QMutexLocker locker(&tracking->mutex);
	if (tracking->inFlight.contains(requestId)) {
		return false;
	}

	// invokeAfter: a request chained after another one waits until that one
	// is no longer in flight. The wrapper itself is not sent to the server.
	const bool afterInFlight = request->after
		&& tracking->inFlight.contains(request->after->requestId);
	return !afterInFlight;
}

ImpulseSessionPrivate::Dispatch ImpulseSessionPrivate::dispatch(
		mtpRequestId requestId,
		const SerializedRequest &request) {
	const RequestCodec *const codec = SharedRequestCodec();
	if (!codec) {
		return Dispatch::Keep;
	}

	const std::span<const std::uint8_t> body = BodyBytes(request);
	EncodedRequest encoded;
	try {
		encoded = codec->transcoder.encodeRequest(body);
	} catch (const TranscodeError &error) {
		LOG(("ImpulseM Error: request %1 was not encoded: %2."
			).arg(requestId
			).arg(QString::fromUtf8(error.what())));
		return Dispatch::Drop;
	}

	if (const std::optional<ConnectionDeclaration> declaration
			= ReadConnectionDeclaration(body)) {
		dispatchDeclaration(EncodeConnectionDeclaration(*declaration));
	}

	RpcCarrierRequest carrierRequest;
	carrierRequest.route.method = encoded.method;
	carrierRequest.route.takeout = encoded.takeoutId.has_value();
	carrierRequest.takeoutId = encoded.takeoutId;
	carrierRequest.data = std::move(encoded.payload);
	carrierRequest.awaitDeclaration = true;

	{
		QMutexLocker locker(&tracking->mutex);
		tracking->inFlight.insert(requestId);
	}

	const std::shared_ptr<Tracking> shared = tracking;
	const std::shared_ptr<MTP::details::SessionData> data = sessionData;
	const not_null<MTP::Instance*> owner = instance;
	const base::weak_ptr<ImpulseSessionPrivate> weak = base::make_weak(this);
	const std::uint32_t methodId = encoded.methodId;
	const SerializedRequest retained = request;

	// The completion runs on the main thread, once the carrier has settled.
	const RpcCarrierDone done = [
		weak,
		shared,
		data,
		requestId,
		methodId,
		retained
	](RpcCarrierReply reply) {
		ImpulseSessionPrivate::settleRequest(
			weak,
			shared,
			data,
			requestId,
			methodId,
			retained,
			std::move(reply));
	};

	// The handover runs on the main thread. Once the session is gone it does
	// nothing, and the request stays queued in SessionData for the next one.
	crl::on_main(weak, [
		owner,
		data,
		shared,
		requestId,
		carrierRequest = std::move(carrierRequest),
		done
	]() mutable {
		{
			QMutexLocker locker(&shared->mutex);
			if (!shared->inFlight.contains(requestId)) {
				// Cancelled before the handover.
				return;
			}
		}
		ImpulseConnection *const connection = owner->impulse();
		if (!connection) {
			QMutexLocker locker(&shared->mutex);
			shared->inFlight.remove(requestId);
			return;
		}
		{
			QWriteLocker locker(data->toSendMutex());
			data->toSendMap().remove(requestId);
		}
		connection->send(requestId, std::move(carrierRequest), done);
	});
	return Dispatch::Sent;
}

void ImpulseSessionPrivate::dispatchDeclaration(
		const std::vector<std::uint8_t> &payload) {
	{
		QMutexLocker locker(&tracking->mutex);
		if (tracking->declared.contains(payload)) {
			return;
		}
		tracking->declared.insert(payload);
	}

	const mtpRequestId requestId = NextDeclarationRequestId();
	RpcCarrierRequest declaration;
	declaration.route.method = "initConnection";
	declaration.data = payload;

	// A declaration that did not reach the server is declared again by the
	// next request of a session that is not in the declared set.
	const std::shared_ptr<Tracking> shared = tracking;
	const RpcCarrierDone done = [shared, payload](RpcCarrierReply reply) {
		if (reply.outcome.kind != CarrierOutcomeKind::Reply) {
			QMutexLocker locker(&shared->mutex);
			shared->declared.remove(payload);
		}
	};

	const not_null<MTP::Instance*> owner = instance;
	const base::weak_ptr<ImpulseSessionPrivate> weak = base::make_weak(this);
	crl::on_main(weak, [
		owner,
		requestId,
		request = std::move(declaration),
		done
	]() mutable {
		ImpulseConnection *const connection = owner->impulse();
		if (connection) {
			connection->send(requestId, std::move(request), done);
		}
	});
}

void ImpulseSessionPrivate::forgetRequest(mtpRequestId requestId) {
	QMutexLocker locker(&tracking->mutex);
	tracking->inFlight.remove(requestId);
}

void ImpulseSessionPrivate::publishState(
		const std::shared_ptr<Tracking> &shared,
		const std::shared_ptr<MTP::details::SessionData> &data,
		int32 newState) {
	{
		QMutexLocker locker(&shared->mutex);
		if (shared->state == newState) {
			return;
		}
		shared->state = newState;
	}
	data->queueConnectionStateChange(newState);
}

void ImpulseSessionPrivate::settleRequest(
		const base::weak_ptr<ImpulseSessionPrivate> &weak,
		const std::shared_ptr<Tracking> &shared,
		const std::shared_ptr<MTP::details::SessionData> &data,
		mtpRequestId requestId,
		std::uint32_t methodId,
		const SerializedRequest &request,
		RpcCarrierReply reply) {
	{
		QMutexLocker locker(&shared->mutex);
		if (!shared->inFlight.contains(requestId)) {
			// Cancelled: the reply of a cancelled request is dropped.
			return;
		}
		shared->inFlight.remove(requestId);
	}

	if (reply.outcome.kind == CarrierOutcomeKind::NetworkFailure) {
		// A network failure is not an answer of the server. The request goes
		// back to the queue and is sent again by the retry of the session.
		{
			QWriteLocker locker(data->toSendMutex());
			data->toSendMap().emplace(requestId, request);
		}
		std::uint64_t generation = 0;
		crl::time delayMs = 0;
		{
			QMutexLocker locker(&shared->mutex);
			generation = ++shared->retryGeneration;
			delayMs = shared->retryTimeout;
		}
		publishState(shared, data, static_cast<int32>(-delayMs));
		scheduleRetry(weak, generation, delayMs);
		return;
	}

	{
		QMutexLocker locker(&shared->mutex);
		shared->retryTimeout = 1;
		++shared->retryGeneration;
	}
	publishState(shared, data, MTP::ConnectedState);

	MTP::Response response;
	if (reply.outcome.kind == CarrierOutcomeKind::Reply) {
		response.reply = ResultBuffer(methodId, reply.data);
	} else {
		response.reply = ErrorBuffer(
			reply.outcome.error.code,
			reply.outcome.error.text);
	}
	response.outerMsgId = OuterMsgIdFromServerTime(
		static_cast<std::uint64_t>(base::unixtime::now()) * 1000);
	response.requestId = requestId;
	{
		QWriteLocker locker(data->haveReceivedMutex());
		data->haveReceivedMessages().push_back(std::move(response));
	}
	data->queueTryToReceive();
}

void ImpulseSessionPrivate::scheduleRetry(
		const base::weak_ptr<ImpulseSessionPrivate> &weak,
		std::uint64_t generation,
		crl::time delayMs) {
	// The timer fires on the main thread; the retry itself runs on the session
	// thread, which owns this object and sends its requests. A queued call to
	// an object that is gone is dropped by Qt.
	base::call_delayed(delayMs, weak, [weak, generation] {
		ImpulseSessionPrivate *const self = weak.get();
		if (self) {
			InvokeQueued(self, [self, generation] {
				self->retryByTimer(generation);
			});
		}
	});
}

void ImpulseSessionPrivate::retryByTimer(std::uint64_t generation) {
	{
		QMutexLocker locker(&tracking->mutex);
		if (tracking->retryGeneration != generation) {
			// A later reply or failure has replaced this retry.
			return;
		}
		tracking->retryTimeout = NextRetryTimeout(tracking->retryTimeout);
	}
	publishState(tracking, sessionData, MTP::ConnectingState);
	tryToSend();

	// Nothing left in flight means that the connection answers again.
	bool idle = false;
	{
		QMutexLocker locker(&tracking->mutex);
		idle = tracking->inFlight.empty();
	}
	if (idle) {
		publishState(tracking, sessionData, MTP::ConnectedState);
	}
}

} // namespace Impulse
