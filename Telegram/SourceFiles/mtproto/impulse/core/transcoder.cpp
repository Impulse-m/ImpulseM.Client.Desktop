#include "mtproto/impulse/core/transcoder.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Impulse {
namespace {

constexpr std::uint32_t kBoolTrueId = 0x997275b5u;
constexpr std::uint32_t kBoolFalseId = 0xbc799737u;
constexpr std::uint32_t kVectorId = 0x1cb5c415u;

// Wrapper constructors of the MTProto query envelope. They are not in the
// mapping file, so the port keeps them as literals.
constexpr std::uint32_t kInvokeAfterMsgId = 0xcb9f372du;
constexpr std::uint32_t kInvokeAfterMsgsId = 0x3dc4b4f0u;
constexpr std::uint32_t kInitConnectionId = 0xc1cd5ea9u;
constexpr std::uint32_t kInvokeWithLayerId = 0xda9b0d0du;
constexpr std::uint32_t kInvokeWithoutUpdatesId = 0xbf9459b7u;
constexpr std::uint32_t kInvokeWithMessagesRangeId = 0x365275f2u;
constexpr std::uint32_t kInvokeWithTakeoutId = 0xaca9fd2eu;
constexpr std::uint32_t kInvokeWithBusinessConnectionId = 0xdd289f8eu;
constexpr std::uint32_t kInvokeWithGooglePlayIntegrityId = 0x1df92984u;
constexpr std::uint32_t kInvokeWithApnsSecretId = 0x0dae54f8u;
constexpr std::uint32_t kInvokeWithReCaptchaId = 0xadbb0f94u;

constexpr std::size_t kMaxDepth = 256;
constexpr std::size_t kInt128Bytes = 16;
constexpr std::size_t kInt256Bytes = 32;
constexpr std::size_t kInitConnectionStringCount = 6;
constexpr std::uint32_t kInitConnectionProxyFlag = 1u;
constexpr std::uint32_t kInitConnectionParamsFlag = 2u;

[[nodiscard]] bool flagBitSet(std::uint32_t flags, std::int32_t bit) {
	return ((flags >> static_cast<std::uint32_t>(bit)) & 1u) != 0;
}

bool readBool(TlReader &reader) {
	const std::uint32_t id = static_cast<std::uint32_t>(reader.readInt32());
	if (id == kBoolTrueId) {
		return true;
	} else if (id == kBoolFalseId) {
		return false;
	}
	throw TranscodeError(kTranscodeFailedType, "bad Bool constructor id");
}

void expectVector(TlReader &reader) {
	if (static_cast<std::uint32_t>(reader.readInt32()) != kVectorId) {
		throw TranscodeError(kTranscodeFailedType, "expected a vector id");
	}
}

void requireConsumed(const TlReader &reader) {
	if (reader.remaining() != 0) {
		throw TranscodeError(
			kTranscodeFailedType,
			"trailing bytes after the TL object");
	}
}

using Occurrence = Transcoder::Occurrence;
using ProtoBody = Transcoder::ProtoBody;

// Occurrences of a proto field number, in wire order. Empty when absent.
[[nodiscard]] std::span<const Occurrence> occurrencesOf(
		const ProtoBody &body,
		std::uint32_t number) {
	const std::unordered_map<std::uint32_t, std::vector<Occurrence>>::const_iterator found
		= body.fields.find(number);
	if (found == body.fields.end()) {
		return {};
	}
	return found->second;
}

[[nodiscard]] ProtoBody parseBody(std::span<const std::uint8_t> proto) {
	ProtoBody body;
	ProtoReader reader(proto);
	while (reader.next()) {
		Occurrence occurrence;
		occurrence.wire = reader.wireType();
		if (occurrence.wire == ProtoWireType::Varint) {
			occurrence.value = reader.readVarint();
		} else if (occurrence.wire == ProtoWireType::Fixed64) {
			occurrence.value = reader.readFixed64();
		} else if (occurrence.wire == ProtoWireType::Fixed32) {
			// Sign-extended, as the reference decoder reads fixed32 values.
			const std::int32_t bits = static_cast<std::int32_t>(reader.readFixed32());
			occurrence.value = static_cast<std::uint64_t>(static_cast<std::int64_t>(bits));
		} else {
			occurrence.bytes = reader.readBytes();
		}
		body.fields[reader.field()].push_back(occurrence);
	}
	return body;
}

[[nodiscard]] std::uint64_t scalarOf(const ProtoBody &body, std::uint32_t number) {
	const std::span<const Occurrence> occurrences = occurrencesOf(body, number);
	if (occurrences.empty()) {
		return std::uint64_t{0};
	}
	return occurrences.back().value;
}

[[nodiscard]] bool isPackableKind(const std::string &kind) {
	return kind == "int"
		|| kind == "long"
		|| kind == "Bool"
		|| kind == "double";
}

[[nodiscard]] const MappingField &findParam(
		const std::vector<MappingField> &params,
		const std::string &name) {
	for (const MappingField &candidate : params) {
		if (candidate.name == name) {
			return candidate;
		}
	}
	throw TranscodeError(
		kTranscodeFailedType,
		"derived_from references an unknown parameter");
}

// Presence of a flagged parameter for its flag bit, as the reference decoder
// reads it: a true flag needs a non-zero last value, a packable vector
// needs one occurrence that is not an empty blob, anything else needs an
// occurrence at all.
[[nodiscard]] bool isPresent(const MappingField &field, const ProtoBody &body) {
	const std::span<const Occurrence> occurrences = occurrencesOf(body, field.number);
	if (occurrences.empty()) {
		return false;
	}
	if (field.kind == "true") {
		return occurrences.back().value != 0;
	}
	if (field.repeated && isPackableKind(field.kind)) {
		for (const Occurrence &occurrence : occurrences) {
			if (occurrence.wire != ProtoWireType::LengthDelimited
				|| !occurrence.bytes.empty()) {
				return true;
			}
		}
		return false;
	}
	return true;
}

[[nodiscard]] std::uint64_t numericValue(
		const MappingField &field,
		const Occurrence &occurrence) {
	if (occurrence.wire != ProtoWireType::Varint
		&& occurrence.wire != ProtoWireType::Fixed32
		&& occurrence.wire != ProtoWireType::Fixed64) {
		throw TranscodeError(
			kTranscodeFailedType,
			"field " + field.name + " must be numeric");
	}
	return occurrence.value;
}

// The bytes of a fixed-size int128 or int256 value. An absent value is an
// error: the reference decoder has no default for these fields.
[[nodiscard]] std::span<const std::uint8_t> fixedBytes(
		const MappingField &field,
		const Occurrence *occurrence,
		std::size_t expected) {
	std::size_t actual = 0;
	if (occurrence) {
		if (occurrence->wire != ProtoWireType::LengthDelimited) {
			throw TranscodeError(
				kTranscodeFailedType,
				"field " + field.name + " must be length-delimited");
		}
		actual = occurrence->bytes.size();
	}
	if (actual != expected) {
		throw TranscodeError(
			kTranscodeFailedType,
			"field " + field.name + " has the wrong size");
	}
	return occurrence->bytes;
}

[[nodiscard]] std::vector<std::uint64_t> packedVarints(
		const MappingField &element,
		std::span<const Occurrence> occurrences) {
	std::vector<std::uint64_t> values;
	for (const Occurrence &occurrence : occurrences) {
		if (occurrence.wire == ProtoWireType::LengthDelimited) {
			const std::vector<std::uint64_t> packed = ProtoReader::unpackVarints(
				occurrence.bytes);
			values.insert(values.end(), packed.begin(), packed.end());
		} else if (occurrence.wire == ProtoWireType::Varint
			|| occurrence.wire == ProtoWireType::Fixed32) {
			values.push_back(occurrence.value);
		} else {
			throw TranscodeError(
				kTranscodeFailedType,
				"bad wire type for a vector of " + element.kind);
		}
	}
	return values;
}

[[nodiscard]] std::vector<std::uint64_t> packedFixed64(
		const MappingField &element,
		std::span<const Occurrence> occurrences) {
	std::vector<std::uint64_t> values;
	for (const Occurrence &occurrence : occurrences) {
		if (occurrence.wire == ProtoWireType::LengthDelimited) {
			if (occurrence.bytes.size() % 8 != 0) {
				throw TranscodeError(
					kTranscodeFailedType,
					"packed double blob has a bad length");
			}
			const std::vector<std::uint64_t> packed = ProtoReader::unpackFixed64(
				occurrence.bytes);
			values.insert(values.end(), packed.begin(), packed.end());
		} else if (occurrence.wire == ProtoWireType::Fixed64) {
			values.push_back(occurrence.value);
		} else {
			throw TranscodeError(
				kTranscodeFailedType,
				"bad wire type for a vector of " + element.kind);
		}
	}
	return values;
}

} // namespace

