// Realtime publication envelope: UTF-8 JSON that wraps one protobuf Updates body.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace Impulse {

// Realtime update type of the updatesTooLong frame (the server UpdateType.UpdatesTooLong).
inline constexpr std::int32_t kUpdatesTooLongType = 221;

struct Envelope {
	enum class Kind : std::uint8_t {
		Undecodable,
		PtsLess,
		Numbered,
		Unnumbered,
		UpdatesTooLong,
		LegacySessionRevoked,
	};

	Kind kind = Kind::Undecodable;
	std::int32_t type = 0;
	std::int64_t pts = 0;
	std::int32_t ptsCount = 0;
	std::int32_t date = 0;

	// Serialized v1.Updates protobuf; empty for the undecodable and legacy kinds.
	std::vector<std::uint8_t> updatesProto;

	// An update that declares no pts: the frame has neither pts nor ptsCount.
	[[nodiscard]] static Envelope PtsLess(
		std::int32_t type,
		std::int32_t date,
		std::vector<std::uint8_t> updatesProto);

	// An update that declares the pts it leaves the client at and the width it consumed.
	[[nodiscard]] static Envelope Numbered(
		std::int32_t type,
		std::int64_t pts,
		std::int32_t ptsCount,
		std::int32_t date,
		std::vector<std::uint8_t> updatesProto);

	// A realtime-only frame that spends no pts: pts 0 and ptsCount 0 are written explicitly.
	[[nodiscard]] static Envelope Unnumbered(
		std::int32_t type,
		std::int32_t date,
		std::vector<std::uint8_t> updatesProto);

	// The frame that tells the client to fetch the difference: body is an updatesTooLong Updates.
	[[nodiscard]] static Envelope UpdatesTooLong(
		std::int32_t date,
		std::vector<std::uint8_t> updatesProto);

	// The legacy session-revoked broadcast, a JSON array of session objects on the user lane.
	[[nodiscard]] static Envelope LegacySessionRevoked();

	// The publication could not be read; the caller should fetch the difference.
	[[nodiscard]] static Envelope Undecodable();

	[[nodiscard]] bool decodable() const {
		return kind != Kind::Undecodable;
	}
};

// Never throws. Returns an Undecodable envelope for anything that breaks the
// rules: invalid JSON, a missing or empty or non-base64 data, a type or date
// that is not a positive int32, a pts below zero or a ptsCount outside int32,
// and an empty legacy array or a legacy array with a non-object element.
[[nodiscard]] Envelope ParseEnvelope(std::string_view text);

} // namespace Impulse
