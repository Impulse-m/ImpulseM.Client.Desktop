/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Impulse::LiveKit {

inline constexpr std::string_view kProtocol = "impulsem-livekit-1";

inline constexpr std::string_view kInvalidJoinError
	= "Invalid LiveKit join parameters for this call";

struct JoinParameters final {
	std::string url;
	std::string token;
	std::string room;
};

// The room name of a 1:1 call: "p." followed by the decimal call id.
[[nodiscard]] std::string OneToOneRoom(std::int64_t callId);

// Validates the join JSON of a 1:1 call. The JSON must hold a
// "livekit" object with non-blank string url, token and room; url must be a
// ws or wss URL with a host and without userinfo or fragment; callId must be
// positive; room must equal OneToOneRoom(callId). The token is never part of
// an error message. Returns nullopt when any check fails.
[[nodiscard]] std::optional<JoinParameters> ParseOneToOneJoin(
	std::string_view json,
	std::int64_t callId);

} // namespace Impulse::LiveKit
