#include "mtproto/impulse/tests/impulse_test.h"

#include "mtproto/impulse/core/json.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::string nestedArrays(std::size_t depth) {
    return std::string(depth, '[') + std::string(depth, ']');
}

} // namespace

IMPULSE_TEST(JsonParsesScalarsAndContainers) {
    const Impulse::Json::ParseResult result = Impulse::Json::parse(R"({"a":[1,true,false,null],"b":"x","c":{}})");
    IMPULSE_CHECK(result.value.has_value());
    if (!result.value) {
        return;
    }
    const Impulse::Json::Value &root = *result.value;
    IMPULSE_CHECK(root.kind() == Impulse::Json::Kind::Object);
    const Impulse::Json::Value *a = root.find("a");
    IMPULSE_CHECK(a != nullptr);
    if (a == nullptr) {
        return;
    }
    const std::vector<Impulse::Json::Value> *items = a->asArray();
    IMPULSE_CHECK(items != nullptr);
    if (items == nullptr) {
        return;
    }
    IMPULSE_CHECK(items->size() == 4);
    IMPULSE_CHECK((*items)[0].asInt() == std::optional<std::int64_t>(1));
    IMPULSE_CHECK((*items)[1].asBool() == std::optional<bool>(true));
    IMPULSE_CHECK((*items)[2].asBool() == std::optional<bool>(false));
    IMPULSE_CHECK((*items)[3].kind() == Impulse::Json::Kind::Null);
    const Impulse::Json::Value *b = root.find("b");
    IMPULSE_CHECK(b != nullptr && b->asString() != nullptr && *b->asString() == "x");
    const Impulse::Json::Value *c = root.find("c");
    IMPULSE_CHECK(c != nullptr && c->kind() == Impulse::Json::Kind::Object);
    IMPULSE_CHECK(root.find("missing") == nullptr);
}

IMPULSE_TEST(JsonDecodesStringEscapes) {
    const Impulse::Json::ParseResult result = Impulse::Json::parse(R"("a\"b\\c\/d\b\f\n\r\té😀")");
    IMPULSE_CHECK(result.value.has_value());
    if (!result.value) {
        return;
    }
    const std::string *text = result.value->asString();
    IMPULSE_CHECK(text != nullptr);
    if (text == nullptr) {
        return;
    }
    const std::string expected = std::string("a\"b\\c/d\b\f\n\r\t") + "\xC3\xA9" + "\xF0\x9F\x98\x80";
    IMPULSE_CHECK(*text == expected);
}

IMPULSE_TEST(JsonKeepsInt64Exact) {
    const Impulse::Json::ParseResult precise = Impulse::Json::parse("9007199254740993");
    IMPULSE_CHECK(precise.value.has_value());
    IMPULSE_CHECK(precise.value && precise.value->asInt() == std::optional<std::int64_t>(9007199254740993LL));

    const Impulse::Json::ParseResult minimum = Impulse::Json::parse("-9223372036854775808");
    IMPULSE_CHECK(minimum.value && minimum.value->asInt() == std::optional<std::int64_t>(std::numeric_limits<std::int64_t>::min()));

    const Impulse::Json::ParseResult maximum = Impulse::Json::parse("9223372036854775807");
    IMPULSE_CHECK(maximum.value && maximum.value->asInt() == std::optional<std::int64_t>(std::numeric_limits<std::int64_t>::max()));

    const Impulse::Json::ParseResult overflow = Impulse::Json::parse("9223372036854775808");
    IMPULSE_CHECK(!overflow.value.has_value());
    IMPULSE_CHECK(!overflow.error.empty());
}

IMPULSE_TEST(JsonParsesDoubles) {
    const Impulse::Json::ParseResult fraction = Impulse::Json::parse("1.5");
    IMPULSE_CHECK(fraction.value && fraction.value->asDouble() == std::optional<double>(1.5));
    IMPULSE_CHECK(fraction.value && !fraction.value->asInt().has_value());

    const Impulse::Json::ParseResult exponent = Impulse::Json::parse("1e3");
    IMPULSE_CHECK(exponent.value && exponent.value->asDouble() == std::optional<double>(1000.0));
}

IMPULSE_TEST(JsonDuplicateKeyKeepsLastValue) {
    const Impulse::Json::ParseResult result = Impulse::Json::parse(R"({"a":1,"a":2})");
    IMPULSE_CHECK(result.value.has_value());
    if (!result.value) {
        return;
    }
    const std::vector<Impulse::Json::Value::Member> *members = result.value->asObject();
    IMPULSE_CHECK(members != nullptr && members->size() == 1);
    const Impulse::Json::Value *a = result.value->find("a");
    IMPULSE_CHECK(a != nullptr && a->asInt() == std::optional<std::int64_t>(2));
}

IMPULSE_TEST(JsonAcceptsSurroundingWhitespace) {
    const Impulse::Json::ParseResult result = Impulse::Json::parse(" \n\t\r{ \"k\" : [ 1 ] } \r\n");
    IMPULSE_CHECK(result.value.has_value());
}

IMPULSE_TEST(JsonDepthLimitIs64) {
    const Impulse::Json::ParseResult atLimit = Impulse::Json::parse(nestedArrays(Impulse::Json::kMaxDepth));
    IMPULSE_CHECK(atLimit.value.has_value());

    const Impulse::Json::ParseResult overLimit = Impulse::Json::parse(nestedArrays(Impulse::Json::kMaxDepth + 1));
    IMPULSE_CHECK(!overLimit.value.has_value());
    IMPULSE_CHECK(!overLimit.error.empty());
}

IMPULSE_TEST(JsonRejectsTrailingGarbage) {
    IMPULSE_CHECK(!Impulse::Json::parse("{} x").value.has_value());
    IMPULSE_CHECK(!Impulse::Json::parse("1 2").value.has_value());
    IMPULSE_CHECK(!Impulse::Json::parse("[1]]").value.has_value());
}

IMPULSE_TEST(JsonRejectsMalformedInput) {
    IMPULSE_CHECK(!Impulse::Json::parse("").value.has_value());
    IMPULSE_CHECK(!Impulse::Json::parse("01").value.has_value());
    IMPULSE_CHECK(!Impulse::Json::parse("[1,]").value.has_value());
    IMPULSE_CHECK(!Impulse::Json::parse("{\"a\" 1}").value.has_value());
    IMPULSE_CHECK(!Impulse::Json::parse("\"abc").value.has_value());
    IMPULSE_CHECK(!Impulse::Json::parse("\"a\x01\"").value.has_value());
    IMPULSE_CHECK(!Impulse::Json::parse("tru").value.has_value());
    IMPULSE_CHECK(!Impulse::Json::parse("-").value.has_value());
    IMPULSE_CHECK(!Impulse::Json::parse("1.").value.has_value());
    const Impulse::Json::ParseResult loneSurrogate = Impulse::Json::parse(R"("\ud83d")");
    IMPULSE_CHECK(!loneSurrogate.value.has_value());
}
