// The user id of a login reply, read from the TL bytes of auth.authorization.
// Qt-free.
#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace Impulse {

// The user id of an auth.authorization result of the transcoder, read from
// its TL bytes. An auth.loginTokenSuccess result is read through its
// authorization. Nullopt when the result is not an authorization with a user.
[[nodiscard]] std::optional<std::int64_t> AuthorizationUserId(
	std::span<const std::uint8_t> tl);

} // namespace Impulse
