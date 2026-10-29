#include "mtproto/impulse/core/mapping.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace {

// Scheme functions of layer 232 are 859. The mapping keeps 801 methods; the
// other 47 are absent by the generator rules (hidden list, exclusions and
// requests without a mapped form), and the generator report lists them.
constexpr std::size_t kMappedMethodCount = 801;
constexpr std::size_t kConstructorCount = 1720;
constexpr std::size_t kWrapperCount = 11;

constexpr std::uint32_t kPeerUserId = 0x59511722u;
constexpr std::uint32_t kMessageId = 0x7600b9d3u;
constexpr std::uint32_t kAuthAuthorizationId = 0x2ea2c0d4u;
constexpr std::uint32_t kVectorId = 0x1cb5c415u;
constexpr std::uint32_t kUsersGetUsersId = 0x0d91a548u;
constexpr std::uint32_t kHelpGetConfigId = 0xc4f9186bu;
constexpr std::uint32_t kInitConnectionId = 0xc1cd5ea9u;
constexpr std::uint32_t kInvokeWithLayerId = 0xda9b0d0du;

[[nodiscard]] std::string DataDirectory(std::string_view sourceFile) {
	const std::size_t separator = sourceFile.find_last_of("/\\");
	return std::string(sourceFile.substr(0, separator)) + "/../data";
}

const std::string kMappingRealPath = DataDirectory(__FILE__) + "/tl-proto-232.json.gz";

[[nodiscard]] std::optional<Impulse::MappingTable> LoadRealMapping(std::string *error) {
	return Impulse::MappingTable::LoadGzipFile(kMappingRealPath, error);
}

[[nodiscard]] bool HasWrapperId(const Impulse::Mapping &mapping, std::uint32_t id) {
	for (const std::uint32_t wrapperId : mapping.wrapperIds) {
		if (wrapperId == id) {
			return true;
		}
	}
	return false;
}

} // namespace

IMPULSE_TEST(MappingRealLoadsLayerAndCounts) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = LoadRealMapping(&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	IMPULSE_CHECK(table->layer() == 232);
	IMPULSE_CHECK(table->mapping().constructors.size() == kConstructorCount);
	IMPULSE_CHECK(table->mapping().methods.size() == kMappedMethodCount);
	IMPULSE_CHECK(table->mapping().wrapperIds.size() == kWrapperCount);
}

IMPULSE_TEST(MappingRealWrapperIdsContainInvokeWithLayer) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = LoadRealMapping(&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	IMPULSE_CHECK(HasWrapperId(table->mapping(), kInvokeWithLayerId));
	IMPULSE_CHECK(HasWrapperId(table->mapping(), kInitConnectionId));
}

IMPULSE_TEST(MappingRealGoldenPeerUser) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = LoadRealMapping(&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	const Impulse::MappingConstructor *peerUser = table->findConstructor(kPeerUserId);
	IMPULSE_CHECK(peerUser != nullptr);
	if (!peerUser) {
		return;
	}
	IMPULSE_CHECK(peerUser->name == "peerUser");
	IMPULSE_CHECK(peerUser->type == "Peer");
	IMPULSE_CHECK(peerUser->protoName == "PeerUser");
	IMPULSE_CHECK(peerUser->arm == 1);
	const Impulse::MappingField *userId = table->findField(kPeerUserId, 1);
	IMPULSE_CHECK(userId != nullptr);
	if (userId) {
		IMPULSE_CHECK(userId->name == "user_id");
		IMPULSE_CHECK(userId->kind == "long");
	}
}

