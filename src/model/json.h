// A small JSON reader for the documents asn1-viz writes itself
// (asn1-layout/1, asn1-schema/1). No engine, no dependencies.
//
// Numbers are doubles; every integer the formats use (bit offsets, ids) is
// far below 2^53, so they are exact. Strings are UTF-8; \uXXXX escapes are
// decoded to UTF-8 (surrogate pairs included).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace asn1viz::json {

class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& what, size_t offset)
        : std::runtime_error(what + " at byte " + std::to_string(offset)), offset_(offset) {}
    size_t offset() const { return offset_; }

private:
    size_t offset_;
};

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    static Value boolean(bool b);
    static Value number(double d);
    static Value string(std::string s);
    static Value array();
    static Value object();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }
    bool isNumber() const { return type_ == Type::Number; }

    // Typed access; each throws std::runtime_error on a type mismatch.
    bool asBool() const;
    double asNumber() const;
    int64_t asInt() const;
    const std::string& asString() const;
    const std::vector<Value>& items() const;
    const std::map<std::string, Value>& members() const;

    // Object member access. at() throws when the key is missing; get()
    // returns a shared null value instead.
    const Value& at(const std::string& key) const;
    const Value& get(const std::string& key) const;
    bool has(const std::string& key) const;

    // Convenience with defaults for optional members.
    std::string str(const std::string& key, const std::string& fallback = "") const;
    int64_t integer(const std::string& key, int64_t fallback = 0) const;

    // Building (used by the parser and by tests).
    std::vector<Value>& mutableItems();
    std::map<std::string, Value>& mutableMembers();

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::shared_ptr<std::vector<Value>> array_;
    std::shared_ptr<std::map<std::string, Value>> object_;
};

// Parses a whole document; trailing non-whitespace is an error.
Value parse(const std::string& text);

// Reads a file and parses it. Throws std::runtime_error if it cannot be read.
Value parseFile(const std::string& path);

}  // namespace asn1viz::json
