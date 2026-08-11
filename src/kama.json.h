#ifndef __KAMA_JSON_H__
#define __KAMA_JSON_H__

// A minimal JSON value + serializer, shared by the two places the compiler speaks JSON: the `kama lsp`
// server's protocol framing (kama.lsp.cpp, which also carries the READER half — nothing else needs to
// parse JSON) and `--json` output from `kama query` / `kama check` (kama.driver.cpp).
//
// It lived inside kama.lsp.cpp until `--json` shipped, under a comment saying no other part of the
// compiler needed a JSON type. That stopped being true the moment a non-editor caller wanted structured
// answers, and one encoder is the point: an editor and a script must not be able to disagree about how a
// symbol kind or a diagnostic severity is spelled.
//
// Scope is deliberately the LSP subset — null / bool / number / string / array / object. Numbers are held
// as double and serialized as integers when whole (positions and ids are integral). Object members keep
// insertion order, which JSON does not care about and which makes output diffable.
//
// NOT to be confused with kama.driver.cpp's `jsonEscape` + ostringstream emitters for kama.lock and the
// registry index. Those are write-only, predate this, and have no value type; folding them in here is a
// tidy-up nobody needs yet.

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool                                   b = false;
    double                                 n = 0;
    std::string                            s;
    std::vector<Json>                      arr;
    std::vector<std::pair<std::string, Json>> obj;

    Json() {}
    Json(bool v)               : type(Bool), b(v) {}
    Json(int v)                : type(Num), n(v) {}
    Json(long v)               : type(Num), n((double)v) {}
    Json(double v)             : type(Num), n(v) {}
    Json(const char* v)        : type(Str), s(v) {}
    Json(const std::string& v) : type(Str), s(v) {}

    static Json array()  { Json j; j.type = Arr; return j; }
    static Json object() { Json j; j.type = Obj; return j; }

    void set(const std::string& k, Json v) {
        type = Obj;
        for (auto& p : obj) if (p.first == k) { p.second = std::move(v); return; }
        obj.emplace_back(k, std::move(v));
    }
    void push(Json v) { type = Arr; arr.push_back(std::move(v)); }

    const Json* get(const std::string& k) const {
        if (type != Obj) return nullptr;
        for (auto& p : obj) if (p.first == k) return &p.second;
        return nullptr;
    }
    // Convenience readers (defaulting) for reaching into request params.
    std::string asStr() const { return type == Str ? s : std::string(); }
    double      asNum() const { return type == Num ? n : 0; }
    int         asInt() const { return (int)asNum(); }
    std::string getStr(const std::string& k) const { const Json* v = get(k); return v ? v->asStr() : std::string(); }
    // Nested string: obj[k1][k2] (e.g. params["textDocument"]["uri"]); "" if any hop is absent.
    std::string getStr2(const std::string& k1, const std::string& k2) const {
        const Json* v = get(k1); return v ? v->getStr(k2) : std::string();
    }
};

// ---- serialize ------------------------------------------------------------------------------
inline void escapeInto(std::string& out, const std::string& s) {
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {                 // other control chars -> \u00XX; valid UTF-8 bytes pass through
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
}

inline void serializeInto(std::string& out, const Json& j) {
    switch (j.type) {
        case Json::Null: out += "null"; break;
        case Json::Bool: out += j.b ? "true" : "false"; break;
        case Json::Num: {
            double d = j.n;
            long long ll = (long long)d;
            char buf[32];
            if ((double)ll == d) snprintf(buf, sizeof buf, "%lld", ll);
            else                 snprintf(buf, sizeof buf, "%g", d);
            out += buf;
            break;
        }
        case Json::Str: out += '"'; escapeInto(out, j.s); out += '"'; break;
        case Json::Arr: {
            out += '[';
            for (size_t i = 0; i < j.arr.size(); ++i) { if (i) out += ','; serializeInto(out, j.arr[i]); }
            out += ']';
            break;
        }
        case Json::Obj: {
            out += '{';
            for (size_t i = 0; i < j.obj.size(); ++i) {
                if (i) out += ',';
                out += '"'; escapeInto(out, j.obj[i].first); out += "\":";
                serializeInto(out, j.obj[i].second);
            }
            out += '}';
            break;
        }
    }
}

inline std::string serialize(const Json& j) { std::string out; serializeInto(out, j); return out; }

#endif // __KAMA_JSON_H__
