/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace ImpulseFeatures {

// Each false constant hides a feature the ImpulseM backend does not implement.
// Flip it to true when the backend ships the feature.

inline constexpr bool STORIES_POSTING = false;
inline constexpr bool BOOSTS = false;
inline constexpr bool STARS_AND_TON = false;
inline constexpr bool SECRET_CHATS = false;
inline constexpr bool AI_COMPOSE = false;
inline constexpr bool CHAT_WALLPAPER_AND_THEME = false;
inline constexpr bool BROWSER_SETTINGS = false;
inline constexpr bool COMMUNITIES = false;
inline constexpr bool PAYMENT_SHIPPING_INFO = false;
inline constexpr bool PROXY = false;
inline constexpr bool HELP_SECTION = false;
inline constexpr bool CALL_VERIFICATION = false;
inline constexpr bool TEST_BACKEND = false;

// VLESS proxy through the in-process Xray core.
inline constexpr bool VLESS = true;

// Premium has no flag. It is hidden by the app-config value
// premium_purchase_blocked, which is read into the premium state.
inline constexpr bool PremiumLockedDefault = false;

[[nodiscard]] inline constexpr bool premiumPurchaseBlocked(bool premiumLocked) {
	return premiumLocked;
}

[[nodiscard]] inline constexpr bool premiumFeaturesBlocked(bool premiumLocked, bool isPremium) {
	return premiumLocked && !isPremium;
}

} // namespace ImpulseFeatures
