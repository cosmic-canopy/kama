// kama.lsp.cpp — the `kama lsp` language server (LSP campaign M1, the walking skeleton).
//
// A JSON-RPC 2.0 server over stdio that reuses the M0 front-end-as-library analysis path
// (CEmitter::analyze + structured Diagnostics) to publish live, as-you-type diagnostics to any LSP
// editor. Scope for M1: lifecycle + full-document sync + publishDiagnostics. Hover / go-to-definition /
// completion are M2+ — the query facade (kama.query.h) is already built and waiting.
//
// Design of record: docs/design/lsp-m1-kickoff.md. Everything is self-contained here (a hand-rolled JSON
// reader/writer, the transport, the dispatch loop) so the only compiler surface it touches is the
// existing analysis facade — keeping the LSP a leaf module the rest of the compiler never depends on.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <string>
#include <vector>
#include <map>
#include <utility>

#include "kama.lsp.h"        // lspAnalyzeBuffer seam + Diagnostic + runLspServer

// ============================================================================================
// Minimal JSON (the LSP subset: null / bool / number / string / array / object). Numbers are held as
// double but serialized as integers when whole (LSP positions/ids are integral). Object member order is
// insertion order (irrelevant to JSON semantics; keeps output deterministic). Kept inside this TU — no
// other part of the compiler needs a JSON type.
// ============================================================================================
namespace {

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
};

// ---- serialize ------------------------------------------------------------------------------
void escapeInto(std::string& out, const std::string& s) {
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

void serializeInto(std::string& out, const Json& j) {
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

std::string serialize(const Json& j) { std::string out; serializeInto(out, j); return out; }

// ---- parse ----------------------------------------------------------------------------------
// A tolerant recursive-descent parser over a std::string. On malformed input it stops and returns what it
// has (LSP bodies from a conforming client are well-formed; the harness catches regressions).
struct Parser {
    const std::string& src;
    size_t             i = 0;
    explicit Parser(const std::string& s) : src(s) {}

    void skipWs() { while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) ++i; }
    bool eof() const { return i >= src.size(); }
    char peek() const { return i < src.size() ? src[i] : '\0'; }

    void appendUtf8(std::string& out, unsigned cp) {
        if (cp <= 0x7f) { out += (char)cp; }
        else if (cp <= 0x7ff) { out += (char)(0xc0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3f)); }
        else if (cp <= 0xffff) { out += (char)(0xe0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3f)); out += (char)(0x80 | (cp & 0x3f)); }
        else { out += (char)(0xf0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3f)); out += (char)(0x80 | ((cp >> 6) & 0x3f)); out += (char)(0x80 | (cp & 0x3f)); }
    }

    unsigned hex4() {
        unsigned v = 0;
        for (int k = 0; k < 4 && i < src.size(); ++k) {
            char c = src[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (c - '0');
            else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
        }
        return v;
    }

    std::string parseString() {
        std::string out;
        ++i;                        // opening quote
        while (i < src.size()) {
            char c = src[i++];
            if (c == '"') break;
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
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    Json parseValue() {
        skipWs();
        if (eof()) return Json();
        char c = peek();
        if (c == '"') { Json j; j.type = Json::Str; j.s = parseString(); return j; }
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == 't') { i += 4; return Json(true); }
        if (c == 'f') { i += 5; return Json(false); }
        if (c == 'n') { i += 4; return Json(); }
        return parseNumber();
    }

    Json parseNumber() {
        size_t start = i;
        while (i < src.size() && (strchr("+-0123456789.eE", src[i]))) ++i;
        Json j; j.type = Json::Num;
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
            break;                  // malformed — stop
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
            if (peek() != '"') break;
            std::string key = parseString();
            skipWs();
            if (peek() == ':') ++i;
            j.obj.emplace_back(key, parseValue());
            skipWs();
            char c = peek();
            if (c == ',') { ++i; continue; }
            if (c == '}') { ++i; break; }
            break;                  // malformed — stop
        }
        return j;
    }
};

Json parse(const std::string& src) { Parser p(src); return p.parseValue(); }

// ============================================================================================
// Transport — LSP base framing (`Content-Length: N\r\n\r\n<N bytes>`) over stdin/stdout.
// ============================================================================================

