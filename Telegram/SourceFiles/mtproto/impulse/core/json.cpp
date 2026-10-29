#include "mtproto/impulse/core/json.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Impulse::Json {
namespace {

[[nodiscard]] bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

[[nodiscard]] int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    } else if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void appendUtf8(std::string &out, std::uint32_t codePoint) {
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

struct Parser final {
    std::string_view text;


    std::size_t pos = 0;


    std::size_t depth = 0;


    std::string error;


    std::size_t errorOffset = 0;


    bool parseDocument(Value &out) {
        if (!parseValue(out)) {
            return false;
        }
        skipWhitespace();
        if (pos != text.size()) {
            return fail("trailing data after document");
        }
        return true;
    }


    bool fail(const char *message) {
        error = message;
        errorOffset = pos;
        return false;
    }


    void skipWhitespace() {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++pos;
        }
    }


    bool consume(char expected) {
        if (pos < text.size() && text[pos] == expected) {
            ++pos;
            return true;
        }
        return false;
    }


    bool matchWord(std::string_view word) {
        if (text.substr(pos, word.size()) == word) {
            pos += word.size();
            return true;
        }
        return false;
    }


    bool parseValue(Value &out) {
        skipWhitespace();
        if (pos >= text.size()) {
            return fail("unexpected end of input");
        }
        const char c = text[pos];
        if (c == '{') {
            return parseObject(out);
        } else if (c == '[') {
            return parseArray(out);
        } else if (c == '"') {
            std::string value;
            if (!parseString(value)) {
                return false;
            }
            out = Value::String(std::move(value));
            return true;
        } else if (matchWord("true")) {
            out = Value::Bool(true);
            return true;
        } else if (matchWord("false")) {
            out = Value::Bool(false);
            return true;
        } else if (matchWord("null")) {
            out = Value::Null();
            return true;
        } else if (c == '-' || isDigit(c)) {
            return parseNumber(out);
        }
        return fail("unexpected character");
    }


    bool parseObject(Value &out) {
        if (depth >= kMaxDepth) {
            return fail("nesting depth exceeds limit");
        }
        ++depth;
        const bool ok = parseObjectBody(out);
        --depth;
        return ok;
    }


    bool parseObjectBody(Value &out) {
        ++pos;
        std::vector<Value::Member> members;
        skipWhitespace();
        if (consume('}')) {
            out = Value::Object(std::move(members));
            return true;
        }
        while (true) {
            skipWhitespace();
            if (pos >= text.size() || text[pos] != '"') {
                return fail("expected object key");
            }
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipWhitespace();
            if (!consume(':')) {
                return fail("expected ':' after object key");
            }
            Value item;
            if (!parseValue(item)) {
                return false;
            }
            bool replaced = false;
            for (Value::Member &member : members) {
                if (member.first == key) {
                    member.second = std::move(item);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                members.emplace_back(std::move(key), std::move(item));
            }
            skipWhitespace();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                break;
            }
            return fail("expected ',' or '}' in object");
        }
        out = Value::Object(std::move(members));
        return true;
    }


    bool parseArray(Value &out) {
        if (depth >= kMaxDepth) {
            return fail("nesting depth exceeds limit");
        }
        ++depth;
        const bool ok = parseArrayBody(out);
        --depth;
        return ok;
    }


    bool parseArrayBody(Value &out) {
        ++pos;
        std::vector<Value> items;
        skipWhitespace();
        if (consume(']')) {
            out = Value::Array(std::move(items));
            return true;
        }
        while (true) {
            Value item;
            if (!parseValue(item)) {
                return false;
            }
            items.push_back(std::move(item));
            skipWhitespace();
            if (consume(',')) {
                continue;
            }
            if (consume(']')) {
                break;
            }
            return fail("expected ',' or ']' in array");
        }
        out = Value::Array(std::move(items));
        return true;
    }


    bool parseHex4(std::uint32_t &out) {
        if (pos + 4 > text.size()) {
            return fail("truncated unicode escape");
        }
        std::uint32_t value = 0;
        for (std::size_t i = 0; i != 4; ++i) {
            const int digit = hexValue(text[pos + i]);
            if (digit < 0) {
                pos += i;
                return fail("invalid unicode escape");
            }
            value = value * 16 + static_cast<std::uint32_t>(digit);
        }
        pos += 4;
        out = value;
        return true;
    }


    bool parseUnicodeEscape(std::string &out) {
        std::uint32_t codePoint = 0;
        if (!parseHex4(codePoint)) {
            return false;
        }
        if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
            return fail("unpaired low surrogate");
        }
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
            if (pos + 1 >= text.size() || text[pos] != '\\' || text[pos + 1] != 'u') {
                return fail("unpaired high surrogate");
            }
            pos += 2;
            std::uint32_t low = 0;
            if (!parseHex4(low)) {
                return false;
            }
            if (low < 0xDC00 || low > 0xDFFF) {
                return fail("invalid surrogate pair");
            }
            codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
        }
        appendUtf8(out, codePoint);
        return true;
    }


    bool parseString(std::string &out) {
        if (pos >= text.size() || text[pos] != '"') {
            return fail("expected string");
        }
        ++pos;
        out.clear();
        while (true) {
            if (pos >= text.size()) {
                return fail("unterminated string");
            }
            const unsigned char c = static_cast<unsigned char>(text[pos]);
            if (c == '"') {
                ++pos;
                return true;
            }
            if (c < 0x20) {
                return fail("control character in string");
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                ++pos;
                continue;
            }
            ++pos;
            if (pos >= text.size()) {
                return fail("unterminated escape");
            }
            const char escape = text[pos];
            ++pos;
            if (escape == '"') {
                out.push_back('"');
            } else if (escape == '\\') {
                out.push_back('\\');
            } else if (escape == '/') {
                out.push_back('/');
            } else if (escape == 'b') {
                out.push_back('\b');
            } else if (escape == 'f') {
                out.push_back('\f');
            } else if (escape == 'n') {
                out.push_back('\n');
            } else if (escape == 'r') {
                out.push_back('\r');
            } else if (escape == 't') {
                out.push_back('\t');
            } else if (escape == 'u') {
                if (!parseUnicodeEscape(out)) {
                    return false;
                }
            } else {
                return fail("invalid escape in string");
            }
        }
    }


    bool parseNumber(Value &out) {
        const std::size_t start = pos;
        if (text[pos] == '-') {
            ++pos;
        }
        if (pos >= text.size()) {
            return fail("invalid number");
        }
        if (text[pos] == '0') {
            ++pos;
        } else if (isDigit(text[pos])) {
            while (pos < text.size() && isDigit(text[pos])) {
                ++pos;
            }
        } else {
            return fail("invalid number");
        }
        bool isReal = false;
        if (pos < text.size() && text[pos] == '.') {
            isReal = true;
            ++pos;
            if (pos >= text.size() || !isDigit(text[pos])) {
                return fail("invalid number fraction");
            }
            while (pos < text.size() && isDigit(text[pos])) {
                ++pos;
            }
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            isReal = true;
            ++pos;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
                ++pos;
            }
            if (pos >= text.size() || !isDigit(text[pos])) {
                return fail("invalid number exponent");
            }
            while (pos < text.size() && isDigit(text[pos])) {
                ++pos;
            }
        }
        const char *first = text.data() + start;
        const char *last = text.data() + pos;
        if (isReal) {
            double real = 0.;
            const std::from_chars_result result = std::from_chars(first, last, real);
            if (result.ec != std::errc() || result.ptr != last) {
                return fail("number out of range");
            }
            out = Value::Double(real);
            return true;
        }
        std::int64_t integer = 0;
        const std::from_chars_result result = std::from_chars(first, last, integer);
        if (result.ec != std::errc() || result.ptr != last) {
            return fail("integer out of range");
        }
        out = Value::Int(integer);
        return true;
    }
};

} // namespace


