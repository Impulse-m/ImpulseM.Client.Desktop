/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/impulse/core/calls_keys.h"

#include <cstddef>

namespace Impulse {

std::int64_t CallsKeyFingerprint(std::span<const std::uint8_t> authKey) {
	const Sha1Digest digest = Sha1(authKey);
	constexpr std::size_t kFingerprintBytes = 8;
	constexpr std::size_t kFingerprintOffset = 20 - kFingerprintBytes;

	std::uint64_t value = 0;
	for (std::size_t index = 0; index < kFingerprintBytes; ++index) {
		value |= static_cast<std::uint64_t>(digest[kFingerprintOffset + index])
			<< (8 * index);
	}
	return static_cast<std::int64_t>(value);
}

std::vector<std::uint8_t> CallsEmojiInput(
		std::span<const std::uint8_t> authKey,
		std::span<const std::uint8_t> gA) {
	std::vector<std::uint8_t> result;
	result.reserve(authKey.size() + gA.size());
	result.insert(result.end(), authKey.begin(), authKey.end());
	result.insert(result.end(), gA.begin(), gA.end());
	return result;
}

Sha256Digest CallsEmojiDigest(
		std::span<const std::uint8_t> authKey,
		std::span<const std::uint8_t> gA) {
	const std::vector<std::uint8_t> input = CallsEmojiInput(authKey, gA);
	return Sha256(std::span<const std::uint8_t>(input));
}

} // namespace Impulse
