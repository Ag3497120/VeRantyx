// A small JSON reader, enough for SteamVR action manifests and binding files.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace klvr {

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;   // keeps file order

    const Json &operator[](const char *key) const;  // Null if absent
    const Json &operator[](size_t i) const;
    bool is_null() const { return type == Null; }
    const std::string &str() const { return s; }
    double num(double def = 0) const { return type == Number ? n : def; }
    size_t size() const { return type == Array ? a.size() : type == Object ? o.size() : 0; }

    // Parse `text`; on failure returns false and sets `err`.
    static bool parse(const std::string &text, Json &out, std::string &err);
    static bool parse_file(const std::string &path, Json &out, std::string &err);
};

} // namespace klvr
