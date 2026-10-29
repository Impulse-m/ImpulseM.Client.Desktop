/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/impulse/core/hash.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Impulse {

// Key fingerprint of a call: the little-endian int64 of the last 8 bytes
// of SHA-1(authKey), read as a signed value like the Android reference.
[[nodiscard]] std::int64_t CallsKeyFingerprint(
	std::span<const std::uint8_t> authKey);

// The emoji input of a call: authKey followed by g_a.
[[nodiscard]] std::vector<std::uint8_t> CallsEmojiInput(
	std::span<const std::uint8_t> authKey,
	std::span<const std::uint8_t> gA);

// SHA-256 of CallsEmojiInput(authKey, gA). The emoji sequence of the call
// is derived from this digest.
[[nodiscard]] Sha256Digest CallsEmojiDigest(
	std::span<const std::uint8_t> authKey,
	std::span<const std::uint8_t> gA);

} // namespace Impulse
