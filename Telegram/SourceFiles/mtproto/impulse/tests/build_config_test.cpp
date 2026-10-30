#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/core/build_config.h"

#include <string>

IMPULSE_TEST(BuildConfigNormalizeUrlStripsTrailingSlashes) {
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeUrl("https://example.org/") == "https://example.org");
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeUrl("https://example.org///") == "https://example.org");
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeUrl("https://example.org/k") == "https://example.org/k");
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeUrl("/") == "");
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeUrl("") == "");
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeUrl("  https://example.org/ \t") == "https://example.org");
}

IMPULSE_TEST(BuildConfigNormalizeLinkHostLowercasesAndStripsScheme) {
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeLinkHost("o.example.org") == "o.example.org");
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeLinkHost("O.Example.ORG") == "o.example.org");
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeLinkHost("https://O.Example.org/") == "o.example.org");
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeLinkHost("HTTP://o.example.org//") == "o.example.org");
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeLinkHost("dev.example.test:8443") == "dev.example.test:8443");
}

IMPULSE_TEST(BuildConfigNormalizeLinkHostTrimsWhitespace) {
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeLinkHost("  o.example.org \r\n") == "o.example.org");
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeLinkHost(" https://o.example.org/ \t") == "o.example.org");
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeLinkHost("   ") == "");
	IMPULSE_CHECK(Impulse::BuildConfig::NormalizeLinkHost("") == "");
}