TranscodeError::TranscodeError(
	std::string_view type,
	std::string_view message)
: std::runtime_error(std::string(message))
, errorType(type) {
}

std::string_view TranscodeError::type() const noexcept {
	return errorType;
}

TlReader::TlReader(std::span<const std::uint8_t> source)
: data(source) {
}

std::int32_t TlReader::readInt32() {
	require(4);
	std::uint32_t value = 0;
	for (std::size_t index = 0; index < 4; ++index) {
		value |= static_cast<std::uint32_t>(data[position + index])
			<< (8 * index);
	}
	position += 4;
	return static_cast<std::int32_t>(value);
}

std::int64_t TlReader::readInt64() {
	require(8);
	std::uint64_t value = 0;
	for (std::size_t index = 0; index < 8; ++index) {
		value |= static_cast<std::uint64_t>(data[position + index])
			<< (8 * index);
	}
	position += 8;
	return static_cast<std::int64_t>(value);
}

std::span<const std::uint8_t> TlReader::readBytes() {
	require(1);
	std::size_t length = data[position];
	std::size_t header = 1;
	if (length >= 254) {
		require(4);
		length = static_cast<std::size_t>(data[position + 1])
			| (static_cast<std::size_t>(data[position + 2]) << 8)
			| (static_cast<std::size_t>(data[position + 3]) << 16);
		header = 4;
	}
	const std::size_t padded = (header + length + 3) / 4 * 4;
	require(padded);
	const std::span<const std::uint8_t> result = data.subspan(
		position + header,
		length);
	position += padded;
	return result;
}

