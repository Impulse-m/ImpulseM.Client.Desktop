/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace Impulse {

using Sha1Digest = std::array<std::uint8_t, 20>;
using Sha256Digest = std::array<std::uint8_t, 32>;

[[nodiscard]] Sha1Digest Sha1(std::span<const std::uint8_t> data);
[[nodiscard]] Sha1Digest Sha1(std::string_view text);

[[nodiscard]] Sha256Digest Sha256(std::span<const std::uint8_t> data);
[[nodiscard]] Sha256Digest Sha256(std::string_view text);

} // namespace Impulse
