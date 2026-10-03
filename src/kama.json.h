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
#include <cstdlib>
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

// ---- parse ----------------------------------------------------------------------------------
// One reader for both callers. The language server reads its messages LENIENTLY (`parseValue`, whose result
// is whatever was read before a malformation), because a client's message is never this program's to refuse;
// a registry's `catalog.json` and `index.json` are read STRICTLY (`jsonParse`), because a malformed one is the
// registry's mistake and must be reported, not shown as an empty list. `err` records the first malformation.
struct JsonParser {
    const std::string& src;
    size_t             i = 0;
    std::string        err;
    explicit JsonParser(const std::string& s) : src(s) {}

    void skipWs() { while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) ++i; }
    bool eof() const { return i >= src.size(); }
    char peek() const { return i < src.size() ? src[i] : '\0'; }
    void fail(const char* m) { if (err.empty()) err = std::string(m) + " at byte " + std::to_string(i); }

    void appendUtf8(std::string& out, unsigned cp) {
        if (cp <= 0x7f) { out += (char)cp; }
        else if (cp <= 0x7ff) { out += (char)(0xc0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3f)); }
        else if (cp <= 0xffff) { out += (char)(0xe0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3f)); out += (char)(0x80 | (cp & 0x3f)); }
        else { out += (char)(0xf0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3f)); out += (char)(0x80 | ((cp >> 6) & 0x3f)); out += (char)(0x80 | (cp & 0x3f)); }
    }

    unsigned hex4() {
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            if (i >= src.size()) { fail("a \\u escape ends early"); return v; }
            char c = src[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (c - '0');
            else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
            else fail("a \\u escape takes four hex digits");
        }
        return v;
    }

    std::string parseString() {
        std::string out;
        ++i;                        // opening quote
        while (i < src.size()) {
            char c = src[i++];
            if (c == '"') return out;
            if (c == '\\' && i < src.size()) {
                char e = src[i++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        unsigned cp = hex4();
                        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < src.size() && src[i] == '\\' && src[i + 1] == 'u') {
                            i += 2;                       // consume the "\u" of the low surrogate
                            unsigned lo = hex4();
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: out += e; fail("an unknown escape in a string"); break;
                }
            } else {
                out += c;
            }
        }
        fail("an unterminated string");
        return out;
    }

    bool literal(const char* word) {
        const size_t n = std::char_traits<char>::length(word);
        if (src.compare(i, n, word) != 0) { fail("an unknown literal"); i = src.size(); return false; }
        i += n; return true;
    }

    Json parseValue() {
        skipWs();
        if (eof()) { fail("a value is missing"); return Json(); }
        char c = peek();
        if (c == '"') { Json j; j.type = Json::Str; j.s = parseString(); return j; }
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == 't') { literal("true");  return Json(true); }
        if (c == 'f') { literal("false"); return Json(false); }
        if (c == 'n') { literal("null");  return Json(); }
        return parseNumber();
    }

    Json parseNumber() {
        size_t start = i;
        while (i < src.size() && (src[i] == '+' || src[i] == '-' || src[i] == '.' || src[i] == 'e' || src[i] == 'E'
                                  || (src[i] >= '0' && src[i] <= '9'))) ++i;
        Json j; j.type = Json::Num;
        if (i == start) { fail("an unexpected character"); i = src.size(); return j; }
        j.n = strtod(src.substr(start, i - start).c_str(), nullptr);
        return j;
    }

    Json parseArray() {
        Json j = Json::array();
        ++i;                        // '['
        skipWs();
        if (peek() == ']') { ++i; return j; }
        for (;;) {
            j.arr.push_back(parseValue());
            skipWs();
            char c = peek();
            if (c == ',') { ++i; continue; }
            if (c == ']') { ++i; break; }
            fail("expected ',' or ']' in an array");
            break;
        }
        return j;
    }

    Json parseObject() {
        Json j = Json::object();
        ++i;                        // '{'
        skipWs();
        if (peek() == '}') { ++i; return j; }
        for (;;) {
            skipWs();
            if (peek() != '"') { fail("expected a key in an object"); break; }
            std::string key = parseString();
            skipWs();
            if (peek() == ':') ++i; else fail("expected ':' after a key");
            j.obj.emplace_back(key, parseValue());
            skipWs();
            char c = peek();
            if (c == ',') { ++i; continue; }
            if (c == '}') { ++i; break; }
            fail("expected ',' or '}' in an object");
            break;
        }
        return j;
    }
};

// Strict: the whole input is one JSON value (and whitespace), or `err` says where it is not.
inline bool jsonParse(const std::string& src, Json& out, std::string& err)
{
    JsonParser p(src);
    out = p.parseValue();
    p.skipWs();
    if (p.err.empty() && !p.eof()) p.fail("trailing bytes after the value");
    err = p.err;
    return err.empty();
}

#endif // __KAMA_JSON_H__
