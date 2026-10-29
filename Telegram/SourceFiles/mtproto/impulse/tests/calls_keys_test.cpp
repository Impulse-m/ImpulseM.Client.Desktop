/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/impulse/core/calls_keys.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

[[nodiscard]] std::vector<std::uint8_t> MakeSequence(
		std::size_t size,
		std::uint8_t step,
		std::uint8_t offset) {
	std::vector<std::uint8_t> result(size);
	for (std::size_t index = 0; index < size; ++index) {
		result[index] = static_cast<std::uint8_t>(
			(index * step + offset) & 0xFF);
	}
	return result;
}

[[nodiscard]] std::vector<std::uint8_t> FromText(std::string_view text) {
	return std::vector<std::uint8_t>(text.begin(), text.end());
}

} // namespace

IMPULSE_TEST(CallsKeyFingerprintFullKey) {
	const std::vector<std::uint8_t> authKey = MakeSequence(256, 7, 3);
	IMPULSE_CHECK(Impulse::CallsKeyFingerprint(authKey) == 3587517436832175774LL);
}

IMPULSE_TEST(CallsKeyFingerprintNegativeValue) {
	const std::vector<std::uint8_t> authKey = FromText("abc");
	IMPULSE_CHECK(Impulse::CallsKeyFingerprint(authKey) == -7072673844527214472LL);
}

IMPULSE_TEST(CallsEmojiInputIsAuthKeyThenGa) {
	const std::vector<std::uint8_t> authKey = MakeSequence(256, 7, 3);
	const std::vector<std::uint8_t> gA = MakeSequence(256, 13, 5);
	const std::vector<std::uint8_t> input = Impulse::CallsEmojiInput(authKey, gA);
	IMPULSE_CHECK(input.size() == 512);
	IMPULSE_CHECK(std::equal(authKey.begin(), authKey.end(), input.begin()));
	IMPULSE_CHECK(std::equal(gA.begin(), gA.end(), input.begin() + 256));
}

IMPULSE_TEST(CallsEmojiDigestFullKey) {
	const std::vector<std::uint8_t> authKey = MakeSequence(256, 7, 3);
	const std::vector<std::uint8_t> gA = MakeSequence(256, 13, 5);
	IMPULSE_CHECK(
		ToHex(Impulse::CallsEmojiDigest(authKey, gA))
		== "dea1054bbfac8f291f4c7f164bc20ade33f9c1772ef76e7e674d24f2e6d9d906");
}

IMPULSE_TEST(CallsEmojiDigestShortInput) {
	const std::vector<std::uint8_t> authKey = FromText("abc");
	const std::vector<std::uint8_t> gA = FromText("xy");
	IMPULSE_CHECK(
		ToHex(Impulse::CallsEmojiDigest(authKey, gA))
		== "5a73d3c860d35077011d2dbbbfc0c86daf68bca9a60463d3daa59aadfbda7781");
}