// Read one framed message body into `out`. Returns false on a clean EOF (client closed) or a malformed
// header (treated as end-of-stream). Mixes getchar (headers) + fread (body) on the same stdin FILE*.
bool readMessage(std::string& out) {
    size_t contentLength = 0;
    bool   gotLength = false;
    std::string line;
    for (;;) {
        line.clear();
        int c;
        while ((c = getchar()) != EOF) {
            if (c == '\n') break;
            if (c != '\r') line.push_back((char)c);
        }
        if (c == EOF && line.empty()) return false;   // clean EOF between messages
        if (line.empty()) break;                      // blank line -> end of headers
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            for (auto& ch : key) ch = (char)tolower((unsigned char)ch);
            // trim leading spaces off the key (defensive)
            while (!key.empty() && key.back() == ' ') key.pop_back();
            if (key == "content-length") {
                contentLength = (size_t)strtoul(line.c_str() + colon + 1, nullptr, 10);
                gotLength = true;
            }
        }
    }
    if (!gotLength) return false;
    out.resize(contentLength);
    size_t got = 0;
    while (got < contentLength) {
        size_t n = fread(&out[got], 1, contentLength - got, stdin);
        if (n == 0) return false;                     // EOF mid-body
        got += n;
    }
    return true;
}

void writeMessage(const std::string& body) {
    printf("Content-Length: %zu\r\n\r\n", body.size());
    fwrite(body.data(), 1, body.size(), stdout);
    fflush(stdout);
}

// ============================================================================================
// URI <-> filesystem path (enough for `file://` URIs; percent-decode the path).
// ============================================================================================
std::string uriToPath(const std::string& uri) {
    std::string s = uri;
    const char* pfx = "file://";
    if (s.rfind(pfx, 0) == 0) s = s.substr(strlen(pfx));
    // A Windows URI is file:///C:/... — strip the leading slash before the drive letter. On POSIX the
    // leading slash IS the root, so keep it.
    std::string path;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hx = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hx(s[i + 1]), lo = hx(s[i + 2]);
            if (hi >= 0 && lo >= 0) { path += (char)((hi << 4) | lo); i += 2; continue; }
        }
        path += s[i];
    }
    return path;
}

// ============================================================================================
// Diagnostics — analyze a buffer via the M0 facade, then map kama Diagnostics to an LSP array.
// ============================================================================================

// kama coords: line 1-based, column 0-based (Diagnostic). LSP: line 0-based, character 0-based. Convert
// in ONE place. An unknown end (endLine/endColumn == 0) collapses to a 1-char span at the start so the
// squiggle is visible.
Json rangeToJson(const Diagnostic& d) {
    int sl = d.line > 0 ? d.line - 1 : 0;
    int sc = d.column >= 0 ? d.column : 0;
    int el, ec;
    if (d.endLine > 0 && d.endColumn > 0) { el = d.endLine - 1; ec = d.endColumn; }
    else                                  { el = sl; ec = sc + 1; }
    Json start = Json::object(); start.set("line", sl); start.set("character", sc);
    Json end   = Json::object(); end.set("line", el);   end.set("character", ec);
    Json range = Json::object(); range.set("start", start); range.set("end", end);
    return range;
}

int severityToLsp(DiagSeverity s) {
    switch (s) {
        case DiagSeverity::Error:       return 1;
        case DiagSeverity::Warning:     return 2;
        case DiagSeverity::Information:  return 3;
        case DiagSeverity::Hint:        return 4;
    }
    return 1;
}

Json diagnosticsArray(const std::vector<Diagnostic>& diags) {
    Json arr = Json::array();
    for (const auto& d : diags) {
        Json j = Json::object();
        j.set("range", rangeToJson(d));
        j.set("severity", severityToLsp(d.severity));
        if (!d.code.empty()) j.set("code", d.code);
        j.set("source", "kama");
        j.set("message", d.message);
        arr.push(std::move(j));
    }
    return arr;
}

// ============================================================================================
// Server state + dispatch.
// ============================================================================================
struct Doc {
    std::string           text;
    long                  version = 0;
    SharedCompilationUnit lastGoodUnit;
};

struct Server {
    std::map<std::string, Doc> docs;    // keyed by URI
    bool initialized = false;
    bool shutdownReceived = false;

    void sendResponse(const Json& id, Json result) {
        Json resp = Json::object();
        resp.set("jsonrpc", "2.0");
        resp.set("id", id);
        resp.set("result", std::move(result));
        writeMessage(serialize(resp));
    }

