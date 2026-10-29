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
