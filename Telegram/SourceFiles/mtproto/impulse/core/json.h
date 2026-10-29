#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Impulse::Json {

inline constexpr std::size_t kMaxDepth = 64;

enum class Kind {
    Null,
    Bool,
    Int,
    Double,
    String,
    Array,
    Object,
};

class Value final {
public:
    using Member = std::pair<std::string, Value>;


    Value() = default;


    [[nodiscard]] static Value Null();


    [[nodiscard]] static Value Bool(bool value);


    [[nodiscard]] static Value Int(std::int64_t value);


    [[nodiscard]] static Value Double(double value);


    [[nodiscard]] static Value String(std::string value);


    [[nodiscard]] static Value Array(std::vector<Value> items);


    [[nodiscard]] static Value Object(std::vector<Member> members);


    [[nodiscard]] Kind kind() const noexcept;


    [[nodiscard]] std::optional<bool> asBool() const noexcept;


    [[nodiscard]] std::optional<std::int64_t> asInt() const noexcept;


    [[nodiscard]] std::optional<double> asDouble() const noexcept;


    [[nodiscard]] const std::string *asString() const noexcept;


    [[nodiscard]] const std::vector<Value> *asArray() const noexcept;


    [[nodiscard]] const std::vector<Member> *asObject() const noexcept;


    // Returns the first member with this key, or nullptr when this value is
    // not an object or has no such key.
    [[nodiscard]] const Value *find(std::string_view key) const noexcept;

private:
    Kind valueKind = Kind::Null;


    bool boolValue = false;


    std::int64_t intValue = 0;


    double doubleValue = 0.;


    std::string stringValue;


    std::vector<Value> arrayValue;


    std::vector<Member> objectValue;
};


struct ParseResult final {
    std::optional<Value> value;


    std::string error;


    std::size_t errorOffset = 0;
};


// Parses one complete JSON document. Trailing non-whitespace data is an
// error. Integers without fraction or exponent are stored as exact int64;
// values outside the int64 range are an error.
[[nodiscard]] ParseResult parse(std::string_view text);

} // namespace Impulse::Json