std::span<const std::uint8_t> TlReader::readRaw(std::size_t count) {
	require(count);
	const std::span<const std::uint8_t> result = data.subspan(position, count);
	position += count;
	return result;
}

std::size_t TlReader::remaining() const noexcept {
	return data.size() - position;
}

void TlWriter::writeInt32(std::int32_t value) {
	const std::uint32_t bits = static_cast<std::uint32_t>(value);
	for (std::size_t index = 0; index < 4; ++index) {
		out.push_back(static_cast<std::uint8_t>(bits >> (8 * index)));
	}
}

void TlWriter::writeInt64(std::int64_t value) {
	const std::uint64_t bits = static_cast<std::uint64_t>(value);
	for (std::size_t index = 0; index < 8; ++index) {
		out.push_back(static_cast<std::uint8_t>(bits >> (8 * index)));
	}
}

void TlWriter::writeBytes(std::span<const std::uint8_t> value) {
	const std::size_t length = value.size();
	std::size_t header = 0;
	if (length < 254) {
		out.push_back(static_cast<std::uint8_t>(length));
		header = 1;
	} else {
		if (length > 0xFFFFFFu) {
			throw TranscodeError(
				kTranscodeFailedType,
				"TL bytes value is too long");
		}
		out.push_back(static_cast<std::uint8_t>(0xFE));
		out.push_back(static_cast<std::uint8_t>(length));
		out.push_back(static_cast<std::uint8_t>(length >> 8));
		out.push_back(static_cast<std::uint8_t>(length >> 16));
		header = 4;
	}
	out.insert(out.end(), value.begin(), value.end());
	while ((header + length) % 4 != 0) {
		out.push_back(0);
	}
}

void TlWriter::writeRaw(std::span<const std::uint8_t> value) {
	out.insert(out.end(), value.begin(), value.end());
}

const std::vector<std::uint8_t> &TlWriter::bytes() const {
	return out;
}

std::vector<std::uint8_t> TlWriter::takeBytes() {
	std::vector<std::uint8_t> result = std::move(out);
	out.clear();
	return result;
}

void TlReader::require(std::size_t count) const {
	if (count > data.size() - position) {
		throw TranscodeError(
			kTranscodeFailedType,
			"TL read past the end of the object");
	}
}

Transcoder::Transcoder(const MappingTable &mapping)
: table(mapping) {
	const Mapping &data = table.mapping();
	const std::vector<MappingMethod> &methods = data.methods;
	methodById.reserve(methods.size());
	for (const MappingMethod &method : methods) {
		methodById.emplace(method.id, &method);
	}
	typeByName.reserve(data.types.size());
	for (const MappingType &type : data.types) {
		typeByName.emplace(type.name, &type);
	}
	for (const MappingConstructor &constructor : data.constructors) {
		constructorsByType[constructor.type].push_back(&constructor);
	}
}

