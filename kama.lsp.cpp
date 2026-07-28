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
#include <algorithm>

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
    // Nested string: obj[k1][k2] (e.g. params["textDocument"]["uri"]); "" if any hop is absent.
    std::string getStr2(const std::string& k1, const std::string& k2) const {
        const Json* v = get(k1); return v ? v->getStr(k2) : std::string();
    }
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

// Inverse of uriToPath: a filesystem path -> a `file://` URI. Percent-encode any byte outside the URI
// unreserved/path-safe set so spaces etc. survive the round-trip. On POSIX the path already starts with
// '/', so `file://` + `/x` yields the conventional `file:///x`.
std::string pathToUri(const std::string& path) {
    auto safe = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':';
    };
    std::string uri = "file://";
    for (unsigned char c : path) {
        if (safe(c)) { uri += (char)c; }
        else { char buf[4]; snprintf(buf, sizeof buf, "%%%02X", c); uri += buf; }
    }
    return uri;
}

// kama SymKind -> LSP SymbolKind (document outline icons). LSP numbering per the spec.
int symKindToLsp(SymKind k) {
    switch (k) {
        case SymKind::Class:
        case SymKind::Value:
        case SymKind::Resource:
        case SymKind::GenericType: return 5;    // Class
        case SymKind::Contract:    return 11;   // Interface
        case SymKind::Enum:        return 10;   // Enum
        case SymKind::EnumMember:  return 22;   // EnumMember
        case SymKind::Function:
        case SymKind::GenericFn:   return 12;   // Function
        case SymKind::Method:      return 6;    // Method
        case SymKind::Ctor:        return 9;    // Constructor
        case SymKind::Field:       return 8;    // Field
        // M3.4 bindings. They never reach documentSymbols (the facade filters them out of the outline),
        // but hover and the symbol-kind decoration in other requests still ask for a kind.
        case SymKind::Local:       return 13;   // Variable
        case SymKind::Param:       return 13;   // Variable
    }
    return 5;
}

// CompletionItemKind — a DIFFERENT numbering from SymbolKind above, which is exactly the kind of thing
// that goes unnoticed: a Class is 5 as a SymbolKind and 7 as a CompletionItemKind.
int completionKindToLsp(CompletionKind k) {
    switch (k) {
        case CompletionKind::Field:      return 5;    // Field
        case CompletionKind::Method:     return 2;    // Method
        case CompletionKind::Ctor:       return 4;    // Constructor
        case CompletionKind::Variant:    return 20;   // EnumMember
        case CompletionKind::EnumMember: return 20;   // EnumMember
        case CompletionKind::Type:       return 7;    // Class
        case CompletionKind::Contract:   return 8;    // Interface
        case CompletionKind::Function:   return 3;    // Function
        case CompletionKind::Local:      return 6;    // Variable
        case CompletionKind::Param:      return 6;    // Variable
        case CompletionKind::Label:      return 10;   // Property — an argument label reads as one
        case CompletionKind::Keyword:    return 14;   // Keyword
        case CompletionKind::Module:     return 9;    // Module
        case CompletionKind::Namespace:  return 9;    // Module
        case CompletionKind::Constant:   return 21;   // Constant
    }
    return 6;
}

// ============================================================================================
// Diagnostics — analyze a buffer via the M0 facade, then map kama Diagnostics to an LSP array.
// ============================================================================================

// kama coords: line 1-based, column 0-based. LSP: line 0-based, character 0-based. Convert in ONE place.
// An unknown end (endLine/endColumn == 0) collapses to a 1-char span at the start so the range is always
// visible/non-empty. Both Diagnostic ranges and query SrcRanges funnel through here.
Json lspRange(int startLine1, int startCol0, int endLine1, int endCol0) {
    int sl = startLine1 > 0 ? startLine1 - 1 : 0;
    int sc = startCol0 >= 0 ? startCol0 : 0;
    int el, ec;
    if (endLine1 > 0 && endCol0 > 0) { el = endLine1 - 1; ec = endCol0; }
    else                             { el = sl; ec = sc + 1; }
    Json start = Json::object(); start.set("line", sl); start.set("character", sc);
    Json end   = Json::object(); end.set("line", el);   end.set("character", ec);
    Json range = Json::object(); range.set("start", start); range.set("end", end);
    return range;
}
// Would this spelling lex as a kama identifier? Guards rename: an editor will happily send `2x`, `my var`
// or `return` as a new name, and writing any of those produces a file that no longer parses. The keyword
// half defers to the lexer's own table (kamaIsKeyword) rather than duplicating it here.
bool validIdentifier(const std::string& s)
{
    if (s.empty()) return false;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (char ch : s)
        if (!(isalnum((unsigned char)ch) || ch == '_')) return false;
    return !kamaIsKeyword(s.c_str());
}

