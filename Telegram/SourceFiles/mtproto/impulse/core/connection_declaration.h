// Connection declaration (initConnection, contract 4.4). The declaration is
// read from the wrappers of a TL request and encoded as the proto payload of
// the initConnection method. Qt-free, std only.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Impulse {

// The fields of ConnectionDeclaration, in contract 4.4 numbering.
struct ConnectionDeclaration {
	std::int32_t apiId = 0;
	std::string deviceModel;
	std::string systemVersion;
	std::string appVersion;
	std::string systemLangCode;
	std::string langPack;
	std::string langCode;
};

// Reads the declaration of a TL request, looking through the invokeWithLayer,
// invokeWithoutUpdates and invokeWithTakeout wrappers in front of it. Returns
// nullopt when the request carries no initConnection or its header is not
// readable; the request itself is still encoded by the transcoder.
[[nodiscard]] std::optional<ConnectionDeclaration> ReadConnectionDeclaration(
	std::span<const std::uint8_t> tl);

// Encodes the declaration as the proto payload of the initConnection method:
// field 1 is the ConnectionDeclaration message.
[[nodiscard]] std::vector<std::uint8_t> EncodeConnectionDeclaration(
	const ConnectionDeclaration &declaration);

} // namespace Impulse
