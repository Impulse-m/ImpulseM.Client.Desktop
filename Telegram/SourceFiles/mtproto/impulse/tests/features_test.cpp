/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/impulse_features.h"
#include "mtproto/impulse/tests/impulse_test.h"

IMPULSE_TEST(FeatureFlagDefaultsHideUnimplemented) {
	IMPULSE_CHECK(!ImpulseFeatures::STORIES_POSTING);
	IMPULSE_CHECK(!ImpulseFeatures::BOOSTS);
	IMPULSE_CHECK(!ImpulseFeatures::STARS_AND_TON);
	IMPULSE_CHECK(!ImpulseFeatures::SECRET_CHATS);
	IMPULSE_CHECK(!ImpulseFeatures::AI_COMPOSE);
	IMPULSE_CHECK(!ImpulseFeatures::CHAT_WALLPAPER_AND_THEME);
	IMPULSE_CHECK(!ImpulseFeatures::BROWSER_SETTINGS);
	IMPULSE_CHECK(!ImpulseFeatures::COMMUNITIES);
	IMPULSE_CHECK(!ImpulseFeatures::PAYMENT_SHIPPING_INFO);
	IMPULSE_CHECK(!ImpulseFeatures::PROXY);
	IMPULSE_CHECK(!ImpulseFeatures::HELP_SECTION);
	IMPULSE_CHECK(!ImpulseFeatures::CALL_VERIFICATION);
	IMPULSE_CHECK(!ImpulseFeatures::TEST_BACKEND);
}

IMPULSE_TEST(FeatureFlagVlessEnabled) {
	IMPULSE_CHECK(ImpulseFeatures::VLESS);
}

IMPULSE_TEST(PremiumUnlockedByDefault) {
	IMPULSE_CHECK(!ImpulseFeatures::PremiumLockedDefault);
	IMPULSE_CHECK(!ImpulseFeatures::premiumPurchaseBlocked(ImpulseFeatures::PremiumLockedDefault));
	IMPULSE_CHECK(!ImpulseFeatures::premiumFeaturesBlocked(ImpulseFeatures::PremiumLockedDefault, false));
}

IMPULSE_TEST(PremiumLockedHidesOnlyForNonPremium) {
	IMPULSE_CHECK(ImpulseFeatures::premiumPurchaseBlocked(true));
	IMPULSE_CHECK(ImpulseFeatures::premiumFeaturesBlocked(true, false));
	IMPULSE_CHECK(!ImpulseFeatures::premiumFeaturesBlocked(true, true));
	IMPULSE_CHECK(!ImpulseFeatures::premiumFeaturesBlocked(false, false));
}
