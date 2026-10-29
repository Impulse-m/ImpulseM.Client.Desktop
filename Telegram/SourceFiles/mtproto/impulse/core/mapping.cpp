#include "mtproto/impulse/core/mapping.h"

#include "mtproto/impulse/core/json.h"

#include <zlib.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <system_error>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Impulse {
namespace {

constexpr std::size_t kMaxMappingBytes = std::size_t{64} * 1024 * 1024;
constexpr std::size_t kInflateChunkBytes = 64 * 1024;
constexpr std::int64_t kMaxUint32 = std::numeric_limits<std::uint32_t>::max();
constexpr std::int64_t kMaxFlagBit = 31;

[[nodiscard]] std::optional<std::uint32_t> uint32Member(
		const Json::Value *node) {
	if (!node) {
		return std::nullopt;
	}
	const std::optional<std::int64_t> value = node->asInt();
	if (!value || *value < 0 || *value > kMaxUint32) {
		return std::nullopt;
	}
	return static_cast<std::uint32_t>(*value);
}

[[nodiscard]] const std::string *stringMember(
		const Json::Value &node,
		std::string_view key) {
	const Json::Value *member = node.find(key);
	return member ? member->asString() : nullptr;
}

bool setError(std::string *error, const std::string &message) {
	if (error) {
		*error = message;
	}
	return false;
}

bool gunzip(
		std::span<const std::uint8_t> data,
		std::string &out,
		std::string &message) {
	if (data.size() > static_cast<std::size_t>(std::numeric_limits<uInt>::max())) {
		message = "mapping file is too large for zlib";
		return false;
	}
	z_stream stream{};
	if (inflateInit2(&stream, 15 + 16) != Z_OK) {
		message = "zlib gzip decoder cannot be initialised";
		return false;
	}
	stream.next_in = const_cast<Bytef *>(data.data());
	stream.avail_in = static_cast<uInt>(data.size());
	std::array<Bytef, kInflateChunkBytes> chunk = {};
	int status = Z_OK;
	while (status != Z_STREAM_END) {
		stream.next_out = chunk.data();
		stream.avail_out = static_cast<uInt>(chunk.size());
		status = inflate(&stream, Z_NO_FLUSH);
		if (status != Z_OK && status != Z_STREAM_END) {
			message = "mapping file is not a complete gzip stream";
			inflateEnd(&stream);
			return false;
		}
		const std::size_t produced = chunk.size() - stream.avail_out;
		if (out.size() + produced > kMaxMappingBytes) {
			message = "decompressed mapping is larger than 64 MiB";
			inflateEnd(&stream);
			return false;
		}
		out.append(reinterpret_cast<const char *>(chunk.data()), produced);
	}
	inflateEnd(&stream);
	return true;
}

bool parseField(
		const Json::Value &node,
		MappingField &field,
		std::string &error) {
	const std::string *name = stringMember(node, "name");
	const std::string *kind = stringMember(node, "kind");
	if (!name || !kind) {
		error = "mapping parameter without name or kind";
		return false;
	}
	field.name = *name;
	field.kind = *kind;
	if (const Json::Value *number = node.find("field")) {
		const std::optional<std::uint32_t> value = uint32Member(number);
		if (!value) {
			error = "mapping parameter " + field.name + " has a bad field number";
			return false;
		}
		field.number = *value;
	}
	if (const std::string *type = stringMember(node, "type")) {
		field.type = *type;
	}
	if (const Json::Value *flag = node.find("flag")) {
		const std::vector<Json::Value> *pair = flag->asArray();
		const std::string *flagsName = nullptr;
		std::optional<std::int64_t> bit;
		if (pair && pair->size() == 2) {
			flagsName = (*pair)[0].asString();
			bit = (*pair)[1].asInt();
		}
		if (!flagsName || !bit || *bit < 0 || *bit > kMaxFlagBit) {
			error = "mapping parameter " + field.name + " has a bad flag";
			return false;
		}
		field.optionalFlag = true;
		field.flagsName = *flagsName;
		field.flagBit = static_cast<std::int32_t>(*bit);
	}
	if (const std::string *derived = stringMember(node, "derived_from")) {
		field.derivedFrom = *derived;
	}
	if (const Json::Value *unmapped = node.find("unmapped")) {
		const std::optional<bool> value = unmapped->asBool();
		if (!value) {
			error = "mapping parameter " + field.name + " has a bad unmapped flag";
			return false;
		}
		field.unmapped = *value;
	}
	if (const Json::Value *elem = node.find("elem")) {
		if (field.kind != "vector") {
			error = "mapping parameter " + field.name + " has elem but is not a vector";
			return false;
		}
		const std::string *elemKind = stringMember(*elem, "kind");
		if (!elemKind) {
			error = "mapping vector " + field.name + " has no element kind";
			return false;
		}
		field.repeated = true;
		field.kind = *elemKind;
		field.type.clear();
		if (const std::string *elemType = stringMember(*elem, "type")) {
			field.type = *elemType;
		}
	} else if (field.kind == "vector") {
		error = "mapping vector " + field.name + " has no elem";
		return false;
	}
	return true;
}

bool parseFieldList(
		const Json::Value *node,
		std::vector<MappingField> &out,
		std::string &error) {
	const std::vector<Json::Value> *items = node ? node->asArray() : nullptr;
	if (!items) {
		error = "mapping parameter list is missing or not an array";
		return false;
	}
	out.reserve(items->size());
	for (const Json::Value &item : *items) {
		MappingField field;
		if (!parseField(item, field, error)) {
			return false;
		}
		out.push_back(std::move(field));
	}
	return true;
}

bool parseCaptureList(
		const Json::Value *node,
		std::vector<MappingField> &out,
		std::string &error) {
	const std::vector<Json::Value> *items = node ? node->asArray() : nullptr;
	if (!items) {
		error = "mapping capture list is missing or not an array";
		return false;
	}
	for (const Json::Value &item : *items) {
		const std::string *name = stringMember(item, "name");
		const std::optional<std::uint32_t> number = uint32Member(item.find("field"));
		if (!name || !number) {
			error = "mapping capture without name or field number";
			return false;
		}
		MappingField field;
		field.name = *name;
		field.number = *number;
		out.push_back(std::move(field));
	}
	return true;
}

bool parseConstructor(
		const Json::Value &node,
		MappingConstructor &ctor,
		std::string &error) {
	const std::optional<std::uint32_t> id = uint32Member(node.find("id"));
	const std::string *predicate = stringMember(node, "predicate");
	const std::string *type = stringMember(node, "type");
	if (!id || !predicate || !type) {
		error = "mapping constructor without id, predicate or type";
		return false;
	}
	ctor.id = *id;
	ctor.name = *predicate;
	ctor.type = *type;
	const Json::Value *proto = node.find("proto");
	if (!proto) {
		error = "mapping constructor " + ctor.name + " has no proto";
		return false;
	}
	if (const std::string *protoName = proto->asString()) {
		ctor.protoName = *protoName;
	} else if (proto->kind() != Json::Kind::Null) {
		error = "mapping constructor " + ctor.name + " has a bad proto";
		return false;
	}
	if (const Json::Value *arm = node.find("arm")) {
		if (arm->kind() != Json::Kind::Null) {
			const std::optional<std::uint32_t> value = uint32Member(arm);
			if (!value || *value > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
				error = "mapping constructor " + ctor.name + " has a bad arm";
				return false;
			}
			ctor.arm = static_cast<std::int32_t>(*value);
		}
	}
	if (!parseFieldList(node.find("params"), ctor.fields, error)) {
		return false;
	}
	return parseCaptureList(node.find("capture"), ctor.captures, error);
}

bool parseResultElement(
		const Json::Value &node,
		MappingField &element,
		std::string &error) {
	const std::string *kind = stringMember(node, "kind");
	if (!kind) {
		error = "mapping result element has no kind";
		return false;
	}
	element.kind = *kind;
	if (const std::string *type = stringMember(node, "type")) {
		element.type = *type;
	}
	return true;
}

bool parseMethod(
		const Json::Value &node,
		MappingMethod &method,
		std::string &error) {
	const std::optional<std::uint32_t> id = uint32Member(node.find("id"));
	const std::string *name = stringMember(node, "method");
	if (!id || !name) {
		error = "mapping method without id or name";
		return false;
	}
	method.id = *id;
	method.name = *name;
	const Json::Value *result = node.find("result");
	const std::string *resultType = nullptr;
	if (result) {
		resultType = result->asString();
		if (!resultType) {
			resultType = stringMember(*result, "type");
		}
		if (!resultType) {
			resultType = stringMember(*result, "kind");
		}
	}
	if (!resultType) {
		error = "mapping method " + method.name + " has no result type";
		return false;
	}
	method.result = *resultType;
	if (const std::string *resultKind = result->asString()) {
		method.resultKind = *resultKind;
	} else {
		const std::string *kind = stringMember(*result, "kind");
		if (!kind) {
			error = "mapping method " + method.name + " has no result kind";
			return false;
		}
		method.resultKind = *kind;
		if (const std::string *type = stringMember(*result, "type")) {
			method.resultType = *type;
		}
		if (const Json::Value *elem = result->find("elem")) {
			if (!parseResultElement(*elem, method.resultElement, error)) {
				return false;
			}
		}
	}
	return parseFieldList(node.find("params"), method.params, error);
}

bool parseArmKey(
		const std::string &text,
		std::uint32_t &arm) {
	if (text.empty()) {
		return false;
	}
	std::uint32_t value = 0;
	const char *first = text.data();
	const char *last = text.data() + text.size();
	const std::from_chars_result parsed = std::from_chars(first, last, value);
	if (parsed.ec != std::errc() || parsed.ptr != last) {
		return false;
	}
	arm = value;
	return true;
}

// The types list is optional: a mapping without it has no decodable objects.
bool parseTypes(
		const Json::Value &root,
		Mapping &out,
		std::string &error) {
	const Json::Value *node = root.find("types");
	if (!node) {
		return true;
	}
	const std::vector<Json::Value> *items = node->asArray();
	if (!items) {
		error = "mapping type list is not an array";
		return false;
	}
	std::unordered_set<std::string> typeNames;
	out.types.reserve(items->size());
	for (const Json::Value &item : *items) {
		const std::string *name = stringMember(item, "name");
		if (!name) {
			error = "mapping type without name";
			return false;
		}
		if (!typeNames.insert(*name).second) {
			error = "duplicate type " + *name;
			return false;
		}
		MappingType type;
		type.name = *name;
		if (const Json::Value *polymorphic = item.find("polymorphic")) {
			const std::optional<bool> flag = polymorphic->asBool();
			if (!flag) {
				error = "mapping type " + type.name + " has a bad polymorphic flag";
				return false;
			}
			type.polymorphic = *flag;
		}
		if (const Json::Value *arms = item.find("arms")) {
			const std::vector<Json::Value::Member> *members = arms->asObject();
			if (!members) {
				error = "mapping type " + type.name + " has bad arms";
				return false;
			}
			for (const Json::Value::Member &member : *members) {
				MappingArm arm;
				const std::optional<std::uint32_t> constructorId = uint32Member(&member.second);
				if (!parseArmKey(member.first, arm.arm) || !constructorId) {
					error = "mapping type " + type.name + " has a bad arm";
					return false;
				}
				arm.constructorId = *constructorId;
				type.arms.push_back(arm);
			}
		}
		out.types.push_back(std::move(type));
	}
	return true;
}

bool parseMapping(
		const Json::Value &root,
		Mapping &out,
		std::string &error) {
	if (root.kind() != Json::Kind::Object) {
		error = "mapping root is not a JSON object";
		return false;
	}
	const Json::Value *layerNode = root.find("layer");
	const std::optional<std::int64_t> layer = layerNode ? layerNode->asInt() : std::nullopt;
	if (!layer) {
		error = "mapping has no layer";
		return false;
	}
	if (*layer != kMappingLayer) {
		error = "unsupported mapping layer " + std::to_string(*layer);
		return false;
	}
	out.layer = static_cast<std::int32_t>(*layer);

	const std::vector<Json::Value> *constructors = nullptr;
	if (const Json::Value *node = root.find("constructors")) {
		constructors = node->asArray();
	}
	if (!constructors) {
		error = "mapping has no constructor list";
		return false;
	}
	std::unordered_set<std::uint32_t> constructorIds;
	std::unordered_set<std::string> constructorNames;
	out.constructors.reserve(constructors->size());
	for (const Json::Value &item : *constructors) {
		MappingConstructor ctor;
		if (!parseConstructor(item, ctor, error)) {
			return false;
		}
		if (!constructorIds.insert(ctor.id).second) {
			error = "duplicate constructor id " + std::to_string(ctor.id);
			return false;
		}
		if (!constructorNames.insert(ctor.name).second) {
			error = "duplicate constructor " + ctor.name;
			return false;
		}
		out.constructors.push_back(std::move(ctor));
	}

	const std::vector<Json::Value> *methods = nullptr;
	if (const Json::Value *node = root.find("methods")) {
		methods = node->asArray();
	}
	if (!methods) {
		error = "mapping has no method list";
		return false;
	}
	std::unordered_set<std::string> methodNames;
	out.methods.reserve(methods->size());
	for (const Json::Value &item : *methods) {
		MappingMethod method;
		if (!parseMethod(item, method, error)) {
			return false;
		}
		if (!methodNames.insert(method.name).second) {
			error = "duplicate method " + method.name;
			return false;
		}
		out.methods.push_back(std::move(method));
	}

	if (!parseTypes(root, out, error)) {
		return false;
	}

	const Json::Value *wrappers = root.find("wrappers");
	const std::vector<Json::Value::Member> *members = wrappers ? wrappers->asObject() : nullptr;
	if (!members) {
		error = "mapping has no wrapper object";
		return false;
	}
	for (const Json::Value::Member &member : *members) {
		const std::optional<std::uint32_t> id = uint32Member(&member.second);
		if (!id) {
			error = "mapping wrapper " + member.first + " has a bad id";
			return false;
		}
		out.wrapperIds.push_back(*id);
	}
	return true;
}

} // namespace

