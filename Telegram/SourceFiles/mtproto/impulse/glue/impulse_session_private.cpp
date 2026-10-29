#include "mtproto/impulse/glue/impulse_session_private.h"

#include "crl/crl_on_main.h"
#include "logs.h"
#include "mtproto/impulse/core/connection_declaration.h"
#include "mtproto/impulse/core/mapping.h"
#include "mtproto/impulse/core/transcoder.h"
#include "mtproto/impulse/glue/impulse_connection.h"
#include "mtproto/impulse/glue/rpc_carrier.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/session.h"
#include "settings.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace Impulse {
namespace {

using SerializedRequest = MTP::details::SerializedRequest;
using RequestMap = base::flat_map<mtpRequestId, SerializedRequest>;

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
	return state;
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
	const RpcCarrierDone done = [shared, requestId](RpcCarrierReply) {
		// The result is delivered by the response side, which is not connected
		// yet. The request only stops holding back the requests after it.
		QMutexLocker locker(&shared->mutex);
		shared->inFlight.remove(requestId);
	};

	// The handover runs on the main thread. Once the session is gone it does
	// nothing, and the request stays queued in SessionData for the next one.
	const base::weak_ptr<ImpulseSessionPrivate> weak = base::make_weak(this);
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

} // namespace Impulse
