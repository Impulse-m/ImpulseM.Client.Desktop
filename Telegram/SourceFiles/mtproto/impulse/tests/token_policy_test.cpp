/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/core/token_policy.h"

namespace {

using namespace Impulse;

[[nodiscard]] RandomUnit constantRandom(double value) {
	return [value] () -> double {
		return value;
	};
}

} // namespace

IMPULSE_TEST(TokenPolicyRefreshPointWindow) {
	constexpr std::int64_t exp = 1000;
	IMPULSE_CHECK(refreshPointMs(exp, 0, constantRandom(0.0)) == 700000);
	IMPULSE_CHECK(refreshPointMs(exp, 0, constantRandom(0.5)) == 790000);
	IMPULSE_CHECK(refreshPointMs(exp, 0, constantRandom(0.999)) < 880000);
	IMPULSE_CHECK(refreshPointMs(exp, 0, constantRandom(0.0)) >= 700000);
}

IMPULSE_TEST(TokenPolicyRefreshPointClipsToNow) {
	constexpr std::int64_t exp = 1000;
	IMPULSE_CHECK(refreshPointMs(exp, 800000, constantRandom(0.0)) == 800000);
	IMPULSE_CHECK(refreshPointMs(exp, 800000, constantRandom(0.5)) == 840000);
}

IMPULSE_TEST(TokenPolicyRefreshPointNowWhenLessThan120Seconds) {
	constexpr std::int64_t exp = 1000;
	IMPULSE_CHECK(refreshPointMs(exp, 880000, constantRandom(0.0)) == 880000);
	IMPULSE_CHECK(refreshPointMs(exp, 950000, constantRandom(0.7)) == 950000);
}

IMPULSE_TEST(TokenPolicyRefreshPointNullRandomUsesWindowStart) {
	IMPULSE_CHECK(refreshPointMs(1000, 0, RandomUnit()) == 700000);
}

IMPULSE_TEST(TokenPolicyIdentityUsableMargin) {
	constexpr std::int64_t exp = 1000;
	IMPULSE_CHECK(identityUsable(exp, 939999));
	IMPULSE_CHECK(!identityUsable(exp, 940000));
	IMPULSE_CHECK(!identityUsable(exp, 999000));
}

IMPULSE_TEST(TokenPolicySameIdentityRule) {
	TokenClaims before;
	before.sub = "i:0123456789abcdef0123456789abcdef";
	before.origIat = 100;
	before.iat = 200;
	before.exp = 1000;

	TokenClaims renewed = before;
	renewed.iat = 300;
	IMPULSE_CHECK(isSameIdentity(before, renewed));

	TokenClaims notMoved = before;
	notMoved.iat = 100;
	IMPULSE_CHECK(!isSameIdentity(before, notMoved));

	TokenClaims otherSub = renewed;
	otherSub.sub = "i:ffffffffffffffffffffffffffffffff";
	IMPULSE_CHECK(!isSameIdentity(before, otherSub));

	TokenClaims otherOrigIat = renewed;
	otherOrigIat.origIat = 150;
	IMPULSE_CHECK(!isSameIdentity(before, otherOrigIat));
}

IMPULSE_TEST(TokenPolicyRetryDoublesToCap) {
	std::int64_t delay = firstRetryDelayMs();
	IMPULSE_CHECK(delay == 1000);
	delay = nextRetryDelayMs(delay);
	IMPULSE_CHECK(delay == 2000);
	delay = nextRetryDelayMs(delay);
	IMPULSE_CHECK(delay == 4000);
	delay = nextRetryDelayMs(delay);
	IMPULSE_CHECK(delay == 8000);
	delay = nextRetryDelayMs(delay);
	IMPULSE_CHECK(delay == 16000);
	delay = nextRetryDelayMs(delay);
	IMPULSE_CHECK(delay == 32000);
	delay = nextRetryDelayMs(delay);
	IMPULSE_CHECK(delay == 60000);
	delay = nextRetryDelayMs(delay);
	IMPULSE_CHECK(delay == 60000);
}

IMPULSE_TEST(TokenPolicyRetryLandsBeforeWindowEnd) {
	constexpr std::int64_t exp = 1000;
	IMPULSE_CHECK(retryLandsBeforeWindowEnd(exp, 800000, 79999));
	IMPULSE_CHECK(!retryLandsBeforeWindowEnd(exp, 800000, 80000));
	IMPULSE_CHECK(!retryLandsBeforeWindowEnd(exp, 800000, -1));
}

IMPULSE_TEST(TokenPolicyFloodDeferralBeforeWindowEnd) {
	constexpr std::int64_t exp = 1000;
	IMPULSE_CHECK(floodDeferralAllowed(exp, 800000, 79));
	IMPULSE_CHECK(!floodDeferralAllowed(exp, 800000, 80));
	IMPULSE_CHECK(!floodDeferralAllowed(exp, 800000, -1));
}

IMPULSE_TEST(TokenPolicyRefreshGraceDefault) {
	IMPULSE_CHECK(refreshGraceMs(0) == 30000);
	IMPULSE_CHECK(refreshGraceMs(-5) == 30000);
	IMPULSE_CHECK(refreshGraceMs(10) == 10000);
}

IMPULSE_TEST(TokenPolicyWithinRefreshGrace) {
	IMPULSE_CHECK(withinRefreshGrace(1000, 30999, 30000));
	IMPULSE_CHECK(!withinRefreshGrace(1000, 31000, 30000));
	IMPULSE_CHECK(!withinRefreshGrace(1000, 500, 30000));
}