Json rangeToJson(const Diagnostic& d) { return lspRange(d.line, d.column, d.endLine, d.endColumn); }
Json srcRangeToJson(const SrcRange& r) { return lspRange(r.line, r.column, r.endLine, r.endColumn); }

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
    std::string    text;
    long           version = 0;
    SharedLspIndex lastGoodIndex;    // last cleanly-analyzed index; kept across a parse failure (queries stay live)
    size_t         linesAtLastGood = 0;   // M4.6: line count when it was built — the geometry it still describes
};

struct Server {
    std::map<std::string, Doc> docs;    // keyed by URI
    const char* argv0 = nullptr;        // compiler path — for resolving the stdlib when loading imports
    bool initialized = false;
    bool shutdownReceived = false;

    // ---- workspace index (M3.5) ----------------------------------------------------------------
    // A SECOND index, spanning the whole project rather than one file's import closure. Only the two
    // queries that need reverse reachability use it (references, rename); diagnostics, hover, go-to-def
    // and the outline stay on the per-document index, which is what keeps typing cheap — go-to-def can
    // only ever land in something the open file imports, so it needs nothing wider.
    //
    // Built LAZILY, on the first gesture that needs it, and dropped whenever any buffer or watched file
    // changes. Real language servers (rust-analyzer, clangd, gopls) keep a warm index instead, but they
    // pay for it with incremental reparse; without that (M5) a warm index would still need a full rebuild
    // after every keystroke, so eager work would be pure waste. Rebuild cost is one whole-project parse
    // per gesture — see docs/design/lsp-m3-kickoff.md for the measured number and the upgrade path.
    std::string    workspaceRoot;       // the editor's folder, from initialize (may be empty)
    SharedLspIndex wsIndex;             // cached project-wide index
    std::string    wsRootOfIndex;       // which project root wsIndex was built for
    bool           wsDirty = true;      // a buffer or watched file changed since wsIndex was built

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

    // Analyze the stored buffer for `uri`, cache the queryable index, and publish (empty array clears old
    // squiggles). On a parse failure lspAnalyze returns nullptr — keep the previous good index so
    // hover/def/outline keep answering on a mid-edit buffer while diagnostics still update.
    void analyzeAndPublish(const std::string& uri) {
        auto it = docs.find(uri);
        if (it == docs.end()) return;
        std::string path = uriToPath(uri);
        std::vector<Diagnostic> diags;
        SharedLspIndex idx = lspAnalyze(path, it->second.text, diags, argv0);
        if (idx) {
            it->second.lastGoodIndex = idx;
            it->second.linesAtLastGood = countLines(it->second.text);
        }
        publish(uri, diags);
    }

    // The project owning `path`, plus a workspace index over it, rebuilt only when something changed.
    // Every open buffer is handed in as an overlay so unsaved edits are what get searched and rewritten.
    // Returns the project even when the index is null (too large / unresolvable) — the caller needs
    // `root`/`tooLarge` to explain the refusal.
    LspProject workspaceIndexFor(const std::string& path) {
        LspProject proj = lspFindProject(path, workspaceRoot);
        if (proj.root.empty() || proj.files.empty()) { wsIndex = nullptr; wsRootOfIndex.clear(); return proj; }
        if (wsIndex && !wsDirty && wsRootOfIndex == proj.root) return proj;
        std::vector<std::pair<std::string, std::string>> overlays;
        for (auto& kv : docs) overlays.push_back({ uriToPath(kv.first), kv.second.text });
        wsIndex        = lspAnalyzeWorkspace(proj.files, overlays, argv0);
        wsRootOfIndex  = proj.root;
        wsDirty        = false;
        return proj;
    }