EncodedRequest Transcoder::encodeRequest(
		std::span<const std::uint8_t> tl) const {
	try {
		TlReader reader(tl);
		std::optional<std::int64_t> takeoutId;
		for (;;) {
			const std::uint32_t id = static_cast<std::uint32_t>(
				reader.readInt32());
			if (id == kInvokeWithTakeoutId) {
				takeoutId = reader.readInt64();
				continue;
			}
			if (skipWrapper(id, reader)) {
				continue;
			}
			const MappingMethod *method = findMethod(id);
			if (!method) {
				throw TranscodeError(
					kMethodNotMappedType,
					"the method is not in the mapping");
			}
			ProtoWriter writer;
			encodeParams(reader, method->params, writer, 0);
			requireConsumed(reader);
			EncodedRequest request;
			request.methodId = id;
			request.method = method->name;
			request.payload = writer.takeBytes();
			request.takeoutId = takeoutId;
			return request;
		}
	} catch (const ProtoWireError &error) {
		throw TranscodeError(kTranscodeFailedType, error.what());
	}
}

std::vector<std::uint8_t> Transcoder::encodeObject(
		std::string_view tlType,
		std::span<const std::uint8_t> tl) const {
	try {
		TlReader reader(tl);
		std::vector<std::uint8_t> result = objectToProto(tlType, reader, 0);
		requireConsumed(reader);
		return result;
	} catch (const ProtoWireError &error) {
		throw TranscodeError(kTranscodeFailedType, error.what());
	}
}

const MappingMethod *Transcoder::findMethod(std::uint32_t id) const {
	const std::unordered_map<std::uint32_t, const MappingMethod *>::const_iterator found
		= methodById.find(id);
	return (found == methodById.end()) ? nullptr : found->second;
}

// Reads a wrapper and returns true when the id is a wrapper constructor. The
// wrapper fields are consumed, not sent: the wrapped query is the request.
bool Transcoder::skipWrapper(std::uint32_t id, TlReader &reader) const {
	switch (id) {
	case kInvokeWithLayerId: {
		reader.readInt32();
		return true;
	}
	case kInvokeWithoutUpdatesId: {
		return true;
	}
	case kInvokeAfterMsgId: {
		reader.readInt64();
		return true;
	}
	case kInvokeAfterMsgsId: {
		expectVector(reader);
		const std::int32_t count = reader.readInt32();
		if (count < 0) {
			throw TranscodeError(kTranscodeFailedType, "bad msg_ids count");
		}
		for (std::int32_t index = 0; index < count; ++index) {
			reader.readInt64();
		}
		return true;
	}
	case kInitConnectionId: {
		// The proxy and params fields are not sent with the declaration.
		const std::uint32_t flags = static_cast<std::uint32_t>(
			reader.readInt32());
		reader.readInt32();
		for (std::size_t index = 0; index < kInitConnectionStringCount; ++index) {
			reader.readBytes();
		}
		if (flags & kInitConnectionProxyFlag) {
			objectToProto("InputClientProxy", reader, 0);
		}
		if (flags & kInitConnectionParamsFlag) {
			objectToProto("JSONValue", reader, 0);
		}
		return true;
	}
	case kInvokeWithMessagesRangeId: {
		objectToProto("MessageRange", reader, 0);
		return true;
	}
	case kInvokeWithBusinessConnectionId:
	case kInvokeWithReCaptchaId: {
		reader.readBytes();
		return true;
	}
	case kInvokeWithGooglePlayIntegrityId:
	case kInvokeWithApnsSecretId: {
		reader.readBytes();
		reader.readBytes();
		return true;
	}
	default: {
		return false;
	}
	}
}

std::vector<std::uint8_t> Transcoder::objectToProto(
		std::string_view tlType,
		TlReader &reader,
		std::size_t depth) const {
	if (depth > kMaxDepth) {
		throw TranscodeError(kTranscodeFailedType, "TL nesting is too deep");
	}
	const std::uint32_t id = static_cast<std::uint32_t>(reader.readInt32());
	const MappingConstructor *constructor = table.findConstructor(id);
	if (!constructor) {
		throw TranscodeError(
			kTranscodeFailedType,
			"unknown constructor while encoding an object");
	}
	if (constructor->type != tlType) {
		throw TranscodeError(
			kTranscodeFailedType,
			"constructor type does not match the expected type");
	}
	// An unmapped constructor has no server message: it is never sent.
	if (constructor->protoName.empty()) {
		throw TranscodeError(
			kTranscodeFailedType,
			"constructor has no server mapping");
	}
	ProtoWriter body;
	encodeParams(reader, constructor->fields, body, depth);
	if (constructor->arm < 0) {
		return body.takeBytes();
	}
	ProtoWriter wrapper;
	wrapper.writeBytesField(
		static_cast<std::uint32_t>(constructor->arm),
		body.bytes());
	return wrapper.takeBytes();
}

