#include "mtproto/impulse/core/login_user_id.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Impulse {
namespace {

// Constructors of the login results, as the layer 232 schema numbers them.
constexpr std::uint32_t AuthorizationConstructor = 0x2ea2c0d4;
constexpr std::uint32_t LoginTokenSuccessConstructor = 0x390d5c5e;
constexpr std::uint32_t UserEmptyConstructor = 0xd3bc4b7a;
constexpr std::uint32_t UserConstructor = 0xb1b8cc83;

// The flags of auth.authorization that add a field: bit 0 tmp_sessions, bit 1
// otherwise_relogin_days and bit 2 future_auth_token.
constexpr std::uint32_t TmpSessionsFlag = 1U << 0;
constexpr std::uint32_t OtherwiseReloginFlag = 1U << 1;
constexpr std::uint32_t FutureAuthTokenFlag = 1U << 2;

// The length byte that announces the long form of a TL bytes value.
constexpr std::uint8_t LongBytesMarker = 0xfe;

// A forward reader of TL words. A read past the end fails the reader, and
// every later read gives zero.
class TlCursor final {
public:
	explicit TlCursor(std::span<const std::uint8_t> bytes)
	: data(bytes) {
	}

	[[nodiscard]] bool ok() const {
		return valid;
	}

	[[nodiscard]] std::uint32_t readWord() {
		if (!take(4)) {
			return 0;
		}
		const std::size_t at = position - 4;
		return static_cast<std::uint32_t>(data[at])
			| (static_cast<std::uint32_t>(data[at + 1]) << 8)
			| (static_cast<std::uint32_t>(data[at + 2]) << 16)
			| (static_cast<std::uint32_t>(data[at + 3]) << 24);
	}

	[[nodiscard]] std::int64_t readLong() {
		const std::uint64_t low = readWord();
		const std::uint64_t high = readWord();
		return static_cast<std::int64_t>(low | (high << 32));
	}

	// A TL bytes value: one length byte or 0xfe and three length bytes, then
	// the data, padded with zeros to a multiple of four bytes.
	void skipBytes() {
		if (!take(1)) {
			return;
		}
		std::size_t length = data[position - 1];
		std::size_t header = 1;
		if (length == LongBytesMarker) {
			if (!take(3)) {
				return;
			}
			const std::size_t at = position - 3;
			length = static_cast<std::size_t>(data[at])
				| (static_cast<std::size_t>(data[at + 1]) << 8)
				| (static_cast<std::size_t>(data[at + 2]) << 16);
			header = 4;
		}
		const std::size_t padded = (header + length + 3) / 4 * 4;
		static_cast<void>(take(padded - header));
	}

private:
	[[nodiscard]] bool take(std::size_t count) {
		if (!valid || data.size() - position < count) {
			valid = false;
			return false;
		}
		position += count;
		return true;
	}

	std::span<const std::uint8_t> data;
	std::size_t position = 0;
	bool valid = true;
};

} // namespace

std::optional<std::int64_t> AuthorizationUserId(
		std::span<const std::uint8_t> tl) {
	TlCursor cursor(tl);
	std::uint32_t constructor = cursor.readWord();
	if (constructor == LoginTokenSuccessConstructor) {
		constructor = cursor.readWord();
	}
	if (!cursor.ok() || constructor != AuthorizationConstructor) {
		return std::nullopt;
	}

	// The fields of auth.authorization in schema order. The bool flag has no
	// data, so only the three fields below read their flag bits.
	const std::uint32_t flags = cursor.readWord();
	if ((flags & OtherwiseReloginFlag) != 0) {
		static_cast<void>(cursor.readWord());
	}
	if ((flags & TmpSessionsFlag) != 0) {
		static_cast<void>(cursor.readWord());
	}
	if ((flags & FutureAuthTokenFlag) != 0) {
		cursor.skipBytes();
	}

	// The user is userEmpty (the id follows) or user (flags, flags2, then id).
	const std::uint32_t userConstructor = cursor.readWord();
	std::int64_t userId = 0;
	if (userConstructor == UserEmptyConstructor) {
		userId = cursor.readLong();
	} else if (userConstructor == UserConstructor) {
		static_cast<void>(cursor.readWord());
		static_cast<void>(cursor.readWord());
		userId = cursor.readLong();
	} else {
		return std::nullopt;
	}
	if (!cursor.ok() || userId == 0) {
		return std::nullopt;
	}
	return userId;
}

} // namespace Impulse
