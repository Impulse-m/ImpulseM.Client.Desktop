/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/impulse/core/livekit_contract.h"

#include "mtproto/impulse/core/json.h"

#include <algorithm>
#include <string>
#include <utility>

namespace Impulse::LiveKit {
namespace {

constexpr std::string_view kRoomPrefix = "p.";
constexpr std::string_view kWsScheme = "ws";
constexpr std::string_view kWssScheme = "wss";
constexpr std::string_view kAuthorityMarker = "//";
constexpr std::string_view kUriIllegalChars = "\"<>\\^`{|}";

// Mirrors Java String.trim(): every character up to U+0020 counts as blank.
[[nodiscard]] bool IsBlank(const std::string &text) {
	for (const char ch : text) {
		if (static_cast<unsigned char>(ch) > ' ') {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool IsHexDigit(char ch) {
	return (ch >= '0' && ch <= '9')
		|| (ch >= 'a' && ch <= 'f')
		|| (ch >= 'A' && ch <= 'F');
}

// Mirrors the Java URI checks of the Android join validation: no characters
// that URI.create rejects, a ws or wss scheme, an authority with a host, no
// userinfo and no fragment.
[[nodiscard]] bool IsValidJoinUrl(const std::string &url) {
	for (const char ch : url) {
		const auto code = static_cast<unsigned char>(ch);
		if (code <= ' '
			|| code == 0x7F
			|| kUriIllegalChars.find(ch) != std::string_view::npos) {
			return false;
		}
	}
	for (std::size_t i = 0; i < url.size(); ++i) {
		if (url[i] == '%'
			&& (i + 2 >= url.size()
				|| !IsHexDigit(url[i + 1])
				|| !IsHexDigit(url[i + 2]))) {
			return false;
		}
	}
	const auto schemeEnd = url.find(':');
	if (schemeEnd == std::string::npos) {
		return false;
	}
	const auto scheme = std::string_view(url).substr(0, schemeEnd);
	if (scheme != kWsScheme && scheme != kWssScheme) {
		return false;
	}
	const auto markerStart = schemeEnd + 1;
	if (url.compare(markerStart, kAuthorityMarker.size(), kAuthorityMarker)) {
		return false;
	}
	if (url.find('#') != std::string::npos) {
		return false;
	}
	const auto authorityStart = schemeEnd + 1 + kAuthorityMarker.size();
	const auto authorityEnd = url.find_first_of("/?", authorityStart);
	const auto authority = std::string_view(url).substr(
		authorityStart,
		(authorityEnd == std::string::npos)
			? std::string::npos
			: authorityEnd - authorityStart);
	if (authority.find('@') != std::string_view::npos) {
		return false;
	}
	auto host = authority;
	if (!host.empty() && host.front() == '[') {
		const auto close = host.find(']');
		if (close == std::string_view::npos) {
			return false;
		}
		host = host.substr(0, close + 1);
	} else {
		host = host.substr(0, host.find(':'));
	}
	// Brackets are legal in java.net.URI only around an IPv6 host.
	const auto ipv6 = !host.empty() && host.front() == '[';
	const auto openCount = std::count(url.begin(), url.end(), '[');
	const auto closeCount = std::count(url.begin(), url.end(), ']');
	if (openCount != (ipv6 ? 1 : 0) || closeCount != openCount) {
		return false;
	}
	return !host.empty();
}

[[nodiscard]] std::optional<std::string> RequiredString(
		const Json::Value &object,
		std::string_view name) {
	const auto value = object.find(name);
	if (!value) {
		return std::nullopt;
	}
	const auto text = value->asString();
	if (!text || IsBlank(*text)) {
		return std::nullopt;
	}
	return *text;
}

} // namespace

std::string OneToOneRoom(std::int64_t callId) {
	return std::string(kRoomPrefix) + std::to_string(callId);
}

std::optional<JoinParameters> ParseOneToOneJoin(
		std::string_view json,
		std::int64_t callId) {
	// The parser message is dropped on purpose: it can echo the bearer token.
	if (callId <= 0) {
		return std::nullopt;
	}
	const auto parsed = Json::parse(json);
	if (!parsed.value || !parsed.value->asObject()) {
		return std::nullopt;
	}
	const auto *livekit = parsed.value->find("livekit");
	if (!livekit || !livekit->asObject()) {
		return std::nullopt;
	}
	auto url = RequiredString(*livekit, "url");
	auto token = RequiredString(*livekit, "token");
	auto room = RequiredString(*livekit, "room");
	if (!url || !token || !room) {
		return std::nullopt;
	}
	if (!IsValidJoinUrl(*url) || *room != OneToOneRoom(callId)) {
		return std::nullopt;
	}
	return JoinParameters{
		.url = std::move(*url),
		.token = std::move(*token),
		.room = std::move(*room),
	};
}

} // namespace Impulse::LiveKit
