#include "mtproto/impulse/glue/carrier_errors.h"

#include "mtproto/impulse/core/rpc_response.h"
#include "mtproto/impulse/glue/centrifugo_rust.h"

#include <optional>

namespace Impulse {
namespace {

[[nodiscard]] CarrierFailure failureOf(const CentrifugoResult &result) {
	CarrierFailure failure;
	if (result.status == kCentrifugoStatusServerError) {
		failure.sdkCode = static_cast<int>(result.serverCode);
		failure.appType = result.appType;
		failure.appCode = result.appCode;
	} else {
		failure.sdkCode = result.status;
	}
	return failure;
}

} // namespace

CarrierOutcome mapCarrierResult(
		const CentrifugoResult &result,
		CarrierKind carrier) {
	CarrierOutcome outcome;
	if (result.status == kCentrifugoStatusOk) {
		outcome.kind = CarrierOutcomeKind::Reply;
		return outcome;
	}
	// ABI errors are local faults. They are never a server answer.
	if (result.status < 0) {
		return outcome;
	}
	const std::optional<MappedError> mapped = mapCarrierFailure(
		failureOf(result),
		carrier);
	if (!mapped) {
		return outcome;
	}
	outcome.kind = CarrierOutcomeKind::RpcError;
	outcome.error = mapped->error;
	outcome.temporary = mapped->temporary;
	return outcome;
}

} // namespace Impulse
