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
	std::string derivedFrom;
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

struct MappingArm {
	std::uint32_t arm = 0;
	std::uint32_t constructorId = 0;
};

struct MappingType {
	std::string name;
	bool polymorphic = false;
	std::vector<MappingArm> arms;
};

struct MappingMethod {
	std::uint32_t id = 0;
	std::string name;
	// The type name of an object result, else its kind (bool, int, long, list).
	std::string result;
	// The result kind: bool, int, long, list or object.
	std::string resultKind;
	// The type name of an object result; empty for the other kinds.
	std::string resultType;
	// The element of a list result; empty kind for the other kinds.
	MappingField resultElement;
	std::vector<MappingField> params;
};

struct Mapping {
	std::int32_t layer = 0;
	std::vector<MappingConstructor> constructors;
	std::vector<MappingType> types;
	std::vector<MappingMethod> methods;
	std::vector<std::uint32_t> wrapperIds;
};

} // namespace Impulse
