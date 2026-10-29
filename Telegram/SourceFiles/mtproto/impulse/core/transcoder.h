// Transcoder between canonical TL and the protobuf payload of a method, and
// the TL reading primitives. This file holds the request encode path; the
// decode path (results, captures and updates) is added in the same files.
#pragma once

#include "mtproto/impulse/core/mapping.h"
#include "mtproto/impulse/core/method_classes.h"
#include "mtproto/impulse/core/wire.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Impulse {

// Encode failure of a local CLIENT_* class. Unknown methods use
// kMethodNotMappedType from method_classes.h instead.
inline constexpr std::string_view kTranscodeFailedType = "CLIENT_TRANSCODE_FAILED";

class TranscodeError final : public std::runtime_error {
public:
	TranscodeError(std::string_view type, std::string_view message);

	[[nodiscard]] std::string_view type() const noexcept;

private:
	std::string errorType;
};

// Reads TL primitives. Every read past the end throws TranscodeError.
class TlReader final {
public:
	explicit TlReader(std::span<const std::uint8_t> source);

	std::int32_t readInt32();
	std::int64_t readInt64();

	// A TL string or bytes value: a one byte length, or 0xFE and a 3-byte
	// length for values of 254 bytes and more, padded to a multiple of four.
	std::span<const std::uint8_t> readBytes();
	std::span<const std::uint8_t> readRaw(std::size_t count);

	[[nodiscard]] std::size_t remaining() const noexcept;

private:
	void require(std::size_t count) const;

	std::span<const std::uint8_t> data;
	std::size_t position = 0;
};

struct EncodedRequest {
	std::uint32_t methodId = 0;
	std::string method;
	std::vector<std::uint8_t> payload;
	std::optional<std::int64_t> takeoutId;
};

// The mapping table must outlive the transcoder.
class Transcoder final {
public:
	explicit Transcoder(const MappingTable &mapping);

	// Encodes a TL request, wrappers included, into the method payload. The
	// invokeWithTakeout id is reported in takeoutId; the carrier forwards it.
	[[nodiscard]] EncodedRequest encodeRequest(
		std::span<const std::uint8_t> tl) const;

	// Encodes one TL object of the given type into its protobuf message.
	[[nodiscard]] std::vector<std::uint8_t> encodeObject(
		std::string_view tlType,
		std::span<const std::uint8_t> tl) const;

private:
	[[nodiscard]] const MappingMethod *findMethod(std::uint32_t id) const;
	bool skipWrapper(std::uint32_t id, TlReader &reader) const;
	std::vector<std::uint8_t> objectToProto(
		std::string_view tlType,
		TlReader &reader,
		std::size_t depth) const;
	void encodeParams(
		TlReader &reader,
		const std::vector<MappingField> &params,
		ProtoWriter &writer,
		std::size_t depth) const;
	void encodeValue(
		TlReader &reader,
		const MappingField &field,
		ProtoWriter &writer,
		std::size_t depth) const;
	void encodeVector(
		TlReader &reader,
		const MappingField &field,
		ProtoWriter &writer,
		std::size_t depth) const;

	const MappingTable &table;
	std::unordered_map<std::uint32_t, const MappingMethod *> methodById;
};

} // namespace Impulse
