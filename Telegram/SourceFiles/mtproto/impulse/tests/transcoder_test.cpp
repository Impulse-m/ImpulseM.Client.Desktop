#include "mtproto/impulse/core/transcoder.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::string DirectoryOf(std::string_view sourceFile) {
	const std::size_t separator = sourceFile.find_last_of("/\\");
	return std::string(sourceFile.substr(0, separator));
}

const std::string kRealMappingPath = DirectoryOf(__FILE__)
	+ "/../data/tl-proto-232.json.gz";

// Hex digits with any separators (spaces, newlines) ignored.
[[nodiscard]] std::vector<std::uint8_t> Hex(std::string_view text) {
	std::vector<std::uint8_t> out;
	int high = -1;
	for (const char symbol : text) {
		int nibble = -1;
		if (symbol >= '0' && symbol <= '9') {
			nibble = symbol - '0';
		} else if (symbol >= 'a' && symbol <= 'f') {
			nibble = symbol - 'a' + 10;
		} else if (symbol >= 'A' && symbol <= 'F') {
			nibble = symbol - 'A' + 10;
		} else {
			continue;
		}
		if (high < 0) {
			high = nibble;
		} else {
			out.push_back(static_cast<std::uint8_t>(high * 16 + nibble));
			high = -1;
		}
	}
	return out;
}

// A TL string of up to 253 bytes: one length byte, the data, padding to four.
[[nodiscard]] std::vector<std::uint8_t> TlString(std::string_view text) {
	std::vector<std::uint8_t> out;
	out.push_back(static_cast<std::uint8_t>(text.size()));
	for (const char symbol : text) {
		out.push_back(static_cast<std::uint8_t>(symbol));
	}
	while (out.size() % 4 != 0) {
		out.push_back(0);
	}
	return out;
}

void Append(std::vector<std::uint8_t> &target, const std::vector<std::uint8_t> &tail) {
	target.insert(target.end(), tail.begin(), tail.end());
}

[[nodiscard]] const std::optional<Impulse::MappingTable> &RealMapping() {
	static const std::optional<Impulse::MappingTable> loaded
		= Impulse::MappingTable::LoadGzipFile(kRealMappingPath, nullptr);
	return loaded;
}

// The local error type of a failed encode, or an empty string on success.
[[nodiscard]] std::string EncodeRequestErrorType(
		const Impulse::Transcoder &transcoder,
		const std::vector<std::uint8_t> &tl) {
	try {
		static_cast<void>(transcoder.encodeRequest(tl));
	} catch (const Impulse::TranscodeError &error) {
		return std::string(error.type());
	}
	return std::string();
}

[[nodiscard]] std::string EncodeObjectErrorType(
		const Impulse::Transcoder &transcoder,
		std::string_view tlType,
		const std::vector<std::uint8_t> &tl) {
	try {
		static_cast<void>(transcoder.encodeObject(tlType, tl));
	} catch (const Impulse::TranscodeError &error) {
		return std::string(error.type());
	}
	return std::string();
}

// The local error type of a failed decode, or an empty string on success.
[[nodiscard]] std::string DecodeObjectErrorType(
		const Impulse::Transcoder &transcoder,
		std::string_view tlType,
		const std::vector<std::uint8_t> &proto) {
	try {
		static_cast<void>(transcoder.decodeObject(tlType, proto, {}));
	} catch (const Impulse::TranscodeError &error) {
		return std::string(error.type());
	}
	return std::string();
}

[[nodiscard]] std::string DecodeResultErrorType(
		const Impulse::Transcoder &transcoder,
		std::uint32_t methodId,
		const std::vector<std::uint8_t> &proto) {
	try {
		static_cast<void>(transcoder.decodeResult(methodId, proto, {}));
	} catch (const Impulse::TranscodeError &error) {
		return std::string(error.type());
	}
	return std::string();
}

// The id of the first method whose result is a list of the element kind.
[[nodiscard]] std::uint32_t FindListMethodId(
		const Impulse::MappingTable &mapping,
		std::string_view elementKind) {
	for (const Impulse::MappingMethod &method : mapping.mapping().methods) {
		if (method.resultKind == "list" && method.resultElement.kind == elementKind) {
			return method.id;
		}
	}
	return 0;
}

// The id of the first method whose result has the given kind (bool, int, long).
[[nodiscard]] std::uint32_t FindResultKindMethodId(
		const Impulse::MappingTable &mapping,
		std::string_view resultKind) {
	for (const Impulse::MappingMethod &method : mapping.mapping().methods) {
		if (method.resultKind == resultKind) {
			return method.id;
		}
	}
	return 0;
}

} // namespace

