#include "mtproto/impulse/core/wire.h"

#include <bit>

namespace Impulse {
namespace {

constexpr std::uint64_t MaxFieldNumber = (std::uint64_t{1} << 29) - 1;
constexpr std::size_t MaxVarintBytes = 10;

} // namespace

void ProtoWriter::writeVarintField(std::uint32_t field, std::uint64_t value) {
	writeTag(field, ProtoWireType::Varint);
	writeRawVarint(value);
}

void ProtoWriter::writeSignedVarintField(
		std::uint32_t field,
		std::int64_t value) {
	writeVarintField(field, static_cast<std::uint64_t>(value));
}

void ProtoWriter::writeFixed64Field(std::uint32_t field, std::uint64_t bits) {
	writeTag(field, ProtoWireType::Fixed64);
	writeRawFixed64(bits);
}

void ProtoWriter::writeFixed32Field(std::uint32_t field, std::uint32_t bits) {
	writeTag(field, ProtoWireType::Fixed32);
	for (std::size_t index = 0; index < 4; ++index) {
		out.push_back(static_cast<std::uint8_t>(bits >> (8 * index)));
	}
}

void ProtoWriter::writeDoubleField(std::uint32_t field, double value) {
	writeFixed64Field(field, std::bit_cast<std::uint64_t>(value));
}

void ProtoWriter::writeBytesField(
		std::uint32_t field,
		std::span<const std::uint8_t> value) {
	writeTag(field, ProtoWireType::LengthDelimited);
	writeRawVarint(value.size());
	out.insert(out.end(), value.begin(), value.end());
}

void ProtoWriter::writeStringField(
		std::uint32_t field,
		std::string_view value) {
	writeBytesField(
		field,
		std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t *>(value.data()),
			value.size()));
}

void ProtoWriter::writeMessageField(
		std::uint32_t field,
		const ProtoWriter &message) {
	writeBytesField(field, message.bytes());
}

void ProtoWriter::writePackedVarints(
		std::uint32_t field,
		std::span<const std::uint64_t> values) {
	ProtoWriter inner;
	for (const std::uint64_t value : values) {
		inner.writeRawVarint(value);
	}
	writeBytesField(field, inner.bytes());
}

void ProtoWriter::writePackedSignedVarints(
		std::uint32_t field,
		std::span<const std::int64_t> values) {
	ProtoWriter inner;
	for (const std::int64_t value : values) {
		inner.writeRawVarint(static_cast<std::uint64_t>(value));
	}
	writeBytesField(field, inner.bytes());
}

void ProtoWriter::writePackedFixed64(
		std::uint32_t field,
		std::span<const std::uint64_t> values) {
	ProtoWriter inner;
	for (const std::uint64_t bits : values) {
		inner.writeRawFixed64(bits);
	}
	writeBytesField(field, inner.bytes());
}

const std::vector<std::uint8_t> &ProtoWriter::bytes() const {
	return out;
}

std::vector<std::uint8_t> ProtoWriter::takeBytes() {
	std::vector<std::uint8_t> result = std::move(out);
	out.clear();
	return result;
}

void ProtoWriter::writeTag(std::uint32_t field, ProtoWireType type) {
	if (field == 0 || field > MaxFieldNumber) {
		throw ProtoWireError("invalid protobuf field number");
	}
	writeRawVarint(
		(static_cast<std::uint64_t>(field) << 3)
		| static_cast<std::uint64_t>(type));
}

void ProtoWriter::writeRawVarint(std::uint64_t value) {
	while (value >= 0x80) {
		out.push_back(static_cast<std::uint8_t>((value & 0x7F) | 0x80));
		value >>= 7;
	}
	out.push_back(static_cast<std::uint8_t>(value));
}

void ProtoWriter::writeRawFixed64(std::uint64_t bits) {
	for (std::size_t index = 0; index < 8; ++index) {
		out.push_back(static_cast<std::uint8_t>(bits >> (8 * index)));
	}
}

ProtoReader::ProtoReader(std::span<const std::uint8_t> source)
: data(source) {
}

std::vector<std::uint64_t> ProtoReader::unpackVarints(
		std::span<const std::uint8_t> payload) {
	ProtoReader reader(payload);
	std::vector<std::uint64_t> values;
	while (reader.hasMore()) {
		values.push_back(reader.readRawVarint());
	}
	return values;
}

