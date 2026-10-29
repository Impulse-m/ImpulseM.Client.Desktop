// Maps the result of one carrier call (Rust Centrifugo ABI) to the app result:
// a reply, an rpc_error, or a network failure that is not an rpc_error.
#pragma once

#include "mtproto/impulse/core/errors.h"

namespace Impulse {

struct CentrifugoResult;

enum class CarrierOutcomeKind {
	Reply,
	RpcError,
	NetworkFailure,
};

struct CarrierOutcome {
	CarrierOutcomeKind kind = CarrierOutcomeKind::NetworkFailure;
	RpcError error;
	bool temporary = false;
};

// Converts a carrier result. Success gives Reply. A server error or a
// transport code with a contract 5.5 mapping gives RpcError. Every other
// failure, including ABI errors (negative status), gives NetworkFailure, so it
// never becomes an rpc_error. Backend codes pass through as the Android
// reference does; only the local refusal and transport rows use the 400 class.
[[nodiscard]] CarrierOutcome mapCarrierResult(
	const CentrifugoResult &result,
	CarrierKind carrier);

} // namespace Impulse