void Transcoder::encodeParams(
		TlReader &reader,
		const std::vector<MappingField> &params,
		ProtoWriter &writer,
		std::size_t depth) const {
	std::unordered_map<std::string, std::uint32_t> registers;
	for (const MappingField &field : params) {
		if (field.kind == "flags") {
			registers[field.name] = static_cast<std::uint32_t>(reader.readInt32());
			continue;
		}
		// A derived flag is implied by another parameter and is not sent.
		if (!field.derivedFrom.empty()) {
			continue;
		}
		if (field.optionalFlag) {
			const std::unordered_map<std::string, std::uint32_t>::const_iterator found
				= registers.find(field.flagsName);
			if (found == registers.end()
				|| !flagBitSet(found->second, field.flagBit)) {
				continue;
			}
		}
		// The value is present on the wire, but the server mapping lacks it.
		if (field.unmapped) {
			throw TranscodeError(
				kTranscodeFailedType,
				"field is not in the server mapping");
		}
		encodeValue(reader, field, writer, depth);
	}
}

void Transcoder::encodeValue(
		TlReader &reader,
		const MappingField &field,
		ProtoWriter &writer,
		std::size_t depth) const {
	if (field.number == 0) {
		throw TranscodeError(
			kTranscodeFailedType,
			"field has no protobuf number");
	}
	if (field.repeated) {
		encodeVector(reader, field, writer, depth);
		return;
	}
	const std::uint32_t number = field.number;
	if (field.kind == "true") {
		writer.writeVarintField(number, 1);
	} else if (field.kind == "Bool") {
		writer.writeVarintField(number, readBool(reader) ? 1 : 0);
	} else if (field.kind == "int") {
		writer.writeSignedVarintField(
			number,
			static_cast<std::int64_t>(reader.readInt32()));
	} else if (field.kind == "long") {
		writer.writeSignedVarintField(number, reader.readInt64());
	} else if (field.kind == "double") {
		writer.writeFixed64Field(
			number,
			static_cast<std::uint64_t>(reader.readInt64()));
	} else if (field.kind == "string" || field.kind == "bytes") {
		writer.writeBytesField(number, reader.readBytes());
	} else if (field.kind == "int128") {
		writer.writeBytesField(number, reader.readRaw(kInt128Bytes));
	} else if (field.kind == "int256") {
		writer.writeBytesField(number, reader.readRaw(kInt256Bytes));
	} else if (field.kind == "object") {
		const std::vector<std::uint8_t> message = objectToProto(
			field.type,
			reader,
			depth + 1);
		writer.writeBytesField(number, message);
	} else {
		throw TranscodeError(
			kTranscodeFailedType,
			"cannot encode a field of this kind");
	}
}

void Transcoder::encodeVector(
		TlReader &reader,
		const MappingField &field,
		ProtoWriter &writer,
		std::size_t depth) const {
	expectVector(reader);
	const std::int32_t count = reader.readInt32();
	if (count < 0 || static_cast<std::size_t>(count) > reader.remaining()) {
		throw TranscodeError(kTranscodeFailedType, "bad vector length");
	}
	if (count == 0) {
		return;
	}
	const std::size_t size = static_cast<std::size_t>(count);
	const std::uint32_t number = field.number;
	// The element kind of a repeated field is stored in field.kind.
	if (field.kind == "int") {
		std::vector<std::int64_t> values;
		values.reserve(size);
		for (std::size_t index = 0; index < size; ++index) {
			values.push_back(reader.readInt32());
		}
		writer.writePackedSignedVarints(number, values);
	} else if (field.kind == "long") {
		std::vector<std::int64_t> values;
		values.reserve(size);
		for (std::size_t index = 0; index < size; ++index) {
			values.push_back(reader.readInt64());
		}
		writer.writePackedSignedVarints(number, values);
	} else if (field.kind == "Bool") {
		std::vector<std::uint64_t> values;
		values.reserve(size);
		for (std::size_t index = 0; index < size; ++index) {
			values.push_back(readBool(reader) ? 1 : 0);
		}
		writer.writePackedVarints(number, values);
	} else if (field.kind == "double") {
		std::vector<std::uint64_t> values;
		values.reserve(size);
		for (std::size_t index = 0; index < size; ++index) {
			values.push_back(static_cast<std::uint64_t>(reader.readInt64()));
		}
		writer.writePackedFixed64(number, values);
	} else if (field.kind == "string" || field.kind == "bytes") {
		for (std::size_t index = 0; index < size; ++index) {
			writer.writeBytesField(number, reader.readBytes());
		}
	} else if (field.kind == "int128") {
		for (std::size_t index = 0; index < size; ++index) {
			writer.writeBytesField(number, reader.readRaw(kInt128Bytes));
		}
	} else if (field.kind == "int256") {
		for (std::size_t index = 0; index < size; ++index) {
			writer.writeBytesField(number, reader.readRaw(kInt256Bytes));
		}
	} else if (field.kind == "object") {
		for (std::size_t index = 0; index < size; ++index) {
			const std::vector<std::uint8_t> message = objectToProto(
				field.type,
				reader,
				depth + 1);
			writer.writeBytesField(number, message);
		}
	} else {
		throw TranscodeError(
			kTranscodeFailedType,
			"cannot encode a vector of this element kind");
	}
}

