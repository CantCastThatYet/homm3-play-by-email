#include "json_lite.hpp"
#include <cstdlib>

namespace hp_json {
namespace {
const Value None;
const std::string Empty;

struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;

    void Space() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i; }
    bool Literal(const char* word) {
        const std::string w(word);
        if (s.compare(i, w.size(), w) != 0) return false;
        i += w.size(); return true;
    }
    static void Utf8(std::string& out, unsigned c) {
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    bool Hex4(unsigned& c) {
        if (s.size() - i < 4) return false;
        c = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s[i++]; c <<= 4;
            if (h >= '0' && h <= '9') c |= h - '0';
            else if (h >= 'a' && h <= 'f') c |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') c |= h - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool String(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            switch (s[i++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned c1 = 0;
                if (!Hex4(c1)) return false;
                if (c1 >= 0xD800 && c1 < 0xDC00) {
                    unsigned c2 = 0;
                    if (s.compare(i, 2, "\\u") != 0) return false;
                    i += 2;
                    if (!Hex4(c2) || c2 < 0xDC00 || c2 >= 0xE000) return false;
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                } else if (c1 >= 0xDC00 && c1 < 0xE000) return false;
                Utf8(out, c1);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Number(double& out) {
        const size_t start = i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
        if (i == start) return false;
        const std::string text = s.substr(start, i - start);
        char* end = nullptr;
        out = std::strtod(text.c_str(), &end);
        return end && *end == 0;
    }
    bool Members(Value& v) {
        v.kind = Value::Object; ++i; Space();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        for (;;) {
            std::string name; Space();
            if (!String(name)) return false;
            Space();
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            Value item;
            if (!Read(item)) return false;
            v.fields.emplace_back(std::move(name), std::move(item));
            Space();
            if (i >= s.size()) return false;
            if (s[i] == ',') { ++i; continue; }
            if (s[i] != '}') return false;
            ++i; return true;
        }
    }
    bool Elements(Value& v) {
        v.kind = Value::Array; ++i; Space();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        for (;;) {
            Value item;
            if (!Read(item)) return false;
            v.items.push_back(std::move(item));
            Space();
            if (i >= s.size()) return false;
            if (s[i] == ',') { ++i; continue; }
            if (s[i] != ']') return false;
            ++i; return true;
        }
    }
    bool Read(Value& v) {
        if (++depth > 64) return false;
        Space();
        bool ok = false;
        if (i >= s.size()) ok = false;
        else if (s[i] == '{') ok = Members(v);
        else if (s[i] == '[') ok = Elements(v);
        else if (s[i] == '"') { v.kind = Value::String; ok = String(v.text); }
        else if (Literal("true")) { v.kind = Value::Bool; v.boolean = true; ok = true; }
        else if (Literal("false")) { v.kind = Value::Bool; ok = true; }
        else if (Literal("null")) { v.kind = Value::Null; ok = true; }
        else { v.kind = Value::Number; ok = Number(v.number); }
        --depth;
        return ok;
    }
};
}

const Value& Value::operator[](const char* name) const {
    if (kind == Object) for (const auto& field : fields) if (field.first == name) return field.second;
    return None;
}
const Value& Value::operator[](size_t index) const { return kind == Array && index < items.size() ? items[index] : None; }
const std::string& Value::str() const { return kind == String ? text : Empty; }

bool Parse(const std::string& input, Value& out) {
    Parser p{input};
    Value v;
    if (!p.Read(v)) return false;
    p.Space();
    if (p.i != input.size()) return false;
    out = std::move(v);
    return true;
}
}
