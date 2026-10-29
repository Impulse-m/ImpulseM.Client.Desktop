/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/livekit/livekit_join.h"

#include <string_view>

namespace Impulse {

std::optional<LiveKit::JoinParameters> ReadOneToOneJoin(
		const MTPDphoneCall &call) {
	// The join JSON of a 1:1 call arrives in custom_parameters only. The
	// carrier is open on Impulse-m/ImpulseM#1038.
	if (!call.vcustom_parameters().has_value()) {
		return std::nullopt;
	}
	const QByteArray &json = call.vcustom_parameters()->c_dataJSON().vdata().v;
	return LiveKit::ParseOneToOneJoin(
		std::string_view(json.constData(), size_t(json.size())),
		int64(call.vid().v));
}

} // namespace Impulse
