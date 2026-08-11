// kama.lsp.cpp — the `kama lsp` language server (LSP campaign M1, the walking skeleton).
//
// A JSON-RPC 2.0 server over stdio that reuses the M0 front-end-as-library analysis path
// (CEmitter::analyze + structured Diagnostics) to publish live, as-you-type diagnostics to any LSP
// editor. Scope for M1: lifecycle + full-document sync + publishDiagnostics. Hover / go-to-definition /
// completion are M2+ — the query facade (kama.query.h) is already built and waiting.
//
// User-facing docs: docs/editors.md. Everything is self-contained here (a hand-rolled JSON
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
#include "kama.json.h"       // the shared Json value + serializer (also used by `--json` output)

namespace {


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
    // ...and here is where that actually happens. The comment above has described this strip since the
    // function was written and the code never performed it, so on Windows every URI became `/C:/Users/…`
    // — a path nothing can open. `kama build` never sees a URI and resolved the same imports fine, so the
    // damage was confined to the editor: no imported module loaded, every cross-file go-to-definition
    // missed, and `import std::collections` reported as not exporting what it plainly exports.
    // Shape-tested rather than #ifdef'd — a POSIX path cannot look like `/X:/`.
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':' &&
        ((path[1] >= 'A' && path[1] <= 'Z') || (path[1] >= 'a' && path[1] <= 'z')))
        path.erase(0, 1);
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
    // The inverse of uriToPath's strip: a Windows path starts at its drive letter, and `file://C:/x`
    // would make `C:` look like a HOSTNAME. The conventional spelling is `file:///C:/x`, which is also
    // what round-trips back through uriToPath.
    if (path.size() >= 2 && path[1] == ':' &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
        uri += '/';
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

// The semanticTokens LEGEND (M6 B2). ⚠️ ORDER IS THE WIRE FORMAT: a token's type is sent as an INDEX into
// this array, so appending a name is safe and reordering silently recolours every buffer in every client.
// Only STANDARD LSP type names are used — a client styles a custom name only if its theme happens to know
// it, whereas every theme has rules for these, so a standard name is the difference between colour and no
// colour. This is the layer that corrects what a regex provably cannot: the TextMate grammar guesses that a
// capitalized word is a type, and only the resolver knows whether `Box` is a type, a local or a field.
static const char* kSemTokenTypes[] = {
    "class", "struct", "interface", "enum", "enumMember",
    "function", "method", "property", "variable", "parameter",
};
static const char* kSemTokenModifiers[] = { "declaration" };

// SymKind -> index into kSemTokenTypes. A kama `value` type maps to "struct" and a `resource` to "class"
// deliberately: the distinction a theme draws between them (a value vs. an entity with identity) is exactly
// the one kama's two kinds draw.
int semTokenType(SymKind k) {
    switch (k) {
        case SymKind::Class:
        case SymKind::Resource:
        case SymKind::GenericType: return 0;   // class
        case SymKind::Value:       return 1;   // struct
        case SymKind::Contract:    return 2;   // interface
        case SymKind::Enum:        return 3;   // enum
        case SymKind::EnumMember:  return 4;   // enumMember
        case SymKind::Function:
        case SymKind::GenericFn:   return 5;   // function
        case SymKind::Method:
        case SymKind::Ctor:        return 6;   // method
        case SymKind::Field:       return 7;   // property
        case SymKind::Local:       return 8;   // variable
        case SymKind::Param:       return 9;   // parameter
    }
    return 0;
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
    SharedLspIndex lastGoodIndex;    // last analyzed index; since M5.4 a mid-edit buffer still produces
                                     // one (recovery keeps what parsed), so this stays current, not stale
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
    // per gesture — measured, and fast enough that the index is rebuilt rather than cached.
    std::string    workspaceRoot;       // the editor's folder, from initialize (may be empty)
    SharedLspIndex wsIndex;             // cached project-wide index
    std::string    wsRootOfIndex;       // which project root wsIndex was built for
    bool           wsDirty = true;      // a buffer or watched file changed since wsIndex was built

    // ---- build configuration (M6 A1) -----------------------------------------------------------
    // ONE configuration per process, pinned by the first opened document that resolves a manifest. It has
    // to be per-process rather than per-file: the M5 parse cache holds units `pruneInactiveDecls` rewrote
    // IN PLACE under the flag set in force, so two configurations cannot share it.
    //
    // KNOWN LIMIT, documented rather than papered over: in a monorepo whose packages declare DIFFERENT
    // flag universes, packages other than the pinned one get the pinned one's configuration — their
    // `@compileFor`-gated declarations may be dropped, and their own flag names may read as undeclared
    // under strict validation. Workarounds, in order: declare the shared flag universe in the ROOT
    // manifest (strict validation then accepts every member's names), or one editor window per package.
    // The real fix is per-project configuration, which needs per-configuration parse caches.
    //
    // Re-pinning on tab switch is deliberately NOT done: it would evict the cache and re-analyze every
    // open closure on every switch between packages — the 86 ms path, repeatedly — to solve a case one
    // window per package already solves.
    std::string configManifest;         // the kama.json this process pinned ("" = none)
    std::string configHint;             // the document that pinned it (a re-resolve replays the same walk)
    bool        configPinned = false;

    // ---- watched files (M6 C0) -------------------------------------------------------------------
    // Whether the client asked us to register the file watcher ourselves. VS Code's client-side
    // `synchronize.fileEvents` list is a vscode-languageclient LIBRARY convenience, not a protocol
    // feature — at the protocol level, DYNAMIC REGISTRATION is the only way a server ever receives
    // `workspace/didChangeWatchedFiles`. Verified against three independent clients, none of which
    // exposes a static glob list: Neovim (`vim/lsp/_watchfiles.lua`, registration-driven), Helix
    // (`did_change_watched_files.dynamic_registration: true` — one of the few it enables at all) and
    // eglot (`eglot-register-capability … workspace/didChangeWatchedFiles`). So without this the other
    // clients would LOOK wired up and silently never re-resolve a manifest — the failure mode a
    // documentation page cannot catch.
    bool clientWatchesDynamically = false;
    int  nextOutgoingId = 1;            // ids for the requests WE send; the client's ids are its own space

    // The globs to watch, in one place because two things must agree on them: this registration and the
    // VS Code client's static list (editor/vscode/extension.js). `tools/check-editors.sh` asserts they do.
    // ⚠️ `**/kama.json` does NOT match `kama.local.json` — the two manifests are separate patterns.
    static const char* const* watchedGlobs(size_t& n) {
        static const char* const kGlobs[] = { "**/*.kama", "**/kama.json", "**/kama.local.json" };
        n = sizeof(kGlobs) / sizeof(kGlobs[0]);
        return kGlobs;
    }

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

    // A server->client REQUEST. The reply comes back on the same stdin stream as everything else and is
    // deliberately ignored (see dispatch) — nothing the server does depends on the answer, so there is no
    // pending-request table to keep. Every prior message the server sent was a response or a notification;
    // this is the first thing it asks FOR.
    void sendRequest(const char* method, Json params) {
        Json req = Json::object();
        req.set("jsonrpc", "2.0");
        req.set("id", nextOutgoingId++);
        req.set("method", method);
        req.set("params", std::move(params));
        writeMessage(serialize(req));
    }

    // Register `workspace/didChangeWatchedFiles` for the three globs, if the client said it accepts
    // dynamic registration. Sent on `initialized`, which is the point the spec designates for it.
    //
    // ⚠️ A client may legitimately decline: Neovim advertises this as FALSE on Linux/BSD on purpose (its
    // watcher backends are too limited). Registering anyway would be ignored at best, so we don't — and
    // docs/editors.md states what such a user loses (an out-of-editor manifest edit needs a restart).
    void registerFileWatchers() {
        if (!clientWatchesDynamically) return;
        size_t n = 0;
        const char* const* globs = watchedGlobs(n);
        Json watchers = Json::array();
        for (size_t i = 0; i < n; ++i) {
            Json w = Json::object();
            w.set("globPattern", globs[i]);
            // No `kind` — omitting it means create|change|delete, which is what we want: a .kama being
            // deleted invalidates the workspace index exactly as much as one being edited.
            watchers.push(std::move(w));
        }
        Json opts = Json::object();
        opts.set("watchers", std::move(watchers));
        Json reg = Json::object();
        reg.set("id", "kama-watched-files");     // stable: we never unregister, so it need not be unique
        reg.set("method", "workspace/didChangeWatchedFiles");
        reg.set("registerOptions", std::move(opts));
        Json regs = Json::array();
        regs.push(std::move(reg));
        Json params = Json::object();
        params.set("registrations", std::move(regs));
        sendRequest("client/registerCapability", std::move(params));
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

    // window/logMessage and window/showMessage. `type`: 1=Error 2=Warning 3=Info 4=Log. logMessage lands in
    // the editor's output channel; showMessage is a visible toast, so it is reserved for a real failure.
    void sendWindowMessage(const char* method, int type, const std::string& msg) {
        Json params = Json::object();
        params.set("type", type);
        params.set("message", msg);
        Json note = Json::object();
        note.set("jsonrpc", "2.0");
        note.set("method", method);
        note.set("params", std::move(params));
        writeMessage(serialize(note));
    }
    void logMessage(int type, const std::string& msg)  { sendWindowMessage("window/logMessage", type, msg); }
    void showMessage(int type, const std::string& msg) { sendWindowMessage("window/showMessage", type, msg); }

    // What the server is analyzing under, in one line. This bug survived five milestones precisely because
    // nothing ever SAID which flags were in force — a server that quietly analyzes the wrong program looks
    // exactly like a server that analyzes the right one. Now it announces itself.
    static std::string describeConfig(const LspBuildConfig& c) {
        std::string s = "config: ";
        s += c.manifest.empty() ? "(no kama.json — permissive defaults)" : c.manifest;
        if (!c.localManifest.empty()) s += " + kama.local.json";
        s += " | target " + c.targetName;
        if (!c.targetTriple.empty()) s += " (" + c.targetTriple + ")";
        s += " | BUILD_TYPE " + c.buildType;
        if (c.strict) s += " | strict";
        s += " | flags:";
        for (const auto& f : c.activeFlags) s += " " + f;
        return s;
    }

    // The same facts as describeConfig, as data (M6 C1). The prose line is for a human reading the output
    // channel; this is for a client that renders a status bar and offers a picker, and it carries what
    // could be SELECTED as well as what is in force so no client re-derives the catalog.
    //
    // `kama/…` is a vendor-namespaced custom notification, the shape every comparable server uses for the
    // same job (rust-analyzer `experimental/serverStatus`, Metals `metals/status`, clangd
    // `textDocument/clangd.fileStatus`). Sent unconditionally: per the spec an unknown notification MUST
    // be ignored, so a client that does not want it is unharmed and one that does needs no negotiation.
    void notifyBuildConfig(const LspBuildConfig& c) {
        Json groups = Json::object();
        for (const auto& g : c.groups) {
            Json values = Json::array();
            for (const auto& v : g.values) values.push(Json(v));
            Json obj = Json::object();
            obj.set("values", std::move(values));
            obj.set("selected", g.selected);
            groups.set(g.name, std::move(obj));
        }
        Json flags = Json::array();
        for (const auto& f : c.activeFlags) flags.push(Json(f));
        Json params = Json::object();
        params.set("manifest", c.manifest);            // "" = no kama.json: the picker must refuse, since
        params.set("localManifest", c.localManifest);  // kama.local.json is only read BESIDE a kama.json
        params.set("triple", c.targetTriple);
        params.set("strict", c.strict);
        params.set("flags", std::move(flags));
        params.set("groups", std::move(groups));
        Json note = Json::object();
        note.set("jsonrpc", "2.0");
        note.set("method", "kama/buildConfig");
        note.set("params", std::move(params));
        writeMessage(serialize(note));
    }

    // Resolve and install the build configuration, and if it CHANGED, make every open document agree with
    // it. Returns true when it re-analyzed, so a caller that was about to analyze does not do it twice.
    //
    // The eviction is not caution. `pruneInactiveDecls` already rewrote every cached unit in place under
    // the OLD flag set, deleting declarations that the new set may want back — and a dropped declaration
    // cannot be recovered from the pruned AST. The cache must go.
    bool applyConfig(const std::string& hintPath) {
        LspBuildConfig cfg;
        std::string err;
        if (!lspResolveBuildConfig(hintPath, workspaceRoot, cfg, err))
            showMessage(1, "kama: " + err + " — analyzing with default build flags");
        logMessage(3, describeConfig(cfg));
        notifyBuildConfig(cfg);

        bool changed = (cfg.manifest != configManifest);
        configManifest = cfg.manifest;
        if (!cfg.manifest.empty()) { configPinned = true; configHint = hintPath; }
        if (!changed) return false;

        lspEvictParsedFile("");            // the fixed-flag-set invariant — see kama.lsp.h
        wsIndex = nullptr;
        wsRootOfIndex.clear();
        wsDirty = true;
        for (auto& kv : docs) analyzeAndPublish(kv.first);
        return true;
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
        if (idx) it->second.lastGoodIndex = idx;
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

        // Does this client want the server to register the file watcher? (M6 C0 — see watchedGlobs.)
        if (const Json* caps = params.get("capabilities"))
            if (const Json* wsc = caps->get("workspace"))
                if (const Json* dcwf = wsc->get("didChangeWatchedFiles"))
                    if (const Json* dyn = dcwf->get("dynamicRegistration"))
                        clientWatchesDynamically = (dyn->type == Json::Bool && dyn->b);

        // Install host defaults now, so nothing is EVER analyzed with a truly empty `@compileFor` set even
        // if the first document resolves no manifest. The real configuration is pinned on the first
        // didOpen, from that file's nearest kama.json — see applyConfig / lspResolveBuildConfig.
        LspBuildConfig hostCfg;
        {
            std::string err;
            lspResolveBuildConfig("", "", hostCfg, err);
            logMessage(3, describeConfig(hostCfg));
        }

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
        // M6 B2. `full` only — deliberately no `range` and no delta variants. Range would save nothing (the
        // answer is a read off an index that is already built for the whole file) and delta would trade a
        // measured 0-cost request for per-document result-id bookkeeping. The legend's ORDER is the wire
        // format; see kSemTokenTypes.
        Json semtok = Json::object();
        Json legend = Json::object();
        Json sttypes = Json::array();
        for (const char* t : kSemTokenTypes) sttypes.push(Json(t));
        Json stmods = Json::array();
        for (const char* m : kSemTokenModifiers) stmods.push(Json(m));
        legend.set("tokenTypes", std::move(sttypes));
        legend.set("tokenModifiers", std::move(stmods));
        semtok.set("legend", std::move(legend));
        semtok.set("full", true);
        caps.set("semanticTokensProvider", std::move(semtok));
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
        // AFTER the result, unlike the log line above. `window/logMessage` is one of the few notifications
        // the spec lets a server send before initialization completes; a custom one is not, and a client
        // that has not yet processed the result may have no handler for it.
        notifyBuildConfig(hostCfg);
    }

    void handleDidOpen(const Json& params) {
        const Json* td = params.get("textDocument");
        if (!td) return;
        std::string uri = td->getStr("uri");
        Doc doc;
        doc.text = td->getStr("text");
        docs[uri] = std::move(doc);
        wsDirty = true;              // a new buffer joins the overlay set
        // Pin the build configuration from the FIRST document that resolves a manifest, before its first
        // analysis — so in the common case this buffer is analyzed exactly once, under the right flags.
        // applyConfig re-analyzes every open document when it changes anything, hence the early return.
        if (!configPinned && applyConfig(uriToPath(uri))) return;
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
        wsDirty = true;              // the workspace index holds a now-stale copy of this buffer
        analyzeAndPublish(uri);
    }

    void handleDidClose(const Json& params) {
        const Json* td = params.get("textDocument");
        if (!td) return;
        std::string uri = td->getStr("uri");
        docs.erase(uri);
        wsDirty = true;              // its overlay is gone; the on-disk copy takes over
        // ...and the on-disk copy is now authoritative, so drop whatever the parse cache holds for it:
        // the editor may have saved without the file watcher firing (or being registered at all).
        lspEvictParsedFile(uriToPath(uri));
        publish(uri, {});                        // clear any lingering squiggles
    }

    // A watched file changed on disk — created, deleted, or edited outside the editor. The client only
    // sends this if it registered watchers (ours does; see editor/vscode/extension.js).
    //
    // Drop the workspace index so the next gesture re-reads the tree, and drop the parse cache wholesale —
    // the notification may name a directory, and one extra closure re-parse is invisible next to serving a
    // stale AST.
    //
    // A MANIFEST change is different in kind: it changes the PROGRAM BEING ANALYZED, not just a file in it.
    // Every open buffer's diagnostics are now answers to the wrong question, so re-resolve and republish.
    // `configManifest.clear()` forces applyConfig to treat it as changed even when the path is identical
    // and only the CONTENTS moved — which is the whole case.
    void handleDidChangeWatchedFiles(const Json& params) {
        bool manifestChanged = false;
        if (const Json* ch = params.get("changes"))
            if (ch->type == Json::Arr)
                for (const auto& c : ch->arr)
                    if (lspIsManifestPath(uriToPath(c.getStr("uri")))) { manifestChanged = true; break; }
        wsDirty = true;
        lspEvictParsedFile("");
        if (manifestChanged) {
            configManifest.clear();
            applyConfig(configHint);
        }
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

    // textDocument/semanticTokens/full -> {data: [...]}, five integers per token:
    //   deltaLine, deltaStartChar, length, tokenType, tokenModifiers
    // Both deltas are relative to the PREVIOUS TOKEN, and deltaStartChar is relative to the previous
    // token's start only when they share a line — an absolute column otherwise. The facade hands back
    // tokens already sorted and guaranteed non-overlapping, which is what makes that encoding expressible
    // at all; it also means the loop below never has to sort or de-duplicate.
    //
    // This is the ONLY place the coordinate convention is converted (kama line 1-based -> LSP 0-based),
    // keeping the campaign's invariant that kamaPos and lspRange are the sole conversion points. It cannot
    // reuse lspRange: the protocol wants a length here, not a range.
    void handleSemanticTokens(const Json& id, const Json& params) {
        std::string uri = params.getStr2("textDocument", "uri");
        Json data = Json::array();
        auto it = docs.find(uri);
        if (it != docs.end()) {
            int prevLine = 0, prevCol = 0;
            for (const auto& t : lspSemanticTokens(it->second.lastGoodIndex, uriToPath(uri))) {
                int line  = t.line - 1;                                  // kama 1-based -> LSP 0-based
                int dLine = line - prevLine;
                int dCol  = (dLine == 0) ? t.column - prevCol : t.column;
                data.push(Json(dLine));
                data.push(Json(dCol));
                data.push(Json(t.length));
                data.push(Json(semTokenType(t.kind)));
                data.push(Json(t.isDecl ? 1 : 0));                       // bit 0 == "declaration"
                prevLine = line;
                prevCol  = t.column;
            }
        }
        Json res = Json::object();
        res.set("data", std::move(data));
        sendResponse(id, std::move(res));
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

    // The index to answer a request with: the last good one, always, in one lookup.
    //
    // M4.6 needed more than that. Without grammar-level error recovery a mid-edit buffer produced no AST
    // at all, so `lastGoodIndex` went stale the moment you typed `p.` and a buffer that had NEVER parsed
    // — a new file, where completion is wanted most — had nothing to answer from. The fix was to blank
    // the cursor's line and RE-ANALYZE per request, which cost a measured 229 ms on every completion
    // against an unparseable buffer, i.e. the common case while typing rather than an edge case.
    //
    // M5.3/M5.4 removed the need for it. Recovery discards the broken statement and keeps the rest, so
    // `didChange` builds a fresh, geometry-accurate index every keystroke even mid-edit, and
    // `analyzeAndPublish` stores it — which is exactly what the repair was reconstructing, one request
    // at a time. Retired against a criterion rather than a hunch: `KAMA_LSP_NO_REPAIR=1
    // tools/check-lsp.sh` passed the ENTIRE harness including the two M4.6 assertions (ids 37/38) that
    // motivated the repair in the first place. Those assertions stay, and they are the guard: weaken
    // recovery and they fail here, where the cause is obvious.
    SharedLspIndex indexForRequest(Doc& doc, const std::string&, int) {
        return doc.lastGoodIndex;
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
            auto push = [&](const std::string& label, CompletionKind kind, const std::string& detail) {
                Json j = Json::object();
                j.set("label", label);
                j.set("kind", completionKindToLsp(kind));
                if (!detail.empty()) j.set("detail", detail);
                items.push(std::move(j));
            };
            // An import names something the file does NOT import yet, so the index cannot know it exists;
            // these two answer from the module resolver instead (M4.7).
            if (ctx.trigger == CompletionTrigger::ImportPath) {
                for (const auto& m : lspImportModules(path, ctx.receiver, argv0))
                    push(m, CompletionKind::Module, "");
            } else if (ctx.trigger == CompletionTrigger::ImportSymbol) {
                for (const auto& sym : lspImportSymbols(path, ctx.receiver, argv0)) {
                    bool already = false;
                    for (const auto& f : ctx.filled) if (f == sym) { already = true; break; }
                    if (!already) push(sym, CompletionKind::Type, ctx.receiver);
                }
            } else {
                for (const auto& item : lspCompletion(indexForRequest(it->second, path, l), path, ctx))
                    push(item.label, item.kind, item.detail);
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
            // Refuse on the DECLARATIONS' locations, not on the uses: a symbol we don't own (std, a
            // dependency) may legitimately be used from project files, and rewriting only those callers
            // while leaving the declaration alone would break the build.
            //
            // Plural since M6 B3c: a contract method and its implementations are ONE name, and a group can
            // straddle the project boundary — a type here implementing `std::Iterator` has an owned
            // definition at the cursor and an unowned one in the stdlib. Checking only the cursor's would
            // pass, and the edit loop below would then silently drop the contract's declaration and leave
            // conformance broken. Every mature server refuses this case instead.
            for (const Location& def : lspRenameDeclarations(idx, path, l, c)) {
                if (!def.uri.empty() && ownsFile(def.uri, proj.files)) continue;
                std::string where = def.uri.empty() ? std::string("the compiler's own prelude")
                                                    : def.uri;
                sendError(id, -32803, "cannot rename: this name is also declared outside the project, in " +
                                      where + ". Only this project's own sources can be rewritten.");
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

        // A RESPONSE to one of our own requests: it carries an `id` but no `method`. Since M6 C0 the server
        // sends `client/registerCapability`, so replies now arrive on this stream. Drop it — nothing waits
        // on the answer. Without this it would look like a request for the method "" and draw a
        // `method not found` ERROR RESPONSE aimed at the client's own id, which is a protocol violation.
        if (method.empty() && idp) return true;

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
        // `initialized` is where the spec says a server registers dynamic capabilities, and it is the only
        // thing we do with it.
        if (method == "initialized") { registerFileWatchers(); return true; }
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
        if (method == "textDocument/semanticTokens/full") { if (isRequest) handleSemanticTokens(*idp, params); return true; }
        if (method == "workspace/didChangeWatchedFiles") { handleDidChangeWatchedFiles(params); return true; }
        // `workspace/didChangeConfiguration` is deliberately NOT handled — see kama.lsp.h. The one
        // configuration channel is `kama.local.json`, which arrives through the watcher above, so this
        // notification carries nothing actionable; a handler would be dead code that reads as though
        // configuration flowed through it. Unhandled NOTIFICATIONS fall through legally (only requests get
        // an error below), so a client that volunteers one — nvim sends `settings = {}` on attach — is
        // silently and correctly ignored.

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
    // Reuse parsed units across analyses for this process only — see lspSetParseCache in kama.lsp.h for
    // why a build deliberately does not opt in. Enabled here rather than inside the driver so the
    // decision reads where the long-lived process is, not where the cache is.
    lspSetParseCache(true);
    Server server;
    server.argv0 = argv0;
    return server.run();
}
