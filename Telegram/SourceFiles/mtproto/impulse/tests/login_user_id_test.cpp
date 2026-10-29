#include "mtproto/impulse/core/login_user_id.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

IMPULSE_TEST(LoginUserIdGoldenAuthorizationUserEmpty) {
	// auth.authorization with flags 0 and userEmpty whose id is 3.
	const std::array<std::uint8_t, 20> tl = {
		0xd4, 0xc0, 0xa2, 0x2e,
		0x00, 0x00, 0x00, 0x00,
		0x7a, 0x4b, 0xbc, 0xd3,
		0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	const std::optional<std::int64_t> userId = Impulse::AuthorizationUserId(
		std::span<const std::uint8_t>(tl.data(), tl.size()));
	IMPULSE_CHECK(userId.has_value());
	IMPULSE_CHECK(userId.value_or(0) == 3);
}