MappingTable::MappingTable(Mapping source) : data(std::move(source)) {
	constructorIndex.reserve(data.constructors.size());
	constructorNameIndex.reserve(data.constructors.size());
	fieldIndex.reserve(data.constructors.size());
	for (std::size_t i = 0; i < data.constructors.size(); ++i) {
		const MappingConstructor &ctor = data.constructors[i];
		constructorIndex.emplace(ctor.id, i);
		constructorNameIndex.emplace(ctor.name, i);
		std::unordered_map<std::uint32_t, std::size_t> numbers;
		for (std::size_t j = 0; j < ctor.fields.size(); ++j) {
			const std::uint32_t number = ctor.fields[j].number;
			if (number != 0) {
				numbers.emplace(number, j);
			}
		}
		fieldIndex.push_back(std::move(numbers));
	}
	methodIndex.reserve(data.methods.size());
	for (std::size_t i = 0; i < data.methods.size(); ++i) {
		methodIndex.emplace(data.methods[i].name, i);
	}
}

std::optional<MappingTable> MappingTable::LoadJson(
		std::string_view text,
		std::string *error) {
	Json::ParseResult parsed = Json::parse(text);
	if (!parsed.value) {
		setError(error, "mapping JSON is invalid: " + parsed.error);
		return std::nullopt;
	}
	Mapping mapping;
	std::string message;
	if (!parseMapping(*parsed.value, mapping, message)) {
		setError(error, message);
		return std::nullopt;
	}
	return MappingTable(std::move(mapping));
}