    void sendError(const Json& id, int code, const std::string& msg) {
        Json err = Json::object();
        err.set("code", code);
        err.set("message", msg);
        Json resp = Json::object();
        resp.set("jsonrpc", "2.0");
        resp.set("id", id);
        resp.set("error", std::move(err));
        writeMessage(serialize(resp));
    }

    void publish(const std::string& uri, const std::vector<Diagnostic>& diags) {
        Json params = Json::object();
        params.set("uri", uri);
        params.set("diagnostics", diagnosticsArray(diags));
        Json note = Json::object();
        note.set("jsonrpc", "2.0");
        note.set("method", "textDocument/publishDiagnostics");
        note.set("params", std::move(params));
        writeMessage(serialize(note));
    }

    // Analyze the stored buffer for `uri` and publish (empty array clears old squiggles).
    void analyzeAndPublish(const std::string& uri) {
        auto it = docs.find(uri);
        if (it == docs.end()) return;
        std::string path = uriToPath(uri);
        std::vector<Diagnostic> diags = lspAnalyzeBuffer(path, it->second.text, it->second.lastGoodUnit);
        publish(uri, diags);
    }

    void handleInitialize(const Json& id) {
        // M1 advertises full-document sync only; hover/def/completion flip on in M2.
        Json caps = Json::object();
        caps.set("textDocumentSync", 1);        // TextDocumentSyncKind.Full
        Json info = Json::object();
        info.set("name", "kama");
        Json result = Json::object();
        result.set("capabilities", std::move(caps));
        result.set("serverInfo", std::move(info));
        sendResponse(id, std::move(result));
        initialized = true;
    }

    void handleDidOpen(const Json& params) {
        const Json* td = params.get("textDocument");
        if (!td) return;
        std::string uri = td->getStr("uri");
        Doc doc;
        doc.text = td->getStr("text");
        const Json* ver = td->get("version");
        doc.version = ver ? (long)ver->asNum() : 0;
        docs[uri] = std::move(doc);
        analyzeAndPublish(uri);
    }

    void handleDidChange(const Json& params) {
        const Json* td = params.get("textDocument");
        if (!td) return;
        std::string uri = td->getStr("uri");
        auto it = docs.find(uri);
        if (it == docs.end()) return;
        // Full-document sync: the whole buffer arrives as the last content change's `text`.
        const Json* changes = params.get("contentChanges");
        if (changes && changes->type == Json::Arr && !changes->arr.empty()) {
            const Json& last = changes->arr.back();
            const Json* t = last.get("text");
            if (t) it->second.text = t->asStr();
        }
        const Json* ver = td->get("version");
        if (ver) it->second.version = (long)ver->asNum();
        analyzeAndPublish(uri);
    }

    void handleDidClose(const Json& params) {
        const Json* td = params.get("textDocument");
        if (!td) return;
        std::string uri = td->getStr("uri");
        docs.erase(uri);
        publish(uri, {});                        // clear any lingering squiggles
    }

    // Returns true to keep looping, false to exit the server.
    bool dispatch(const Json& msg, int& exitCode) {
        std::string method = msg.getStr("method");
        const Json* idp = msg.get("id");
        bool isRequest = idp != nullptr;
        const Json* paramsp = msg.get("params");
        Json params = paramsp ? *paramsp : Json::object();

        if (method == "exit") {
            exitCode = shutdownReceived ? 0 : 1;
            return false;
        }
        if (method == "initialize") {
            if (isRequest) handleInitialize(*idp);
            return true;
        }
        if (!initialized) {
            // Per spec: requests before `initialize` get an error; notifications are dropped.
            if (isRequest) sendError(*idp, -32002, "server not initialized");
            return true;
        }
        if (method == "initialized") return true;    // notification, no-op
        if (method == "shutdown") {
            shutdownReceived = true;
            if (isRequest) sendResponse(*idp, Json());   // result: null
            return true;
        }
        if (method == "textDocument/didOpen")   { handleDidOpen(params);   return true; }
        if (method == "textDocument/didChange") { handleDidChange(params); return true; }
        if (method == "textDocument/didClose")  { handleDidClose(params);  return true; }

        // Anything else: a request needs a response (or the client hangs); notifications are ignored.
        if (isRequest) sendError(*idp, -32601, "method not found: " + method);
        return true;
    }

    int run() {
        std::string body;
        int exitCode = 0;
        while (readMessage(body)) {
            Json msg = parse(body);
            if (!dispatch(msg, exitCode)) break;
        }
        return exitCode;
    }
};

} // namespace

int runLspServer() {
    Server server;
    return server.run();
}
