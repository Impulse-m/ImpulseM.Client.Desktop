/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/impulse/glue/mtp_buffer_bridge.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Impulse {
namespace {

constexpr std::size_t kWordBytes = 4;

} // namespace

std::vector<std::int32_t> TlBytesToWords(std::span<const std::uint8_t> bytes) {
	if (bytes.size() % kWordBytes != 0) {
		throw MtpBufferBridgeError("TL stream length is not a multiple of four");
	}

	std::vector<std::int32_t> words;
	words.reserve(bytes.size() / kWordBytes);
	for (std::size_t offset = 0; offset < bytes.size(); offset += kWordBytes) {
		const std::uint32_t word = static_cast<std::uint32_t>(bytes[offset])
			| (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
			| (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
			| (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
		words.push_back(static_cast<std::int32_t>(word));
	}
	return words;
}

std::vector<std::uint8_t> WordsToTlBytes(std::span<const std::int32_t> words) {
	std::vector<std::uint8_t> bytes;
	bytes.reserve(words.size() * kWordBytes);
	for (const std::int32_t value : words) {
		const std::uint32_t word = static_cast<std::uint32_t>(value);
		bytes.push_back(static_cast<std::uint8_t>(word & 0xFFu));
		bytes.push_back(static_cast<std::uint8_t>((word >> 8) & 0xFFu));
		bytes.push_back(static_cast<std::uint8_t>((word >> 16) & 0xFFu));
		bytes.push_back(static_cast<std::uint8_t>((word >> 24) & 0xFFu));
	}
	return bytes;
}

} // namespace Impulse
