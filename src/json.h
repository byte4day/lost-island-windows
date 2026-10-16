// Small JSON value type: parse, edit, serialize. Enough for config files,
// Claude Code hook payloads and ~/.claude/settings.json.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), b_(b) {}
    Value(int n) : type_(Type::Number), n_(n) {}
    Value(double n) : type_(Type::Number), n_(n) {}
    Value(const char* s) : type_(Type::String), s_(s) {}
    Value(std::string s) : type_(Type::String), s_(std::move(s)) {}
    Value(const std::wstring& s);

    static Value Array() { Value v; v.type_ = Type::Array; return v; }
    static Value Object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
    double asNumber(double def = 0) const { return type_ == Type::Number ? n_ : def; }
    int asInt(int def = 0) const { return type_ == Type::Number ? (int)n_ : def; }
    const std::string& asString() const;
    std::string asString(const std::string& def) const { return isString() ? s_ : def; }
    std::wstring asWString(const std::wstring& def = L"") const;

    // arrays
    size_t size() const;
    const Value& operator[](size_t i) const;
    const Value& operator[](int i) const { return (*this)[(size_t)i]; }
    Value& at(size_t i) { return a_[i]; }
    void push(Value v);
    void erase(size_t i);
    std::vector<Value>& items() { return a_; }
    const std::vector<Value>& items() const { return a_; }

    // objects (keys keep insertion order so rewritten files stay readable)
    bool has(const std::string& key) const;
    const Value& operator[](const std::string& key) const;
    const Value& operator[](const char* key) const { return (*this)[std::string(key)]; }
    Value& set(const std::string& key, Value v);
    Value& get(const std::string& key);          // creates null member
    void remove(const std::string& key);
    const std::vector<std::pair<std::string, Value>>& members() const { return o_; }

    std::string dump(int indent = 2) const;

private:
    void dumpTo(std::string& out, int indent, int depth) const;
    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> a_;
    std::vector<std::pair<std::string, Value>> o_;
};

// Returns false on malformed input; `out` is then Null.
bool Parse(const std::string& text, Value& out);

std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

bool ReadFile(const std::wstring& path, std::string& out);
bool WriteFileAtomic(const std::wstring& path, const std::string& data);

}  // namespace json
