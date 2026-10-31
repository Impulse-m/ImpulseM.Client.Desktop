#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/core/impulse_links.h"

#include <optional>
#include <string>

IMPULSE_TEST(LinkParseImpulseHttpsShortLink) {
	const std::optional<std::string> path = Impulse::Links::ShortLinkPath(
		"https://o.example.org/x",
		"o.example.org");
	IMPULSE_CHECK(path.has_value());
	IMPULSE_CHECK(*path == "x");
}

IMPULSE_TEST(LinkParseImpulseHostIgnoresCase) {
	const std::optional<std::string> path = Impulse::Links::ShortLinkPath(
		"HTTPS://O.Example.ORG/Some_Name",
		"o.example.org");
	IMPULSE_CHECK(path.has_value());
	IMPULSE_CHECK(*path == "Some_Name");
}

IMPULSE_TEST(LinkParseImpulseNoConfigHostNoFallback) {
	// The prefix comes from help.getConfig only; without it nothing is ImpulseM.
	IMPULSE_CHECK(!Impulse::Links::ShortLinkPath(
		"https://o.example.org/x",
		"").has_value());
}

IMPULSE_TEST(LinkParseConfiguredHostOtherThanDefault) {
	const std::optional<std::string> path = Impulse::Links::ShortLinkPath(
		"https://dev.example.test/x",
		"dev.example.test");
	IMPULSE_CHECK(path.has_value());
	IMPULSE_CHECK(*path == "x");
	IMPULSE_CHECK(!Impulse::Links::ShortLinkPath(
		"https://o.example.org/x",
		"dev.example.test").has_value());
}

IMPULSE_TEST(LinkParseLegacyTelegramHosts) {
	const std::optional<std::string> tme = Impulse::Links::ShortLinkPath(
		"https://t.me/durov",
		"o.example.org");
	IMPULSE_CHECK(tme.has_value());
	IMPULSE_CHECK(*tme == "durov");
	const std::optional<std::string> www = Impulse::Links::ShortLinkPath(
		"www.telegram.me/durov",
		"");
	IMPULSE_CHECK(www.has_value());
	IMPULSE_CHECK(*www == "durov");
	IMPULSE_CHECK(Impulse::Links::ShortLinkPath("telegram.dog/x", "").has_value());
}

IMPULSE_TEST(LinkParseRejectsEmptyPathAndOtherHosts) {
	IMPULSE_CHECK(!Impulse::Links::ShortLinkPath("https://t.me/", "").has_value());
	IMPULSE_CHECK(!Impulse::Links::ShortLinkPath("https://t.me", "").has_value());
	IMPULSE_CHECK(!Impulse::Links::ShortLinkPath(
		"https://evil-t.me/x",
		"").has_value());
	IMPULSE_CHECK(!Impulse::Links::ShortLinkPath(
		"https://example.com/x",
		"o.example.org").has_value());
}

IMPULSE_TEST(LinkParseTgSchemeIsInternal) {
	IMPULSE_CHECK(Impulse::Links::IsInternalScheme("tg://resolve?domain=x"));
	IMPULSE_CHECK(Impulse::Links::IsInternalScheme("TG://resolve?domain=x"));
	IMPULSE_CHECK(!Impulse::Links::IsInternalScheme("https://o.example.org/x"));
	IMPULSE_CHECK(!Impulse::Links::IsInternalScheme("tg:"));
	IMPULSE_CHECK(!Impulse::Links::ShortLinkPath("tg://resolve?domain=x", "").has_value());
}

IMPULSE_TEST(LinkParseImSchemeIsInternal) {
	IMPULSE_CHECK(Impulse::Links::IsInternalScheme("im://resolve?domain=x"));
	IMPULSE_CHECK(Impulse::Links::IsInternalScheme("IM://resolve?domain=x"));
	IMPULSE_CHECK(!Impulse::Links::IsInternalScheme("im:"));
	IMPULSE_CHECK(Impulse::Links::ToInternalScheme("im://login?token=abc") == "tg://login?token=abc");
	IMPULSE_CHECK(Impulse::Links::ToInternalScheme("IM://join?invite=x") == "tg://join?invite=x");
	IMPULSE_CHECK(Impulse::Links::ToInternalScheme("tg://resolve?domain=x") == "tg://resolve?domain=x");
	IMPULSE_CHECK(Impulse::Links::ToInternalScheme("https://t.me/x") == "https://t.me/x");
}

IMPULSE_TEST(LinkParsePrefixHost) {
	IMPULSE_CHECK(Impulse::Links::PrefixHost("https://o.example.org/") == "o.example.org");
	IMPULSE_CHECK(Impulse::Links::PrefixHost("https://O.Example.org:8443/") == "o.example.org");
	IMPULSE_CHECK(Impulse::Links::PrefixHost("http://dev.example.test") == "dev.example.test");
	IMPULSE_CHECK(Impulse::Links::PrefixHost("") == "");
}
