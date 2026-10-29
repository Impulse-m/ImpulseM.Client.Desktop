/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>
#include <string_view>

namespace ImpulseBrand {

inline constexpr std::string_view BrandName = "ImpulseM Desktop";
inline constexpr std::string_view BrandShortName = "ImpulseM";
inline constexpr std::string_view BrandUpper = "IMPULSEM";

// Used by tests and documentation only. No code reads it as a link prefix:
// the prefix comes from help.getConfig (me_url_prefix).
inline constexpr std::string_view LinkHost = "o.impulsem.net";

inline constexpr std::int32_t ApiIdDesktop = 4;

inline constexpr std::uint32_t PrimaryDay = 0xFF2085DF;
inline constexpr std::uint32_t LinkDay = 0xFF106ACC;
inline constexpr std::uint32_t PrimaryNight = 0xFF58AEEE;
inline constexpr std::uint32_t NightSurface = 0xFF1D2128;
inline constexpr std::uint32_t NightBackground = 0xFF14181F;
inline constexpr std::uint32_t NightDivider = 0xFF0B0F15;

} // namespace ImpulseBrand
