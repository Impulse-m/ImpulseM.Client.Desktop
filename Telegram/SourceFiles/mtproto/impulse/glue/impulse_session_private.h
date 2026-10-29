// Session side of the ImpulseM transport. It replaces SessionPrivate with the
// same surface and runs on the MTP thread. It encodes each request with the
// transcoder and hands it to the account connection, which owns the sockets
// and runs on the main thread. The response side of the requests is not
// connected here yet: a completion only marks its request as settled.
#pragma once

#include "base/flat_set.h"
#include "base/weak_ptr.h"
#include "mtproto/core_types.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/facade.h"

#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <cstdint>
#include <memory>
#include <vector>

class QThread;

namespace MTP {

class Instance;

namespace details {

class SessionData;

} // namespace details
} // namespace MTP

namespace Impulse {

class ImpulseSessionPrivate final
	: public QObject
	, public base::has_weak_ptr {
public:
	ImpulseSessionPrivate(
		not_null<MTP::Instance*> instance,
		not_null<QThread*> thread,
		std::shared_ptr<MTP::details::SessionData> data,
		MTP::ShiftedDcId shiftedDcId);
	~ImpulseSessionPrivate();

	[[nodiscard]] MTP::ShiftedDcId getShiftedDcId() const;
	void dcOptionsChanged();
	void cdnConfigChanged();

	[[nodiscard]] int32 getState() const;
	[[nodiscard]] QString transport() const;

	void updateAuthKey();
	void restartNow();
	void sendPingForce();
	void tryToSend();

	// Forgets a cancelled request, so that the requests chained after it by
	// invokeAfter are not held back by it any more.
	void forgetRequest(mtpRequestId requestId);

private:
	// Shared with the completions that run on the main thread, so that they
	// never touch the session itself.
	struct Tracking {
		QMutex mutex;
		base::flat_set<mtpRequestId> inFlight;
		base::flat_set<std::vector<std::uint8_t>> declared;
	};

	enum class Dispatch {
		Sent,
		Keep,
		Drop,
	};

	[[nodiscard]] bool isReady(
		mtpRequestId requestId,
		const MTP::details::SerializedRequest &request);
	[[nodiscard]] Dispatch dispatch(
		mtpRequestId requestId,
		const MTP::details::SerializedRequest &request);
	void dispatchDeclaration(const std::vector<std::uint8_t> &payload);

	const not_null<MTP::Instance*> instance;
	const MTP::ShiftedDcId shiftedDcId = 0;
	const std::shared_ptr<MTP::details::SessionData> sessionData;
	const std::shared_ptr<Tracking> tracking;
	int32 state = MTP::DisconnectedState;

};

} // namespace Impulse
