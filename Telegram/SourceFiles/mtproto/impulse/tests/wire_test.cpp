#include "mtproto/impulse/core/wire.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

template <typename Callable>
bool throwsWireError(Callable &&callable) {
	try {
		callable();
	} catch (const Impulse::ProtoWireError &) {
		return true;
	}
	return false;
}

[[nodiscard]] std::vector<std::uint8_t> copyOf(
		std::span<const std::uint8_t> bytes) {
	return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

} // namespace

IMPULSE_TEST(WireVarintKnownAnswer) {
	Impulse::ProtoWriter writer;
	writer.writeVarintField(1, 300);
	const std::vector<std::uint8_t> expected = { 0x08, 0xac, 0x02 };
	IMPULSE_CHECK(writer.bytes() == expected);
}

IMPULSE_TEST(WireSignedNegativeKnownAnswer) {
	Impulse::ProtoWriter writer;
	writer.writeSignedVarintField(1, -5);
	const std::vector<std::uint8_t> expected = {
		0x08, 0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01,
	};
	IMPULSE_CHECK(writer.bytes() == expected);
}

IMPULSE_TEST(WireBytesLengthKnownAnswers) {
	const std::vector<std::uint8_t> header0 = { 0x0a, 0x00 };
	const std::vector<std::uint8_t> header3 = { 0x0a, 0x03 };
	const std::vector<std::uint8_t> header253 = { 0x0a, 0xfd, 0x01 };
	const std::vector<std::uint8_t> header254 = { 0x0a, 0xfe, 0x01 };

	const std::vector<std::uint8_t> payload3(3, 0x5a);
	const std::vector<std::uint8_t> payload253(253, 0x5a);
	const std::vector<std::uint8_t> payload254(254, 0x5a);

	Impulse::ProtoWriter writer0;
	writer0.writeBytesField(1, {});
	IMPULSE_CHECK(writer0.bytes() == header0);

	Impulse::ProtoWriter writer3;
	writer3.writeBytesField(1, payload3);
	IMPULSE_CHECK(copyOf(std::span(writer3.bytes()).first(2)) == header3);
	IMPULSE_CHECK(writer3.bytes().size() == header3.size() + payload3.size());

	Impulse::ProtoWriter writer253;
	writer253.writeBytesField(1, payload253);
	IMPULSE_CHECK(copyOf(std::span(writer253.bytes()).first(3)) == header253);
	IMPULSE_CHECK(
		writer253.bytes().size() == header253.size() + payload253.size());

	Impulse::ProtoWriter writer254;
	writer254.writeBytesField(1, payload254);
	IMPULSE_CHECK(copyOf(std::span(writer254.bytes()).first(3)) == header254);
	IMPULSE_CHECK(
		writer254.bytes().size() == header254.size() + payload254.size());
}

IMPULSE_TEST(WireFixed64KnownAnswer) {
	Impulse::ProtoWriter writer;
	writer.writeDoubleField(1, 1.5);
	const std::vector<std::uint8_t> expected = {
		0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x3f,
	};
	IMPULSE_CHECK(writer.bytes() == expected);
}

IMPULSE_TEST(WirePackedVarintsKnownAnswer) {
	const std::vector<std::uint64_t> values = { 1, 2, 300 };
	Impulse::ProtoWriter writer;
	writer.writePackedVarints(4, values);
	const std::vector<std::uint8_t> expected = {
		0x22, 0x04, 0x01, 0x02, 0xac, 0x02,
	};
	IMPULSE_CHECK(writer.bytes() == expected);
}

IMPULSE_TEST(WirePackedSignedVarintsRoundTrip) {
	const std::vector<std::int64_t> values = { -1, 0, 7 };
	Impulse::ProtoWriter writer;
	writer.writePackedSignedVarints(2, values);

	Impulse::ProtoReader reader(writer.bytes());
	IMPULSE_CHECK(reader.next());
	IMPULSE_CHECK(reader.field() == 2);
	const std::vector<std::uint64_t> unpacked = Impulse::ProtoReader::unpackVarints(
		reader.readBytes());
	IMPULSE_CHECK(unpacked.size() == 3);
	IMPULSE_CHECK(static_cast<std::int64_t>(unpacked[0]) == -1);
	IMPULSE_CHECK(static_cast<std::int64_t>(unpacked[1]) == 0);
	IMPULSE_CHECK(static_cast<std::int64_t>(unpacked[2]) == 7);
}