    void handleInitialize(const Json& id, const Json& params) {
        // The editor's workspace folder bounds project discovery (lspFindProject never rises above it).
        // `rootUri` is deprecated in the spec but still what most clients send; workspaceFolders wins.
        std::string rootUri = params.getStr("rootUri");
        if (const Json* folders = params.get("workspaceFolders"))
            if (folders->type == Json::Arr && !folders->arr.empty()) {
                std::string first = folders->arr.front().getStr("uri");
                if (!first.empty()) rootUri = first;
            }
        if (!rootUri.empty()) workspaceRoot = uriToPath(rootUri);

        // M1 advertised full-document sync only; M2 turns on the first interactive features.
        Json caps = Json::object();
        caps.set("textDocumentSync", 1);        // TextDocumentSyncKind.Full
        caps.set("hoverProvider", true);
        caps.set("definitionProvider", true);
        caps.set("documentSymbolProvider", true);
        caps.set("referencesProvider", true);
        caps.set("workspaceSymbolProvider", true);
        Json rename = Json::object();
        rename.set("prepareProvider", true);   // we can tell the editor up front whether F2 is offered
        caps.set("renameProvider", std::move(rename));
        // M4. `.` and `:` fire member completion; the second `:` of a `::` re-fires it, which is harmless
        // (the lexical scan sees the same context). No resolveProvider: every item is complete as sent.
        Json completion = Json::object();
        Json ctrig = Json::array(); ctrig.push(Json(".")); ctrig.push(Json(":"));
        completion.set("triggerCharacters", std::move(ctrig));
        completion.set("resolveProvider", false);
        caps.set("completionProvider", std::move(completion));
        Json sighelp = Json::object();
        Json strig = Json::array(); strig.push(Json("(")); strig.push(Json(","));
        sighelp.set("triggerCharacters", std::move(strig));
        caps.set("signatureHelpProvider", std::move(sighelp));
        Json folders = Json::object();
        folders.set("supported", true);
        Json ws = Json::object();
        ws.set("workspaceFolders", std::move(folders));
        caps.set("workspace", std::move(ws));
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
        wsDirty = true;              // a new buffer joins the overlay set
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
        wsDirty = true;              // the workspace index holds a now-stale copy of this buffer
        analyzeAndPublish(uri);
    }

    void handleDidClose(const Json& params) {
        const Json* td = params.get("textDocument");
        if (!td) return;
        std::string uri = td->getStr("uri");
        docs.erase(uri);
        wsDirty = true;              // its overlay is gone; the on-disk copy takes over
        publish(uri, {});                        // clear any lingering squiggles
    }

    // ---- M2 query requests (reads off the doc's cached last-good index) --------------------------
    // Read an LSP {line,character} position (0-based) and convert to kama coords (line 1-based, col
    // 0-based). Missing fields default to 0.
    static void kamaPos(const Json& params, int& kamaLine, int& kamaCol) {
        const Json* pos = params.get("position");
        int lspLine = 0, lspChar = 0;
        if (pos) {
            if (const Json* l = pos->get("line"))      lspLine = l->asInt();
            if (const Json* c = pos->get("character")) lspChar = c->asInt();
        }
        kamaLine = lspLine + 1;
        kamaCol  = lspChar;
    }

    // textDocument/documentSymbol -> a flat DocumentSymbol[] (each {name, kind, range, selectionRange};
    // container nesting is a later refinement). Empty array if the doc has no good index yet.
    void handleDocumentSymbol(const Json& id, const Json& params) {
        std::string uri = params.getStr2("textDocument", "uri");
        Json arr = Json::array();
        auto it = docs.find(uri);
        if (it != docs.end()) {
            for (const auto& s : lspDocumentSymbols(it->second.lastGoodIndex, uriToPath(uri))) {
                Json sym = Json::object();
                sym.set("name", s.name);
                sym.set("kind", symKindToLsp(s.kind));
                sym.set("range", srcRangeToJson(s.range));
                sym.set("selectionRange", srcRangeToJson(s.selectionRange));
                arr.push(std::move(sym));
            }
        }
        sendResponse(id, std::move(arr));
    }

    // textDocument/definition -> a Location {uri, range} at the decl, or null. Works on decl names +
    // signature/type references (body use-sites are M3 find-references).
    void handleDefinition(const Json& id, const Json& params) {
        std::string uri = params.getStr2("textDocument", "uri");
        auto it = docs.find(uri);
        if (it == docs.end()) { sendResponse(id, Json()); return; }
        int l, c; kamaPos(params, l, c);
        Location loc = lspDefinition(it->second.lastGoodIndex, uriToPath(uri), l, c);
        if (loc.range.line == 0) { sendResponse(id, Json()); return; }   // no def -> null
        Json result = Json::object();
        result.set("uri", pathToUri(loc.uri));
        result.set("range", srcRangeToJson(loc.range));
        sendResponse(id, std::move(result));
    }

