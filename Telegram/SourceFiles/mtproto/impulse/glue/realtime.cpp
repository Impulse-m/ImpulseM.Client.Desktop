#include "mtproto/impulse/glue/realtime.h"

#include "mtproto/impulse/core/envelope.h"
#include "mtproto/impulse/core/transcoder.h"

#include <array>
#include <exception>
#include <string_view>
#include <utility>

namespace Impulse {

namespace {

constexpr std::string_view kUserLanePrefix = "user:";
constexpr std::string_view kChannelLanePrefix = "channel:";

// Constructors of the TL Updates type: updates, updatesCombined, updateShort,
// updateShortMessage, updateShortChatMessage, updateShortSentMessage and
// updatesTooLong.
constexpr std::uint32_t kUpdatesTooLongConstructor = 0xe317af7e;
constexpr std::array<std::uint32_t, 7> kUpdatesConstructors = {
	0x74ae4240,
	0x725b04c3,
	0x78d4dec1,
	0x313bc7f8,
	0x4d6deea5,
	0x9015e101,
	kUpdatesTooLongConstructor,
};

// Reasons of the difference requests, named as the Android client names them.
constexpr std::string_view kReasonPublicationDecodeFailed = "publication_decode_failed";
constexpr std::string_view kReasonUnexpectedConstructor = "publication_unexpected_constructor";
constexpr std::string_view kReasonUpdatesTooLong = "updates_too_long";
constexpr std::string_view kReasonSubscribePositionDropped = "subscribe_position_dropped";
constexpr std::string_view kReasonSubscribeNotRecovered = "subscribe_not_recovered";

[[nodiscard]] bool IsUserLane(std::string_view channel) {
	return channel.starts_with(kUserLanePrefix);
}

[[nodiscard]] bool IsChannelLane(std::string_view channel) {
	return channel.starts_with(kChannelLanePrefix);
}

// Reads the constructor of a serialized TL object: a little-endian int32.
// The caller checks the size first.
[[nodiscard]] std::uint32_t ReadConstructor(const std::vector<std::uint8_t> &bytes) {
	return static_cast<std::uint32_t>(bytes[0])
		| (static_cast<std::uint32_t>(bytes[1]) << 8)
		| (static_cast<std::uint32_t>(bytes[2]) << 16)
		| (static_cast<std::uint32_t>(bytes[3]) << 24);
}

[[nodiscard]] bool IsUpdatesConstructor(std::uint32_t constructor) {
	for (const std::uint32_t known : kUpdatesConstructors) {
		if (known == constructor) {
			return true;
		}
	}
	return false;
}

} // namespace

Realtime::Realtime(
		const Transcoder &transcoderRef,
		UpdatesSink updates,
		DifferenceRequest difference,
		UserLaneSubscribed userLane)
: transcoder(transcoderRef)
, updatesSink(std::move(updates))
, differenceRequest(std::move(difference))
, userLaneSubscribed(std::move(userLane))
, alive(std::make_shared<bool>(true)) {
}

Realtime::~Realtime() {
	*alive = false;
}

std::int32_t Realtime::subscribe(
		CentrifugeClient &client,
		const std::string &channel) {
	std::shared_ptr<bool> keepAlive = alive;
	return client.subscribe(
		channel,
		[this, keepAlive, channel](const CentrifugeSubscriptionEvent &event) {
			if (*keepAlive) {
				onEvent(channel, event);
			}
		});
}

std::int32_t Realtime::unsubscribe(
		CentrifugeClient &client,
		const std::string &channel) {
	return client.unsubscribe(channel);
}

void Realtime::onEvent(
		const std::string &channel,
		const CentrifugeSubscriptionEvent &event) {
	switch (event.kind) {
	case CentrifugeSubscriptionKind::Subscribing:
		// The client resubscribes on its own after code 2502 and reports it
		// here. The gap tracker must see the dropped position, or the next
		// subscribe would look like a recovery.
		if (event.code == static_cast<std::uint32_t>(kPositionDroppedCode)) {
			gaps.onUnsubscribed(channel, kPositionDroppedCode);
		}
		break;

	case CentrifugeSubscriptionKind::Subscribed:
		onSubscribed(channel, event.recovered, event.wasRecovering);
		break;

	case CentrifugeSubscriptionKind::Publication:
		onPublication(event.data);
		break;

	case CentrifugeSubscriptionKind::Unsubscribed:
		gaps.onUnsubscribed(channel, static_cast<int>(event.code));
		break;

	case CentrifugeSubscriptionKind::Refused:
		// A refused lane carries no updates. The client reports the refusal
		// itself, and the gap is judged on the next subscribe of the lane.
		break;
	}
}

void Realtime::onSubscribed(
		const std::string &channel,
		bool recovered,
		bool wasRecovering) {
	const GapAction action = gaps.onSubscribed(channel, recovered, wasRecovering);
	if (action == GapAction::AccountDifference) {
		requestDifference(
			recovered ? kReasonSubscribePositionDropped : kReasonSubscribeNotRecovered,
			std::string());
	} else if (action == GapAction::ChannelDifference) {
		// Only a channel lane names its channel; the other lanes fall back to
		// the account difference.
		requestDifference(
			recovered ? kReasonSubscribePositionDropped : kReasonSubscribeNotRecovered,
			IsChannelLane(channel) ? channel : std::string());
	}
	if (IsUserLane(channel) && userLaneSubscribed) {
		userLaneSubscribed();
	}
}

void Realtime::onPublication(const std::vector<std::uint8_t> &data) {
	const std::string_view text(
		reinterpret_cast<const char *>(data.data()),
		data.size());
	const Envelope envelope = ParseEnvelope(text);
	switch (envelope.kind) {
	case Envelope::Kind::Undecodable:
	case Envelope::Kind::LegacySessionRevoked:
		// The legacy session-revoked array carries no TL body. Android reads
		// it as undecodable and fetches the difference, so this does the same.
		requestDifference(kReasonPublicationDecodeFailed, std::string());
		break;

	case Envelope::Kind::UpdatesTooLong:
		requestDifference(kReasonUpdatesTooLong, std::string());
		break;

	case Envelope::Kind::PtsLess:
	case Envelope::Kind::Numbered:
	case Envelope::Kind::Unnumbered:
		decodeAndDeliver(envelope.updatesProto);
		break;
	}
}

void Realtime::decodeAndDeliver(const std::vector<std::uint8_t> &updatesProto) {
	std::vector<std::uint8_t> updates;
	try {
		updates = transcoder.decodeUpdates(updatesProto, TranscodeCaptureSink{});
	} catch (const std::exception &) {
		requestDifference(kReasonPublicationDecodeFailed, std::string());
		return;
	}
	if (updates.size() < 4) {
		requestDifference(kReasonPublicationDecodeFailed, std::string());
		return;
	}

	const std::uint32_t constructor = ReadConstructor(updates);
	if (!IsUpdatesConstructor(constructor)) {
		requestDifference(kReasonUnexpectedConstructor, std::string());
		return;
	}
	if (constructor == kUpdatesTooLongConstructor) {
		// The too-long frame is a difference trigger, not an update to apply.
		requestDifference(kReasonUpdatesTooLong, std::string());
		return;
	}
	if (updatesSink) {
		updatesSink(updates);
	}
}

void Realtime::requestDifference(
		std::string_view reason,
		const std::string &channel) {
	if (differenceRequest) {
		differenceRequest(std::string(reason), channel);
	}
}

} // namespace Impulse