IMPULSE_TEST(WireReaderRoundTripAllWireTypes) {
	const std::string text = "hi";
	Impulse::ProtoWriter writer;
	writer.writeVarintField(1, 300);
	writer.writeStringField(2, text);
	writer.writeDoubleField(3, 1.5);
	writer.writeFixed32Field(5, 0x01020304);

	Impulse::ProtoReader reader(writer.bytes());

	IMPULSE_CHECK(reader.next());
	IMPULSE_CHECK(reader.field() == 1);
	IMPULSE_CHECK(reader.wireType() == Impulse::ProtoWireType::Varint);
	IMPULSE_CHECK(reader.readVarint() == 300);

	IMPULSE_CHECK(reader.next());
	IMPULSE_CHECK(reader.field() == 2);
	IMPULSE_CHECK(reader.wireType() == Impulse::ProtoWireType::LengthDelimited);
	const std::vector<std::uint8_t> bytes = copyOf(reader.readBytes());
	const std::vector<std::uint8_t> expectedText = { 'h', 'i' };
	IMPULSE_CHECK(bytes == expectedText);

	IMPULSE_CHECK(reader.next());
	IMPULSE_CHECK(reader.field() == 3);
	IMPULSE_CHECK(reader.wireType() == Impulse::ProtoWireType::Fixed64);
	IMPULSE_CHECK(reader.readDouble() == 1.5);

	IMPULSE_CHECK(reader.next());
	IMPULSE_CHECK(reader.field() == 5);
	IMPULSE_CHECK(reader.wireType() == Impulse::ProtoWireType::Fixed32);
	IMPULSE_CHECK(reader.readFixed32() == 0x01020304);

	IMPULSE_CHECK(!reader.next());
	IMPULSE_CHECK(!reader.hasMore());
}

IMPULSE_TEST(WireReaderSkipsUnreadFields) {
	Impulse::ProtoWriter writer;
	writer.writeVarintField(9, 42);
	writer.writeVarintField(1, 7);

	Impulse::ProtoReader reader(writer.bytes());
	IMPULSE_CHECK(reader.next());
	IMPULSE_CHECK(reader.field() == 9);
	IMPULSE_CHECK(reader.next());
	IMPULSE_CHECK(reader.field() == 1);
	IMPULSE_CHECK(reader.readVarint() == 7);
	IMPULSE_CHECK(!reader.next());
}

IMPULSE_TEST(WireReaderMaxVarintAccepted) {
	std::vector<std::uint8_t> bytes = { 0x08 };
	bytes.insert(bytes.end(), 9, 0xff);
	bytes.push_back(0x01);

	Impulse::ProtoReader reader(bytes);
	IMPULSE_CHECK(reader.next());
	IMPULSE_CHECK(reader.readVarint() == std::numeric_limits<std::uint64_t>::max());
}

IMPULSE_TEST(WireReaderRejectsTruncatedVarint) {
	const std::vector<std::uint8_t> bytes = { 0x08, 0x80 };
	Impulse::ProtoReader reader(bytes);
	IMPULSE_CHECK(reader.next());
	const bool thrown = throwsWireError([&] {
		static_cast<void>(reader.readVarint());
	});
	IMPULSE_CHECK(thrown);
}

IMPULSE_TEST(WireReaderRejectsElevenByteVarint) {
	std::vector<std::uint8_t> bytes = { 0x08 };
	bytes.insert(bytes.end(), 10, 0x80);
	bytes.push_back(0x00);

	Impulse::ProtoReader reader(bytes);
	IMPULSE_CHECK(reader.next());
	const bool thrown = throwsWireError([&] {
		static_cast<void>(reader.readVarint());
	});
	IMPULSE_CHECK(thrown);
}

IMPULSE_TEST(WireReaderRejectsTruncatedLengthDelimited) {
	const std::vector<std::uint8_t> bytes = { 0x0a, 0x05, 0x01 };
	Impulse::ProtoReader reader(bytes);
	IMPULSE_CHECK(reader.next());
	const bool thrown = throwsWireError([&] {
		static_cast<void>(reader.readBytes());
	});
	IMPULSE_CHECK(thrown);
}

IMPULSE_TEST(WireReaderRejectsTruncatedFixed64) {
	const std::vector<std::uint8_t> bytes = { 0x09, 0x01, 0x02 };
	Impulse::ProtoReader reader(bytes);
	IMPULSE_CHECK(reader.next());
	const bool thrown = throwsWireError([&] {
		static_cast<void>(reader.readFixed64());
	});
	IMPULSE_CHECK(thrown);
}

