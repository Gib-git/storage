#pragma once

// Small self-contained JSON value with an order-preserving object type.
// Enough for CLI output, the REST/MCP APIs and talking to Ollama.

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace storage::json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Value {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : v_(b) {}
    Value(int i) : v_(static_cast<int64_t>(i)) {}
    Value(unsigned i) : v_(static_cast<int64_t>(i)) {}
    Value(long i) : v_(static_cast<int64_t>(i)) {}
    Value(unsigned long i) : v_(static_cast<int64_t>(i)) {}
    Value(long long i) : v_(static_cast<int64_t>(i)) {}
    Value(unsigned long long i) : v_(static_cast<int64_t>(i)) {}
    Value(double d) : v_(d) {}
    Value(const char* s) : v_(std::string(s)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(Array a) : v_(std::move(a)) {}
    Value(Object o) : v_(std::move(o)) {}

    static Value object(std::initializer_list<std::pair<std::string, Value>> items = {}) { return Value(Object(items)); }
    static Value array(std::initializer_list<Value> items = {}) { return Value(Array(items)); }

    Type type() const { return static_cast<Type>(v_.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isNumber() const { return type() == Type::Int || type() == Type::Double; }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    bool asBool(bool fallback = false) const;
    int64_t asInt(int64_t fallback = 0) const;
    double asDouble(double fallback = 0.0) const;
    std::string asString(const std::string& fallback = {}) const;

    Array& items();
    const Array& items() const;
    Object& members();
    const Object& members() const;

    // Object access: operator[] inserts (turning null into an object); get() returns null when missing.
    Value& operator[](const std::string& key);
    const Value& get(const std::string& key) const;
    bool contains(const std::string& key) const;

    // Array access.
    Value& operator[](size_t i) { return items().at(i); }
    const Value& operator[](size_t i) const { return items().at(i); }
    void push_back(Value v);
    size_t size() const;

    // indent < 0 produces compact output.
    std::string dump(int indent = -1) const;
    static Value parse(const std::string& text);

private:
    void dumpTo(std::string& out, int indent, int depth) const;
    std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> v_;
};

std::string escape(const std::string& s);

}  // namespace storage::json
