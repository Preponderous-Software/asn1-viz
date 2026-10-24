#include "model/json.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace asn1viz::json {

namespace {

const Value& nullValue() {
    static const Value v;
    return v;
}

[[noreturn]] void typeError(const char* want) { throw std::runtime_error(std::string("JSON value is not ") + want); }

class Parser {
public:
    explicit Parser(const std::string& t) : text_(t) {}

    Value document() {
        Value v = value(0);
        skipSpace();
        if (pos_ != text_.size()) fail("trailing characters after the document");
        return v;
    }

private:
    static constexpr int kMaxDepth = 256;
    const std::string& text_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const std::string& what) const { throw ParseError(what, pos_); }

    void skipSpace() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    char peek() {
        skipSpace();
        if (pos_ >= text_.size()) fail("unexpected end of input");
        return text_[pos_];
    }

    void expect(const char* word) {
        for (const char* p = word; *p; ++p, ++pos_)
            if (pos_ >= text_.size() || text_[pos_] != *p) fail(std::string("expected ") + word);
    }

    Value value(int depth) {
        if (depth > kMaxDepth) fail("nested too deeply");
        const char c = peek();
        switch (c) {
            case '{': return object(depth);
            case '[': return array(depth);
            case '"': return Value::string(string());
            case 't': expect("true"); return Value::boolean(true);
            case 'f': expect("false"); return Value::boolean(false);
            case 'n': expect("null"); return Value();
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return number();
                fail(std::string("unexpected character '") + c + "'");
        }
    }

    Value object(int depth) {
        Value v = Value::object();
        ++pos_;  // {
        if (peek() == '}') {
            ++pos_;
            return v;
        }
        for (;;) {
            if (peek() != '"') fail("expected a member name");
            std::string key = string();
            if (peek() != ':') fail("expected ':'");
            ++pos_;
            v.mutableMembers()[key] = value(depth + 1);
            const char c = peek();
            ++pos_;
            if (c == '}') return v;
            if (c != ',') fail("expected ',' or '}'");
        }
    }

    Value array(int depth) {
        Value v = Value::array();
        ++pos_;  // [
        if (peek() == ']') {
            ++pos_;
            return v;
        }
        for (;;) {
            v.mutableItems().push_back(value(depth + 1));
            const char c = peek();
            ++pos_;
            if (c == ']') return v;
            if (c != ',') fail("expected ',' or ']'");
        }
    }

    unsigned hex4() {
        if (pos_ + 4 > text_.size()) fail("short \\u escape");
        unsigned u = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            u <<= 4;
            if (h >= '0' && h <= '9') u |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') u |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') u |= static_cast<unsigned>(h - 'A' + 10);
            else fail("bad hex digit in \\u escape");
        }
        return u;
    }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::string string() {
        ++pos_;  // "
        std::string out;
        for (;;) {
            if (pos_ >= text_.size()) fail("unterminated string");
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) fail("unterminated escape");
            const char e = text_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u')
                            fail("lone high surrogate");
                        pos_ += 2;
                        const unsigned lo = hex4();
                        if (lo < 0xDC00 || lo >= 0xE000) fail("bad low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
    }

    Value number() {
        const size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        auto digits = [&] {
            const size_t s = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (pos_ == s) fail("expected a digit");
        };
        digits();
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            digits();
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            digits();
        }
        const std::string s = text_.substr(start, pos_ - start);
        return Value::number(std::strtod(s.c_str(), nullptr));
    }
};

}  // namespace

Value Value::boolean(bool b) {
    Value v;
    v.type_ = Type::Bool;
    v.bool_ = b;
    return v;
}
Value Value::number(double d) {
    Value v;
    v.type_ = Type::Number;
    v.number_ = d;
    return v;
}
Value Value::string(std::string s) {
    Value v;
    v.type_ = Type::String;
    v.string_ = std::move(s);
    return v;
}
Value Value::array() {
    Value v;
    v.type_ = Type::Array;
    v.array_ = std::make_shared<std::vector<Value>>();
    return v;
}
Value Value::object() {
    Value v;
    v.type_ = Type::Object;
    v.object_ = std::make_shared<std::map<std::string, Value>>();
    return v;
}

bool Value::asBool() const {
    if (type_ != Type::Bool) typeError("a boolean");
    return bool_;
}
double Value::asNumber() const {
    if (type_ != Type::Number) typeError("a number");
    return number_;
}
int64_t Value::asInt() const {
    const double d = asNumber();
    if (d != std::floor(d)) typeError("an integer");
    return static_cast<int64_t>(d);
}
const std::string& Value::asString() const {
    if (type_ != Type::String) typeError("a string");
    return string_;
}
const std::vector<Value>& Value::items() const {
    if (type_ != Type::Array) typeError("an array");
    return *array_;
}
const std::map<std::string, Value>& Value::members() const {
    if (type_ != Type::Object) typeError("an object");
    return *object_;
}
const Value& Value::at(const std::string& key) const {
    const auto& m = members();
    const auto it = m.find(key);
    if (it == m.end()) throw std::runtime_error("JSON object has no member \"" + key + "\"");
    return it->second;
}
const Value& Value::get(const std::string& key) const {
    if (type_ != Type::Object) return nullValue();
    const auto it = object_->find(key);
    return it == object_->end() ? nullValue() : it->second;
}
bool Value::has(const std::string& key) const { return type_ == Type::Object && object_->count(key) != 0; }
std::string Value::str(const std::string& key, const std::string& fallback) const {
    const Value& v = get(key);
    return v.isString() ? v.asString() : fallback;
}
int64_t Value::integer(const std::string& key, int64_t fallback) const {
    const Value& v = get(key);
    return v.isNumber() ? v.asInt() : fallback;
}
std::vector<Value>& Value::mutableItems() {
    if (type_ != Type::Array) typeError("an array");
    return *array_;
}
std::map<std::string, Value>& Value::mutableMembers() {
    if (type_ != Type::Object) typeError("an object");
    return *object_;
}

Value parse(const std::string& text) { return Parser(text).document(); }

Value parseFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse(ss.str());
}

}  // namespace asn1viz::json
