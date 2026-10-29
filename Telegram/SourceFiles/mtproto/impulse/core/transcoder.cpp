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

void TlReader::require(std::size_t count) const {
	if (count > data.size() - position) {
		throw TranscodeError(
			kTranscodeFailedType,
			"TL read past the end of the object");
	}
}

Transcoder::Transcoder(const MappingTable &mapping)
: table(mapping) {
	const std::vector<MappingMethod> &methods = table.mapping().methods;
	methodById.reserve(methods.size());
	for (const MappingMethod &method : methods) {
		methodById.emplace(method.id, &method);
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

} // namespace Impulse