    // textDocument/hover -> {contents:{kind:"plaintext", value:"<kind> <name>"}} or null. No range for v1.
    void handleHover(const Json& id, const Json& params) {
        std::string uri = params.getStr2("textDocument", "uri");
        auto it = docs.find(uri);
        if (it == docs.end()) { sendResponse(id, Json()); return; }
        int l, c; kamaPos(params, l, c);
        std::string text = lspHover(it->second.lastGoodIndex, uriToPath(uri), l, c);
        if (text.empty()) { sendResponse(id, Json()); return; }          // nothing here -> null
        Json contents = Json::object();
        contents.set("kind", "plaintext");
        contents.set("value", text);
        Json result = Json::object();
        result.set("contents", std::move(contents));
        sendResponse(id, std::move(result));
    }

    // ---- M4 completion + signature help --------------------------------------------------------------
    //
    // Both read `Doc::text` — the LIVE buffer — for their context, and `Doc::lastGoodIndex` for the
    // semantics. That split is the whole M4 design: a buffer being completed into does not parse, so the
    // index necessarily predates the receiver the user just typed, while its LINE GEOMETRY is still right
    // (typing `p.` adds no lines). Neither handler may touch the workspace index: completion fires on
    // every keystroke, and rebuilding a project per character is not a thing an editor survives.

    static size_t countLines(const std::string& t) {
        size_t n = 1;
        for (char c : t) if (c == '\n') ++n;
        return n;
    }

    // The index to answer a request at `kamaLine` with.
    //
    // Normally that is `lastGoodIndex`, and it is CORRECT rather than merely tolerable: the realistic
    // sequence — press Enter (still parses, index refreshes), then type `p.` (breaks the parse, adds no
    // lines) — leaves the last good index geometry-accurate for the whole file. Every single-line
    // statement, which is essentially every completion, lands in that case.
    //
    // Two cases do not: a buffer that has NEVER parsed (a new file — where completion is most wanted and
    // there is nothing at all to answer from), and one whose LINE COUNT has moved since the last good
    // parse, where every position below the edit is off. Both are repaired by blanking the cursor's line
    // and re-analyzing. Blanking rather than substituting a placeholder is deliberate twice over: it needs
    // no grammar knowledge (`p.__hole;` is not a legal statement — `statement_expression` is invocation /
    // assignment / increment only), and it preserves line and column geometry EXACTLY, which is the whole
    // invariant. The lexical context still comes from the UNTOUCHED buffer, so the receiver the user typed
    // is not lost.
    //
    // Cost is one analysis, and only on that gate; `didChange` already runs one per keystroke, so this is
    // at worst a second one on a file that is not currently parseable.
    SharedLspIndex indexForRequest(Doc& doc, const std::string& path, int kamaLine) {
        if (doc.lastGoodIndex && countLines(doc.text) == doc.linesAtLastGood) return doc.lastGoodIndex;
        size_t off = 0;
        for (int i = 1; i < kamaLine; ++i) {
            off = doc.text.find('\n', off);
            if (off == std::string::npos) return doc.lastGoodIndex;
            ++off;
        }
        if (off > doc.text.size()) return doc.lastGoodIndex;
        std::string repaired = doc.text;
        size_t eol = repaired.find('\n', off);
        if (eol == std::string::npos) eol = repaired.size();
        for (size_t i = off; i < eol; ++i) repaired[i] = ' ';
        std::vector<Diagnostic> ignored;   // the repaired buffer is not what the user has; never publish it
        SharedLspIndex idx = lspAnalyze(path, repaired, ignored, argv0);
        return idx ? idx : doc.lastGoodIndex;
    }

    // textDocument/completion -> CompletionList. `isIncomplete: false` is load-bearing — it tells the
    // client to filter the list itself as the user keeps typing, so one `.` costs one request rather than
    // one per character.
    void handleCompletion(const Json& id, const Json& params) {
        std::string uri = params.getStr2("textDocument", "uri");
        Json items = Json::array();
        auto it = docs.find(uri);
        if (it != docs.end()) {
            int l, c; kamaPos(params, l, c);
            std::string path = uriToPath(uri);
            CompletionContext ctx = completionContextAt(it->second.text, l, c);
            for (const auto& item : lspCompletion(indexForRequest(it->second, path, l), path, ctx)) {
                Json j = Json::object();
                j.set("label", item.label);
                j.set("kind", completionKindToLsp(item.kind));
                if (!item.detail.empty()) j.set("detail", item.detail);
                items.push(std::move(j));
            }
        }
        Json result = Json::object();
        result.set("isIncomplete", false);
        result.set("items", std::move(items));
        sendResponse(id, std::move(result));
    }

