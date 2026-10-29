/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace Impulse {

using TokenBlob = std::vector<std::uint8_t>;

// Persists the account token blob (access token, refresh token, user id) and
// the connection identity token. Implementations never keep the plaintext on
// disk.
class TokenStore {
public:
	virtual ~TokenStore() = default;

	// Returns the decrypted blob. An empty vector means that nothing is stored
	// or that the stored data cannot be decrypted by the current user account.
	[[nodiscard]] virtual TokenBlob load() = 0;

	// Replaces the stored blob. Returns false when the new blob was not
	// persisted; the previously stored blob then stays in place.
	[[nodiscard]] virtual bool save(std::span<const std::uint8_t> blob) = 0;

	// Removes the stored blob (sign-out).
	virtual void clear() = 0;
};

// Windows DPAPI (CryptProtectData, current-user scope, no entropy) over one
// file. Other user accounts cannot decrypt the file, and it does not decrypt
// on another machine. Writes go through a temporary file and a rename, so a
// crash never leaves a half-written token file behind.
class DpapiTokenStore final : public TokenStore {
private:
	const std::filesystem::path filePath;

public:
	explicit DpapiTokenStore(std::filesystem::path path);

	[[nodiscard]] TokenBlob load() override;
	[[nodiscard]] bool save(std::span<const std::uint8_t> blob) override;
	void clear() override;
};

} // namespace Impulse
