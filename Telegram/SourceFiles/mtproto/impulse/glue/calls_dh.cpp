/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/impulse/glue/calls_dh.h"

#include "base/openssl_help.h"
#include "mtproto/mtproto_dh_utils.h"

#include <cstddef>

namespace Impulse {
namespace {

[[nodiscard]] bytes::vector FitToKeySize(bytes::vector value) {
	const auto size = static_cast<std::size_t>(kCallsAuthKeySize);
	if (value.size() == size) {
		return value;
	} else if (value.size() > size) {
		return bytes::vector(
			value.end() - static_cast<std::ptrdiff_t>(size),
			value.end());
	}

	auto result = bytes::vector(size, gsl::byte{});
	const auto offset = size - value.size();
	for (std::size_t index = 0; index < value.size(); ++index) {
		result[offset + index] = value[index];
	}
	return result;
}

} // namespace

bool IsGoodCallsPrime(bytes::const_span prime, int g) {
	return MTP::IsPrimeAndGood(prime, g);
}

bool IsGoodCallsGa(bytes::const_span ga, bytes::const_span prime) {
	const auto value = openssl::BigNum(ga);
	const auto modulus = openssl::BigNum(prime);
	if (value.failed() || modulus.failed()) {
		return false;
	}
	const auto upperBound = openssl::BigNum(modulus).subWord(1);
	if (upperBound.failed()) {
		return false;
	}
	const auto one = openssl::BigNum(1u);
	return openssl::BigNum::Compare(value, one) > 0
		&& openssl::BigNum::Compare(value, upperBound) < 0;
}

bytes::vector CallsAuthKey(
		bytes::const_span peerValue,
		bytes::const_span secret,
		bytes::const_span prime) {
	if (!IsGoodCallsGa(peerValue, prime)) {
		return {};
	}
	const auto result = openssl::BigNum::ModExp(
		openssl::BigNum(peerValue),
		openssl::BigNum(secret),
		openssl::BigNum(prime));
	if (result.failed()) {
		return {};
	}
	return FitToKeySize(result.getBytes());
}

} // namespace Impulse
