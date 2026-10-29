#pragma once

#include "mtproto/impulse/core/mapping_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Impulse {

inline constexpr std::int32_t kMappingLayer = 232;

class MappingTable final {
private:
	Mapping data;
	std::unordered_map<std::uint32_t, std::size_t> constructorIndex;
	std::unordered_map<std::string, std::size_t> constructorNameIndex;
	std::unordered_map<std::string, std::size_t> methodIndex;
	std::vector<std::unordered_map<std::uint32_t, std::size_t>> fieldIndex;

public:
	// The loaders return std::nullopt on failure and store the reason in
	// *error when error is not null. Only layer 232 is accepted.
	[[nodiscard]] static std::optional<MappingTable> LoadJson(
		std::string_view text,
		std::string *error);
	[[nodiscard]] static std::optional<MappingTable> LoadGzip(
		std::span<const std::uint8_t> data,
		std::string *error);
	[[nodiscard]] static std::optional<MappingTable> LoadGzipFile(
		const std::string &path,
		std::string *error);

	[[nodiscard]] std::int32_t layer() const noexcept;
	[[nodiscard]] const Mapping &mapping() const noexcept;

	// Returns nullptr when the id or the name is unknown.
	[[nodiscard]] const MappingConstructor *findConstructor(
		std::uint32_t id) const;
	[[nodiscard]] const MappingConstructor *findConstructorByName(
		std::string_view name) const;

	// Method names are canonical TL names, such as "messages.sendMessage".
	[[nodiscard]] const MappingMethod *findMethod(
		std::string_view name) const;
	[[nodiscard]] std::optional<std::uint32_t> methodId(
		std::string_view name) const;

	// Looks up a proto field of a constructor by its field number. Flag-only
	// and unnumbered parameters are not indexed.
	[[nodiscard]] const MappingField *findField(
		std::uint32_t constructorId,
		std::uint32_t number) const;

private:
	explicit MappingTable(Mapping source);
};

} // namespace Impulse
