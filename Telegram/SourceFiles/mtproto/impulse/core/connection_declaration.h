// Connection declaration (initConnection). The declaration is
// read from the wrappers of a TL request and encoded as the proto payload of
// the initConnection method. Qt-free, std only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Impulse {

// The field limits of the server (DeclaredConnection), in UTF-16 code units.
inline constexpr std::size_t kDeviceModelMaxLength = 100;
inline constexpr std::size_t kSystemVersionMaxLength = 50;
inline constexpr std::size_t kAppVersionMaxLength = 50;
inline constexpr std::size_t kSystemLangCodeMaxLength = 32;
inline constexpr std::size_t kLangPackMaxLength = 50;
inline constexpr std::size_t kLangCodeMaxLength = 64;

// The fields of ConnectionDeclaration, in the proto field numbering of initConnection.
struct ConnectionDeclaration {
	std::int32_t apiId = 0;
	std::string deviceModel;
	std::string systemVersion;
	std::string appVersion;
	std::string systemLangCode;
	std::string langPack;
	std::string langCode;

	bool operator==(const ConnectionDeclaration &other) const = default;
};

// Trims the spaces at both ends of each field and cuts it to the server limit,
// at a whole UTF-8 character. The server cuts the same way, so the declared
// value is the one the server stores.
[[nodiscard]] ConnectionDeclaration ClampConnectionDeclaration(
	ConnectionDeclaration declaration);

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
