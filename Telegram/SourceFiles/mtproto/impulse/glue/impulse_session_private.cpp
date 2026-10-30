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

// The local permanent failure of a carrier error with a negative code.
constexpr std::string_view kLocalFailureType = "CLIENT_LOCAL_FAILURE";
constexpr std::string_view kLocalFailureDescription = "The request failed locally.";

// The constructors that the download lines read: upload.getFile and its
// replies upload.file and upload.webFile, the input locations of a photo and a
// document, and the wrappers that a request may carry in front of the call.
constexpr std::uint32_t kUploadGetFileConstructor = 0xbe5335be;
constexpr std::uint32_t kUploadFileConstructor = 0x096a18d5;
constexpr std::uint32_t kUploadWebFileConstructor = 0x21e753bc;
constexpr std::uint32_t kInputPhotoFileLocationConstructor = 0x40181ffe;
constexpr std::uint32_t kInputDocumentFileLocationConstructor = 0xbad07584;
constexpr std::uint32_t kInvokeWithoutUpdatesConstructor = 0xbf9459b7;
constexpr std::uint32_t kInvokeWithLayerConstructor = 0xda9b0d0d;
constexpr std::uint32_t kInvokeWithTakeoutConstructor = 0xaca9fd2e;
constexpr std::uint32_t kInvokeAfterMsgConstructor = 0xcb9f372d;

// The bytes of a value that a diagnostic line shows, and the characters of a
// string that it shows.
constexpr std::size_t kDiagnosticBytes = 8;
constexpr std::size_t kDiagnosticStringBytes = 16;

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

// The byte string of a value in hex digit pairs, at most limit bytes. None
// when the value has no bytes.
[[nodiscard]] QString HexOf(
		std::span<const std::uint8_t> bytes,
		std::size_t limit) {
	const std::size_t shown = (bytes.size() < limit) ? bytes.size() : limit;
	QString hex;
	for (std::size_t index = 0; index < shown; ++index) {
		hex += QString::number(bytes[index] >> 4, 16);
		hex += QString::number(bytes[index] & 0x0F, 16);
	}
	return hex.isEmpty() ? QString::fromLatin1("none") : hex;
}

// A constructor or type id in hex: the 0x prefix and eight digits.
[[nodiscard]] QString HexWord(std::uint32_t value) {
	return QString::fromLatin1("0x")
		+ QString::number(value, 16).rightJustified(8, QChar('0'));
}

// A thumb_size string for a diagnostic line: its text when every shown byte is
// printable, else its hex. Only the first bytes are shown.
[[nodiscard]] QString ThumbSizeText(std::span<const std::uint8_t> value) {
	if (value.empty()) {
		return QString::fromLatin1("empty");
	}
	const std::size_t shown = (value.size() < kDiagnosticStringBytes)
		? value.size()
		: kDiagnosticStringBytes;
	bool printable = true;
	for (std::size_t index = 0; index < shown; ++index) {
		if (value[index] < 0x20 || value[index] > 0x7E) {
			printable = false;
		}
	}
	if (!printable) {
		return QString::fromLatin1("hex ") + HexOf(value, kDiagnosticStringBytes);
	}
	return QString::fromLatin1("\"%1\"").arg(QString::fromLatin1(
		reinterpret_cast<const char *>(value.data()),
		static_cast<int>(shown)));
}

// The facts of an upload.getFile request that the download lines show: the
// constructor of its InputFileLocation, and the thumb_size of a photo or a
// document location. Every read is bounded by the request.
struct DownloadRequestFacts {
	bool located = false;
	std::uint32_t locationConstructor = 0;
	QString thumbSize = QString::fromLatin1("none");
};

