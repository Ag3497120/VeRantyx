// A small JSON reader for the SteamVR action manifest and binding files.
// Objects keep insertion order; numbers are doubles; no exceptions.
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdlib>
#include <cstring>

namespace json {

inline std::string quote(const std::string &s) {
    std::string out = "\"";
    const char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += c;
    }
    return out + '"';
}

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    bool is_object() const { return type == Object; }
    bool is_array() const { return type == Array; }
    bool is_string() const { return type == String; }
    const Value *get(const char *key) const {
        if (type != Object) return nullptr;
        for (auto &kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    const Value &operator[](const char *key) const {
        static const Value none;
        const Value *v = get(key);
        return v ? *v : none;
    }
    std::string string_or(const char *dflt) const { return type == String ? str : std::string(dflt); }
    double number_or(double dflt) const { return type == Number ? num : (type == Bool ? (b ? 1 : 0) : dflt); }
    bool bool_or(bool dflt) const { return type == Bool ? b : (type == Number ? num != 0 : dflt); }
};

class Parser {
public:
    explicit Parser(const std::string &text) : s(text) {}
    bool parse(Value &out) { skip(); if (!value(out)) return false; skip(); return pos == s.size(); }
    std::string error;
private:
    const std::string &s;
    size_t pos = 0;
    void skip() {
        for (;;) {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) pos++;
            if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '/') { while (pos < s.size() && s[pos] != '\n') pos++; continue; }
            if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '*') { pos += 2; while (pos + 1 < s.size() && !(s[pos] == '*' && s[pos + 1] == '/')) pos++; pos += 2; continue; }
            break;
        }
    }
    bool fail(const char *m) { if (error.empty()) { error = m; error += " at "; error += std::to_string(pos); } return false; }
    bool value(Value &v) {
        if (pos >= s.size()) return fail("unexpected end");
        char c = s[pos];
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') { v.type = Value::String; return string(v.str); }
        if (c == 't' && s.compare(pos, 4, "true") == 0) { v.type = Value::Bool; v.b = true; pos += 4; return true; }
        if (c == 'f' && s.compare(pos, 5, "false") == 0) { v.type = Value::Bool; v.b = false; pos += 5; return true; }
        if (c == 'n' && s.compare(pos, 4, "null") == 0) { v.type = Value::Null; pos += 4; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            char *end = nullptr;
            v.type = Value::Number; v.num = strtod(s.c_str() + pos, &end);
            if (end == s.c_str() + pos) return fail("bad number");
            pos = (size_t)(end - s.c_str()); return true;
        }
        return fail("unexpected character");
    }
    bool string(std::string &out) {
        if (s[pos] != '"') return fail("expected string");
        pos++;
        while (pos < s.size() && s[pos] != '"') {
            char c = s[pos++];
            if (c == '\\' && pos < s.size()) {
                char e = s[pos++];
                switch (e) {
                case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                case 'u': {
                    if (pos + 4 > s.size()) return fail("bad \\u");
                    unsigned cp = (unsigned)strtoul(s.substr(pos, 4).c_str(), nullptr, 16); pos += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                    break; }
                default: out += e; break;
                }
            } else out += c;
        }
        if (pos >= s.size()) return fail("unterminated string");
        pos++; return true;
    }
    bool array(Value &v) {
        v.type = Value::Array; pos++; skip();
        if (pos < s.size() && s[pos] == ']') { pos++; return true; }
        for (;;) {
            Value e; skip();
            if (!value(e)) return false;
            v.arr.push_back(std::move(e)); skip();
            if (pos < s.size() && s[pos] == ',') { pos++; continue; }
            if (pos < s.size() && s[pos] == ']') { pos++; return true; }
            return fail("expected , or ]");
        }
    }
    bool object(Value &v) {
        v.type = Value::Object; pos++; skip();
        if (pos < s.size() && s[pos] == '}') { pos++; return true; }
        for (;;) {
            skip(); std::string k;
            if (!string(k)) return false;
            skip(); if (pos >= s.size() || s[pos] != ':') return fail("expected :"); pos++; skip();
            Value e; if (!value(e)) return false;
            v.obj.emplace_back(std::move(k), std::move(e)); skip();
            if (pos < s.size() && s[pos] == ',') { pos++; continue; }
            if (pos < s.size() && s[pos] == '}') { pos++; return true; }
            return fail("expected , or }");
        }
    }
};

inline bool parse_file(const std::string &path, Value &out, std::string *err = nullptr) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::string text; char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
    Parser p(text);
    if (!p.parse(out)) { if (err) *err = path + ": " + p.error; return false; }
    return true;
}

} // namespace json
