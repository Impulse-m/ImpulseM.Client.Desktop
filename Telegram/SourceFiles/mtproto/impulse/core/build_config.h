// Build variables of the ImpulseM client. The values come from CMake, see
// Telegram/cmake/impulse/build_config.cmake. An unset variable is an empty string.
#pragma once

#include <string>
#include <string_view>

namespace Impulse::BuildConfig {

// Trims spaces, tabs and line breaks from both ends.
[[nodiscard]] inline std::string_view TrimSpaces(std::string_view value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos) {
		return std::string_view();
	}
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

// Removes every slash at the end of the value.
[[nodiscard]] inline std::string_view WithoutTrailingSlashes(
		std::string_view value) {
	const auto last = value.find_last_not_of('/');
	return (last == std::string_view::npos)
		? std::string_view()
		: value.substr(0, last + 1);
}

// Normalises a base URL: trimmed, no trailing slashes, e.g. " https://example.org/" gives
// "https://example.org".
[[nodiscard]] inline std::string NormalizeUrl(std::string_view value) {
	return std::string(WithoutTrailingSlashes(TrimSpaces(value)));
}

// Normalises a share link host: trimmed, lowercase, without a scheme and
// without trailing slashes, e.g. " HTTPS://O.Example.org/ " gives
// "o.example.org".
[[nodiscard]] inline std::string NormalizeLinkHost(std::string_view value) {
	auto result = std::string(TrimSpaces(value));
	for (auto &ch : result) {
		if (ch >= 'A' && ch <= 'Z') {
			ch = static_cast<char>(ch - 'A' + 'a');
		}
	}
	auto host = std::string_view(result);
	if (host.starts_with("https://")) {
		host.remove_prefix(std::string_view("https://").size());
	} else if (host.starts_with("http://")) {
		host.remove_prefix(std::string_view("http://").size());
	}
	return std::string(WithoutTrailingSlashes(host));
}

// Base URL of the transport host, e.g. "https://rpc.example.org".
[[nodiscard]] std::string_view RpcUrl();

// Base URL of the project website without a trailing slash.
[[nodiscard]] std::string_view SiteUrl();

// Host of public share links without a scheme, e.g. "o.example.org".
[[nodiscard]] std::string_view LinkHost();

// URL of the web client.
[[nodiscard]] std::string_view WebUrl();

} // namespace Impulse::BuildConfig
