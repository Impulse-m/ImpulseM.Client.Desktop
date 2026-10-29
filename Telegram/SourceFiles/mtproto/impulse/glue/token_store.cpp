/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/impulse/glue/token_store.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>

#ifdef _MSC_VER
#pragma comment(lib, "Crypt32.lib")
#endif // _MSC_VER
#endif // _WIN32

namespace Impulse {
namespace {

constexpr const wchar_t *kProtectDescription = L"ImpulseM tokens";

[[nodiscard]] std::filesystem::path TempPathFor(
		const std::filesystem::path &filePath) {
	std::filesystem::path result = filePath;
	result += L".tmp";
	return result;
}

[[nodiscard]] TokenBlob ReadWholeFile(const std::filesystem::path &path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return {};
	}
	file.seekg(0, std::ios::end);
	const std::streamoff size = static_cast<std::streamoff>(file.tellg());
	file.seekg(0, std::ios::beg);
	if (size <= 0) {
		return {};
	}
	TokenBlob result(static_cast<std::size_t>(size));
	file.read(
		reinterpret_cast<char *>(result.data()),
		static_cast<std::streamsize>(result.size()));
	if (!file) {
		return {};
	}
	return result;
}

[[nodiscard]] bool WriteWholeFile(
		const std::filesystem::path &path,
		std::span<const std::uint8_t> bytes) {
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) {
		return false;
	}
	file.write(
		reinterpret_cast<const char *>(bytes.data()),
		static_cast<std::streamsize>(bytes.size()));
	file.close();
	return !file.fail();
}

#ifdef _WIN32

[[nodiscard]] bool Protect(
		std::span<const std::uint8_t> plain,
		TokenBlob &encrypted) {
	if (plain.size() > (std::numeric_limits<DWORD>::max)()) {
		return false;
	}
	TokenBlob copy(plain.begin(), plain.end());
	DATA_BLOB input{};
	input.pbData = copy.data();
	input.cbData = static_cast<DWORD>(copy.size());
	DATA_BLOB output{};
	const BOOL ok = CryptProtectData(
		&input,
		kProtectDescription,
		nullptr,
		nullptr,
		nullptr,
		CRYPTPROTECT_UI_FORBIDDEN,
		&output);
	if (!ok) {
		return false;
	}
	encrypted.assign(output.pbData, output.pbData + output.cbData);
	LocalFree(output.pbData);
	return true;
}

[[nodiscard]] bool Unprotect(TokenBlob &encrypted, TokenBlob &plain) {
	if (encrypted.size() > (std::numeric_limits<DWORD>::max)()) {
		return false;
	}
	DATA_BLOB input{};
	input.pbData = encrypted.data();
	input.cbData = static_cast<DWORD>(encrypted.size());
	DATA_BLOB output{};
	const BOOL ok = CryptUnprotectData(
		&input,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		CRYPTPROTECT_UI_FORBIDDEN,
		&output);
	if (!ok) {
		return false;
	}
	plain.assign(output.pbData, output.pbData + output.cbData);
	SecureZeroMemory(output.pbData, output.cbData);
	LocalFree(output.pbData);
	return true;
}

#else // _WIN32

[[nodiscard]] bool Protect(
		std::span<const std::uint8_t> plain,
		TokenBlob &encrypted) {
	return false;
}

[[nodiscard]] bool Unprotect(TokenBlob &encrypted, TokenBlob &plain) {
	return false;
}

#endif // _WIN32

} // namespace

DpapiTokenStore::DpapiTokenStore(std::filesystem::path path)
: filePath(std::move(path)) {
}

TokenBlob DpapiTokenStore::load() {
	TokenBlob encrypted = ReadWholeFile(filePath);
	if (encrypted.empty()) {
		return {};
	}
	TokenBlob plain;
	if (!Unprotect(encrypted, plain)) {
		return {};
	}
	return plain;
}

bool DpapiTokenStore::save(std::span<const std::uint8_t> blob) {
	if (blob.empty()) {
		clear();
		return true;
	}
	TokenBlob encrypted;
	if (!Protect(blob, encrypted)) {
		return false;
	}
	std::error_code error;
	std::filesystem::create_directories(filePath.parent_path(), error);
	const std::filesystem::path temp = TempPathFor(filePath);
	if (!WriteWholeFile(temp, encrypted)) {
		std::filesystem::remove(temp, error);
		return false;
	}
	std::filesystem::rename(temp, filePath, error);
	if (error) {
		std::filesystem::remove(temp, error);
		return false;
	}
	return true;
}

void DpapiTokenStore::clear() {
	std::error_code error;
	std::filesystem::remove(filePath, error);
	std::filesystem::remove(TempPathFor(filePath), error);
}

} // namespace Impulse