    // textDocument/signatureHelp -> SignatureHelp, or null when nothing callable encloses the cursor.
    void handleSignatureHelp(const Json& id, const Json& params) {
        std::string uri = params.getStr2("textDocument", "uri");
        auto it = docs.find(uri);
        if (it == docs.end()) { sendResponse(id, Json()); return; }
        int l, c; kamaPos(params, l, c);
        std::string path = uriToPath(uri);
        CompletionContext ctx = completionContextAt(it->second.text, l, c);
        SignatureHelp h = lspSignatureHelp(indexForRequest(it->second, path, l), path, ctx);
        if (h.label.empty()) { sendResponse(id, Json()); return; }
        Json ps = Json::array();
        for (const auto& p : h.params) {
            Json jp = Json::object();
            jp.set("label", p.label + ": " + p.detail);   // a string label: the client substring-matches it
            ps.push(std::move(jp));
        }
        Json sig = Json::object();
        sig.set("label", h.label);
        sig.set("parameters", std::move(ps));
        Json sigs = Json::array();
        sigs.push(std::move(sig));
        Json result = Json::object();
        result.set("signatures", std::move(sigs));
        result.set("activeSignature", 0);
        if (h.activeParam >= 0) result.set("activeParameter", h.activeParam);
        sendResponse(id, std::move(result));
    }

    // ---- M3 find-references + rename ---------------------------------------------------------------

    // Is `file` inside the project rooted at `root`, and not in its package store? Rename must not rewrite
    // the standard library or a dependency's sources: they're read-only from the project's point of view,
    // and the index holds their units too (DefSite.unit == nullptr filters only the built-in prelude, so
    // std MODULES would otherwise look like ordinary renameable user code).
    // Does the project OWN this file — i.e. may a rename rewrite it?
    //
    // The authority is the project's own FILE SET, not a path prefix. That matters in both directions: a
    // dependency lives *inside* the project root (`<root>/.kama/deps/...`) and must never be rewritten,
    // while a `"sources": ["../shared"]` entry lives *outside* it and must be, because the project said so.
    // A prefix test gets both backwards. It is also what retires the realpath trap that nearly let a rename
    // rewrite the stdlib: module resolution names std relative to the compiler binary
    // (`<exeDir>/../../lib/std/…`), which in a dev tree literally carries the project root as a prefix —
    // but it is not in the file set, so membership excludes it without any string reasoning at all.
    // Paths are still normalized, since the same file reaches us spelled several ways.
    static bool ownsFile(const std::string& file, const std::vector<std::string>& files) {
        std::string f = lspRealPath(file);
        for (const auto& p : files) if (lspRealPath(p) == f) return true;
        return false;
    }

    // textDocument/references -> Location[]. Always an array (never null) per the spec's usual shape.
    // Honors context.includeDeclaration. Results can name other files, so each Location's own uri is
    // mapped, not the request's.
    //
    // Uses the WORKSPACE index when the file belongs to a project (M3.5): the per-document index only sees
    // what the open file imports, so it would silently omit every file that uses this symbol without being
    // imported back — the same blind spot that forced rename to refuse. Falls back to the document index
    // for a file in no project, where that is the best honest answer available.
    void handleReferences(const Json& id, const Json& params) {
        std::string uri = params.getStr2("textDocument", "uri");
        Json arr = Json::array();
        auto it = docs.find(uri);
        if (it != docs.end()) {
            int l, c; kamaPos(params, l, c);
            bool includeDecl = true;
            if (const Json* ctx = params.get("context"))
                if (const Json* inc = ctx->get("includeDeclaration")) includeDecl = inc->b;
            std::string path = uriToPath(uri);
            workspaceIndexFor(path);
            const SharedLspIndex& idx = wsIndex ? wsIndex : it->second.lastGoodIndex;
            for (const auto& r : lspReferences(idx, path, l, c, includeDecl)) {
                Json loc = Json::object();
                loc.set("uri", pathToUri(r.uri));
                loc.set("range", srcRangeToJson(r.range));
                arr.push(std::move(loc));
            }
        }
        sendResponse(id, std::move(arr));
    }

