/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/bytes.h"

namespace Impulse {

inline constexpr auto kCallsAuthKeySize = 256;

// Android isGoodPrime for the calls prime and generator.
[[nodiscard]] bool IsGoodCallsPrime(bytes::const_span prime, int g);

// Android isGoodGaAndGb: the peer value must lie strictly between 1 and p - 1.
[[nodiscard]] bool IsGoodCallsGa(bytes::const_span ga, bytes::const_span prime);

// Computes peerValue^secret mod prime. The result is big-endian and exactly
// kCallsAuthKeySize bytes: a longer value keeps its last 256 bytes, a shorter
// one is left-padded with zeros. Returns an empty vector when a value fails
// the checks above.
[[nodiscard]] bytes::vector CallsAuthKey(
	bytes::const_span peerValue,
	bytes::const_span secret,
	bytes::const_span prime);

} // namespace Impulse