[[nodiscard]] DownloadRequestFacts ReadGetFileRequest(
		std::span<const std::uint8_t> body) {
	DownloadRequestFacts facts;
	try {
		TlReader reader(body);
		std::uint32_t constructor = static_cast<std::uint32_t>(reader.readInt32());

		// The wrappers in front of the call each carry a fixed prefix: the layer
		// or the takeout id, or a message id. The prefix is read past.
		while (constructor == kInvokeWithoutUpdatesConstructor
			|| constructor == kInvokeWithLayerConstructor
			|| constructor == kInvokeWithTakeoutConstructor
			|| constructor == kInvokeAfterMsgConstructor) {
			if (constructor == kInvokeWithLayerConstructor) {
				static_cast<void>(reader.readInt32());
			} else if (constructor == kInvokeWithTakeoutConstructor
				|| constructor == kInvokeAfterMsgConstructor) {
				static_cast<void>(reader.readInt64());
			}
			constructor = static_cast<std::uint32_t>(reader.readInt32());
		}
		if (constructor != kUploadGetFileConstructor) {
			return facts;
		}

		// The flags word: the precise and cdn_supported flags carry no value.
		static_cast<void>(reader.readInt32());
		facts.locationConstructor = static_cast<std::uint32_t>(reader.readInt32());
		facts.located = true;
		if (facts.locationConstructor == kInputPhotoFileLocationConstructor
			|| facts.locationConstructor == kInputDocumentFileLocationConstructor) {
			static_cast<void>(reader.readInt64());
			static_cast<void>(reader.readInt64());
			static_cast<void>(reader.readBytes());
			facts.thumbSize = ThumbSizeText(reader.readBytes());
		}
	} catch (const TranscodeError &) {
		// The facts read before the short end are kept.
	}
	return facts;
}

// The request of a download, for the download lines.
[[nodiscard]] QString DownloadRequestText(const SerializedRequest &request) {
	const DownloadRequestFacts facts = ReadGetFileRequest(BodyBytes(request));
	if (!facts.located) {
		return QString::fromLatin1("request not read");
	}
	return QString::fromLatin1("location %1, thumb_size %2")
		.arg(HexWord(facts.locationConstructor))
		.arg(facts.thumbSize);
}

// The name of a download call, upload.getFile or upload.getWebFile, or an
// empty string for every other method.
[[nodiscard]] QString DownloadNameOf(
		const RequestCodec &codec,
		std::uint32_t methodId) {
	const std::optional<std::uint32_t> getFile
		= codec.table.methodId("upload.getFile");
	if (getFile && *getFile == methodId) {
		return QString::fromLatin1("upload.getFile");
	}
	const std::optional<std::uint32_t> getWebFile
		= codec.table.methodId("upload.getWebFile");
	if (getWebFile && *getWebFile == methodId) {
		return QString::fromLatin1("upload.getWebFile");
	}
	return QString();
}

// The TL name of a method, for the lines that name it.
[[nodiscard]] QString MethodNameOf(
		const RequestCodec &codec,
		std::uint32_t methodId) {
	for (const MappingMethod &method : codec.table.mapping().methods) {
		if (method.id == methodId) {
			return QString::fromStdString(method.name);
		}
	}
	return QString::fromLatin1("unknown method");
}

// The facts of a successful download reply, read from its TL object: the
// constructor, the storage.FileType of upload.file, and the length and the
// first bytes of the bytes field. Every read is bounded; a reply that is short
// keeps the facts read before its end.
[[nodiscard]] QString TlFileReplyText(std::span<const std::uint8_t> tl) {
	std::uint32_t constructor = 0;
	QString storage = QString::fromLatin1("none");
	std::span<const std::uint8_t> bytes;
	try {
		TlReader reader(tl);
		constructor = static_cast<std::uint32_t>(reader.readInt32());
		if (constructor == kUploadFileConstructor) {
			storage = HexWord(static_cast<std::uint32_t>(reader.readInt32()));
			static_cast<void>(reader.readInt32());
			bytes = reader.readBytes();
		} else if (constructor == kUploadWebFileConstructor) {
			static_cast<void>(reader.readInt32());
			static_cast<void>(reader.readBytes());
			storage = HexWord(static_cast<std::uint32_t>(reader.readInt32()));
			static_cast<void>(reader.readInt32());
			bytes = reader.readBytes();
		}
	} catch (const TranscodeError &) {
		// The facts read before the short end are kept.
	}
	return QString::fromLatin1("reply %1, storage %2, bytes %3, first %4")
		.arg(HexWord(constructor))
		.arg(storage)
		.arg(QString::number(static_cast<qulonglong>(bytes.size())))
		.arg(HexOf(bytes, kDiagnosticBytes));
}

// The facts of a download reply that is still proto: the top-level field with
// the longest length-delimited value is taken as the bytes field, and its
// number, length and first bytes are shown. The mapping is not consulted.
[[nodiscard]] QString ProtoFileReplyText(const std::vector<std::uint8_t> &proto) {
	std::uint32_t bestField = 0;
	std::span<const std::uint8_t> best;
	bool found = false;
	try {
		ProtoReader reader(proto);
		while (reader.next()) {
			if (reader.wireType() == ProtoWireType::LengthDelimited) {
				const std::span<const std::uint8_t> value = reader.readBytes();
				if (!found || value.size() > best.size()) {
					bestField = reader.field();
					best = value;
					found = true;
				}
			} else {
				reader.skip();
			}
		}
	} catch (const ProtoWireError &) {
		// The fields read before the bad one are kept.
	}
	if (!found) {
		return QString::fromLatin1("proto no bytes field");
	}
	return QString::fromLatin1("proto field %1, bytes %2, first %3")
		.arg(QString::number(bestField))
		.arg(QString::number(static_cast<qulonglong>(best.size())))
		.arg(HexOf(best, kDiagnosticBytes));
}

