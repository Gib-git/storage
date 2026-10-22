#include "storage/core/json.hpp"

#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstdlib>

namespace storage::json {

namespace {
const Value& nullValue() {
    static const Value v;
    return v;
}
}  // namespace

bool Value::asBool(bool fallback) const {
    return isBool() ? std::get<bool>(v_) : fallback;
}

int64_t Value::asInt(int64_t fallback) const {
    if (type() == Type::Int) return std::get<int64_t>(v_);
    if (type() == Type::Double) return static_cast<int64_t>(std::get<double>(v_));
    return fallback;
}

double Value::asDouble(double fallback) const {
    if (type() == Type::Double) return std::get<double>(v_);
    if (type() == Type::Int) return static_cast<double>(std::get<int64_t>(v_));
    return fallback;
}

std::string Value::asString(const std::string& fallback) const {
    return isString() ? std::get<std::string>(v_) : fallback;
}

Array& Value::items() {
    if (isNull()) v_ = Array{};
    if (!isArray()) throw std::logic_error("json: not an array");
    return std::get<Array>(v_);
}

const Array& Value::items() const {
    static const Array empty;
    return isArray() ? std::get<Array>(v_) : empty;
}

Object& Value::members() {
    if (isNull()) v_ = Object{};
    if (!isObject()) throw std::logic_error("json: not an object");
    return std::get<Object>(v_);
}

const Object& Value::members() const {
    static const Object empty;
    return isObject() ? std::get<Object>(v_) : empty;
}

Value& Value::operator[](const std::string& key) {
    auto& o = members();
    for (auto& [k, v] : o)
        if (k == key) return v;
    o.emplace_back(key, Value());
    return o.back().second;
}

const Value& Value::get(const std::string& key) const {
    for (const auto& [k, v] : members())
        if (k == key) return v;
    return nullValue();
}

bool Value::contains(const std::string& key) const {
    for (const auto& kv : members())
        if (kv.first == key) return true;
    return false;
}

void Value::push_back(Value v) {
    items().push_back(std::move(v));
}

size_t Value::size() const {
    if (isArray()) return std::get<Array>(v_).size();
    if (isObject()) return std::get<Object>(v_).size();
    return 0;
}

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(ch);  // UTF-8 passes through untouched
                }
        }
    }
    return out;
}

std::string Value::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

void Value::dumpTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent < 0) return;
        out.push_back('\n');
        out.append(static_cast<size_t>(indent * d), ' ');
    };
    switch (type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += std::get<bool>(v_) ? "true" : "false"; break;
        case Type::Int: out += std::to_string(std::get<int64_t>(v_)); break;
        case Type::Double: {
            const double d = std::get<double>(v_);
            if (!std::isfinite(d)) {
                out += "null";
                break;
            }
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.17g", d);
            out += buf;
            break;
        }
        case Type::String:
            out.push_back('"');
            out += escape(std::get<std::string>(v_));
            out.push_back('"');
            break;
        case Type::Array: {
            const auto& a = std::get<Array>(v_);
            if (a.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (size_t i = 0; i < a.size(); ++i) {
                if (i) out.push_back(',');
                newline(depth + 1);
                a[i].dumpTo(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            const auto& o = std::get<Object>(v_);
            if (o.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (size_t i = 0; i < o.size(); ++i) {
                if (i) out.push_back(',');
                newline(depth + 1);
                out.push_back('"');
                out += escape(o[i].first);
                out += indent < 0 ? "\":" : "\": ";
                o[i].second.dumpTo(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back('}');
            break;
        }
    }
}

namespace {

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    Value parseDocument() {
        Value v = parseValue(0);
        skipWs();
        if (pos_ != s_.size()) error("trailing characters");
        return v;
    }

private:
    [[noreturn]] void error(const char* what) const {
        throw ParseError(std::string("json: ") + what + " at offset " + std::to_string(pos_));
    }

    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) ++pos_;
    }

    bool consume(const char* lit) {
        size_t n = 0;
        while (lit[n]) ++n;
        if (s_.compare(pos_, n, lit) != 0) return false;
        pos_ += n;
        return true;
    }

    Value parseValue(int depth) {
        if (depth > 512) error("nesting too deep");
        skipWs();
        if (pos_ >= s_.size()) error("unexpected end");
        const char c = s_[pos_];
        if (c == '{') return parseObject(depth);
        if (c == '[') return parseArray(depth);
        if (c == '"') return Value(parseString());
        if (consume("true")) return Value(true);
        if (consume("false")) return Value(false);
        if (consume("null")) return Value();
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        error("unexpected character");
    }

    Value parseObject(int depth) {
        ++pos_;
        Value v = Value::object();
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return v;
        }
        for (;;) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') error("expected key");
            std::string key = parseString();
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') error("expected ':'");
            ++pos_;
            v.members().emplace_back(std::move(key), parseValue(depth + 1));
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return v;
            }
            error("expected ',' or '}'");
        }
    }

    Value parseArray(int depth) {
        ++pos_;
        Value v = Value::array();
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return v;
        }
        for (;;) {
            v.push_back(parseValue(depth + 1));
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return v;
            }
            error("expected ',' or ']'");
        }
    }

    unsigned hex4() {
        if (pos_ + 4 > s_.size()) error("bad \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else error("bad \\u escape");
        }
        return v;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::string parseString() {
        ++pos_;  // opening quote
        std::string out;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) error("control character in string");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) break;
            const char e = s_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && s_.compare(pos_, 2, "\\u") == 0) {
                        pos_ += 2;
                        const unsigned lo = hex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else error("bad surrogate pair");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: error("bad escape");
            }
        }
        error("unterminated string");
    }

    Value parseNumber() {
        const size_t start = pos_;
        bool isDouble = false;
        if (s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c >= '0' && c <= '9') {
                ++pos_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                isDouble = true;
                ++pos_;
            } else {
                break;
            }
        }
        const std::string num = s_.substr(start, pos_ - start);
        char* end = nullptr;
        if (!isDouble) {
            errno = 0;
            const long long v = std::strtoll(num.c_str(), &end, 10);
            if (errno == 0 && end && *end == 0) return Value(v);
        }
        const double d = std::strtod(num.c_str(), &end);
        if (!end || *end != 0) error("bad number");
        return Value(d);
    }

    const std::string& s_;
    size_t pos_ = 0;
};

}  // namespace

Value Value::parse(const std::string& text) {
    return Parser(text).parseDocument();
}

}  // namespace storage::json
