// Minimal flat-JSON reader for the host test harness (objects of scalars).
// GPL-3.0
#pragma once

#include <string>
#include <vector>
#include <cstdlib>

struct JsonKV {
    std::string key;
    std::string str;   // raw string value (unescaped) when is_str
    double num = 0;
    bool is_str = false;
    bool is_null = false;
    bool is_bool = false;
};

// Parses {"k":v,...}; nested objects/arrays are skipped. Returns false on error.
inline bool json_parse_flat(const std::string& s, std::vector<JsonKV>* out) {
    size_t i = 0;
    auto ws = [&]() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) i++; };
    auto str = [&](std::string* o) -> bool {
        if (s[i] != '"') return false;
        i++;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                i++;
                char c = s[i];
                if (c == 'n') o->push_back('\n');
                else if (c == 't') o->push_back('\t');
                else if (c == 'u' && i + 4 < s.size()) {
                    unsigned v = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
                    if (v < 0x80) o->push_back((char)v);
                    else if (v < 0x800) { o->push_back((char)(0xC0 | (v >> 6))); o->push_back((char)(0x80 | (v & 0x3F))); }
                    else { o->push_back((char)(0xE0 | (v >> 12))); o->push_back((char)(0x80 | ((v >> 6) & 0x3F))); o->push_back((char)(0x80 | (v & 0x3F))); }
                    i += 4;
                } else o->push_back(c);
                i++;
            } else {
                o->push_back(s[i++]);
            }
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    };
    ws();
    if (i >= s.size() || s[i] != '{') return false;
    i++;
    for (;;) {
        ws();
        if (i < s.size() && s[i] == '}') { i++; return true; }
        JsonKV kv;
        if (!str(&kv.key)) return false;
        ws();
        if (s[i] != ':') return false;
        i++;
        ws();
        if (s[i] == '"') {
            kv.is_str = true;
            if (!str(&kv.str)) return false;
        } else if (s[i] == '{' || s[i] == '[') {
            int depth = 0;
            bool in_s = false;
            for (; i < s.size(); i++) {
                char c = s[i];
                if (in_s) { if (c == '\\') i++; else if (c == '"') in_s = false; continue; }
                if (c == '"') in_s = true;
                else if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']') { depth--; if (depth == 0) { i++; break; } }
            }
            kv.is_null = true;
        } else if (s.compare(i, 4, "null") == 0) {
            kv.is_null = true; i += 4;
        } else if (s.compare(i, 4, "true") == 0) {
            kv.is_bool = true; kv.num = 1; i += 4;
        } else if (s.compare(i, 5, "false") == 0) {
            kv.is_bool = true; kv.num = 0; i += 5;
        } else {
            char* end = nullptr;
            kv.num = strtod(s.c_str() + i, &end);
            if (end == s.c_str() + i) return false;
            i = (size_t)(end - s.c_str());
        }
        out->push_back(kv);
        ws();
        if (s[i] == ',') { i++; continue; }
        if (s[i] == '}') { i++; return true; }
        return false;
    }
}