// The line of a server reply that does not convert to TL. A download adds the
// facts of its request and of its proto reply, which is still proto here.
void LogNotConverted(
		mtpRequestId requestId,
		const RequestCodec &codec,
		std::uint32_t methodId,
		const std::vector<std::uint8_t> &proto,
		const SerializedRequest &request,
		const QString &reason) {
	const QString download = DownloadNameOf(codec, methodId);
	if (download.isEmpty()) {
		DEBUG_LOG(("Impulse Info: request %1 %2 was not converted, %3."
			).arg(QString::number(requestId)
			).arg(MethodNameOf(codec, methodId)
			).arg(reason));
		return;
	}
	DEBUG_LOG(("Impulse Info: request %1 %2 was not converted, %3, %4, %5."
		).arg(QString::number(requestId)
		).arg(download
		).arg(reason
		).arg(DownloadRequestText(request)
		).arg(ProtoFileReplyText(proto)));
}

// The line of a download that the carrier failed. The connection line before
// it gives the code and the text of the failure.
void LogDownloadFailure(
		mtpRequestId requestId,
		std::uint32_t methodId,
		const SerializedRequest &request) {
	const RequestCodec *const codec = SharedRequestCodec();
	if (!codec) {
		return;
	}
	const QString download = DownloadNameOf(*codec, methodId);
	if (download.isEmpty()) {
		return;
	}
	DEBUG_LOG(("Impulse Info: request %1 %2 failed, %3."
		).arg(QString::number(requestId)
		).arg(download
		).arg(DownloadRequestText(request)));
}

// The TL result of a server reply. A reply that does not decode is a local
// permanent failure, so it becomes an rpc_error of the CLIENT_* class.
[[nodiscard]] mtpBuffer ResultBuffer(
		mtpRequestId requestId,
		std::uint32_t methodId,
		const std::vector<std::uint8_t> &proto,
		const SerializedRequest &request) {
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
		const QString download = DownloadNameOf(*codec, methodId);
		if (!download.isEmpty()) {
			DEBUG_LOG(("Impulse Info: request %1 %2 replied, %3, %4."
				).arg(QString::number(requestId)
				).arg(download
				).arg(TlFileReplyText(tl)
				).arg(ProtoFileReplyText(proto)));
		}
		return TlBytesToBuffer<mtpBuffer>(tl);
	} catch (const TranscodeError &error) {
		LogNotConverted(
			requestId,
			*codec,
			methodId,
			proto,
			request,
			QString::fromUtf8(error.what()));
		const RpcErrorPayload payload = MakeClientError(
			error.type(),
			error.what());
		return ErrorBuffer(payload.code, RpcErrorText(
			payload.type,
			payload.description));
	} catch (const MtpBufferBridgeError &error) {
		LogNotConverted(
			requestId,
			*codec,
			methodId,
			proto,
			request,
			QString::fromUtf8(error.what()));
		const RpcErrorPayload payload = MakeClientError(
			kResponseParseFailedType,
			error.what());
		return ErrorBuffer(payload.code, RpcErrorText(
			payload.type,
			payload.description));
	}
}

