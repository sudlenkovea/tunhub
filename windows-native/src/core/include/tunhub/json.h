#pragma once
// A small JSON DOM. Deliberately dependency-free: configs and IPC payloads are simple, and
// vendoring a full library would add build surface for no benefit.

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tunhub {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(double n) : type_(Type::Number), num_(n) {}
    Json(int n) : type_(Type::Number), num_(n) {}
    Json(long long n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Json(unsigned long long n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isBool() const { return type_ == Type::Bool; }

    // Accessors with defaults — parsing user-supplied files should never throw.
    bool asBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double asDouble(double def = 0) const { return type_ == Type::Number ? num_ : def; }
    int asInt(int def = 0) const { return type_ == Type::Number ? static_cast<int>(num_) : def; }
    long long asInt64(long long def = 0) const {
        return type_ == Type::Number ? static_cast<long long>(num_) : def;
    }
    unsigned long long asUInt64(unsigned long long def = 0) const {
        return type_ == Type::Number ? static_cast<unsigned long long>(num_) : def;
    }
    const std::string& asString() const { return str_; }
    std::string asString(const std::string& def) const { return type_ == Type::String ? str_ : def; }

    const std::vector<Json>& items() const { return arr_; }
    std::vector<Json>& items() { return arr_; }
    const std::map<std::string, Json>& fields() const { return obj_; }

    /// Object member lookup; returns a null Json when absent.
    const Json& operator[](const std::string& key) const;
    Json& operator[](const std::string& key);
    bool has(const std::string& key) const { return obj_.find(key) != obj_.end(); }

    void push(Json v) { type_ = Type::Array; arr_.push_back(std::move(v)); }
    void set(const std::string& key, Json v) { type_ = Type::Object; obj_[key] = std::move(v); }

    /// `indent < 0` produces compact output (used for IPC), otherwise pretty-printed
    /// with sorted keys (used for on-disk configs, so diffs stay stable).
    std::string dump(int indent = -1) const;

    static Json parse(std::string_view text, std::string* error = nullptr);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<Json> arr_;
    std::map<std::string, Json> obj_;
};

}  // namespace tunhub