IMPULSE_TEST(MappingRealGoldenMessage) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = LoadRealMapping(&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	const Impulse::MappingConstructor *message = table->findConstructor(kMessageId);
	IMPULSE_CHECK(message != nullptr);
	if (!message) {
		return;
	}
	IMPULSE_CHECK(message->name == "message");
	IMPULSE_CHECK(message->protoName == "MessageValue");
	// Golden fields: out (1, flags bit 1), id (16), peer_id (20), date (26), message (27).
	const Impulse::MappingField *out = table->findField(kMessageId, 1);
	IMPULSE_CHECK(out != nullptr);
	if (out) {
		IMPULSE_CHECK(out->name == "out");
		IMPULSE_CHECK(out->optionalFlag);
		IMPULSE_CHECK(out->flagsName == "flags");
		IMPULSE_CHECK(out->flagBit == 1);
	}
	const Impulse::MappingField *id = table->findField(kMessageId, 16);
	IMPULSE_CHECK(id != nullptr);
	if (id) {
		IMPULSE_CHECK(id->name == "id");
		IMPULSE_CHECK(id->kind == "int");
	}
	const Impulse::MappingField *peerId = table->findField(kMessageId, 20);
	IMPULSE_CHECK(peerId != nullptr);
	if (peerId) {
		IMPULSE_CHECK(peerId->name == "peer_id");
		IMPULSE_CHECK(peerId->kind == "object");
		IMPULSE_CHECK(peerId->type == "Peer");
	}
	const Impulse::MappingField *date = table->findField(kMessageId, 26);
	IMPULSE_CHECK(date != nullptr);
	if (date) {
		IMPULSE_CHECK(date->name == "date");
		IMPULSE_CHECK(date->kind == "int");
	}
	const Impulse::MappingField *text = table->findField(kMessageId, 27);
	IMPULSE_CHECK(text != nullptr);
	if (text) {
		IMPULSE_CHECK(text->name == "message");
		IMPULSE_CHECK(text->kind == "string");
	}
}

IMPULSE_TEST(MappingRealGoldenAuthAuthorizationCaptures) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = LoadRealMapping(&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	const Impulse::MappingConstructor *authorization = table->findConstructor(kAuthAuthorizationId);
	IMPULSE_CHECK(authorization != nullptr);
	if (!authorization) {
		return;
	}
	IMPULSE_CHECK(authorization->name == "auth.authorization");
	IMPULSE_CHECK(authorization->protoName == "AuthAuthorizationValue");
	IMPULSE_CHECK(authorization->captures.size() == 2);
	if (authorization->captures.size() == 2) {
		IMPULSE_CHECK(authorization->captures[0].name == "session_token");
		IMPULSE_CHECK(authorization->captures[0].number == 6);
		IMPULSE_CHECK(authorization->captures[1].name == "refresh_token");
		IMPULSE_CHECK(authorization->captures[1].number == 7);
	}
}

IMPULSE_TEST(MappingRealGoldenVectorConstructorAndMethodIds) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = LoadRealMapping(&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	// Vector is the generic container of the users.getUsers golden result.
	IMPULSE_CHECK(table->findConstructor(kVectorId) != nullptr);
	IMPULSE_CHECK(table->methodId("users.getUsers") == std::optional<std::uint32_t>(kUsersGetUsersId));
	const Impulse::MappingMethod *getConfig = table->findMethod("help.getConfig");
	IMPULSE_CHECK(getConfig != nullptr);
	if (getConfig) {
		IMPULSE_CHECK(getConfig->id == kHelpGetConfigId);
		IMPULSE_CHECK(getConfig->params.empty());
	}
}

IMPULSE_TEST(MappingRealRejectsUnknownGolden) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = LoadRealMapping(&error);
	IMPULSE_CHECK(table.has_value());
	if (!table) {
		return;
	}
	IMPULSE_CHECK(table->findConstructor(0u) == nullptr);
	IMPULSE_CHECK(table->findMethod("help.noSuchMethod") == nullptr);
}

IMPULSE_TEST(MappingRealMissingFileFails) {
	std::string error;
	const std::optional<Impulse::MappingTable> table = Impulse::MappingTable::LoadGzipFile(
		DataDirectory(__FILE__) + "/no_such_file.json.gz",
		&error);
	IMPULSE_CHECK(!table.has_value());
	IMPULSE_CHECK(!error.empty());
}