IMPULSE_TEST(TranscoderEncodesMessageGolden) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	const std::vector<std::uint8_t> tl = Hex(
		"d3b90076 02000001 02000000 2a000000 22175159 0500000000000000 00f15365 02686900");
	const std::vector<std::uint8_t> expected = Hex(
		"121c 0801 4801 6001 80012a a20104 0a020805 d00180e2cfaa06 da01026869");
	IMPULSE_CHECK(transcoder.encodeObject("Message", tl) == expected);
}

IMPULSE_TEST(TranscoderEncodesPeerUserGolden) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	const std::vector<std::uint8_t> tl = Hex("22175159 0500000000000000");
	IMPULSE_CHECK(transcoder.encodeObject("Peer", tl) == Hex("0a020805"));
}

IMPULSE_TEST(TranscoderEncodesWrappedGetConfig) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	// invokeWithLayer(229), initConnection(api_id 3, six strings), help.getConfig.
	std::vector<std::uint8_t> tl = Hex("0d0d9bda e5000000 a95ecdc1 00000000 03000000");
	for (char index = 0; index < 6; ++index) {
		Append(tl, TlString(std::string("v") + static_cast<char>('0' + index)));
	}
	Append(tl, Hex("6b18f9c4"));
	const Impulse::EncodedRequest request = transcoder.encodeRequest(tl);
	IMPULSE_CHECK(request.method == "help.getConfig");
	IMPULSE_CHECK(request.methodId == 0xc4f9186bu);
	IMPULSE_CHECK(request.payload.empty());
	IMPULSE_CHECK(!request.takeoutId.has_value());
}

IMPULSE_TEST(TranscoderReportsTakeoutId) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	const std::vector<std::uint8_t> tl = Hex(
		"2efda9ac 2a00000000000000 6b18f9c4");
	const Impulse::EncodedRequest request = transcoder.encodeRequest(tl);
	IMPULSE_CHECK(request.method == "help.getConfig");
	IMPULSE_CHECK(request.takeoutId.has_value());
	IMPULSE_CHECK(request.takeoutId.value_or(0) == 42);
}

IMPULSE_TEST(TranscoderRejectsUnmappedMethod) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	const std::vector<std::uint8_t> tl = Hex("01000000");
	IMPULSE_CHECK(
		EncodeRequestErrorType(transcoder, tl) == "CLIENT_METHOD_NOT_MAPPED");
}

IMPULSE_TEST(TranscoderRejectsUnknownConstructor) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	IMPULSE_CHECK(
		EncodeObjectErrorType(transcoder, "Message", Hex("01000000"))
		== "CLIENT_TRANSCODE_FAILED");
}

IMPULSE_TEST(TranscoderRejectsUnmappedConstructor) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	// botVerification (232 id) has no server mapping: proto is null.
	const std::vector<std::uint8_t> tl = Hex("d90e2ef7 0000000000000000 0000000000000000");
	IMPULSE_CHECK(
		EncodeObjectErrorType(transcoder, "BotVerification", tl)
		== "CLIENT_TRANSCODE_FAILED");
}

IMPULSE_TEST(TranscoderRejectsWrongTypeAndTruncation) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	const std::vector<std::uint8_t> peer = Hex("22175159 0500000000000000");
	IMPULSE_CHECK(
		EncodeObjectErrorType(transcoder, "Message", peer)
		== "CLIENT_TRANSCODE_FAILED");
	const std::vector<std::uint8_t> truncated = Hex("d3b90076 0200");
	IMPULSE_CHECK(
		EncodeObjectErrorType(transcoder, "Message", truncated)
		== "CLIENT_TRANSCODE_FAILED");
}

IMPULSE_TEST(TlReaderBytesPaddingAndLongForm) {
	const std::vector<std::uint8_t> shortForm = Hex("03 61 62 63");
	Impulse::TlReader shortReader(shortForm);
	const std::span<const std::uint8_t> abc = shortReader.readBytes();
	IMPULSE_CHECK(abc.size() == 3);
	IMPULSE_CHECK(shortReader.remaining() == 0);

	std::vector<std::uint8_t> longForm = {0xFE, 0x00, 0x01, 0x00};
	longForm.resize(longForm.size() + 256, 0x55);
	Impulse::TlReader longReader(longForm);
	const std::span<const std::uint8_t> payload = longReader.readBytes();
	IMPULSE_CHECK(payload.size() == 256);
	IMPULSE_CHECK(longReader.remaining() == 0);
}

IMPULSE_TEST(TlReaderRejectsReadPastEnd) {
	const std::vector<std::uint8_t> bytes = Hex("01 02");
	Impulse::TlReader reader(bytes);
	bool thrown = false;
	try {
		static_cast<void>(reader.readInt32());
	} catch (const Impulse::TranscodeError &error) {
		thrown = error.type() == "CLIENT_TRANSCODE_FAILED";
	}
	IMPULSE_CHECK(thrown);
}

