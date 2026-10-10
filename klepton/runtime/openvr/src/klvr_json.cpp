#include "klvr_json.h"
#include <cstdio>
#include <cstdlib>

namespace klvr {

static const Json kNull;

const Json &Json::operator[](const char *key) const {
    if (type != Object) return kNull;
    for (auto &kv : o)
        if (kv.first == key) return kv.second;
    return kNull;
}

const Json &Json::operator[](size_t i) const {
    return type == Array && i < a.size() ? a[i] : kNull;
}

namespace {
struct Parser {
    const std::string &t;
    size_t i = 0;
    std::string err;

    explicit Parser(const std::string &text) : t(text) {}

    void ws() {
        for (;;) {
            while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
            // SteamVR's own files carry // comments now and then.
            if (i + 1 < t.size() && t[i] == '/' && t[i + 1] == '/') {
                while (i < t.size() && t[i] != '\n') i++;
                continue;
            }
            break;
        }
    }
    bool fail(const char *what) {
        if (err.empty()) {
            char b[96];
            snprintf(b, sizeof b, "%s at offset %zu", what, i);
            err = b;
        }
        return false;
    }
    static void put_utf8(std::string &s, unsigned c) {
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    bool string(std::string &out) {
        if (t[i] != '"') return fail("expected string");
        i++;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= t.size()) return fail("bad escape");
            char e = t[i++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 > t.size()) return fail("bad \\u");
                put_utf8(out, (unsigned)strtoul(t.substr(i, 4).c_str(), nullptr, 16));
                i += 4;
                break;
            }
            default: out += e;
            }
        }
        if (i >= t.size()) return fail("unterminated string");
        i++;
        return true;
    }
    bool value(Json &v, int depth) {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (i >= t.size()) return fail("unexpected end");
        char c = t[i];
        if (c == '{') {
            v.type = Json::Object;
            i++;
            ws();
            if (i < t.size() && t[i] == '}') { i++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (i >= t.size() || t[i] != ':') return fail("expected ':'");
                i++;
                Json child;
                if (!value(child, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(child));
                ws();
                if (i < t.size() && t[i] == ',') { i++; ws(); if (i < t.size() && t[i] == '}') { i++; return true; } continue; }
                if (i < t.size() && t[i] == '}') { i++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Json::Array;
            i++;
            ws();
            if (i < t.size() && t[i] == ']') { i++; return true; }
            for (;;) {
                Json child;
                if (!value(child, depth + 1)) return false;
                v.a.push_back(std::move(child));
                ws();
                if (i < t.size() && t[i] == ',') { i++; ws(); if (i < t.size() && t[i] == ']') { i++; return true; } continue; }
                if (i < t.size() && t[i] == ']') { i++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.type = Json::String; return string(v.s); }
        if (t.compare(i, 4, "true") == 0) { v.type = Json::Bool; v.b = true; i += 4; return true; }
        if (t.compare(i, 5, "false") == 0) { v.type = Json::Bool; v.b = false; i += 5; return true; }
        if (t.compare(i, 4, "null") == 0) { v.type = Json::Null; i += 4; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            char *end = nullptr;
            v.type = Json::Number;
            v.n = strtod(t.c_str() + i, &end);
            i = end - t.c_str();
            return true;
        }
        return fail("unexpected character");
    }
};
} // namespace

bool Json::parse(const std::string &text, Json &out, std::string &err) {
    Parser p(text);
    // A UTF-8 byte order mark is legal at the top of these files.
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) p.i = 3;
    out = Json();
    if (!p.value(out, 0)) { err = p.err; return false; }
    return true;
}

bool Json::parse_file(const std::string &path, Json &out, std::string &err) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    return parse(text, out, err);
}

} // namespace klvr
