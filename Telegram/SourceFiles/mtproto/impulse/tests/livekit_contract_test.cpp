/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/impulse/core/livekit_contract.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <optional>
#include <string>

namespace {

[[nodiscard]] std::string JoinJson(
		const std::string &url,
		const std::string &token,
		const std::string &room) {
	return std::string("{\"livekit\":{\"url\":\"") + url
		+ "\",\"token\":\"" + token
		+ "\",\"room\":\"" + room + "\"}}";
}

} // namespace

IMPULSE_TEST(LiveKitOneToOneRoomName) {
	using Impulse::LiveKit::OneToOneRoom;
	IMPULSE_CHECK(OneToOneRoom(42) == "p.42");
	IMPULSE_CHECK(OneToOneRoom(1) == "p.1");
}

IMPULSE_TEST(LiveKitJoinAcceptsValidWss) {
	using Impulse::LiveKit::ParseOneToOneJoin;
	const std::string json = JoinJson(
		"wss://project.livekit.cloud",
		"token-value",
		"p.7");
	const std::optional<Impulse::LiveKit::JoinParameters> parsed = ParseOneToOneJoin(json, 7);
	IMPULSE_CHECK(parsed.has_value());
	if (parsed) {
		IMPULSE_CHECK(parsed->url == "wss://project.livekit.cloud");
		IMPULSE_CHECK(parsed->token == "token-value");
		IMPULSE_CHECK(parsed->room == "p.7");
	}
}

IMPULSE_TEST(LiveKitJoinAcceptsWsAndPort) {
	using Impulse::LiveKit::ParseOneToOneJoin;
	IMPULSE_CHECK(ParseOneToOneJoin(
		JoinJson("ws://127.0.0.1:7880/rtc", "t", "p.3"),
		3).has_value());
	IMPULSE_CHECK(ParseOneToOneJoin(
		JoinJson("wss://[::1]:443", "t", "p.3"),
		3).has_value());
}

IMPULSE_TEST(LiveKitJoinRejectsWrongScheme) {
	using Impulse::LiveKit::ParseOneToOneJoin;
	IMPULSE_CHECK(!ParseOneToOneJoin(
		JoinJson("https://project.livekit.cloud", "t", "p.3"),
		3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin(
		JoinJson("project.livekit.cloud", "t", "p.3"),
		3).has_value());
}

IMPULSE_TEST(LiveKitJoinRejectsUserinfoFragmentAndNoHost) {
	using Impulse::LiveKit::ParseOneToOneJoin;
	IMPULSE_CHECK(!ParseOneToOneJoin(
		JoinJson("wss://user@project.livekit.cloud", "t", "p.3"),
		3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin(
		JoinJson("wss://project.livekit.cloud#frag", "t", "p.3"),
		3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin(
		JoinJson("wss://", "t", "p.3"),
		3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin(
		JoinJson("wss://:443", "t", "p.3"),
		3).has_value());
}

IMPULSE_TEST(LiveKitJoinRejectsRoomMismatchAndBadCallId) {
	using Impulse::LiveKit::ParseOneToOneJoin;
	const std::string url = std::string("wss://project.livekit.cloud");
	IMPULSE_CHECK(!ParseOneToOneJoin(JoinJson(url, "t", "p.4"), 3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin(JoinJson(url, "t", "g.3"), 3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin(JoinJson(url, "t", "p.3"), 0).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin(JoinJson(url, "t", "p.3"), -3).has_value());
}

IMPULSE_TEST(LiveKitJoinRejectsBlankAndMissingFields) {
	using Impulse::LiveKit::ParseOneToOneJoin;
	IMPULSE_CHECK(!ParseOneToOneJoin(
		JoinJson("wss://project.livekit.cloud", " ", "p.3"),
		3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin(
		JoinJson("wss://project.livekit.cloud", "t", ""),
		3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin(
		"{\"livekit\":{\"url\":\"wss://project.livekit.cloud\"}}",
		3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin("{}", 3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin("[]", 3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin("not json", 3).has_value());
}

IMPULSE_TEST(LiveKitJoinRejectsUriIllegalCharacters) {
	using Impulse::LiveKit::ParseOneToOneJoin;
	IMPULSE_CHECK(!ParseOneToOneJoin(
		JoinJson("wss://project.livekit.cloud/a{b", "t", "p.3"),
		3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin(
		JoinJson("wss://project.livekit.cloud/a%zz", "t", "p.3"),
		3).has_value());
	IMPULSE_CHECK(!ParseOneToOneJoin(
		JoinJson("wss://project.livekit.cloud/a[b", "t", "p.3"),
		3).has_value());
	IMPULSE_CHECK(ParseOneToOneJoin(
		JoinJson("wss://project.livekit.cloud/a%20b", "t", "p.3"),
		3).has_value());
}

IMPULSE_TEST(LiveKitJoinRejectsNonStringFields) {
	using Impulse::LiveKit::ParseOneToOneJoin;
	IMPULSE_CHECK(!ParseOneToOneJoin(
		"{\"livekit\":{\"url\":5,\"token\":\"t\",\"room\":\"p.3\"}}",
		3).has_value());
}