std::vector<std::uint8_t> Transcoder::decodeResult(
		std::uint32_t methodId,
		std::span<const std::uint8_t> proto,
		const TranscodeCaptureSink &sink) const {
	const MappingMethod *method = findMethod(methodId);
	if (!method) {
		throw TranscodeError(
			kMethodNotMappedType,
			"the method is not in the mapping");
	}
	try {
		TlWriter writer;
		decodeResultSpec(*method, proto, writer, sink);
		return writer.takeBytes();
	} catch (const TranscodeError &error) {
		throw TranscodeError(kResponseParseFailedType, error.what());
	} catch (const ProtoWireError &error) {
		throw TranscodeError(kResponseParseFailedType, error.what());
	}
}

std::vector<std::uint8_t> Transcoder::decodeUpdates(
		std::span<const std::uint8_t> proto,
		const TranscodeCaptureSink &sink) const {
	return decodeObject("Updates", proto, sink);
}

std::vector<std::uint8_t> Transcoder::decodeObject(
		std::string_view tlType,
		std::span<const std::uint8_t> proto,
		const TranscodeCaptureSink &sink) const {
	try {
		TlWriter writer;
		protoToObject(tlType, proto, writer, sink, 0);
		return writer.takeBytes();
	} catch (const ProtoWireError &error) {
		throw TranscodeError(kTranscodeFailedType, error.what());
	}
}

// Results are not wrapped in a constructor, except objects, which carry their
// own. Bool, int and long read field 1; a list reads its repeated field 1.
void Transcoder::decodeResultSpec(
		const MappingMethod &method,
		std::span<const std::uint8_t> proto,
		TlWriter &writer,
		const TranscodeCaptureSink &sink) const {
	if (method.resultKind == "object") {
		protoToObject(method.resultType, proto, writer, sink, 0);
		return;
	}
	const ProtoBody body = parseBody(proto);
	if (method.resultKind == "bool") {
		const std::span<const Occurrence> values = occurrencesOf(body, 1);
		if (values.empty()) {
			throw TranscodeError(
				kTranscodeFailedType,
				"Bool result carries no value");
		}
		writer.writeInt32(static_cast<std::int32_t>(
			values.back().value != 0 ? kBoolTrueId : kBoolFalseId));
	} else if (method.resultKind == "int") {
		writer.writeInt32(static_cast<std::int32_t>(scalarOf(body, 1)));
	} else if (method.resultKind == "long") {
		writer.writeInt64(static_cast<std::int64_t>(scalarOf(body, 1)));
	} else if (method.resultKind == "list") {
		writeVector(method.resultElement, occurrencesOf(body, 1), writer, sink, 0);
	} else {
		throw TranscodeError(
			kTranscodeFailedType,
			"unsupported result kind " + method.resultKind);
	}
}