// The rpc_error of a carrier failure that reaches the session. Instance retries
// every negative code and every code from 500 up forever, so a negative code
// becomes the local permanent class. Only a local fault of the connection
// produces one (the login went away), and it is never a server answer.
[[nodiscard]] mtpBuffer OutcomeErrorBuffer(const RpcError &error) {
	if (error.code >= 0) {
		return ErrorBuffer(error.code, error.text);
	}
	const RpcErrorPayload payload = MakeClientError(
		kLocalFailureType,
		error.text.empty()
			? kLocalFailureDescription
			: std::string_view(error.text));
	return ErrorBuffer(payload.code, RpcErrorText(
		payload.type,
		payload.description));
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
	// The layer 232 mapping has no ping method, so no ping is sent on the wire.
	// Liveness is the socket's own ping and pong frames (the endpoints of
	// core/endpoints request cf_ws_frame_ping_pong), which replace the session
	// ping timers of session_private.cpp:41-42.
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
	const base::weak_qptr<ImpulseSessionPrivate> weak = base::make_weak(this);
	const std::uint32_t methodId = encoded.methodId;
	const SerializedRequest retained = request;

	// The handover runs on the main thread. Once the session is gone it does
	// nothing, and the request stays queued in SessionData for the next one.
	crl::on_main(weak, [
		owner,
		weak,
		data,
		shared,
		requestId,
		methodId,
		retained,
		carrierRequest = std::move(carrierRequest)
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

		// The epoch is read as the request starts on the main thread. A
		// failure after a proxy change that advanced it is a proxy-change
		// failure, and it is retried without counting toward the budget.
		const std::uint64_t startEpoch = ImpulseConnection::proxyEpoch();

		// The completion runs on the main thread, once the carrier has settled.
		const RpcCarrierDone done = [
			weak,
			shared,
			data,
			requestId,
			methodId,
			retained,
			startEpoch
		](RpcCarrierReply reply) {
			ImpulseSessionPrivate::settleRequest(
				weak,
				shared,
				data,
				requestId,
				methodId,
				retained,
				startEpoch,
				std::move(reply));
		};
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
	const base::weak_qptr<ImpulseSessionPrivate> weak = base::make_weak(this);
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
	{
		QMutexLocker locker(&tracking->mutex);
		tracking->inFlight.remove(requestId);
	}

	// The account connection is owned by the main thread, so the cancel of
	// the call on the carrier is handed over to it. The hop is queued after
	// any handover of this request, so the call is cut off once it is sent.
	const not_null<MTP::Instance*> owner = instance;
	const base::weak_qptr<ImpulseSessionPrivate> weak = base::make_weak(this);
	crl::on_main(weak, [owner, requestId] {
		ImpulseConnection *const connection = owner->impulse();
		if (connection) {
			connection->cancel(requestId);
		}
	});
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
		const base::weak_qptr<ImpulseSessionPrivate> &weak,
		const std::shared_ptr<Tracking> &shared,
		const std::shared_ptr<MTP::details::SessionData> &data,
		mtpRequestId requestId,
		std::uint32_t methodId,
		const SerializedRequest &request,
		std::uint64_t startEpoch,
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
		// A failure after a proxy change that started before it is a
		// proxy-change failure: it does not count toward the retry budget.
		const bool proxyChangeFailure
			= ImpulseConnection::proxyEpoch() != startEpoch;
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
		scheduleRetry(
			weak,
			shared,
			data,
			generation,
			delayMs,
			!proxyChangeFailure);
		return;
	}

	{
		QMutexLocker locker(&shared->mutex);
		shared->retryTimeout = 1;
		++shared->retryGeneration;
	}
	if (reply.outcome.kind != CarrierOutcomeKind::Reply) {
		LogDownloadFailure(requestId, methodId, request);
	}
	publishState(shared, data, MTP::ConnectedState);

	MTP::Response response;
	if (reply.outcome.kind == CarrierOutcomeKind::Reply) {
		response.reply = ResultBuffer(requestId, methodId, reply.data, request);
	} else {
		response.reply = OutcomeErrorBuffer(reply.outcome.error);
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
		const base::weak_qptr<ImpulseSessionPrivate> &weak,
		const std::shared_ptr<Tracking> &shared,
		const std::shared_ptr<MTP::details::SessionData> &data,
		std::uint64_t generation,
		crl::time delayMs,
		bool counted) {
	// The timer fires on the main thread. While this session lives, the retry
	// runs on its session thread, which owns it and sends its requests. When
	// the session was replaced or killed, the requests stay in SessionData, and
	// the data asks the session of the account to resume and send them.
	base::call_delayed(delayMs, [weak, shared, data, generation, counted] {
		{
			QMutexLocker locker(&shared->mutex);
			if (shared->retryGeneration != generation) {
				// A later reply or failure has replaced this retry.
				return;
			}
		}
		ImpulseSessionPrivate *const self = weak.get();
		if (self) {
			InvokeQueued(self, [self, generation, counted] {
				self->retryByTimer(generation, counted);
			});
		} else {
			data->queueNeedToResumeAndSend();
		}
	});
}

void ImpulseSessionPrivate::retryByTimer(
		std::uint64_t generation,
		bool counted) {
	{
		QMutexLocker locker(&tracking->mutex);
		if (tracking->retryGeneration != generation) {
			// A later reply or failure has replaced this retry.
			return;
		}
		if (counted) {
			tracking->retryTimeout = NextRetryTimeout(tracking->retryTimeout);
		}
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