std::optional<MappingTable> MappingTable::LoadGzip(
		std::span<const std::uint8_t> data,
		std::string *error) {
	std::string json;
	std::string message;
	if (!gunzip(data, json, message)) {
		setError(error, message);
		return std::nullopt;
	}
	return LoadJson(json, error);
}

std::optional<MappingTable> MappingTable::LoadGzipFile(
		const std::string &path,
		std::string *error) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		setError(error, "cannot open mapping file " + path);
		return std::nullopt;
	}
	const std::string bytes(
		(std::istreambuf_iterator<char>(file)),
		std::istreambuf_iterator<char>());
	return LoadGzip(
		std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t *>(bytes.data()),
			bytes.size()),
		error);
}

std::int32_t MappingTable::layer() const noexcept {
	return data.layer;
}

const Mapping &MappingTable::mapping() const noexcept {
	return data;
}

const MappingConstructor *MappingTable::findConstructor(
		std::uint32_t id) const {
	const std::unordered_map<std::uint32_t, std::size_t>::const_iterator found = constructorIndex.find(id);
	return found == constructorIndex.end()
		? nullptr
		: &data.constructors[found->second];
}

const MappingConstructor *MappingTable::findConstructorByName(
		std::string_view name) const {
	const std::unordered_map<std::string, std::size_t>::const_iterator found = constructorNameIndex.find(std::string(name));
	return found == constructorNameIndex.end()
		? nullptr
		: &data.constructors[found->second];
}

const MappingMethod *MappingTable::findMethod(std::string_view name) const {
	const std::unordered_map<std::string, std::size_t>::const_iterator found = methodIndex.find(std::string(name));
	return found == methodIndex.end()
		? nullptr
		: &data.methods[found->second];
}

std::optional<std::uint32_t> MappingTable::methodId(
		std::string_view name) const {
	const MappingMethod *method = findMethod(name);
	return method ? std::make_optional(method->id) : std::nullopt;
}

const MappingField *MappingTable::findField(
		std::uint32_t constructorId,
		std::uint32_t number) const {
	const std::unordered_map<std::uint32_t, std::size_t>::const_iterator found = constructorIndex.find(constructorId);
	if (found == constructorIndex.end()) {
		return nullptr;
	}
	const std::unordered_map<std::uint32_t, std::size_t> &numbers =
		fieldIndex[found->second];
	const std::unordered_map<std::uint32_t, std::size_t>::const_iterator field = numbers.find(number);
	return field == numbers.end()
		? nullptr
		: &data.constructors[found->second].fields[field->second];
}

} // namespace Impulse
