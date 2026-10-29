#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Impulse {

struct MappingField {
	std::string name;
	std::uint32_t number = 0;
	std::string kind;
	std::string type;
	bool repeated = false;
	bool optionalFlag = false;
	std::string flagsName;
	std::int32_t flagBit = -1;
	std::string oneofGroup;
	bool unmapped = false;
};

struct MappingConstructor {
	std::uint32_t id = 0;
	std::string name;
	std::string type;
	std::string protoName;
	std::int32_t arm = -1;
	std::vector<MappingField> fields;
	std::vector<MappingField> captures;
};

struct MappingMethod {
	std::uint32_t id = 0;
	std::string name;
	std::string result;
	std::vector<MappingField> params;
};

struct Mapping {
	std::int32_t layer = 0;
	std::vector<MappingConstructor> constructors;
	std::vector<MappingMethod> methods;
	std::vector<std::uint32_t> wrapperIds;
};

} // namespace Impulse
