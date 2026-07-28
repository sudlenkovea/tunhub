#include "tunhub/json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tunhub {
namespace {

const Json kNull;

void escapeTo(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);   // UTF-8 passes through unchanged
                }
        }
    }
    out += '"';
}

void numberTo(std::string& out, double v) {
    if (std::isfinite(v) && v == static_cast<double>(static_cast<long long>(v))) {
        out += std::to_string(static_cast<long long>(v));
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    out += buf;
}

/// Encode one code point as UTF-8 (for \uXXXX escapes, surrogate pairs included).
void appendUtf8(std::string& out, unsigned cp) {
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

class Parser {
public:
    Parser(std::string_view text) : s_(text) {}

    bool parseValue(Json& out) {
        skipWs();
        if (pos_ >= s_.size()) return fail("unexpected end of input");
        switch (s_[pos_]) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': {
                std::string v;
                if (!parseString(v)) return false;
                out = Json(std::move(v));
                return true;
            }
            case 't':
                if (s_.compare(pos_, 4, "true") == 0) { pos_ += 4; out = Json(true); return true; }
                return fail("invalid literal");
            case 'f':
                if (s_.compare(pos_, 5, "false") == 0) { pos_ += 5; out = Json(false); return true; }
                return fail("invalid literal");
            case 'n':
                if (s_.compare(pos_, 4, "null") == 0) { pos_ += 4; out = Json(); return true; }
                return fail("invalid literal");
            default: return parseNumber(out);
        }
    }

    void skipWs() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }

    bool atEndAfterWs() { skipWs(); return pos_ >= s_.size(); }
    const std::string& error() const { return err_; }

private:
    bool fail(const char* msg) {
        if (err_.empty()) err_ = std::string(msg) + " at offset " + std::to_string(pos_);
        return false;
    }

    bool parseObject(Json& out) {
        out = Json::object();
        ++pos_;  // '{'
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        while (true) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected object key");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':'");
            ++pos_;
            Json value;
            if (!parseValue(value)) return false;
            out.set(key, std::move(value));
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(Json& out) {
        out = Json::array();
        ++pos_;  // '['
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        while (true) {
            Json value;
            if (!parseValue(value)) return false;
            out.push(std::move(value));
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parseString(std::string& out) {
        out.clear();
        ++pos_;  // opening quote
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (pos_ >= s_.size()) break;
            char e = s_[pos_++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!readHex4(cp)) return false;
                    // Combine surrogate pairs into one code point.
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() &&
                        s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned lo = 0;
                        if (!readHex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            appendUtf8(out, lo);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }

    bool readHex4(unsigned& cp) {
        if (pos_ + 4 > s_.size()) return fail("truncated \\u escape");
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= unsigned(c - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }

    bool parseNumber(Json& out) {
        size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        bool any = false;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) { ++pos_; any = true; }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) { ++pos_; any = true; }
        }
        if (any && pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        }
        if (!any) return fail("invalid number");
        out = Json(std::strtod(std::string(s_.substr(start, pos_ - start)).c_str(), nullptr));
        return true;
    }

    std::string_view s_;
    size_t pos_ = 0;
    std::string err_;
};

}  // namespace

const Json& Json::operator[](const std::string& key) const {
    auto it = obj_.find(key);
    return it == obj_.end() ? kNull : it->second;
}

Json& Json::operator[](const std::string& key) {
    type_ = Type::Object;
    return obj_[key];
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    const bool pretty = indent >= 0;
    auto newlineIndent = [&](int d) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * d), ' ');
    };

    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Number: numberTo(out, num_); break;
        case Type::String: escapeTo(out, str_); break;
        case Type::Array: {
            if (arr_.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += ',';
                newlineIndent(depth + 1);
                arr_[i].dumpTo(out, indent, depth + 1);
            }
            newlineIndent(depth);
            out += ']';
            break;
        }
        case Type::Object: {
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            bool first = true;
            for (const auto& [key, value] : obj_) {   // std::map → keys sorted, stable diffs
                if (!first) out += ',';
                first = false;
                newlineIndent(depth + 1);
                escapeTo(out, key);
                out += pretty ? ": " : ":";
                value.dumpTo(out, indent, depth + 1);
            }
            newlineIndent(depth);
            out += '}';
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

Json Json::parse(std::string_view text, std::string* error) {
    Parser p(text);
    Json out;
    if (!p.parseValue(out)) {
        if (error) *error = p.error();
        return Json();
    }
    if (!p.atEndAfterWs()) {
        if (error) *error = "trailing data after JSON value";
        return Json();
    }
    if (error) error->clear();
    return out;
}

}  // namespace tunhub