IMPULSE_TEST(TranscoderDecodesMessageGolden) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	const std::vector<std::uint8_t> proto = Hex(
		"121c 0801 4801 6001 80012a a20104 0a020805 d00180e2cfaa06 da01026869");
	const std::vector<std::uint8_t> expected = Hex(
		"d3b90076 02000001 02000000 2a000000 22175159 0500000000000000 00f15365 02686900");
	IMPULSE_CHECK(transcoder.decodeObject("Message", proto, {}) == expected);
}

IMPULSE_TEST(TranscoderMessageRoundTripsThroughProto) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	const std::vector<std::uint8_t> tl = Hex(
		"d3b90076 02000001 02000000 2a000000 22175159 0500000000000000 00f15365 02686900");
	const std::vector<std::uint8_t> proto = transcoder.encodeObject("Message", tl);
	IMPULSE_CHECK(transcoder.decodeObject("Message", proto, {}) == tl);
}

IMPULSE_TEST(TranscoderDecodesPeerUserGolden) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	IMPULSE_CHECK(
		transcoder.decodeObject("Peer", Hex("0a020805"), {})
		== Hex("22175159 0500000000000000"));
}

IMPULSE_TEST(TranscoderSkipsUnknownProtoFields) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	// Field 99 (varint 1) sits inside the peerUser body and is not in the mapping.
	IMPULSE_CHECK(
		transcoder.decodeObject("Peer", Hex("0a05 0805 980601"), {})
		== Hex("22175159 0500000000000000"));
}

IMPULSE_TEST(TranscoderDecodesUsersGetUsersGolden) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	const std::uint32_t methodId = loaded->methodId("users.getUsers").value_or(0);
	IMPULSE_CHECK(methodId != 0);
	IMPULSE_CHECK(
		transcoder.decodeResult(methodId, Hex("0a040a02080b0a040a02080b"), {})
		== Hex("15c4b51c 02000000 7a4bbcd3 0b00000000000000 7a4bbcd3 0b00000000000000"));
}

IMPULSE_TEST(TranscoderDecodesAuthorizationAndCapturesTokens) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	const std::vector<std::uint8_t> proto = Hex("0a0c 2a040a020803 320141 3a0142");
	std::vector<std::string> captured;
	const Impulse::TranscodeCaptureSink sink = [&captured](
			std::string_view protoMessage,
			std::string_view fieldName,
			std::string_view value) {
		captured.push_back(
			std::string(protoMessage) + "|" + std::string(fieldName) + "|" + std::string(value));
	};
	IMPULSE_CHECK(
		transcoder.decodeObject("auth.Authorization", proto, sink)
		== Hex("d4c0a22e 00000000 7a4bbcd3 0300000000000000"));
	IMPULSE_CHECK(captured.size() == 2);
	if (captured.size() == 2) {
		IMPULSE_CHECK(captured[0] == "AuthAuthorizationValue|session_token|A");
		IMPULSE_CHECK(captured[1] == "AuthAuthorizationValue|refresh_token|B");
	}
}

IMPULSE_TEST(TranscoderRejectsOneofWithoutKnownArm) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	// Field 9 is a varint, not an arm of auth.Authorization.
	IMPULSE_CHECK(
		DecodeObjectErrorType(transcoder, "auth.Authorization", Hex("48 01"))
		== "CLIENT_TRANSCODE_FAILED");
}

IMPULSE_TEST(TranscoderRejectsUnknownTypeWhenDecoding) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	IMPULSE_CHECK(
		DecodeObjectErrorType(transcoder, "NoSuchType", Hex("08 01"))
		== "CLIENT_TRANSCODE_FAILED");
}

IMPULSE_TEST(TranscoderRejectsTruncatedAndMalformedProto) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	// A length-delimited field longer than the data.
	IMPULSE_CHECK(
		DecodeObjectErrorType(transcoder, "Peer", Hex("0a 02"))
		== "CLIENT_TRANSCODE_FAILED");
	// A field whose varint value runs to 11 bytes.
	IMPULSE_CHECK(
		DecodeObjectErrorType(transcoder, "Peer", Hex("08 ff ff ff ff ff ff ff ff ff ff 01"))
		== "CLIENT_TRANSCODE_FAILED");
}

IMPULSE_TEST(TranscoderDecodesBoolResults) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	const std::uint32_t methodId = loaded->methodId("account.updateStatus").value_or(0);
	IMPULSE_CHECK(methodId != 0);
	IMPULSE_CHECK(transcoder.decodeResult(methodId, Hex("08 01"), {}) == Hex("b5757299"));
	IMPULSE_CHECK(transcoder.decodeResult(methodId, Hex("08 00"), {}) == Hex("37 97 79 bc"));
	IMPULSE_CHECK(
		DecodeResultErrorType(transcoder, methodId, {})
		== "CLIENT_RESPONSE_PARSE_FAILED");
}

