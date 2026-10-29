/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace Impulse {

// A TL byte stream whose length is not a multiple of four cannot be read as
// int32 words. The caller maps this to its local error class.
class MtpBufferBridgeError final : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// Splits a TL byte stream into little-endian int32 words. The stream length
// must be a multiple of four; otherwise MtpBufferBridgeError is thrown.
[[nodiscard]] std::vector<std::int32_t> TlBytesToWords(
	std::span<const std::uint8_t> bytes);

// Joins little-endian int32 words back into a TL byte stream.
[[nodiscard]] std::vector<std::uint8_t> WordsToTlBytes(
	std::span<const std::int32_t> words);

// Typed forms for the tdesktop buffer (mtpBuffer, a container of mtpPrime).
// They are templates so that this header does not depend on Qt.
template <typename Buffer>
[[nodiscard]] Buffer TlBytesToBuffer(std::span<const std::uint8_t> bytes) {
	const std::vector<std::int32_t> words = TlBytesToWords(bytes);
	return Buffer(words.begin(), words.end());
}

template <typename Buffer>
[[nodiscard]] std::vector<std::uint8_t> BufferToTlBytes(const Buffer &buffer) {
	const std::vector<std::int32_t> words(buffer.begin(), buffer.end());
	return WordsToTlBytes(words);
}

} // namespace Impulse
