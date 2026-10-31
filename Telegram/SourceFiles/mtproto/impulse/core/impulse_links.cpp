#include "mtproto/impulse/core/impulse_links.h"

#include <cctype>
#include <cstddef>

namespace Impulse::Links {
namespace {

[[nodiscard]] std::string LowerAscii(std::string_view text) {
	std::string result(text);
	for (char &symbol : result) {
		symbol = static_cast<char>(
			std::tolower(static_cast<unsigned char>(symbol)));
	}
	return result;
}

// The prefix must be lowercase ASCII.
[[nodiscard]] bool StartsWithIgnoreCase(
		std::string_view text,
		std::string_view prefix) {
	return text.size() >= prefix.size()
		&& LowerAscii(text.substr(0, prefix.size())) == prefix;
}

[[nodiscard]] std::string_view WithoutScheme(std::string_view url) {
	if (StartsWithIgnoreCase(url, "https://")) {
		return url.substr(8);
	} else if (StartsWithIgnoreCase(url, "http://")) {
		return url.substr(7);
	}
	return url;
}

} // namespace

std::string PrefixHost(std::string_view prefix) {
	const std::string_view afterScheme = WithoutScheme(prefix);
	const std::string_view authority = afterScheme.substr(
		0,
		afterScheme.find('/'));
	const std::string_view host = authority.substr(
		0,
		authority.find(':'));
	return LowerAscii(host);
}

std::optional<std::string> ShortLinkPath(
		std::string_view url,
		std::string_view configHost) {
	const std::string_view rest = WithoutScheme(url);
	const std::size_t slash = rest.find('/');
	if (slash == std::string_view::npos || slash + 1 >= rest.size()) {
		return std::nullopt;
	}

	std::string host = LowerAscii(rest.substr(0, slash));
	if (host.starts_with("www.")) {
		host.erase(0, 4);
	}
	const std::string configured = LowerAscii(configHost);
	const bool recognised = host == "t.me"
		|| host == "telegram.me"
		|| host == "telegram.dog"
		|| (!configured.empty() && host == configured);
	if (!recognised) {
		return std::nullopt;
	}
	return std::string(rest.substr(slash + 1));
}

bool IsInternalScheme(std::string_view url) {
	return StartsWithIgnoreCase(url, "tg://")
		|| StartsWithIgnoreCase(url, "im://");
}

std::string ToInternalScheme(std::string_view url) {
	if (!StartsWithIgnoreCase(url, "im://")) {
		return std::string(url);
	}
	return "tg://" + std::string(url.substr(5));
}

} // namespace Impulse::Links