IMPULSE_TEST(TranscoderRejectsUnmappedResultMethod) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	IMPULSE_CHECK(
		DecodeResultErrorType(transcoder, 1u, {}) == "CLIENT_METHOD_NOT_MAPPED");
}

IMPULSE_TEST(TranscoderDecodesIntAndLongResults) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	const std::uint32_t intMethodId = FindResultKindMethodId(*loaded, "int");
	IMPULSE_CHECK(intMethodId != 0);
	if (intMethodId != 0) {
		IMPULSE_CHECK(transcoder.decodeResult(intMethodId, Hex("08 2a"), {}) == Hex("2a000000"));
		// An absent int result is zero.
		IMPULSE_CHECK(transcoder.decodeResult(intMethodId, {}, {}) == Hex("00000000"));
	}
	const std::uint32_t longMethodId = FindResultKindMethodId(*loaded, "long");
	IMPULSE_CHECK(longMethodId != 0);
	if (longMethodId != 0) {
		IMPULSE_CHECK(
			transcoder.decodeResult(longMethodId, Hex("08 2a"), {})
			== Hex("2a00000000000000"));
	}
}

IMPULSE_TEST(TranscoderDecodesPackedAndUnpackedIntVectors) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	const std::uint32_t listMethodId = FindListMethodId(*loaded, "int");
	IMPULSE_CHECK(listMethodId != 0);
	if (listMethodId == 0) {
		return;
	}
	const std::vector<std::uint8_t> expected = Hex(
		"15c4b51c 03000000 01000000 02000000 2c010000");
	// Packed: one length-delimited field of varints 1, 2 and 300.
	IMPULSE_CHECK(
		transcoder.decodeResult(listMethodId, Hex("0a04 01 02 ac 02"), {}) == expected);
	// Unpacked: one varint field per element.
	IMPULSE_CHECK(
		transcoder.decodeResult(listMethodId, Hex("08 01 08 02 08 ac 02"), {}) == expected);
}

IMPULSE_TEST(TlWriterPadsBytesToFourAndTerminates) {
	Impulse::TlWriter writer;
	writer.writeBytes(Hex("68 69"));
	IMPULSE_CHECK(writer.bytes() == Hex("02 68 69 00"));

	// The long form frames 254 bytes with four header bytes, so 258 pads to 260.
	Impulse::TlWriter longWriter;
	const std::vector<std::uint8_t> payload(254, 0x55);
	longWriter.writeBytes(payload);
	IMPULSE_CHECK(longWriter.bytes().size() == 260);
}

IMPULSE_TEST(TranscoderRejectsZeroFieldTagLoop) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	// A zero tag is field number 0, which protobuf does not allow.
	IMPULSE_CHECK(
		DecodeObjectErrorType(transcoder, "Peer", Hex("00 00 00 00"))
		== "CLIENT_TRANSCODE_FAILED");
}

IMPULSE_TEST(TranscoderZeroLengthArmDecodesWithoutLooping) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	// An empty peerUser body: the absent user_id reads as zero, and the
	// repeated zero-length arms do not repeat the decode.
	IMPULSE_CHECK(
		transcoder.decodeObject("Peer", Hex("0a00 0a00 0a00"), {})
		== Hex("22175159 0000000000000000"));
}

IMPULSE_TEST(TranscoderRejectsProtoLengthBeyondInput) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	// Field 1 claims 16 bytes, and one byte follows.
	IMPULSE_CHECK(
		DecodeObjectErrorType(transcoder, "Peer", Hex("0a 10 01"))
		== "CLIENT_TRANSCODE_FAILED");
}

IMPULSE_TEST(TranscoderRejectsMsgIdsCountBeyondInput) {
	const std::optional<Impulse::MappingTable> &loaded = RealMapping();
	IMPULSE_CHECK(loaded.has_value());
	if (!loaded) {
		return;
	}
	const Impulse::Transcoder transcoder(*loaded);
	// invokeAfterMsgs with a vector count of 0x7fffffff and no msg_ids after it.
	IMPULSE_CHECK(
		EncodeRequestErrorType(transcoder, Hex("f0b4c43d 15c4b51c ffffff7f"))
		== "CLIENT_TRANSCODE_FAILED");
}

IMPULSE_TEST(TlReaderRejectsBytesLengthBeyondInput) {
	const std::vector<std::uint8_t> bytes = Hex("fe ffffff");
	Impulse::TlReader reader(bytes);
	bool thrown = false;
	try {
		static_cast<void>(reader.readBytes());
	} catch (const Impulse::TranscodeError &error) {
		thrown = error.type() == "CLIENT_TRANSCODE_FAILED";
	}
	IMPULSE_CHECK(thrown);
}
