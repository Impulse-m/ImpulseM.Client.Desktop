#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/core/proxy_input.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

IMPULSE_TEST(ProxyInputDetectEmptyIsUnknown) {
	using Impulse::ProxyInput::Detect;
	using Impulse::ProxyInput::InputKind;
	IMPULSE_CHECK(Detect("") == InputKind::Unknown);
	IMPULSE_CHECK(Detect("  \r\n\t ") == InputKind::Unknown);
}

IMPULSE_TEST(ProxyInputDetectHttpUrlIsSubscription) {
	using Impulse::ProxyInput::Detect;
	using Impulse::ProxyInput::InputKind;
	IMPULSE_CHECK(Detect("https://example.com/sub") == InputKind::Subscription);
	IMPULSE_CHECK(Detect("  http://example.com/sub  ") == InputKind::Subscription);
	IMPULSE_CHECK(Detect("HTTPS://example.com/sub") == InputKind::Subscription);
}

IMPULSE_TEST(ProxyInputDetectUrlWithWhitespaceIsNotSubscription) {
	using Impulse::ProxyInput::Detect;
	using Impulse::ProxyInput::InputKind;
	IMPULSE_CHECK(Detect("https://example.com/a b") == InputKind::Unknown);
	IMPULSE_CHECK(Detect("https://example.com/a\nb") == InputKind::Unknown);
}

IMPULSE_TEST(ProxyInputDetectVlessLineIsLinks) {
	using Impulse::ProxyInput::Detect;
	using Impulse::ProxyInput::InputKind;
	IMPULSE_CHECK(Detect("vless://uuid@host:443") == InputKind::Links);
	IMPULSE_CHECK(Detect("VLESS://uuid@host:443") == InputKind::Links);
	IMPULSE_CHECK(Detect("junk\r\n   vless://uuid@host:443\r\n") == InputKind::Links);
	IMPULSE_CHECK(Detect("https://example.com/sub\nvless://uuid@host:443") == InputKind::Links);
}

IMPULSE_TEST(ProxyInputDetectOtherTextIsUnknown) {
	using Impulse::ProxyInput::Detect;
	using Impulse::ProxyInput::InputKind;
	IMPULSE_CHECK(Detect("ss://something") == InputKind::Unknown);
	IMPULSE_CHECK(Detect("example.com") == InputKind::Unknown);
}

IMPULSE_TEST(ProxyInputSubscriptionIdIsFirst24HexOfSha256) {
	using Impulse::ProxyInput::SubscriptionId;
	IMPULSE_CHECK(SubscriptionId("https://example.com/sub") == "db108858ab35e999573b9186");
	IMPULSE_CHECK(SubscriptionId("").size() == 24);
	IMPULSE_CHECK(SubscriptionId("") == "e3b0c44298fc1c149afbf4c8");
}

IMPULSE_TEST(ProxyInputSubscriptionIdUsesTrimmedUrl) {
	using Impulse::ProxyInput::SubscriptionId;
	IMPULSE_CHECK(SubscriptionId("  https://example.com/sub \n") == SubscriptionId("https://example.com/sub"));
}

IMPULSE_TEST(ProxyInputStripBomRemovesLeadingBomOnly) {
	using Impulse::ProxyInput::StripBom;
	IMPULSE_CHECK(StripBom("\xEF\xBB\xBFvless://x") == "vless://x");
	IMPULSE_CHECK(StripBom("vless://x\xEF\xBB\xBF") == "vless://x\xEF\xBB\xBF");
	IMPULSE_CHECK(StripBom("") == "");
}

IMPULSE_TEST(ProxyInputBodyCapIsFourMiB) {
	using Impulse::ProxyInput::BodyWithinCap;
	IMPULSE_CHECK(BodyWithinCap(0));
	IMPULSE_CHECK(BodyWithinCap(4 * 1024 * 1024));
	IMPULSE_CHECK(!BodyWithinCap(4 * 1024 * 1024 + 1));
}

IMPULSE_TEST(ProxyInputErrorStrings) {
	using Impulse::ProxyInput::HttpError;
	using Impulse::ProxyInput::NoLinksError;
	IMPULSE_CHECK(HttpError(404) == "err:http:404");
	IMPULSE_CHECK(NoLinksError(0) == "err:noLinks");
	IMPULSE_CHECK(NoLinksError(3) == "err:noLinks:3");
}

