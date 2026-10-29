/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/impulse/core/hash.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace {

template <std::size_t Size>
[[nodiscard]] std::string ToHex(const std::array<std::uint8_t, Size> &digest) {
	static constexpr std::string_view kDigits = "0123456789abcdef";
	std::string result;
	result.reserve(Size * 2);
	for (const std::uint8_t byte : digest) {
		result.push_back(kDigits[byte >> 4]);
		result.push_back(kDigits[byte & 0x0F]);
	}
	return result;
}

[[nodiscard]] std::string RepeatChar(char symbol, std::size_t count) {
	return std::string(count, symbol);
}

} // namespace

IMPULSE_TEST(Sha1EmptyInput) {
	IMPULSE_CHECK(ToHex(Impulse::Sha1(std::string_view())) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

IMPULSE_TEST(Sha1Abc) {
	IMPULSE_CHECK(ToHex(Impulse::Sha1(std::string_view("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

IMPULSE_TEST(Sha1TwoBlockFipsVector) {
	const std::string_view message = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	IMPULSE_CHECK(ToHex(Impulse::Sha1(message)) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

IMPULSE_TEST(Sha1BlockBoundaries) {
	IMPULSE_CHECK(ToHex(Impulse::Sha1(RepeatChar('a', 55))) == "c1c8bbdc22796e28c0e15163d20899b65621d65a");
	IMPULSE_CHECK(ToHex(Impulse::Sha1(RepeatChar('a', 56))) == "c2db330f6083854c99d4b5bfb6e8f29f201be699");
	IMPULSE_CHECK(ToHex(Impulse::Sha1(RepeatChar('a', 64))) == "0098ba824b5c16427bd7a1122a5a442a25ec644d");
	IMPULSE_CHECK(ToHex(Impulse::Sha1(RepeatChar('a', 120))) == "f34c1488385346a55709ba056ddd08280dd4c6d6");
}

IMPULSE_TEST(Sha1MillionAs) {
	IMPULSE_CHECK(ToHex(Impulse::Sha1(RepeatChar('a', 1000000))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

IMPULSE_TEST(Sha256EmptyInput) {
	IMPULSE_CHECK(ToHex(Impulse::Sha256(std::string_view())) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

IMPULSE_TEST(Sha256Abc) {
	IMPULSE_CHECK(ToHex(Impulse::Sha256(std::string_view("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

IMPULSE_TEST(Sha256TwoBlockFipsVector) {
	const std::string_view message = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	IMPULSE_CHECK(ToHex(Impulse::Sha256(message)) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

IMPULSE_TEST(Sha256BlockBoundaries) {
	IMPULSE_CHECK(ToHex(Impulse::Sha256(RepeatChar('a', 55))) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
	IMPULSE_CHECK(ToHex(Impulse::Sha256(RepeatChar('a', 56))) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
	IMPULSE_CHECK(ToHex(Impulse::Sha256(RepeatChar('a', 64))) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
	IMPULSE_CHECK(ToHex(Impulse::Sha256(RepeatChar('a', 120))) == "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
}

IMPULSE_TEST(Sha256MillionAs) {
	IMPULSE_CHECK(ToHex(Impulse::Sha256(RepeatChar('a', 1000000))) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

IMPULSE_TEST(HashSpanOverloadMatchesStringOverload) {
	const std::string message = "abc";
	const std::span<const std::uint8_t> bytes(
		reinterpret_cast<const std::uint8_t *>(message.data()),
		message.size());
	IMPULSE_CHECK(Impulse::Sha1(bytes) == Impulse::Sha1(std::string_view(message)));
	IMPULSE_CHECK(Impulse::Sha256(bytes) == Impulse::Sha256(std::string_view(message)));
}
