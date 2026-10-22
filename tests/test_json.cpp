#include "check.hpp"
#include "storage/core/json.hpp"

using storage::json::Value;

TEST_CASE("json dumps objects in insertion order") {
    Value v = Value::object({{"b", 1}, {"a", "x"}, {"list", Value::array({true, nullptr, 2.5})}});
    CHECK(v.dump() == R"({"b":1,"a":"x","list":[true,null,2.5]})");
    CHECK(Value::object().dump() == "{}");
    CHECK(Value::array().dump(2) == "[]");
}

TEST_CASE("json escapes strings") {
    CHECK(Value("a\"b\\c\n\x01").dump() == R"("a\"b\\c\n\u0001")");
    CHECK(Value("héllo").dump() == "\"héllo\"");
}

TEST_CASE("json parses what it dumps") {
    const std::string text = R"({"name":"x y","n":-42,"big":9007199254740993,"f":1e3,"ok":false,"nested":{"arr":[1,[2,{}]]},"u":"é😀"})";
    Value v = Value::parse(text);
    CHECK(v.get("name").asString() == "x y");
    CHECK(v.get("n").asInt() == -42);
    CHECK(v.get("big").asInt() == 9007199254740993LL);
    CHECK(v.get("f").asDouble() == 1000.0);
    CHECK(v.get("ok").isBool());
    CHECK(!v.get("ok").asBool(true));
    CHECK(v.get("nested").get("arr").size() == 2);
    CHECK(v.get("u").asString() == "é😀");
    CHECK(v.get("missing").isNull());
    CHECK(Value::parse(v.dump(2)).dump() == v.dump());
}

TEST_CASE("json rejects malformed input") {
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "tru", "\"unterminated", "1 2", "{\"a\":1,}"}) {
        bool threw = false;
        try {
            Value::parse(bad);
        } catch (const storage::json::ParseError&) {
            threw = true;
        }
        CHECK(threw);
    }
}