IMPULSE_TEST(WireReaderRejectsGroupsAndUnknownWireTypes) {
	const std::vector<std::uint8_t> startGroup = { 0x0b };
	const std::vector<std::uint8_t> endGroup = { 0x0c };
	const std::vector<std::uint8_t> wireType6 = { 0x0e };
	const std::vector<std::uint8_t> wireType7 = { 0x0f };

	Impulse::ProtoReader readerStart(startGroup);
	const bool thrownStart = throwsWireError([&] {
		readerStart.next();
	});
	IMPULSE_CHECK(thrownStart);

	Impulse::ProtoReader readerEnd(endGroup);
	const bool thrownEnd = throwsWireError([&] {
		readerEnd.next();
	});
	IMPULSE_CHECK(thrownEnd);

	Impulse::ProtoReader reader6(wireType6);
	const bool thrown6 = throwsWireError([&] {
		reader6.next();
	});
	IMPULSE_CHECK(thrown6);

	Impulse::ProtoReader reader7(wireType7);
	const bool thrown7 = throwsWireError([&] {
		reader7.next();
	});
	IMPULSE_CHECK(thrown7);
}

IMPULSE_TEST(WireReaderRejectsZeroFieldNumber) {
	const std::vector<std::uint8_t> bytes = { 0x00, 0x01 };
	Impulse::ProtoReader reader(bytes);
	const bool thrown = throwsWireError([&] {
		reader.next();
	});
	IMPULSE_CHECK(thrown);
}

IMPULSE_TEST(WireReaderRejectsTypeMismatch) {
	const std::vector<std::uint8_t> bytes = { 0x08, 0x01 };
	Impulse::ProtoReader reader(bytes);
	IMPULSE_CHECK(reader.next());
	const bool thrown = throwsWireError([&] {
		static_cast<void>(reader.readBytes());
	});
	IMPULSE_CHECK(thrown);
}

IMPULSE_TEST(WireReaderFixed32Accepted) {
	const std::vector<std::uint8_t> bytes = { 0x0d, 0x01, 0x00, 0x00, 0x00 };
	Impulse::ProtoReader reader(bytes);
	IMPULSE_CHECK(reader.next());
	IMPULSE_CHECK(reader.wireType() == Impulse::ProtoWireType::Fixed32);
	IMPULSE_CHECK(reader.readFixed32() == 1);
	IMPULSE_CHECK(!reader.next());
}

IMPULSE_TEST(WireUnpackKnownAnswers) {
	const std::vector<std::uint8_t> varints = { 0x01, 0x02, 0xac, 0x02 };
	const std::vector<std::uint64_t> values = Impulse::ProtoReader::unpackVarints(varints);
	const std::vector<std::uint64_t> expectedValues = { 1, 2, 300 };
	IMPULSE_CHECK(values == expectedValues);

	const std::vector<std::uint8_t> fixed = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x3f,
	};
	const std::vector<std::uint64_t> fixedValues = Impulse::ProtoReader::unpackFixed64(fixed);
	IMPULSE_CHECK(fixedValues.size() == 1);
	IMPULSE_CHECK(fixedValues[0] == 0x3ff8000000000000ULL);
}

IMPULSE_TEST(WireZeroLengthBytesRoundTrip) {
	Impulse::ProtoWriter writer;
	writer.writeBytesField(3, {});

	Impulse::ProtoReader reader(writer.bytes());
	IMPULSE_CHECK(reader.next());
	IMPULSE_CHECK(reader.field() == 3);
	IMPULSE_CHECK(reader.readBytes().empty());
	IMPULSE_CHECK(!reader.next());
}

IMPULSE_TEST(WireWriterRejectsInvalidFieldNumbers) {
	Impulse::ProtoWriter zeroWriter;
	const bool thrownZero = throwsWireError([&] {
		zeroWriter.writeVarintField(0, 1);
	});
	IMPULSE_CHECK(thrownZero);

	Impulse::ProtoWriter tooLargeWriter;
	const bool thrownTooLarge = throwsWireError([&] {
		tooLargeWriter.writeVarintField(std::uint32_t{1} << 29, 1);
	});
	IMPULSE_CHECK(thrownTooLarge);

	Impulse::ProtoWriter maxWriter;
	maxWriter.writeVarintField((std::uint32_t{1} << 29) - 1, 0);
	const std::vector<std::uint8_t> expected = { 0xf8, 0xff, 0xff, 0xff, 0x0f, 0x00 };
	IMPULSE_CHECK(maxWriter.bytes() == expected);
}