Value Value::Null() {
    return Value();
}


Value Value::Bool(bool value) {
    Value result;
    result.valueKind = Kind::Bool;
    result.boolValue = value;
    return result;
}


Value Value::Int(std::int64_t value) {
    Value result;
    result.valueKind = Kind::Int;
    result.intValue = value;
    return result;
}


Value Value::Double(double value) {
    Value result;
    result.valueKind = Kind::Double;
    result.doubleValue = value;
    return result;
}


Value Value::String(std::string value) {
    Value result;
    result.valueKind = Kind::String;
    result.stringValue = std::move(value);
    return result;
}


Value Value::Array(std::vector<Value> items) {
    Value result;
    result.valueKind = Kind::Array;
    result.arrayValue = std::move(items);
    return result;
}


Value Value::Object(std::vector<Member> members) {
    Value result;
    result.valueKind = Kind::Object;
    result.objectValue = std::move(members);
    return result;
}


Kind Value::kind() const noexcept {
    return valueKind;
}


std::optional<bool> Value::asBool() const noexcept {
    if (valueKind != Kind::Bool) {
        return std::nullopt;
    }
    return boolValue;
}


std::optional<std::int64_t> Value::asInt() const noexcept {
    if (valueKind != Kind::Int) {
        return std::nullopt;
    }
    return intValue;
}


std::optional<double> Value::asDouble() const noexcept {
    if (valueKind != Kind::Double) {
        return std::nullopt;
    }
    return doubleValue;
}


const std::string *Value::asString() const noexcept {
    return (valueKind == Kind::String) ? &stringValue : nullptr;
}


const std::vector<Value> *Value::asArray() const noexcept {
    return (valueKind == Kind::Array) ? &arrayValue : nullptr;
}


const std::vector<Value::Member> *Value::asObject() const noexcept {
    return (valueKind == Kind::Object) ? &objectValue : nullptr;
}


const Value *Value::find(std::string_view key) const noexcept {
    if (valueKind != Kind::Object) {
        return nullptr;
    }
    for (const Member &member : objectValue) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}


ParseResult parse(std::string_view text) {
    Parser parser{ text };
    Value root;
    ParseResult result;
    if (parser.parseDocument(root)) {
        result.value = std::move(root);
        return result;
    }
    result.error = parser.error;
    result.errorOffset = parser.errorOffset;
    return result;
}

} // namespace Impulse::Json