    // workspace/symbol -> SymbolInformation[]: project-wide symbol search (the editor's Ctrl+T picker).
    void handleWorkspaceSymbol(const Json& id, const Json& params) {
        std::string query = params.getStr("query");
        Json arr = Json::array();
        // Unlike every other request this one carries NO document, so the project has to be inferred. Two
        // rules, in order: the project we already hold an index for (what the user has been working in),
        // then the first open document that belongs to a project at all.
        //
        // ⚠️ NOT simply `docs.begin()` — `docs` is keyed by URI, so that picks by string sort order:
        // `file:///Users/…` sorts before `file:///bindings.kama` but `file:///workspace/…` sorts after, so
        // the anchor would change with the checkout path. That passed on macOS and failed in the container.
        std::string anchor;
        for (auto& kv : docs) {
            std::string p = uriToPath(kv.first);
            LspProject probe = lspFindProject(p, workspaceRoot);
            if (probe.root.empty() || probe.files.empty()) continue;
            if (anchor.empty()) anchor = p;
            if (!wsRootOfIndex.empty() && probe.root == wsRootOfIndex) { anchor = p; break; }
        }
        if (!anchor.empty()) {
            LspProject proj = workspaceIndexFor(anchor);
            for (const auto& s : lspWorkspaceSymbols(wsIndex, query, proj.files)) {
                Json loc = Json::object();
                loc.set("uri", pathToUri(s.uri));
                loc.set("range", srcRangeToJson(s.selectionRange));
                Json sym = Json::object();
                sym.set("name", s.name);
                sym.set("kind", symKindToLsp(s.kind));
                sym.set("location", std::move(loc));
                if (!s.container.empty()) sym.set("containerName", s.container);
                arr.push(std::move(sym));
            }
        }
        sendResponse(id, std::move(arr));
    }

    // textDocument/prepareRename -> the identifier Range, or null when the symbol isn't renameable
    // (builtins, prelude/std, unresolved names, and — until M3.4 — locals/params/fields). Null makes the
    // editor grey F2 out instead of offering a rename that would do nothing.
    void handlePrepareRename(const Json& id, const Json& params) {
        std::string uri = params.getStr2("textDocument", "uri");
        auto it = docs.find(uri);
        if (it == docs.end()) { sendResponse(id, Json()); return; }
        int l, c; kamaPos(params, l, c);
        SrcRange r = lspPrepareRename(it->second.lastGoodIndex, uriToPath(uri), l, c);
        if (r.line == 0) { sendResponse(id, Json()); return; }
        sendResponse(id, srcRangeToJson(r));
    }

