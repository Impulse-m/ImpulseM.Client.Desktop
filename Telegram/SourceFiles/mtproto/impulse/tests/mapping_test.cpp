#include "mtproto/impulse/core/mapping.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::string FixtureDirectory(std::string_view sourceFile) {
	const std::size_t separator = sourceFile.find_last_of("/\\");
	return std::string(sourceFile.substr(0, separator)) + "/fixtures";
}

const std::string kFixtureDirectory = FixtureDirectory(__FILE__);
const std::string kMiniFixturePath = kFixtureDirectory + "/mapping_mini.json";

[[nodiscard]] std::string ReadFile(const std::string &path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::string();
	}
	return std::string(
		(std::istreambuf_iterator<char>(file)),
		std::istreambuf_iterator<char>());
}

[[nodiscard]] std::vector<std::uint8_t> GzipCompress(std::string_view text) {
	z_stream stream{};
	if (deflateInit2(
			&stream,
			Z_BEST_COMPRESSION,
			Z_DEFLATED,
			15 + 16,
			8,
			Z_DEFAULT_STRATEGY) != Z_OK) {
		return {};
	}
	std::vector<std::uint8_t> out(
		static_cast<std::size_t>(deflateBound(&stream, static_cast<uLong>(text.size()))) + 64);
	stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(text.data()));
	stream.avail_in = static_cast<uInt>(text.size());
	stream.next_out = out.data();
	stream.avail_out = static_cast<uInt>(out.size());
	const int status = deflate(&stream, Z_FINISH);
	const std::size_t written = out.size() - stream.avail_out;
	deflateEnd(&stream);
	if (status != Z_STREAM_END) {
		return {};
	}
	out.resize(written);
	return out;
}

[[nodiscard]] std::optional<Impulse::MappingTable> LoadMiniFixture(std::string *error) {
	const std::string text = ReadFile(kMiniFixturePath);
	if (text.empty()) {
		*error = "fixture is missing: " + kMiniFixturePath;
		return std::nullopt;
	}
	return Impulse::MappingTable::LoadJson(text, error);
}

} // namespace

IMPULSE_TEST(MappingFixtureLoadsLayerAndCounts) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = LoadMiniFixture(&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	IMPULSE_CHECK(table->layer() == 232);
	IMPULSE_CHECK(table->mapping().constructors.size() == 5);
	IMPULSE_CHECK(table->mapping().methods.size() == 1);
	IMPULSE_CHECK(table->mapping().wrapperIds.size() == 11);
}

IMPULSE_TEST(MappingConstructorByIdAndName) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = LoadMiniFixture(&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	const Impulse::MappingConstructor *peerUser = table->findConstructor(1498486562);
	IMPULSE_CHECK(peerUser != nullptr);
	if (peerUser) {
		IMPULSE_CHECK(peerUser->name == "peerUser");
		IMPULSE_CHECK(peerUser->type == "Peer");
		IMPULSE_CHECK(peerUser->protoName == "PeerUser");
		IMPULSE_CHECK(peerUser->arm == 1);
	}
	const Impulse::MappingConstructor *byName = table->findConstructorByName("boolTrue");
	IMPULSE_CHECK(byName != nullptr);
	if (byName) {
		IMPULSE_CHECK(byName->id == 2574415285u);
		IMPULSE_CHECK(byName->protoName.empty());
		IMPULSE_CHECK(byName->arm == -1);
	}
	IMPULSE_CHECK(table->findConstructor(999u) == nullptr);
	IMPULSE_CHECK(table->findConstructorByName("noSuchConstructor") == nullptr);
}

IMPULSE_TEST(MappingMethodNameToId) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = LoadMiniFixture(&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	IMPULSE_CHECK(table->methodId("help.getConfig") == std::optional<std::uint32_t>(3304659051u));
	IMPULSE_CHECK(table->findMethod("help.getConfig") != nullptr);
	IMPULSE_CHECK(!table->methodId("messages.noSuchMethod").has_value());
	IMPULSE_CHECK(table->findMethod("help.getconfig") == nullptr);
}

IMPULSE_TEST(MappingProtoFieldTable) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = LoadMiniFixture(&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	const Impulse::MappingField *userId = table->findField(1498486562u, 1);
	IMPULSE_CHECK(userId != nullptr);
	if (userId) {
		IMPULSE_CHECK(userId->name == "user_id");
		IMPULSE_CHECK(userId->kind == "long");
		IMPULSE_CHECK(!userId->repeated);
		IMPULSE_CHECK(!userId->optionalFlag);
	}
	const Impulse::MappingField *note = table->findField(1780335806u, 5);
	IMPULSE_CHECK(note != nullptr);
	if (note) {
		IMPULSE_CHECK(note->name == "note");
		IMPULSE_CHECK(note->kind == "object");
		IMPULSE_CHECK(note->type == "TextWithEntities");
		IMPULSE_CHECK(note->optionalFlag);
		IMPULSE_CHECK(note->flagsName == "flags");
		IMPULSE_CHECK(note->flagBit == 0);
	}
	IMPULSE_CHECK(table->findField(1780335806u, 99) == nullptr);
	IMPULSE_CHECK(table->findField(999u, 1) == nullptr);
}

