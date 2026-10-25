#pragma once
#include <string>
#include <utility>
#include <vector>

// A small JSON reader for the server's replies. Strings stay UTF-8.
namespace hp_json {
struct Value {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> fields;

    // Missing members and wrong kinds read as null, false, 0 or "".
    const Value& operator[](const char* name) const;
    const Value& operator[](size_t index) const;
    const Value& operator[](int index) const { return index < 0 ? (*this)[static_cast<size_t>(-1)] : (*this)[static_cast<size_t>(index)]; }
    size_t size() const { return kind == Array ? items.size() : 0; }
    const std::string& str() const;
    long long integer() const { return kind == Number ? static_cast<long long>(number) : 0; }
    bool truthy() const { return kind == Bool && boolean; }
};

// False on malformed input or trailing content; nesting is limited to 64 levels.
bool Parse(const std::string& input, Value& out);
}