    // textDocument/rename -> a WorkspaceEdit {changes:{<uri>:TextEdit[]}}, spanning every project file
    // that uses the symbol (M3.5).
    //
    // M3.3 refused any cross-file rename, because the loaded unit set was only what the open file
    // transitively imports: a file using THIS symbol without being imported back was invisible, so a
    // rewrite could silently break a caller we never saw. The workspace index removes that blind spot, so
    // the refusal is replaced by three narrower ones — no project, a project too big to trust, and a
    // definition we don't own. Everything else is rewritten across all its files at once.
    void handleRename(const Json& id, const Json& params) {
        std::string uri = params.getStr2("textDocument", "uri");
        auto it = docs.find(uri);
        if (it == docs.end()) { sendError(id, -32602, "rename: document not open"); return; }
        std::string newName = params.getStr("newName");
        if (!validIdentifier(newName)) {
            sendError(id, -32602, "`" + newName + "` is not a valid kama identifier");
            return;
        }
        int l, c; kamaPos(params, l, c);
        std::string path = uriToPath(uri);

        LspProject proj = workspaceIndexFor(path);
        // No project (no kama.json, or a tree too big to trust as one) => no way to know who else uses this
        // symbol. That does NOT make rename impossible: a symbol whose uses are all inside the open file —
        // every local, param and private field, the commonest rename by far — needs no workspace knowledge.
        // So fall back to the document index under M3.3's original rule: rewrite when nothing escapes this
        // file, refuse when something does, and say which piece of project setup would lift the refusal.
        bool haveProject = wsIndex != nullptr;
        const SharedLspIndex& idx = haveProject ? wsIndex : it->second.lastGoodIndex;

        SrcRange target = lspPrepareRename(idx, path, l, c);
        if (target.line == 0) { sendError(id, -32602, "there is nothing renameable here"); return; }

        if (haveProject) {
            // Refuse on the DEFINITION's location, not on the uses: a symbol we don't own (std, a
            // dependency) may legitimately be used from project files, and rewriting only those callers
            // while leaving the declaration alone would break the build.
            Location def = lspDefinition(idx, path, l, c);
            if (!def.uri.empty() && !ownsFile(def.uri, proj.files)) {
                sendError(id, -32803, "cannot rename: this symbol is defined outside the project, in " +
                                      def.uri + ". Only this project's own sources can be rewritten.");
                return;
            }
        }

        std::vector<Location> refs = lspReferences(idx, path, l, c, /*includeDecl*/ true);
        if (!haveProject) {
            for (const auto& r : refs)
                if (r.uri != path) {
                    std::string why = proj.tooLarge
                        ? (proj.root + " holds more than " + std::to_string(proj.cap) +
                           " .kama files, which is a source tree rather than a package. Add a kama.json "
                           "next to the sources you want indexed")
                        : std::string("this file is not part of a kama package. Add a kama.json next to "
                                      "your sources");
                    sendError(id, -32803, "cannot rename: this symbol is also used in " + r.uri +
                                          ", and " + why + " so a rename can see every file that uses it.");
                    return;
                }
        }
        // Group by file. Within a file the edits must be sorted and non-overlapping (LSP requirement), so
        // sort by position and drop exact duplicates.
        std::map<std::string, std::vector<SrcRange>> byFile;
        for (const auto& r : refs) {
            // Never write outside the project (the open file itself always counts — it may be project-less).
            if (haveProject && !ownsFile(r.uri, proj.files) && r.uri != path) continue;
            byFile[r.uri].push_back(r.range);
        }
        Json changes = Json::object();
        for (auto& kv : byFile) {
            std::vector<SrcRange>& rs = kv.second;
            std::sort(rs.begin(), rs.end(), [](const SrcRange& a, const SrcRange& b) {
                if (a.line != b.line) return a.line < b.line;
                return a.column < b.column;
            });
            rs.erase(std::unique(rs.begin(), rs.end(), [](const SrcRange& a, const SrcRange& b) {
                return a.line == b.line && a.column == b.column;
            }), rs.end());
            Json edits = Json::array();
            for (const auto& r : rs) {
                Json e = Json::object();
                e.set("range", srcRangeToJson(r));
                e.set("newText", newName);
                edits.push(std::move(e));
            }
            changes.set(pathToUri(kv.first), std::move(edits));
        }
        Json result = Json::object();
        result.set("changes", std::move(changes));
        sendResponse(id, std::move(result));
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
            if (isRequest) handleInitialize(*idp, params);
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
        if (method == "textDocument/documentSymbol") { if (isRequest) handleDocumentSymbol(*idp, params); return true; }
        if (method == "textDocument/definition")     { if (isRequest) handleDefinition(*idp, params);     return true; }
        if (method == "textDocument/hover")          { if (isRequest) handleHover(*idp, params);          return true; }
        if (method == "textDocument/references")     { if (isRequest) handleReferences(*idp, params);     return true; }
        if (method == "textDocument/prepareRename")  { if (isRequest) handlePrepareRename(*idp, params);  return true; }
        if (method == "textDocument/rename")         { if (isRequest) handleRename(*idp, params);         return true; }
        if (method == "workspace/symbol")            { if (isRequest) handleWorkspaceSymbol(*idp, params); return true; }
        if (method == "textDocument/completion")     { if (isRequest) handleCompletion(*idp, params);     return true; }
        if (method == "textDocument/signatureHelp")  { if (isRequest) handleSignatureHelp(*idp, params);  return true; }
        // A watched .kama file changed on disk — created, deleted, or edited outside the editor. The client
        // only sends this if it registered watchers (ours does; see editor/vscode/extension.js). Nothing to
        // re-publish: just drop the workspace index so the next gesture re-reads the tree.
        if (method == "workspace/didChangeWatchedFiles") { wsDirty = true; return true; }

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

int runLspServer(const char* argv0) {
    Server server;
    server.argv0 = argv0;
    return server.run();
}
