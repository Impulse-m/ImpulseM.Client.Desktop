#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Impulse {

class ProtoWireError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

enum class ProtoWireType : std::uint8_t {
	Varint = 0,
	Fixed64 = 1,
	LengthDelimited = 2,
	Fixed32 = 5,
};

class ProtoWriter {
public:
	void writeVarintField(std::uint32_t field, std::uint64_t value);
	void writeSignedVarintField(std::uint32_t field, std::int64_t value);
	void writeFixed64Field(std::uint32_t field, std::uint64_t bits);
	void writeFixed32Field(std::uint32_t field, std::uint32_t bits);
	void writeDoubleField(std::uint32_t field, double value);
	void writeBytesField(
		std::uint32_t field,
		std::span<const std::uint8_t> value);
	void writeStringField(std::uint32_t field, std::string_view value);
	void writeMessageField(std::uint32_t field, const ProtoWriter &message);
	void writePackedVarints(
		std::uint32_t field,
		std::span<const std::uint64_t> values);
	void writePackedSignedVarints(
		std::uint32_t field,
		std::span<const std::int64_t> values);
	void writePackedFixed64(
		std::uint32_t field,
		std::span<const std::uint64_t> values);

	[[nodiscard]] const std::vector<std::uint8_t> &bytes() const;
	[[nodiscard]] std::vector<std::uint8_t> takeBytes();

private:
	void writeTag(std::uint32_t field, ProtoWireType type);
	void writeRawVarint(std::uint64_t value);
	void writeRawFixed64(std::uint64_t bits);

	std::vector<std::uint8_t> out;
};

class ProtoReader {
public:
	explicit ProtoReader(std::span<const std::uint8_t> source);

	[[nodiscard]] static std::vector<std::uint64_t> unpackVarints(
		std::span<const std::uint8_t> payload);
	[[nodiscard]] static std::vector<std::uint64_t> unpackFixed64(
		std::span<const std::uint8_t> payload);

	[[nodiscard]] bool hasMore() const;

	// Advances to the next field. Returns false at the end of the data.
	// Throws ProtoWireError on a zero field number, a field number above
	// 2^29 - 1, or a wire type other than 0, 1, 2 and 5 (groups rejected).
	bool next();

	[[nodiscard]] std::uint32_t field() const;
	[[nodiscard]] ProtoWireType wireType() const;

	[[nodiscard]] std::uint64_t readVarint();
	[[nodiscard]] std::uint64_t readFixed64();
	[[nodiscard]] std::uint32_t readFixed32();
	[[nodiscard]] double readDouble();
	[[nodiscard]] std::span<const std::uint8_t> readBytes();

	// Skips the value of the current field if it was not read yet.
	void skip();

private:
	[[nodiscard]] std::uint64_t readRawVarint();
	[[nodiscard]] std::uint64_t readRawFixed64();
	[[nodiscard]] std::uint32_t readRawFixed32();
	[[nodiscard]] std::span<const std::uint8_t> readRawBytes();
	void consume(ProtoWireType expected);
	void require(std::size_t count) const;

	std::span<const std::uint8_t> data;
	std::size_t position = 0;
	std::uint32_t currentField = 0;
	ProtoWireType currentType = ProtoWireType::Varint;
	bool valueConsumed = true;
};

} // namespace Impulse
