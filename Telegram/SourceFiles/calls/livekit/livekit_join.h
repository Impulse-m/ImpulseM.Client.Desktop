/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/impulse/core/livekit_contract.h"
#include "mtproto/sender.h"

#include <optional>

namespace Impulse {

// Reads the 1:1 join parameters from custom_parameters of a phoneCall and
// validates them with the core contract. Gives nullopt when the field is
// absent or the parameters are invalid. The result holds the bearer token,
// so it must never be logged.
[[nodiscard]] std::optional<LiveKit::JoinParameters> ReadOneToOneJoin(
	const MTPDphoneCall &call);

} // namespace Impulse