// Constructor bodies: a polymorphic type takes the first length-delimited
// field that is one of its arms (unknown fields are skipped); any other type
// has exactly one constructor, and the whole message is its body.
void Transcoder::protoToObject(
		std::string_view tlType,
		std::span<const std::uint8_t> proto,
		TlWriter &writer,
		const TranscodeCaptureSink &sink,
		std::size_t depth) const {
	if (depth > kMaxDepth) {
		throw TranscodeError(kTranscodeFailedType, "TL nesting is too deep");
	}
	const std::unordered_map<std::string, const MappingType *>::const_iterator typeFound
		= typeByName.find(std::string(tlType));
	if (typeFound == typeByName.end()) {
		throw TranscodeError(kTranscodeFailedType, "unknown type");
	}
	const MappingType &type = *typeFound->second;
	const MappingConstructor *constructor = nullptr;
	std::span<const std::uint8_t> body = proto;
	if (type.polymorphic) {
		ProtoReader reader(proto);
		while (reader.next()) {
			if (reader.wireType() != ProtoWireType::LengthDelimited) {
				continue;
			}
			const std::uint32_t number = reader.field();
			const MappingArm *arm = nullptr;
			for (const MappingArm &candidate : type.arms) {
				if (candidate.arm == number) {
					arm = &candidate;
					break;
				}
			}
			if (!arm) {
				continue;
			}
			constructor = table.findConstructor(arm->constructorId);
			if (!constructor) {
				throw TranscodeError(
					kTranscodeFailedType,
					"unknown constructor in a oneof");
			}
			body = reader.readBytes();
			break;
		}
		if (!constructor) {
			throw TranscodeError(
				kTranscodeFailedType,
				"unset oneof for " + std::string(tlType));
		}
	} else {
		const std::unordered_map<std::string, std::vector<const MappingConstructor *>>::const_iterator found
			= constructorsByType.find(type.name);
		if (found == constructorsByType.end() || found->second.size() != 1) {
			throw TranscodeError(
				kTranscodeFailedType,
				"type is not polymorphic and has no single constructor");
		}
		constructor = found->second.front();
	}
	const ProtoBody message = parseBody(body);
	writer.writeInt32(static_cast<std::int32_t>(constructor->id));
	decodeParams(
		constructor->fields,
		constructor->captures,
		constructor->protoName,
		message,
		writer,
		sink,
		depth);
}

void Transcoder::decodeParams(
		const std::vector<MappingField> &params,
		const std::vector<MappingField> &captures,
		std::string_view protoMessage,
		const ProtoBody &body,
		TlWriter &writer,
		const TranscodeCaptureSink &sink,
		std::size_t depth) const {
	std::unordered_map<std::string, std::uint32_t> registers;
	for (const MappingField &field : params) {
		if (field.kind == "flags") {
			registers[field.name] = 0;
		}
	}
	for (const MappingField &field : params) {
		if (!field.optionalFlag) {
			continue;
		}
		const MappingField &source = field.derivedFrom.empty()
			? field
			: findParam(params, field.derivedFrom);
		const std::unordered_map<std::string, std::uint32_t>::iterator found
			= registers.find(field.flagsName);
		if (found == registers.end()) {
			throw TranscodeError(
				kTranscodeFailedType,
				"flag register is not declared");
		}
		if (isPresent(source, body)) {
			found->second |= std::uint32_t{1}
				<< static_cast<std::uint32_t>(field.flagBit);
		}
	}
	if (sink) {
		for (const MappingField &capture : captures) {
			const std::span<const Occurrence> occurrences = occurrencesOf(
				body,
				capture.number);
			if (occurrences.empty()) {
				continue;
			}
			const Occurrence &last = occurrences.back();
			std::string value;
			if (last.wire == ProtoWireType::LengthDelimited) {
				value = std::string(
					reinterpret_cast<const char *>(last.bytes.data()),
					last.bytes.size());
			}
			sink(protoMessage, capture.name, value);
		}
	}
	for (const MappingField &field : params) {
		if (field.kind == "flags") {
			writer.writeInt32(static_cast<std::int32_t>(registers[field.name]));
			continue;
		}
		if (!field.derivedFrom.empty()) {
			continue;
		}
		if (field.optionalFlag
			&& (registers[field.flagsName]
				& (std::uint32_t{1} << static_cast<std::uint32_t>(field.flagBit))) == 0) {
			continue;
		}
		decodeValue(field, body, writer, sink, depth);
	}
}

