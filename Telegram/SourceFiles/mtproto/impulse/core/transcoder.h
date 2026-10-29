// Transcoder between canonical TL and the protobuf payload of a method, and
// the TL reading and writing primitives. The request encode path builds the
// method payload; the decode path turns server results, captures and updates
// back into TL.
#pragma once

#include "mtproto/impulse/core/mapping.h"
#include "mtproto/impulse/core/method_classes.h"
#include "mtproto/impulse/core/wire.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Impulse {

// Encode or decode failure of a local CLIENT_* class. Unknown methods use
// kMethodNotMappedType from method_classes.h instead.
inline constexpr std::string_view kTranscodeFailedType = "CLIENT_TRANSCODE_FAILED";

// A server result that does not decode into its TL object. Updates never use
// it: an undecodable batch triggers a difference instead.
inline constexpr std::string_view kResponseParseFailedType = "CLIENT_RESPONSE_PARSE_FAILED";

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

// Appends TL primitives to a buffer. Little-endian, as TlReader reads them.
class TlWriter final {
public:
	void writeInt32(std::int32_t value);
	void writeInt64(std::int64_t value);

	// A TL string or bytes value: the same framing TlReader::readBytes reads.
	void writeBytes(std::span<const std::uint8_t> value);
	void writeRaw(std::span<const std::uint8_t> value);

	[[nodiscard]] const std::vector<std::uint8_t> &bytes() const;
	[[nodiscard]] std::vector<std::uint8_t> takeBytes();

private:
	std::vector<std::uint8_t> out;
};

struct EncodedRequest {
	std::uint32_t methodId = 0;
	std::string method;
	std::vector<std::uint8_t> payload;
	std::optional<std::int64_t> takeoutId;
};

// Called for every captured string of a constructor: the proto
// message name, the TL capture name and the UTF-8 value. An empty function
// captures nothing.
using TranscodeCaptureSink = std::function<void(
	std::string_view protoMessage,
	std::string_view fieldName,
	std::string_view value)>;

// The mapping table must outlive the transcoder.
class Transcoder final {
public:
	// One occurrence of a proto field, in wire order. For a fixed32 value the
	// bits are sign-extended, as the reference decoder does.
	struct Occurrence {
		ProtoWireType wire = ProtoWireType::Varint;
		std::uint64_t value = 0;
		std::span<const std::uint8_t> bytes;
	};

	// The occurrences of every field number of one proto message.
	struct ProtoBody {
		std::unordered_map<std::uint32_t, std::vector<Occurrence>> fields;
	};

	explicit Transcoder(const MappingTable &mapping);

	// Encodes a TL request, wrappers included, into the method payload. The
	// invokeWithTakeout id is reported in takeoutId; the carrier forwards it.
	[[nodiscard]] EncodedRequest encodeRequest(
		std::span<const std::uint8_t> tl) const;

	// Encodes one TL object of the given type into its protobuf message.
	[[nodiscard]] std::vector<std::uint8_t> encodeObject(
		std::string_view tlType,
		std::span<const std::uint8_t> tl) const;

	// Decodes the proto result of a method into its TL result. An unknown
	// method is CLIENT_METHOD_NOT_MAPPED; any other failure is
	// CLIENT_RESPONSE_PARSE_FAILED.
	[[nodiscard]] std::vector<std::uint8_t> decodeResult(
		std::uint32_t methodId,
		std::span<const std::uint8_t> proto,
		const TranscodeCaptureSink &sink) const;

	// Decodes a realtime Updates message into TL Updates. Failures are
	// CLIENT_TRANSCODE_FAILED.
	[[nodiscard]] std::vector<std::uint8_t> decodeUpdates(
		std::span<const std::uint8_t> proto,
		const TranscodeCaptureSink &sink) const;

	// Decodes one proto message of the given TL type into its TL object.
	// Failures are CLIENT_TRANSCODE_FAILED.
	[[nodiscard]] std::vector<std::uint8_t> decodeObject(
		std::string_view tlType,
		std::span<const std::uint8_t> proto,
		const TranscodeCaptureSink &sink) const;

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

	void decodeResultSpec(
		const MappingMethod &method,
		std::span<const std::uint8_t> proto,
		TlWriter &writer,
		const TranscodeCaptureSink &sink) const;
	void protoToObject(
		std::string_view tlType,
		std::span<const std::uint8_t> proto,
		TlWriter &writer,
		const TranscodeCaptureSink &sink,
		std::size_t depth) const;
	void decodeParams(
		const std::vector<MappingField> &params,
		const std::vector<MappingField> &captures,
		std::string_view protoMessage,
		const ProtoBody &body,
		TlWriter &writer,
		const TranscodeCaptureSink &sink,
		std::size_t depth) const;
	void decodeValue(
		const MappingField &field,
		const ProtoBody &body,
		TlWriter &writer,
		const TranscodeCaptureSink &sink,
		std::size_t depth) const;
	void writeVector(
		const MappingField &element,
		std::span<const Occurrence> occurrences,
		TlWriter &writer,
		const TranscodeCaptureSink &sink,
		std::size_t depth) const;

	const MappingTable &table;
	std::unordered_map<std::uint32_t, const MappingMethod *> methodById;
	std::unordered_map<std::string, const MappingType *> typeByName;
	std::unordered_map<std::string, std::vector<const MappingConstructor *>> constructorsByType;
};

} // namespace Impulse