std::vector<std::uint64_t> ProtoReader::unpackFixed64(
		std::span<const std::uint8_t> payload) {
	ProtoReader reader(payload);
	std::vector<std::uint64_t> values;
	while (reader.hasMore()) {
		values.push_back(reader.readRawFixed64());
	}
	return values;
}

bool ProtoReader::hasMore() const {
	return position < data.size();
}

bool ProtoReader::next() {
	skip();
	if (!hasMore()) {
		return false;
	}
	const std::uint64_t tag = readRawVarint();
	const std::uint64_t number = tag >> 3;
	const std::uint64_t wire = tag & 0x7;
	if (number == 0 || number > MaxFieldNumber) {
		throw ProtoWireError("invalid protobuf field number");
	}
	if (wire != 0 && wire != 1 && wire != 2 && wire != 5) {
		throw ProtoWireError("unsupported protobuf wire type");
	}
	currentField = static_cast<std::uint32_t>(number);
	currentType = static_cast<ProtoWireType>(wire);
	valueConsumed = false;
	return true;
}

std::uint32_t ProtoReader::field() const {
	return currentField;
}

ProtoWireType ProtoReader::wireType() const {
	return currentType;
}

std::uint64_t ProtoReader::readVarint() {
	consume(ProtoWireType::Varint);
	return readRawVarint();
}

std::uint64_t ProtoReader::readFixed64() {
	consume(ProtoWireType::Fixed64);
	return readRawFixed64();
}

std::uint32_t ProtoReader::readFixed32() {
	consume(ProtoWireType::Fixed32);
	return readRawFixed32();
}

double ProtoReader::readDouble() {
	return std::bit_cast<double>(readFixed64());
}

std::span<const std::uint8_t> ProtoReader::readBytes() {
	consume(ProtoWireType::LengthDelimited);
	return readRawBytes();
}

void ProtoReader::skip() {
	if (valueConsumed) {
		return;
	}
	if (currentType == ProtoWireType::Varint) {
		static_cast<void>(readRawVarint());
	} else if (currentType == ProtoWireType::Fixed64) {
		static_cast<void>(readRawFixed64());
	} else if (currentType == ProtoWireType::LengthDelimited) {
		static_cast<void>(readRawBytes());
	} else {
		static_cast<void>(readRawFixed32());
	}
	valueConsumed = true;
}

std::uint64_t ProtoReader::readRawVarint() {
	std::uint64_t result = 0;
	for (std::size_t index = 0; index < MaxVarintBytes; ++index) {
		require(1);
		const std::uint8_t byte = data[position];
		++position;
		result |= static_cast<std::uint64_t>(byte & 0x7F) << (7 * index);
		if ((byte & 0x80) == 0) {
			return result;
		}
	}
	throw ProtoWireError("malformed protobuf varint");
}

std::uint64_t ProtoReader::readRawFixed64() {
	require(8);
	std::uint64_t result = 0;
	for (std::size_t index = 0; index < 8; ++index) {
		result |= static_cast<std::uint64_t>(data[position + index]) << (8 * index);
	}
	position += 8;
	return result;
}

std::uint32_t ProtoReader::readRawFixed32() {
	require(4);
	std::uint32_t result = 0;
	for (std::size_t index = 0; index < 4; ++index) {
		result |= static_cast<std::uint32_t>(data[position + index]) << (8 * index);
	}
	position += 4;
	return result;
}

std::span<const std::uint8_t> ProtoReader::readRawBytes() {
	const std::uint64_t length = readRawVarint();
	if (length > data.size() - position) {
		throw ProtoWireError("protobuf length out of bounds");
	}
	const std::span<const std::uint8_t> result = data.subspan(
		position,
		static_cast<std::size_t>(length));
	position += static_cast<std::size_t>(length);
	return result;
}

void ProtoReader::consume(ProtoWireType expected) {
	if (valueConsumed || currentType != expected) {
		throw ProtoWireError("protobuf value does not match the wire type");
	}
	valueConsumed = true;
}

void ProtoReader::require(std::size_t count) const {
	if (count > data.size() - position) {
		throw ProtoWireError("protobuf read past end");
	}
}

} // namespace Impulse