IMPULSE_TEST(ProxyInputMetaTitlePlainIsTrimmed) {
	using Impulse::ProxyInput::ParseMeta;
	const Impulse::ProxyInput::SubscriptionMeta meta = ParseMeta(std::string_view(" My Sub "), std::nullopt, std::nullopt);
	IMPULSE_CHECK(meta.title.has_value());
	IMPULSE_CHECK(*meta.title == "My Sub");
}

IMPULSE_TEST(ProxyInputMetaTitleBase64Decodes) {
	using Impulse::ProxyInput::ParseMeta;
	const Impulse::ProxyInput::SubscriptionMeta meta = ParseMeta(std::string_view("base64:SGVsbG8="), std::nullopt, std::nullopt);
	IMPULSE_CHECK(meta.title.has_value());
	IMPULSE_CHECK(*meta.title == "Hello");
}

IMPULSE_TEST(ProxyInputMetaTitleBadOrEmptyBase64IsAbsent) {
	using Impulse::ProxyInput::ParseMeta;
	const Impulse::ProxyInput::SubscriptionMeta bad = ParseMeta(std::string_view("base64:!!!"), std::nullopt, std::nullopt);
	IMPULSE_CHECK(!bad.title.has_value());
	const Impulse::ProxyInput::SubscriptionMeta empty = ParseMeta(std::string_view("base64:"), std::nullopt, std::nullopt);
	IMPULSE_CHECK(!empty.title.has_value());
	const Impulse::ProxyInput::SubscriptionMeta blank = ParseMeta(std::string_view("   "), std::nullopt, std::nullopt);
	IMPULSE_CHECK(!blank.title.has_value());
	const Impulse::ProxyInput::SubscriptionMeta absent = ParseMeta(std::nullopt, std::nullopt, std::nullopt);
	IMPULSE_CHECK(!absent.title.has_value());
}

IMPULSE_TEST(ProxyInputMetaUserInfoParses) {
	using Impulse::ProxyInput::ParseMeta;
	const Impulse::ProxyInput::SubscriptionMeta meta = ParseMeta(
		std::nullopt,
		std::string_view("upload=1; download=2;total=3; EXPIRE=4"),
		std::nullopt);
	IMPULSE_CHECK(meta.upload == 1);
	IMPULSE_CHECK(meta.download == 2);
	IMPULSE_CHECK(meta.total == 3);
	IMPULSE_CHECK(meta.expire == 4);
}

IMPULSE_TEST(ProxyInputMetaUserInfoMissingOrBadIsMinusOne) {
	using Impulse::ProxyInput::ParseMeta;
	const Impulse::ProxyInput::SubscriptionMeta absent = ParseMeta(std::nullopt, std::nullopt, std::nullopt);
	IMPULSE_CHECK(absent.upload == -1);
	IMPULSE_CHECK(absent.download == -1);
	IMPULSE_CHECK(absent.total == -1);
	IMPULSE_CHECK(absent.expire == -1);
	const Impulse::ProxyInput::SubscriptionMeta bad = ParseMeta(
		std::nullopt,
		std::string_view("upload=1;download=x;total=3=4"),
		std::nullopt);
	IMPULSE_CHECK(bad.upload == 1);
	IMPULSE_CHECK(bad.download == -1);
	IMPULSE_CHECK(bad.total == -1);
	IMPULSE_CHECK(bad.expire == -1);
}

IMPULSE_TEST(ProxyInputMetaUpdateIntervalDefaultsAndFloor) {
	using Impulse::ProxyInput::ParseMeta;
	IMPULSE_CHECK(ParseMeta(std::nullopt, std::nullopt, std::nullopt).updateIntervalHours == 24);
	IMPULSE_CHECK(ParseMeta(std::nullopt, std::nullopt, std::string_view("0")).updateIntervalHours == 1);
	IMPULSE_CHECK(ParseMeta(std::nullopt, std::nullopt, std::string_view("-5")).updateIntervalHours == 1);
	IMPULSE_CHECK(ParseMeta(std::nullopt, std::nullopt, std::string_view("6")).updateIntervalHours == 6);
	IMPULSE_CHECK(ParseMeta(std::nullopt, std::nullopt, std::string_view(" 12 ")).updateIntervalHours == 12);
	IMPULSE_CHECK(ParseMeta(std::nullopt, std::nullopt, std::string_view("abc")).updateIntervalHours == 24);
	IMPULSE_CHECK(ParseMeta(std::nullopt, std::nullopt, std::string_view("3000000000")).updateIntervalHours == 24);
}