void Transcoder::decodeValue(
		const MappingField &field,
		const ProtoBody &body,
		TlWriter &writer,
		const TranscodeCaptureSink &sink,
		std::size_t depth) const {
	const std::span<const Occurrence> occurrences = occurrencesOf(body, field.number);
	if (field.repeated) {
		writeVector(field, occurrences, writer, sink, depth);
		return;
	}
	const Occurrence *last = occurrences.empty() ? nullptr : &occurrences.back();
	if (field.kind == "true") {
		return;
	} else if (field.kind == "Bool") {
		writer.writeInt32(static_cast<std::int32_t>(
			(last && last->value != 0) ? kBoolTrueId : kBoolFalseId));
	} else if (field.kind == "int") {
		writer.writeInt32(last
			? static_cast<std::int32_t>(numericValue(field, *last))
			: 0);
	} else if (field.kind == "long") {
		writer.writeInt64(last
			? static_cast<std::int64_t>(numericValue(field, *last))
			: 0);
	} else if (field.kind == "double") {
		if (last && last->wire != ProtoWireType::Fixed64) {
			throw TranscodeError(
				kTranscodeFailedType,
				"field " + field.name + " must be fixed64");
		}
		writer.writeInt64(last ? static_cast<std::int64_t>(last->value) : 0);
	} else if (field.kind == "string" || field.kind == "bytes") {
		if (!last) {
			writer.writeBytes(std::span<const std::uint8_t>());
		} else if (last->wire != ProtoWireType::LengthDelimited) {
			throw TranscodeError(
				kTranscodeFailedType,
				"field " + field.name + " must be length-delimited");
		} else {
			writer.writeBytes(last->bytes);
		}
	} else if (field.kind == "int128") {
		writer.writeRaw(fixedBytes(field, last, kInt128Bytes));
	} else if (field.kind == "int256") {
		writer.writeRaw(fixedBytes(field, last, kInt256Bytes));
	} else if (field.kind == "object") {
		if (!last) {
			throw TranscodeError(
				kTranscodeFailedType,
				"missing required object field " + field.name
					+ " of type " + field.type);
		}
		if (last->wire != ProtoWireType::LengthDelimited) {
			throw TranscodeError(
				kTranscodeFailedType,
				"field " + field.name + " must be length-delimited");
		}
		protoToObject(field.type, last->bytes, writer, sink, depth + 1);
	} else {
		throw TranscodeError(
			kTranscodeFailedType,
			"cannot decode a field of this kind");
	}
}

void Transcoder::writeVector(
		const MappingField &element,
		std::span<const Occurrence> occurrences,
		TlWriter &writer,
		const TranscodeCaptureSink &sink,
		std::size_t depth) const {
	writer.writeInt32(static_cast<std::int32_t>(kVectorId));
	if (element.kind == "int"
		|| element.kind == "long"
		|| element.kind == "Bool") {
		const std::vector<std::uint64_t> values = packedVarints(
			element,
			occurrences);
		writer.writeInt32(static_cast<std::int32_t>(values.size()));
		for (const std::uint64_t value : values) {
			if (element.kind == "int") {
				writer.writeInt32(static_cast<std::int32_t>(value));
			} else if (element.kind == "long") {
				writer.writeInt64(static_cast<std::int64_t>(value));
			} else {
				writer.writeInt32(static_cast<std::int32_t>(
					value != 0 ? kBoolTrueId : kBoolFalseId));
			}
		}
	} else if (element.kind == "double") {
		const std::vector<std::uint64_t> values = packedFixed64(
			element,
			occurrences);
		writer.writeInt32(static_cast<std::int32_t>(values.size()));
		for (const std::uint64_t value : values) {
			writer.writeInt64(static_cast<std::int64_t>(value));
		}
	} else if (element.kind == "string"
		|| element.kind == "bytes"
		|| element.kind == "int128"
		|| element.kind == "int256"
		|| element.kind == "object") {
		writer.writeInt32(static_cast<std::int32_t>(occurrences.size()));
		for (const Occurrence &occurrence : occurrences) {
			if (occurrence.wire != ProtoWireType::LengthDelimited) {
				throw TranscodeError(
					kTranscodeFailedType,
					"vector element must be length-delimited");
			}
			if (element.kind == "object") {
				protoToObject(element.type, occurrence.bytes, writer, sink, depth + 1);
			} else if (element.kind == "int128") {
				writer.writeRaw(fixedBytes(element, &occurrence, kInt128Bytes));
			} else if (element.kind == "int256") {
				writer.writeRaw(fixedBytes(element, &occurrence, kInt256Bytes));
			} else {
				writer.writeBytes(occurrence.bytes);
			}
		}
	} else {
		throw TranscodeError(
			kTranscodeFailedType,
			"unsupported vector element " + element.kind);
	}
}

} // namespace Impulse