IMPULSE_TEST(MappingCapturesOfAuthorization) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = LoadMiniFixture(&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	const Impulse::MappingConstructor *authorization = table->findConstructorByName("auth.authorization");
	IMPULSE_CHECK(authorization != nullptr);
	if (!authorization) {
		return;
	}
	IMPULSE_CHECK(authorization->captures.size() == 2);
	if (authorization->captures.size() == 2) {
		IMPULSE_CHECK(authorization->captures[0].name == "session_token");
		IMPULSE_CHECK(authorization->captures[0].number == 6);
		IMPULSE_CHECK(authorization->captures[1].name == "refresh_token");
		IMPULSE_CHECK(authorization->captures[1].number == 7);
	}
}

IMPULSE_TEST(MappingGzipRoundTripMatchesJson) {
	std::string error;
	const std::string text = ReadFile(kMiniFixturePath);
	IMPULSE_CHECK(!text.empty());
	const std::vector<std::uint8_t> compressed = GzipCompress(text);
	IMPULSE_CHECK(!compressed.empty());
	const std::optional<Impulse::MappingTable> table = Impulse::MappingTable::LoadGzip(
		std::span<const std::uint8_t>(compressed.data(), compressed.size()),
		&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	IMPULSE_CHECK(table->layer() == 232);
	IMPULSE_CHECK(table->mapping().constructors.size() == 5);
	IMPULSE_CHECK(table->findConstructor(782418132u) != nullptr);
	IMPULSE_CHECK(table->methodId("help.getConfig").has_value());
}

IMPULSE_TEST(MappingRejectsUnknownLayer) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = Impulse::MappingTable::LoadJson(
		R"({"layer":229,"constructors":[],"methods":[],"wrappers":{}})",
		&error);
	IMPULSE_CHECK(!table.has_value());
	IMPULSE_CHECK(!error.empty());
}

IMPULSE_TEST(MappingAcceptsEmptyLayer232) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = Impulse::MappingTable::LoadJson(
		R"({"layer":232,"constructors":[],"methods":[],"wrappers":{}})",
		&error);
	IMPULSE_CHECK(table.has_value());
	if (table) {
		IMPULSE_CHECK(table->mapping().constructors.empty());
	}
}

IMPULSE_TEST(MappingRejectsMissingLayer) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = Impulse::MappingTable::LoadJson(
		R"({"constructors":[],"methods":[],"wrappers":{}})",
		&error);
	IMPULSE_CHECK(!table.has_value());
	IMPULSE_CHECK(!error.empty());
}

IMPULSE_TEST(MappingRejectsDuplicateConstructorId) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = Impulse::MappingTable::LoadJson(
		R"({"layer":232,"constructors":[)"
		R"({"id":1,"predicate":"a","type":"T","proto":null,"params":[],"capture":[],"arm":null},)"
		R"({"id":1,"predicate":"b","type":"T","proto":null,"params":[],"capture":[],"arm":null}],)"
		R"("methods":[],"wrappers":{}})",
		&error);
	IMPULSE_CHECK(!table.has_value());
	IMPULSE_CHECK(!error.empty());
}

IMPULSE_TEST(MappingRejectsMalformedJson) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = Impulse::MappingTable::LoadJson(
		R"({"layer":232,"constructors":[],"methods":[],"wrappers":{}} trailing)",
		&error);
	IMPULSE_CHECK(!table.has_value());
	IMPULSE_CHECK(!error.empty());
}

IMPULSE_TEST(MappingRejectsNonGzipBytes) {
	std::string error;
	const std::string junk = "this is not a gzip stream";
	const std::optional<Impulse::MappingTable> table = Impulse::MappingTable::LoadGzip(
		std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t *>(junk.data()),
			junk.size()),
		&error);
	IMPULSE_CHECK(!table.has_value());
	IMPULSE_CHECK(!error.empty());
}

IMPULSE_TEST(MappingRejectsTruncatedGzip) {
	std::string error;
	const std::string text = ReadFile(kMiniFixturePath);
	std::vector<std::uint8_t> compressed = GzipCompress(text);
	IMPULSE_CHECK(compressed.size() > 16);
	compressed.resize(compressed.size() / 2);
	const std::optional<Impulse::MappingTable> table = Impulse::MappingTable::LoadGzip(
		std::span<const std::uint8_t>(compressed.data(), compressed.size()),
		&error);
	IMPULSE_CHECK(!table.has_value());
	IMPULSE_CHECK(!error.empty());
}

IMPULSE_TEST(MappingGzipFileMissing) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = Impulse::MappingTable::LoadGzipFile(
		kFixtureDirectory + "/no_such_mapping.json.gz",
		&error);
	IMPULSE_CHECK(!table.has_value());
	IMPULSE_CHECK(!error.empty());
}
