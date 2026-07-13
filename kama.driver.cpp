// kama driver: parse kama source and transpile to portable C, optionally
// invoking a C compiler to produce a native executable.
//
//   kama transpile <in.kama> [-o out.c] [--no-line]
//   kama build     <in.kama> [-o out] [--target native|wasm] [--webgpu]
//                              [--cc <compiler>] [--no-line] [--keep-c]
//
// native builds invoke clang; wasm builds invoke emcc (Emscripten), keying the
// output format off the -o extension (.html harness by default).
// (LLVM is gone; the backend is kama.cemit.*.)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <memory>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <algorithm>

#include <limits.h>
#include <sys/stat.h>           // stat / S_ISDIR (directory check) — POSIX + mingw-w64 UCRT
#include <dirent.h>             // opendir / readdir (module directory listing) — POSIX + mingw-w64 UCRT
#ifdef _WIN32
  // NB: do NOT include <windows.h> here — it is compiled in the same TU as kama.parser.hpp, whose token
  // enum (BOOL, CHAR, CONST, INT8, VOID, …) collides with windows.h typedefs/macros. mingw-w64's POSIX
  // dirent/stat cover everything the driver needs, so windows.h is unnecessary.
  #include <stdlib.h>           // _fullpath, _MAX_PATH
  #ifndef PATH_MAX
    #define PATH_MAX _MAX_PATH
  #endif
#else
  #include <unistd.h>
  #include <sys/wait.h>         // WEXITSTATUS
#endif

#include "kama.parser.hpp"
#include "kama.lexer.hpp"
#include "kama.context.h"
#include "kama.cemit.h"
#include "kama.prelude.h"   // KAMA_PRELUDE_SRC + KAMA_PRELUDE_MODULES (embedded built-in kama)

#ifndef KAMA_VERSION
#define KAMA_VERSION "0.0.0-dev"
#endif

namespace {

std::string absolutePath(const std::string& path)
{
    char buf[PATH_MAX];
#ifdef _WIN32
    if (_fullpath(buf, path.c_str(), PATH_MAX)) return std::string(buf);
#else
    if (realpath(path.c_str(), buf)) return std::string(buf);
#endif
    return path; // fall back to as-given (e.g. file doesn't exist yet)
}

// Split on either separator so the same code works on Windows paths.
std::string dirName(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
}

std::string baseName(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

std::string stripExtension(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    size_t dot   = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path;
    return path.substr(0, dot);
}

bool fileExists(const std::string& p)
{
    std::ifstream f(p.c_str());
    return f.good();
}

// Where kama_runtime.h lives, resolved so an INSTALLED binary finds it from any
// cwd: $KAMA_HOME, else <exeDir>/../include (bin/kama -> ../include), else
// <exeDir> (repo root layout), else ".".
std::string resolveRuntimeDir(const char* argv0)
{
    if (const char* home = getenv("KAMA_HOME")) return home;
    std::string exeDir = dirName(absolutePath(argv0 ? argv0 : "kama"));
    if (fileExists(exeDir + "/../include/kama_runtime.h")) return exeDir + "/../include";
    if (fileExists(exeDir + "/kama_runtime.h"))            return exeDir;
    return ".";
}

bool dirExists(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);   // POSIX + mingw-w64
}

// The `*.kama` files directly inside `dir`, sorted for deterministic emit order.
std::vector<std::string> listKamaFiles(const std::string& dir)
{
    std::vector<std::string> out;
    auto keep = [&](const std::string& name) {
        return name.size() > 5 && name.compare(name.size() - 5, 5, ".kama") == 0;
    };
    if (DIR* d = opendir(dir.c_str())) {   // POSIX + mingw-w64 (wraps FindFirstFile internally on Windows)
        while (struct dirent* e = readdir(d)) { std::string n = e->d_name; if (keep(n)) out.push_back(dir + "/" + n); }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The stdlib root, resolved from the binary like resolveRuntimeDir: $KAMA_HOME/lib,
// else <exeDir>/../lib/kama (installed, bin/kama -> ../lib/kama), else <exeDir>/lib
// (repo/dev layout), else "lib". The stdlib ships INSIDE the install; `std::*` resolves here.
std::string resolveStdlibDir(const char* argv0)
{
    if (const char* home = getenv("KAMA_HOME")) return std::string(home) + "/lib";
    std::string exeDir = dirName(absolutePath(argv0 ? argv0 : "kama"));
    if (dirExists(exeDir + "/../lib/kama")) return exeDir + "/../lib/kama";
    if (dirExists(exeDir + "/lib"))          return exeDir + "/lib";
    return "lib";
}

// The native C compiler. A "-bundled" install ships a static zig at
// <exe>/../libexec/zig/zig[.exe]; prefer it so `kama build` works with no system
// toolchain. Otherwise fall back to system clang. (--cc overrides both, upstream.)
std::string resolveCCompiler(const char* argv0)
{
    std::string exeDir = dirName(absolutePath(argv0 ? argv0 : "kama"));
#ifdef _WIN32
    std::string zig = exeDir + "/../libexec/zig/zig.exe";
#else
    std::string zig = exeDir + "/../libexec/zig/zig";
#endif
    if (fileExists(zig)) return "\"" + zig + "\" cc";
    return "clang";
}

// Split a `:`-separated search-path env (KAMA_PATH) into roots.
std::vector<std::string> splitSearchPath(const char* env)
{
    std::vector<std::string> out;
    if (!env) return out;
    std::string cur;
    for (const char* p = env; ; ++p) {
        if (*p == ':' || *p == '\0') { if (!cur.empty()) out.push_back(cur); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
    return out;
}

// Resolve module segments (["std","memory"]) to source file(s) under the first matching
// root: a file-module (<root>/std/memory.kama) or every *.kama in a directory-module
// (<root>/std/memory/). Empty result => unresolved.
std::vector<std::string> resolveModuleFiles(const std::vector<std::string>& segs,
                                            const std::vector<std::string>& roots)
{
    std::string rel;
    for (size_t i = 0; i < segs.size(); ++i) rel += (i ? "/" : "") + segs[i];
    for (auto& root : roots) {
        std::string file = root + "/" + rel + ".kama";
        if (fileExists(file)) return { file };
        std::string dir = root + "/" + rel;
        if (dirExists(dir)) { auto fs = listKamaFiles(dir); if (!fs.empty()) return fs; }
    }
    return {};
}

SharedCompilationUnit parseFile(const std::string& inputFile);   // defined below
SharedCompilationUnit parseString(const char* src, const std::string& name);   // defined below

// --- Serialization codegen (`@generate`) --------------------------------------------------------------
// A `@generate(Serialize|Deserialize)` type gets its `implements` block SYNTHESIZED as kama source, parsed,
// and injected into the declaring unit — so it rides the full pipeline (contract dispatch, RAII, ownership)
// instead of hand-emitted C. This runs after all modules load (so the field types are present) and before
// collection/emission.

// Reconstruct a type's source spelling (name + one level of generic arg), for a `foreach` binding.
static std::string typeToStr(SharedIdentifier t)
{
    if (!t || !t->value) return "";
    std::string s = *t->value;
    if (t->genericArg) s += "<" + typeToStr(t->genericArg) + ">";
    return s;
}

// A type's FULL source spelling incl. every generic arg (`Map<string, int32>`, `List<Point>`) — unlike
// typeToStr, which drops all but the first arg. Used as a turbofish type arg (`decode::<Map<K,V>>`).
static std::string typeSpell(SharedIdentifier t)
{
    if (!t || !t->value) return "";
    std::string s = *t->value;
    if (t->genericArgs && !t->genericArgs->empty()) {
        s += "<";
        for (size_t i = 0; i < t->genericArgs->size(); i++) { if (i) s += ", "; s += typeSpell((*t->genericArgs)[i]); }
        s += ">";
    } else if (t->genericArg) {
        s += "<" + typeSpell(t->genericArg) + ">";
    }
    return s;
}

// The Serializer call that writes a field of `type` read via `access` (e.g. "this.x"). Primitive/string map
// to a per-width write; a List/Array/Fixed becomes a JSON array (foreach over the elements); any other
// (user) type must itself be `Serialize` and is recursed into.
static std::string serializeWriteStmt(SharedIdentifier type, const std::string& access)
{
    if (!type) return "";
    // Optional<T> -> the inner value, or JSON null. The payload write must be a single statement (a
    // collection-valued Optional is deferred — it can't fit a single-expression match arm).
    if (type->value && *type->value == "Optional" && type->genericArg) {
        std::string inner = serializeWriteStmt(type->genericArg, "__opt");
        if (inner.empty() || inner.find('\n') != std::string::npos) return "";
        return "match (" + access + ") { case Some(__opt): " + inner
             + " case None: w.writeNull(); };";
    }
    // `Fixed<T,N>` is a compiler-intrinsic (macro) type and can't carry a hand-written conformance, so it
    // keeps this inline JSON-array emission. `List`/`Array`/`Map`/`Set` (and user types) implement `Serialize`
    // themselves, so they fall through to the `access.serialize(w: w)` delegation below (uniform contract).
    if (type->value && type->genericArg && *type->value == "Fixed") {
        std::string elemWrite = serializeWriteStmt(type->genericArg, access + "[__i]");
        if (elemWrite.empty()) return "";
        return "w.beginArray(count: " + access + ".length());\n"
               "        for (int32 __i = 0; __i < " + access + ".length(); __i = __i + 1) { " + elemWrite + " }\n"
               "        w.endArray();";
    }
    switch (type->builtInVal) {
        case IDENTIFIER_INT8_VAL:    return "w.writeI8(v: " + access + ");";
        case IDENTIFIER_INT16_VAL:   return "w.writeI16(v: " + access + ");";
        case IDENTIFIER_INT32_VAL:   return "w.writeI32(v: " + access + ");";
        case IDENTIFIER_INT64_VAL:   return "w.writeI64(v: " + access + ");";
        case IDENTIFIER_UINT8_VAL:   return "w.writeU8(v: " + access + ");";
        case IDENTIFIER_UINT16_VAL:  return "w.writeU16(v: " + access + ");";
        case IDENTIFIER_UINT32_VAL:  return "w.writeU32(v: " + access + ");";
        case IDENTIFIER_UINT64_VAL:  return "w.writeU64(v: " + access + ");";
        case IDENTIFIER_BOOL_VAL:    return "w.writeBool(v: " + access + ");";
        case IDENTIFIER_FLOAT32_VAL: return "w.writeF32(v: " + access + ");";
        case IDENTIFIER_FLOAT64_VAL: return "w.writeF64(v: " + access + ");";
        case IDENTIFIER_CHAR_VAL:    return "w.writeChar(v: " + access + ");";
        case IDENTIFIER_STRING_VAL:  return "w.writeString(v: ref " + access + ");";
        default: break;
    }
    // platform-width integer aliases carry no builtInVal
    if (type->value && *type->value == "usize") return "w.writeU64(v: cast<uint64>(" + access + "));";
    if (type->value && *type->value == "isize") return "w.writeI64(v: cast<int64>(" + access + "));";
    // a nested user type — it must itself implement Serialize (derived or hand-written)
    return access + ".serialize(w: w);";
}

// Read `@generate(Serialize|Deserialize)` off a type; returns which directions were requested.
static void generateDirections(ClassDeclarationNode* cd, bool& ser, bool& de)
{
    ser = false; de = false;
    if (!cd->attributes) return;
    for (auto& at : *cd->attributes) {
        if (!at || !at->name || *at->name != "generate" || !at->args) continue;
        for (auto& a : *at->args)
            if (a && a->name && a->name->value && !a->expression) {
                if (*a->name->value == "Serialize")   ser = true;
                if (*a->name->value == "Deserialize") de  = true;
            }
    }
}

// The body statements of the synthesized `serialize` (the per-field write calls), for a non-generic
// `@generate(Serialize)` type. Returns "" for a generic type (v1-deferred).
static std::string buildSerializeBody(ClassDeclarationNode* cd)
{
    if (!cd || !cd->members) return "";
    if (cd->typeParams && !cd->typeParams->empty()) return "";   // generic @generate: deferred
    std::string body;
    for (auto& m : *cd->members) {
        auto* fd = dynamic_cast<ClassFieldDeclarationNode*>(m.get());
        if (!fd || !fd->declarators) continue;
        bool skip = false; std::string rename;
        if (fd->attributes)
            for (auto& at : *fd->attributes) {
                if (!at || !at->name) continue;
                if (*at->name == "skip") skip = true;
                else if (*at->name == "field" && at->args)
                    for (auto& a : *at->args)
                        if (a && a->name && a->name->value && *a->name->value == "name" && a->expression)
                            if (auto* sn = dynamic_cast<StringNode*>(a->expression.get()))
                                rename = sn->value ? *sn->value : "";
            }
        if (skip) continue;
        for (auto& d : *fd->declarators) {
            if (!d->name || !d->name->value) continue;
            std::string fname = *d->name->value;
            std::string wire  = rename.empty() ? fname : rename;
            std::string ws    = serializeWriteStmt(fd->type, "this." + fname);
            if (ws.empty()) continue;
            body += "        w.fieldName(name: \"" + wire + "\");\n";
            body += "        " + ws + "\n";
        }
    }
    return body;
}

// A pointer field the graph machinery writes as an interned id: a direct `Shared`/`Weak`, or a *nullable*
// edge `Optional<Shared>`/`Optional<Weak>` (essential — a never-null `Shared` can't express a list tail or
// bootstrap a cycle). The object-graph trigger (see std::serialization::graph).
static bool typeIsGraphRef(SharedIdentifier t)
{
    if (!t || !t->value) return false;
    if (*t->value == "Shared" || *t->value == "Weak") return true;
    if (*t->value == "Optional" && t->genericArg && t->genericArg->value
        && (*t->genericArg->value == "Shared" || *t->genericArg->value == "Weak")) return true;
    return false;
}

// True iff `cd` is a graph type: a `@generate` product with a (non-`@skip`) `Shared`/`Weak` field.
static bool cdHasGraphRef(ClassDeclarationNode* cd)
{
    if (!cd || !cd->members) return false;
    for (auto& m : *cd->members) {
        auto* fd = dynamic_cast<ClassFieldDeclarationNode*>(m.get());
        if (!fd || !fd->type) continue;
        bool skip = false;
        if (fd->attributes) for (auto& at : *fd->attributes) if (at && at->name && *at->name == "skip") skip = true;
        if (!skip && typeIsGraphRef(fd->type)) return true;
    }
    return false;
}

// The body of a graph type's `__serializeInto`: writes each `@field`. Non-pointer fields reuse the ordinary
// per-field write; a `Shared`/`Weak` field is written as `w.writeRef(id: ctx.intern(...))` — interning the
// pointee (deduped by address, enqueued for the id table). An expired `Weak` writes id `0`. "" if a
// non-pointer field is unsupported (same rule as buildSerializeBody).
static std::string buildGraphSerializeInto(ClassDeclarationNode* cd)
{
    if (!cd || !cd->members) return "";
    std::string body;
    for (auto& m : *cd->members) {
        auto* fd = dynamic_cast<ClassFieldDeclarationNode*>(m.get());
        if (!fd || !fd->declarators) continue;
        bool skip = false; std::string rename;
        if (fd->attributes)
            for (auto& at : *fd->attributes) {
                if (!at || !at->name) continue;
                if (*at->name == "skip") skip = true;
                else if (*at->name == "field" && at->args)
                    for (auto& a : *at->args)
                        if (a && a->name && a->name->value && *a->name->value == "name" && a->expression)
                            if (auto* sn = dynamic_cast<StringNode*>(a->expression.get()))
                                rename = sn->value ? *sn->value : "";
            }
        if (skip) continue;
        for (auto& d : *fd->declarators) {
            if (!d->name || !d->name->value) continue;
            std::string f = *d->name->value, wire = rename.empty() ? f : rename;
            body += "        w.fieldName(name: \"" + wire + "\");\n";
            if (typeIsGraphRef(fd->type)) {
                // Intern a `Shared`-valued expression `sx` (dedup + enqueue) and write its id.
                auto internShared = [](const std::string& sx, const std::string& k) {
                    return "Shared<GraphNode> __gref_" + k + " = " + sx + "; "
                           "w.writeRef(id: ctx.intern(pid: cast<uint64>(" + sx + ".__ptrId()), node: __gref_"
                           + k + "));";
                };
                // Upgrade a `Weak`-valued expression `wx` to reach its pointee; an expired weak is id 0.
                auto internWeak = [&](const std::string& wx, const std::string& k) {
                    return "match (" + wx + ".tryUpgrade()) {\n"
                           "            case Some(__u_" + k + "): { " + internShared("__u_" + k, k) + " }\n"
                           "            case None: { w.writeRef(id: 0ui64); }\n"
                           "        };";
                };
                const std::string& tv = *fd->type->value;
                if (tv == "Shared") {
                    body += "        " + internShared("this." + f, f) + "\n";
                } else if (tv == "Weak") {
                    body += "        " + internWeak("this." + f, f) + "\n";
                } else {   // Optional<Shared|Weak> — a nullable edge; absent -> id 0
                    bool optWeak = fd->type->genericArg && fd->type->genericArg->value
                                   && *fd->type->genericArg->value == "Weak";
                    std::string inner = optWeak ? internWeak("__o_" + f, f) : internShared("__o_" + f, f);
                    body += "        match (this." + f + ") {\n";
                    body += "            case Some(__o_" + f + "): { " + inner + " }\n";
                    body += "            case None: { w.writeRef(id: 0ui64); }\n";
                    body += "        };\n";
                }
            } else {
                std::string ws = serializeWriteStmt(fd->type, "this." + f);
                if (ws.empty()) return "";
                body += "        " + ws + "\n";
            }
        }
    }
    return body;
}

// The scalar/string `r.readX()` expression for `type`, used for a field or a container element. "" if not
// a scalar/string (a nested user type — its element-level deserialize is a v1-deferred follow-up).
static std::string deserializeReadScalar(SharedIdentifier type)
{
    if (!type) return "";
    switch (type->builtInVal) {
        case IDENTIFIER_INT8_VAL:    return "r.readI8()";
        case IDENTIFIER_INT16_VAL:   return "r.readI16()";
        case IDENTIFIER_INT32_VAL:   return "r.readI32()";
        case IDENTIFIER_INT64_VAL:   return "r.readI64()";
        case IDENTIFIER_UINT8_VAL:   return "r.readU8()";
        case IDENTIFIER_UINT16_VAL:  return "r.readU16()";
        case IDENTIFIER_UINT32_VAL:  return "r.readU32()";
        case IDENTIFIER_UINT64_VAL:  return "r.readU64()";
        case IDENTIFIER_BOOL_VAL:    return "r.readBool()";
        case IDENTIFIER_FLOAT32_VAL: return "r.readF32()";
        case IDENTIFIER_FLOAT64_VAL: return "r.readF64()";
        case IDENTIFIER_CHAR_VAL:    return "r.readChar()";
        case IDENTIFIER_STRING_VAL:  return "r.readString()";
        default: return "";
    }
}

// The read EXPRESSION for one element/field of `type`: a scalar/string `r.readX()`, OR a nested user
// `@generate(Deserialize)` type's static factory `T::deserialize(r: r)` (returns the value/resource by
// value; the reader is shared so nested reads advance the same stream). "" => unsupported (Array, smart
// pointers, etc. — deferred). Used for a scalar/nested field, an `Optional<E>` payload, or a `List<E>` elem.
static std::string deserializeReadElem(SharedIdentifier type)
{
    std::string rs = deserializeReadScalar(type);
    if (!rs.empty()) return rs;
    if (type && type->value && type->builtInVal == 0) {
        const std::string& n = *type->value;
        // Delegate to the type's static `deserialize` factory — a nested user type OR a collection that
        // implements `Deserialize` (`List`/`Map`/`Set`). `Array`/`Fixed` deserialize is a follow-up (fixed-size
        // construction from an unknown-length array); `Optional` is handled below; smart pointers are rejected.
        // Route through the prelude `__kamaDeserialize<T>` helper: there is no surface syntax to call a static
        // on a generic instance (`List<int32>::deserialize` / `List::<int32>::deserialize` both fail to parse),
        // but a turbofish on a free fn works (`__kamaDeserialize::<List<int32>>(r)`), and it calls `T::deserialize`.
        if (n != "Optional" && n != "Array" && n != "Fixed"
            && n != "Owned" && n != "Shared" && n != "Weak" && n != "Bindable")
            return "__kamaDeserialize::<" + typeSpell(type) + ">(r: r)";
    }
    return "";
}

// The statement(s) that read a value of `type` directly into `dst` (a `result.<field>` place). Scalar/nested
// => assign; `Optional<E>` => null→None else Some(read E); `List<E>` => read a JSON array element-by-element.
// The RHS is always an rvalue (a `readX()`/`deserialize(...)` return), so owned values move in with no `give`
// keyword, and the field was zero-initialized so no stale value is dropped. "" => an unsupported field type.
static std::string deserializeFieldSet(SharedIdentifier type, const std::string& dst)
{
    if (!type) return "";
    if (type->value && *type->value == "Optional" && type->genericArg) {
        std::string re = deserializeReadElem(type->genericArg);
        if (re.empty()) return "";
        return "if (r.readNull()) { " + dst + " = Optional::None; } "
               "else { " + dst + " = Optional::Some(value: " + re + "); }";
    }
    // `List`/`Map`/`Set` fields delegate to the collection's own static `deserialize` (via `deserializeReadElem`
    // below) — `dst = List<T>::deserialize(r)` — no inline append loop. `Optional` stays special (null mapping).
    std::string re = deserializeReadElem(type);
    if (re.empty()) return "";
    return dst + " = " + re + ";";
}

// (The by-value `deserialize` factory for a non-graph `@generate` struct is now emitted directly in C by
// the compiler intrinsic — CEmitter::emitDeserializeDefinition. The deserializeReadScalar/ReadElem/FieldSet
// helpers below remain: the GRAPH read path — buildGraphShell/buildGraphWire — still uses them.)

// Parse a wrapper `type … implements C { … }` and merge its interface clause + members into `cd` — so the
// generated method(s) live on the type as a NORMAL conformance (fat-pointer vtable), not a retroactive block.
// Parsed wrapper units whose AST nodes get spliced into user types — kept alive for the whole compile so
// the CodeGenContext they reference outlives emission (they'd otherwise free on return -> use-after-free).
static std::vector<SharedCompilationUnit> g_generatedUnits;

static void mergeGeneratedInto(ClassDeclarationNode* cd, const std::string& wrapSrc)
{
    SharedCompilationUnit pu = parseString(wrapSrc.c_str(), "<generated-serde>");
    if (!pu || !pu->codeDeclarationList || pu->codeDeclarationList->empty()) return;
    g_generatedUnits.push_back(pu);   // keep the spliced-from AST + its context alive
    auto* gcd = dynamic_cast<ClassDeclarationNode*>((*pu->codeDeclarationList)[0].get());
    if (!gcd) return;
    if (gcd->baseTypes && gcd->baseTypes->interfaces) {
        if (!cd->baseTypes) cd->baseTypes = gcd->baseTypes;
        else if (cd->baseTypes->interfaces)
            for (auto& itf : *gcd->baseTypes->interfaces) cd->baseTypes->interfaces->push_back(itf);
        else cd->baseTypes->interfaces = gcd->baseTypes->interfaces;
    }
    if (gcd->members) {
        if (!cd->members) cd->members = std::make_shared<ClassMemberDeclarationList>();
        for (auto& mm : *gcd->members) cd->members->push_back(mm);
    }
}

// ---------------------------------------------------------------------------
// Graph DESERIALIZE codegen (3a-read). A graph deserializes INTO a `Shared<Root>` via a two-pass build over
// per-concrete-type id registries: pass 1 constructs every object as a zeroed heap SHELL (HeapShell) and
// registers it by id; pass 2 wires the pointer fields by id lookup + copy/downgrade; the root is handed back.
// Cycles/forward-refs work because all shells exist before any wiring — no closures, no deferral table.
//
// It's spread over several MERGED methods (each with private access to its own type's fields) + free helpers:
//   K::__gShell(r) -> Shared<K>          build a shell, read own scalar fields, skip ref fields (pass 1)
//   K::__gWire(r, <regs>)   (instance)   read own ref fields, resolve via the registries, store (pass 2)
//   Root::deserialize(r) -> Shared<Root> the driver: registries + two passes + return root (roots only)
//   __glookup_P(ids, objs, id) -> Optional<Shared<P>>   linear-scan id lookup (parallel Lists, because
//                                        `Map<_, Shared<_>>` can't be used — its `cloneVal(ref V)` rejects a
//                                        smart-pointer `ref`; so registries mirror SerContext's parallel Lists)
// Registries are `List<uint64> __ids_K` + `List<Shared<K>> __objs_K`, one pair per graph type in the unit.

// Classify a pointer `@field`: fills `kind` (Shared/Weak/OptShared/OptWeak) + the pointee type. False otherwise.
struct GraphRefInfo { std::string kind; SharedIdentifier pointee; };
static bool classifyGraphRef(SharedIdentifier t, GraphRefInfo& out)
{
    if (!t || !t->value) return false;
    const std::string& v = *t->value;
    if (v == "Shared" && t->genericArg) { out.kind = "Shared"; out.pointee = t->genericArg; return true; }
    if (v == "Weak"   && t->genericArg) { out.kind = "Weak";   out.pointee = t->genericArg; return true; }
    if (v == "Optional" && t->genericArg && t->genericArg->value && t->genericArg->genericArg) {
        const std::string& iv = *t->genericArg->value;
        if (iv == "Shared") { out.kind = "OptShared"; out.pointee = t->genericArg->genericArg; return true; }
        if (iv == "Weak")   { out.kind = "OptWeak";   out.pointee = t->genericArg->genericArg; return true; }
    }
    return false;
}

// Walk a class's `@field` declarators, invoking `fn(fd, declName, wireName)` for each non-`@skip` field.
template <typename F>
static void forEachField(ClassDeclarationNode* cd, F fn)
{
    if (!cd || !cd->members) return;
    for (auto& m : *cd->members) {
        auto* fd = dynamic_cast<ClassFieldDeclarationNode*>(m.get());
        if (!fd || !fd->declarators) continue;
        bool skip = false; std::string rename;
        if (fd->attributes)
            for (auto& at : *fd->attributes) {
                if (!at || !at->name) continue;
                if (*at->name == "skip") skip = true;
                else if (*at->name == "field" && at->args)
                    for (auto& a : *at->args)
                        if (a && a->name && a->name->value && *a->name->value == "name" && a->expression)
                            if (auto* sn = dynamic_cast<StringNode*>(a->expression.get())) rename = sn->value ? *sn->value : "";
            }
        if (skip) continue;
        for (auto& d : *fd->declarators) {
            if (!d->name || !d->name->value) continue;
            std::string f = *d->name->value;
            fn(fd, f, rename.empty() ? f : rename);
        }
    }
}

// The `K::__gShell` body: a HeapShell local `__sh`, then read each SCALAR `@field` into the pointee; the read
// loop's `else { skipValue }` consumes ref-field values (wired in pass 2). Returns "" if a scalar field is
// unsupported (mirrors buildDeserializeBody).
static std::string buildGraphShell(ClassDeclarationNode* cd)
{
    std::string tn = *cd->name->value, chain, defaults; bool first = true; bool bad = false;
    forEachField(cd, [&](ClassFieldDeclarationNode* fd, const std::string& f, const std::string& wire) {
        if (typeIsGraphRef(fd->type)) return;   // ref field — wired in __gWire; skipped here
        std::string set = deserializeFieldSet(fd->type, "__sh.deref()." + f);
        if (set.empty()) { bad = true; return; }
        if (fd->type && fd->type->value && *fd->type->value == "Optional")
            defaults += "        __sh.deref()." + f + " = Optional::None;\n";
        chain += std::string("            ") + (first ? "if" : "else if")
               + " (__k == \"" + wire + "\") { " + set + " }\n";
        first = false;
    });
    if (bad) return "";
    // A type with no scalar fields (all ref fields, e.g. a pure-pointer node) skips every field here — no
    // if-chain, so emit a bare `skipValue` (a lone `else` would be a parse error).
    std::string loopBody = first
        ? "            r.skipValue();\n"
        : chain + "            else { r.skipValue(); }\n";
    return "        Shared<" + tn + "> __sh;\n"           // initializer -> HeapShellNode post-parse
         + defaults +
           "        while (r.moreFields()) {\n"
           "            string __k = r.fieldName();\n"
         + loopBody +
           "        }\n"
           "        return __sh;\n";
}

// The `K::__gWire` param list + body: read each pointer `@field`'s id and store the resolved handle. Params are
// `ref List<uint64> __ids_P, ref List<Shared<P>> __objs_P` for each distinct pointee type P; `pointees` returns
// them in order (the driver passes the matching registries).
static std::string buildGraphWire(ClassDeclarationNode* cd, std::vector<std::string>& pointees)
{
    std::string body; bool first = true;
    std::vector<std::string> seen;
    forEachField(cd, [&](ClassFieldDeclarationNode* fd, const std::string& f, const std::string& wire) {
        GraphRefInfo gi;
        if (!classifyGraphRef(fd->type, gi)) return;
        std::string P = typeSpell(gi.pointee);
        if (std::find(seen.begin(), seen.end(), P) == seen.end()) { seen.push_back(P); pointees.push_back(P); }
        std::string look = "__glookup_" + P + "(ids: ref __ids_" + P + ", objs: ref __objs_" + P + ", id: __rid)";
        std::string w;
        if (gi.kind == "Shared")
            w = "uint64 __rid = r.readRef(); Optional<Shared<" + P + ">> __e = " + look + ";\n"
                "                match (__e) { case Some(__tv): { this." + f + " = __tv; } "
                "case None: { r.failWith(e: DeError::UnresolvedReference); } };";
        else if (gi.kind == "Weak")
            w = "uint64 __rid = r.readRef(); Optional<Shared<" + P + ">> __e = " + look + ";\n"
                "                match (__e) { case Some(__tv): { this." + f + " = __tv.downgrade(); } "
                "case None: { if (__rid != 0ui64) { r.failWith(e: DeError::UnresolvedReference); } } };";
        else if (gi.kind == "OptShared")
            w = "uint64 __rid = r.readRef();\n"
                "                if (__rid == 0ui64) { this." + f + " = Optional::None; }\n"
                "                else { Optional<Shared<" + P + ">> __e = " + look + ";\n"
                "                    match (__e) { case Some(__tv): { this." + f + " = Optional::Some(value: __tv); } "
                "case None: { r.failWith(e: DeError::UnresolvedReference); } }; }";
        else /* OptWeak */
            w = "uint64 __rid = r.readRef();\n"
                "                if (__rid == 0ui64) { this." + f + " = Optional::None; }\n"
                "                else { Optional<Shared<" + P + ">> __e = " + look + ";\n"
                "                    match (__e) { case Some(__tv): { this." + f + " = Optional::Some(value: __tv.downgrade()); } "
                "case None: { r.failWith(e: DeError::UnresolvedReference); } }; }";
        body += std::string("            ") + (first ? "if" : "else if")
              + " (__k == \"" + wire + "\") { " + w + " }\n";
        first = false;
    });
    std::string loopBody = first
        ? "            r.skipValue();\n"
        : body + "            else { r.skipValue(); }\n";
    return "        while (r.moreFields()) {\n"
           "            string __k = r.fieldName();\n"
         + loopBody +
           "        }\n";
}

// A per-pointee-type linear-scan lookup: id -> Optional<Shared<P>> (Some = a retained copy; None = absent/0).
static std::string buildGraphLookup(const std::string& P)
{
    return "fn Optional<Shared<" + P + ">> __glookup_" + P
         + "(ref List<uint64> ids, ref List<Shared<" + P + ">> objs, uint64 id) {\n"
           "    if (id == 0ui64) { return Optional::None; }\n"
           "    int32 i = 0; int32 n = cast<int32>(ids.length());\n"
           "    while (i < n) {\n"
           "        if (ids[i] == id) { return Optional::Some(value: copy objs[i]); }\n"   // `copy` op = retain
           "        i = i + 1;\n"
           "    }\n"
           "    return Optional::None;\n"
           "}\n";
}

// The `Root::deserialize` two-pass driver body: registries for every graph type, pass 1 builds+registers shells,
// pass 2 wires ref fields, then the root is looked up and returned (a dangling root id -> UnresolvedReference).
static std::string buildGraphDriver(ClassDeclarationNode* rootCd,
                                    const std::vector<ClassDeclarationNode*>& graphTypes)
{
    std::string rn = *rootCd->name->value;
    std::string regs, pass1, pass2;
    bool f1 = true, f2 = true;
    for (auto* K : graphTypes) {
        std::string kn = *K->name->value;
        regs += "        List<uint64> __ids_" + kn + " = List(); List<Shared<" + kn + ">> __objs_" + kn + " = List();\n";
        pass1 += std::string("            ") + (f1 ? "if" : "else if") + " (__ty == \"" + kn + "\") { "
               + "__ids_" + kn + ".add(item: __id); __objs_" + kn + ".add(item: " + kn + "::__gShell(r: r)); }\n";
        f1 = false;
        if (cdHasGraphRef(K)) {
            std::vector<std::string> pts; buildGraphWire(K, pts);   // just to collect the pointee order for the call
            std::string args;
            for (auto& P : pts) args += ", __ids_" + P + ": ref __ids_" + P + ", __objs_" + P + ": ref __objs_" + P;
            pass2 += std::string("            ") + (f2 ? "if" : "else if") + " (__ty == \"" + kn + "\") {\n"
                   + "                Optional<Shared<" + kn + ">> __cur = __glookup_" + kn
                   + "(ids: ref __ids_" + kn + ", objs: ref __objs_" + kn + ", id: __id);\n"
                   + "                match (__cur) { case Some(__c): { __c.__gWire(r: r" + args + "); } "
                     "case None: { while (r.moreFields()) { string __sk = r.fieldName(); r.skipValue(); } } };\n"
                   + "            }\n";
        } else {
            pass2 += std::string("            ") + (f2 ? "if" : "else if") + " (__ty == \"" + kn + "\") { "
                   + "while (r.moreFields()) { string __sk = r.fieldName(); r.skipValue(); } }\n";
        }
        f2 = false;
    }
    std::string skipEntry = "{ while (r.moreFields()) { string __sk = r.fieldName(); r.skipValue(); } }";
    return "        uint64 __root = r.beginGraph();\n"
         + regs +
           "        r.beginObject();\n"
           "        while (r.moreFields()) {\n"
           "            uint64 __id = r.entryKey();\n"
           "            r.beginObject();\n"
           "            string __tk = r.fieldName(); string __ty = r.readString();\n"
         + pass1 +
           "            else " + skipEntry + "\n"
           "        }\n"
           "        r.rewindGraph();\n"
           "        r.beginObject();\n"
           "        while (r.moreFields()) {\n"
           "            uint64 __id = r.entryKey();\n"
           "            r.beginObject();\n"
           "            string __tk = r.fieldName(); string __ty = r.readString();\n"
         + pass2 +
           "            else " + skipEntry + "\n"
           "        }\n"
           "        r.endGraph();\n"
           "        Optional<Shared<" + rn + ">> __rr = __glookup_" + rn
             + "(ids: ref __ids_" + rn + ", objs: ref __objs_" + rn + ", id: __root);\n"
           "        Shared<" + rn + "> __fb;\n"          // fallback; initializer -> HeapShellNode post-parse
           "        match (__rr) { case Some(__rv): { return __rv; } "
             "case None: { r.failWith(e: DeError::UnresolvedReference); return __fb; } };\n";
}

// Replace the null initializer of `varName` in method `methodName` with a HeapShellNode of the decl's type
// (mirrors injectZeroInitForDeserialize, but the internal node is a heap-adopt shell rather than `(T){0}`).
static void injectHeapShell(ClassDeclarationNode* cd, const std::string& methodName, const std::string& varName)
{
    if (!cd || !cd->members) return;
    static SharedCodeGenContext genCtx =
        std::make_shared<CodeGenContext>(std::make_shared<std::string>("<gen-heapshell>"));
    for (auto& m : *cd->members) {
        auto* md = dynamic_cast<ClassMethodDeclarationNode*>(m.get());
        if (!md || !md->name || !md->name->value || *md->name->value != methodName) continue;
        if (!md->body || !md->body->statements) continue;
        for (auto& st : *md->body->statements) {
            auto* lvd = dynamic_cast<LocalVariableDeclaration*>(st.get());
            if (!lvd || !lvd->variables) continue;
            for (auto& v : *lvd->variables)
                if (v && v->name && v->name->value && *v->name->value == varName && !v->initializer) {
                    v->initializer = std::make_shared<HeapShellNode>(*genCtx, lvd->type);
                    return;
                }
        }
    }
}

// Read `@generate(Serialize|Deserialize)` off an enum declaration.
static void enumDirections(EnumDeclarationNode* ed, bool& ser, bool& de)
{
    ser = false; de = false;
    if (!ed->attributes) return;
    for (auto& at : *ed->attributes) {
        if (!at || !at->name || *at->name != "generate" || !at->args) continue;
        for (auto& a : *at->args)
            if (a && a->name && a->name->value && !a->expression) {
                if (*a->name->value == "Serialize")   ser = true;
                if (*a->name->value == "Deserialize") de  = true;
            }
    }
}

// Enum serialize: a free `ref Enum` helper that `match`es the value (so a primitive receiver resolves — see
// the string retro-impl) writing `{"tag":"Variant"[,"value":{fields}]}`, plus the retro `implements Serialize`
// that delegates to it. Enums can't carry methods (grammar), so the conformance is RETROACTIVE — static
// dispatch, which is exactly how an enum field (`this.e.serialize(w)`) / a `List<Enum>` element is reached.
static std::string buildEnumSerialize(EnumDeclarationNode* ed)
{
    if (!ed || !ed->identifier || !ed->identifier->value || !ed->body) return "";
    if (ed->typeParams && !ed->typeParams->empty()) return "";   // generic enum: deferred
    std::string en = *ed->identifier->value, arms;
    for (auto& m : *ed->body) {
        auto* mm = dynamic_cast<EnumMemberDeclarationNode*>(m.get());
        if (!mm || !mm->identifier || !mm->identifier->value) continue;
        std::string vn = *mm->identifier->value;
        if (mm->payload && !mm->payload->empty()) {
            std::string binds, writes;
            for (size_t i = 0; i < mm->payload->size(); i++) {
                auto* p = dynamic_cast<FunctionParameterNode*>((*mm->payload)[i].get());
                if (!p || !p->identifier || !p->identifier->value) return "";
                // Bind the payload to a PREFIXED name (`__p_<field>`), not the field name itself — a field
                // named `w` or `__e` would otherwise shadow the Serializer param / match subject and
                // `w.fieldName(...)` would dispatch on the payload. The field name is only the wire key.
                std::string fn = *p->identifier->value, bind = "__p_" + fn;
                std::string ws = serializeWriteStmt(p->type, bind);
                if (ws.empty()) return "";
                if (i) binds += ", ";
                binds  += bind;
                writes += "            w.fieldName(name: \"" + fn + "\"); " + ws + "\n";
            }
            arms += "        case " + vn + "(" + binds + "): {\n"
                    "            w.fieldName(name: \"tag\"); string __t = \"" + vn + "\"; w.writeString(v: ref __t);\n"
                    "            w.fieldName(name: \"value\"); w.beginObject();\n" + writes +
                    "            w.endObject();\n        }\n";
        } else {
            arms += "        case " + vn + ": { w.fieldName(name: \"tag\"); string __t = \"" + vn
                  + "\"; w.writeString(v: ref __t); }\n";
        }
    }
    if (arms.empty()) return "";
    return "fn void __kamaSer_" + en + "(ref " + en + " __e, ref Serializer w) {\n"
           "    w.beginObject();\n    match (__e) {\n" + arms + "    };\n    w.endObject();\n}\n"
           "implements Serialize for " + en + " {\n"
           "    public fn void serialize(ref Serializer w) { __kamaSer_" + en + "(__e: this, w: w); }\n}\n";
}

// Enum deserialize: a retro static factory (rides the retro-static-method support). Reads the `tag` first,
// then dispatches — a no-payload variant is `Enum::V`, a payload variant reads its `value` object positionally
// (`moreFields` consumes the structure) and constructs `Enum::V(field: [give] read)`. An unknown tag flags the
// reader (`r.fail()`) and returns a no-payload variant as a benign discarded default.
static std::string buildEnumDeserialize(EnumDeclarationNode* ed)
{
    if (!ed || !ed->identifier || !ed->identifier->value || !ed->body) return "";
    if (ed->typeParams && !ed->typeParams->empty()) return "";
    std::string en = *ed->identifier->value, chain, dflt;
    bool first = true;
    for (auto& m : *ed->body) {
        auto* mm = dynamic_cast<EnumMemberDeclarationNode*>(m.get());
        if (!mm || !mm->identifier || !mm->identifier->value) continue;
        std::string vn = *mm->identifier->value, body;
        bool hasPayload = mm->payload && !mm->payload->empty();
        if (!hasPayload && dflt.empty()) dflt = vn;   // a no-payload variant is the unknown-tag fallback
        if (hasPayload) {
            std::string reads, args;
            for (size_t i = 0; i < mm->payload->size(); i++) {
                auto* p = dynamic_cast<FunctionParameterNode*>((*mm->payload)[i].get());
                if (!p || !p->identifier || !p->identifier->value) return "";
                std::string fn = *p->identifier->value, re = deserializeReadElem(p->type);
                if (re.empty()) return "";
                std::string idx = std::to_string(i);
                reads += "            bool __pf" + idx + " = r.moreFields(); string __pk" + idx
                       + " = r.fieldName(); " + typeSpell(p->type) + " __p_" + fn + " = " + re + ";\n";
                // Always `give` the read-into-local payload: a no-op for a true scalar (int/float/bool),
                // a move for a `string`/collection/resource — so a `string` payload isn't rejected as a bare
                // named collection into a variant (`string` carries a `builtInVal` but still owns a buffer).
                if (i) args += ", ";
                args += fn + ": give __p_" + fn;
            }
            body = "            bool __fv = r.moreFields(); string __kv = r.fieldName();\n"
                   "            r.beginObject();\n" + reads +
                   "            bool __pe = r.moreFields();\n            bool __oe = r.moreFields();\n"
                   "            return " + en + "::" + vn + "(" + args + ");\n";
        } else {
            body = "            bool __oe = r.moreFields();\n            return " + en + "::" + vn + ";\n";
        }
        chain += std::string("        ") + (first ? "if" : "else if")
               + " (__tag.equals(other: \"" + vn + "\")) {\n" + body + "        }\n";
        first = false;
    }
    if (dflt.empty()) return "";   // no no-payload variant to default to on failure (deferred)
    return "implements Deserialize for " + en + " {\n"
           "    public static fn " + en + " deserialize(Deserializer r) {\n"
           "        r.beginObject();\n"
           "        bool __f = r.moreFields(); string __k = r.fieldName(); string __tag = r.readString();\n"
         + chain +
           "        r.fail();\n        return " + en + "::" + dflt + ";\n    }\n}\n";
}

// Scan every unit for `@generate` types and give each the generated `serialize`/`deserialize` method +
// `implements Serialize`/`Deserialize` clause, MERGED INTO THE TYPE ITSELF (a normal conformance with a
// fat-pointer vtable — a retroactive block is static-dispatch-only and couldn't be a contract value). An
// `@generate` ENUM instead gets RETROACTIVE impls (enums can't carry methods) appended to the unit.
void synthesizeSerialization(std::vector<SharedCompilationUnit>& units)
{
    for (auto& u : units) {
        if (!u || !u->codeDeclarationList) continue;
        // A graph unit: any `@generate` type has a `Shared`/`Weak` field. In such a unit EVERY
        // `@generate(Serialize)` type implements `GraphNode` (so it can be a graph pointee — a `Shared<Leaf>`
        // field needs `Leaf` to be a GraphNode to enqueue it), and graph-ref types additionally drive graphWrite.
        bool unitIsGraph = false;
        for (auto& decl : *u->codeDeclarationList) {
            auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get());
            if (!cd) continue;
            bool s = false, d = false; generateDirections(cd, s, d);
            if ((s || d) && cdHasGraphRef(cd)) { unitIsGraph = true; break; }
        }
        std::vector<std::string> genEnumSrc;   // generated enum retro-impl sources, appended after the loop
        for (auto& decl : *u->codeDeclarationList) {
            if (auto* ed = dynamic_cast<EnumDeclarationNode*>(decl.get())) {
                bool eser = false, ede = false;
                enumDirections(ed, eser, ede);
                if (eser) { std::string s = buildEnumSerialize(ed);   if (!s.empty()) genEnumSrc.push_back(s); }
                if (ede)  { std::string s = buildEnumDeserialize(ed); if (!s.empty()) genEnumSrc.push_back(s); }
                continue;
            }
            auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get());
            if (!cd || !cd->name || !cd->name->value) continue;
            bool ser = false, de = false;
            generateDirections(cd, ser, de);
            // Serialize is used dynamically (behind the `Serialize` interface, in `toString`), so it's MERGED
            // into the type as a normal conformance (fat-pointer vtable).
            if (ser && unitIsGraph) {
                // In a graph unit every `@generate(Serialize)` type is a GraphNode (a potential pointee).
                // `__serializeInto` writes its fields (pointers as interned ids); `__typeName` tags its entry.
                // A graph-ref type's `serialize` drives graphWrite (root inline + drained id table); a plain
                // pointee keeps the ordinary object `serialize` for standalone `encode`. Resolves via the
                // injected std::serialization::graph import.
                std::string into = buildGraphSerializeInto(cd);
                bool generic = cd->typeParams && !cd->typeParams->empty();
                if (!into.empty() && !generic) {
                    std::string serMethod;
                    if (cdHasGraphRef(cd)) {
                        // `cast<usize>(this)` is the object's own base address (== a `Shared`/`Weak` handle's
                        // `__ptrId` to it), so a back-edge to the root dedups to the root id. (`addr(of: this.f)`
                        // can't be used — a `Shared` field auto-derefs to its pointee's address.)
                        serMethod =
                            "    public fn void serialize(ref Serializer w) {\n"
                            "        uint64 __rp = 0ui64;\n"
                            "        unsafe { __rp = cast<uint64>(cast<usize>(this)); }\n"
                            "        graphWrite(w: ref w, root: this, rootPid: __rp);\n"
                            "    }\n";
                    } else {
                        serMethod =
                            "    public fn void serialize(ref Serializer w) {\n"
                            "        w.beginObject();\n" + buildSerializeBody(cd) +
                            "        w.endObject();\n"
                            "    }\n";
                    }
                    mergeGeneratedInto(cd,
                        "type value __KamaGenSer implements Serialize, GraphNode {\n" + serMethod +
                        "    public fn void __serializeInto(ref Serializer w, ref SerContext ctx) {\n" + into +
                        "    }\n"
                        "    public fn string __typeName() { return \"" + *cd->name->value + "\"; }\n"
                        "}\n");
                }
            }
            // A NON-graph `@generate(Serialize)` struct's `serialize` is now emitted directly in C by the
            // compiler intrinsic (CEmitter::emitSerializeDefinition), which fires when the type has no
            // `serialize` method — so the driver synthesizes ONLY the graph serialize (above). Enums keep
            // their driver-side retro-impl synthesis; collections are hand-written. (Phase C.)
            // Deserialize is a marker (method-less) contract, used only statically (`T::deserialize` via the
            // bound in `tryParse`). Merge the static `deserialize` factory + the marker into the type: the
            // empty marker means the vtable has no method slots (no `This` to resolve), and a merged static
            // member keeps its `static` (a retro impl would drop it).
            // In a graph unit a `@generate(Deserialize)` type's `deserialize` IS the graph driver (returns
            // `Shared<N>`, generated below), so suppress the ordinary object `deserialize` (returns `N`) here —
            // otherwise `Shared<N>::deserialize` (shared.kama) would delegate to a `N::deserialize` of the wrong
            // return type. A graph is decoded as `decode::<Shared<N>>`, never `decode::<N>`.
            // A NON-graph `@generate(Deserialize)` struct's static `deserialize` factory is likewise emitted
            // directly in C (CEmitter::emitDeserializeDefinition), gated on the type having no `deserialize`
            // method. The driver keeps the GRAPH deserialize (the two-pass driver below) + enum retro-impls.
        }
        // Append the generated enum retro-impls (`implements Serialize/Deserialize for E` + the `__kamaSer_E`
        // helper) to the unit AFTER iterating (mutating the list mid-loop would invalidate the iterator). The
        // parsed unit is kept alive in g_generatedUnits so its CodeGenContext outlives emission.
        for (auto& src : genEnumSrc) {
            SharedCompilationUnit pu = parseString(src.c_str(), "<generated-enum-serde>");
            if (!pu || !pu->codeDeclarationList) continue;
            g_generatedUnits.push_back(pu);
            for (auto& d : *pu->codeDeclarationList) u->codeDeclarationList->push_back(d);
        }

        // Graph DESERIALIZE: in a graph unit, every `@generate(Deserialize)` class that can be a graph node
        // (its scalars are shellable) gets `__gShell`; graph-ref types additionally get `__gWire` + the
        // `Root::deserialize` two-pass driver. A free `__glookup_P` is emitted per distinct pointee type. All
        // `Shared<K>` shells route through the inline conditional `Shared<T>: Deserialize` in shared.kama, so
        // `decode::<Shared<Root>>` resolves. See the graph codegen helpers above.
        if (unitIsGraph) {
            std::vector<ClassDeclarationNode*> graphTypes;
            for (auto& decl : *u->codeDeclarationList) {
                auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get());
                if (!cd || !cd->name || !cd->name->value) continue;
                bool s = false, d = false; generateDirections(cd, s, d);
                if (!d) continue;
                if (cd->typeParams && !cd->typeParams->empty()) continue;   // generic graph type: deferred
                if (buildGraphShell(cd).empty()) continue;                  // a field we can't shell yet
                graphTypes.push_back(cd);
            }
            for (auto* cd : graphTypes) {
                mergeGeneratedInto(cd,
                    "type value __KamaGenGShell {\n"
                    "    public static fn Shared<" + *cd->name->value + "> __gShell(Deserializer r) {\n"
                    + buildGraphShell(cd) +
                    "    }\n"
                    "}\n");
                injectHeapShell(cd, "__gShell", "__sh");
                if (cdHasGraphRef(cd)) {
                    std::vector<std::string> pts; std::string wbody = buildGraphWire(cd, pts);
                    std::string params;
                    for (auto& P : pts)
                        params += ", ref List<uint64> __ids_" + P + ", ref List<Shared<" + P + ">> __objs_" + P;
                    mergeGeneratedInto(cd,
                        "type value __KamaGenGWire {\n"
                        "    public fn void __gWire(Deserializer r" + params + ") {\n"
                        + wbody +
                        "    }\n"
                        "}\n");
                }
            }
            // Free lookups for EVERY graph type (a pointee is looked up in `__gWire`; a root type is looked up
            // in pass 2 to fetch the shell to wire and to return the root), then the driver merged into each
            // type (with `implements Deserialize` so `decode::<Shared<N>>` resolves).
            std::string lookups;
            for (auto* cd : graphTypes) lookups += buildGraphLookup(*cd->name->value);
            if (!lookups.empty()) {
                SharedCompilationUnit pu = parseString(lookups.c_str(), "<generated-graph-lookup>");
                if (pu && pu->codeDeclarationList) {
                    g_generatedUnits.push_back(pu);
                    for (auto& d : *pu->codeDeclarationList) u->codeDeclarationList->push_back(d);
                }
            }
            // Every graph type gets the driver as its `deserialize` (returning `Shared<N>`), so any can be a
            // graph ROOT — `decode::<Shared<N>>` for a leaf node is a one-object graph. `implements Deserialize`
            // (marker) is what lets `Shared<N>::deserialize` resolve `N::deserialize`.
            for (auto* cd : graphTypes) {
                mergeGeneratedInto(cd,
                    "type value __KamaGenGDe implements Deserialize {\n"
                    "    public static fn Shared<" + *cd->name->value + "> deserialize(Deserializer r) {\n"
                    + buildGraphDriver(cd, graphTypes) +
                    "    }\n"
                    "}\n");
                injectHeapShell(cd, "deserialize", "__fb");
            }
        }
    }
}

// Parse the CLI inputs, then transitively resolve + parse imported modules. Dedup by
// absolute path so cycles load exactly once. `std`/`core` are reserved roots (stdlib only);
// other modules search the importing file's dir, then $KAMA_PATH, then the stdlib. Returns
// false on any parse/resolution failure. `units`/`paths` come back parallel, in load order.
// True iff a unit declares a `@generate` type with a (non-`@skip`) `Shared`/`Weak` field — i.e. an object
// graph. Such a unit needs the `std::serialization::graph` runtime, whose import the loader injects below.
static bool unitHasGraphType(SharedCompilationUnit u)
{
    if (!u || !u->codeDeclarationList) return false;
    for (auto& decl : *u->codeDeclarationList) {
        auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get());
        if (!cd || !cd->members) continue;
        bool ser = false, de = false; generateDirections(cd, ser, de);
        if (!ser && !de) continue;
        for (auto& m : *cd->members) {
            auto* fd = dynamic_cast<ClassFieldDeclarationNode*>(m.get());
            if (!fd || !fd->type) continue;
            bool skip = false;
            if (fd->attributes) for (auto& at : *fd->attributes) if (at && at->name && *at->name == "skip") skip = true;
            if (!skip && typeIsGraphRef(fd->type)) return true;
        }
    }
    return false;
}

// Splice `import std::serialization::graph::{…}` into a graph-using unit (unless already present) so the graph
// runtime module loads via the normal import BFS and its symbols resolve in the unit's generated `serialize`.
static void injectGraphImport(SharedCompilationUnit u)
{
    if (!u) return;
    if (!u->importDeclarationList) u->importDeclarationList = std::make_shared<ImportDeclarationList>();
    // True iff the unit already imports the module `a::b[::c]` (so we don't inject a duplicate).
    auto hasImport = [&](std::vector<std::string> path) {
        for (auto& imp : *u->importDeclarationList)
            if (imp && imp->modulePath && imp->modulePath->size() == path.size()) {
                bool eq = true;
                for (size_t i = 0; i < path.size(); i++) if (*(*imp->modulePath)[i] != path[i]) { eq = false; break; }
                if (eq) return true;
            }
        return false;
    };
    auto inject = [&](const char* src) {
        SharedCompilationUnit iu = parseString(src, "<graph-import>");
        if (iu && iu->importDeclarationList && !iu->importDeclarationList->empty())
            u->importDeclarationList->push_back((*iu->importDeclarationList)[0]);
    };
    // The write side needs the graph runtime; the (read-side) generated deserialize driver builds its id
    // registries out of `List`, so a graph unit also needs `std::collections::List` nameable in its own scope.
    if (!hasImport({"std", "serialization", "graph"}))
        inject("import std::serialization::graph::{GraphNode, SerContext, graphWrite};\nfn int32 __gimp() { return 0; }\n");
    if (!hasImport({"std", "collections"}))
        inject("import std::collections::{List};\nfn int32 __gimp2() { return 0; }\n");
}

bool loadProgramUnits(const std::vector<std::string>& cliInputs, const char* argv0,
                      std::vector<SharedCompilationUnit>& units,
                      std::vector<std::string>& paths)
{
    std::string stdlibDir = resolveStdlibDir(argv0);
    std::vector<std::string> extraRoots = splitSearchPath(getenv("KAMA_PATH"));
    // A unit's namespace as an `a::b` key (empty for a private/no-namespace file).
    auto nsKey = [](SharedCompilationUnit u) -> std::string {
        if (!u || !u->nameSpace || !u->nameSpace->name) return "";
        std::string s; auto id = u->nameSpace->name;
        if (id->qualifier) for (auto& seg : *id->qualifier) s += *seg + "::";
        if (id->value) s += *id->value;
        return s;
    };
    std::set<std::string> seen;      // resolved absolute paths already parsed
    std::set<std::string> provided;  // namespaces already in the compilation (satisfy an import w/o disk lookup)
    // The smart-pointer triad is now a built-in module (embedded, always in scope — see preludeModuleUnits),
    // so an explicit `import std::memory` is a satisfied no-op: skip the disk lookup rather than re-parse it
    // (which would double-define the triad, and would fail outright in a `--no-std` install with no lib/).
    provided.insert("std::memory");
    for (auto& in : cliInputs) {
        std::string abs = absolutePath(in);
        if (!seen.insert(abs).second) continue;
        SharedCompilationUnit u = parseFile(in);
        if (!u) return false;
        units.push_back(u); paths.push_back(abs);
        std::string k = nsKey(u); if (!k.empty()) provided.insert(k);
    }
    for (size_t i = 0; i < units.size(); ++i) {          // grows as imports are discovered (BFS)
        // A graph-using unit needs the graph runtime; inject its import here so this same BFS pass loads
        // std::serialization::graph (and its transitive std::collections/std::memory deps).
        if (unitHasGraphType(units[i])) injectGraphImport(units[i]);
        if (!units[i]->importDeclarationList) continue;
        std::string here = dirName(paths[i]);
        for (auto& imp : *units[i]->importDeclarationList) {
            std::vector<std::string> segs;
            if (imp->modulePath) for (auto& s : *imp->modulePath) segs.push_back(*s);
            if (segs.empty()) continue;
            std::string key;                                  // "a::b" — match against loaded namespaces
            for (size_t k = 0; k < segs.size(); ++k) key += (k ? "::" : "") + segs[k];
            if (provided.count(key)) continue;                // already in the compilation (CLI input / earlier import)
            bool reserved = (segs[0] == "std" || segs[0] == "core");
            std::vector<std::string> roots;
            if (!reserved) { roots.push_back(here); for (auto& r : extraRoots) roots.push_back(r); }
            roots.push_back(stdlibDir);
            auto files = resolveModuleFiles(segs, roots);
            if (files.empty()) {
                std::string name;
                for (size_t k = 0; k < segs.size(); ++k) name += (k ? "::" : "") + segs[k];
                fprintf(stderr, "kama: error: cannot resolve module '%s' (from %s)\n",
                        name.c_str(), reserved ? stdlibDir.c_str() : here.c_str());
                return false;
            }
            for (auto& f : files) {
                std::string abs = absolutePath(f);
                if (!seen.insert(abs).second) continue;
                SharedCompilationUnit mu = parseFile(f);
                if (!mu) return false;
                units.push_back(mu); paths.push_back(abs);
                std::string mk = nsKey(mu); if (!mk.empty()) provided.insert(mk);
            }
        }
    }
    // Inject synthesized `implements Serialize/Deserialize` blocks for every `@generate`d type before the
    // program is handed to the emitter (they are collected + emitted like ordinary declarations).
    synthesizeSerialization(units);
    return true;
}

// Parse one kama file into a CompilationUnit. Returns nullptr on failure.
SharedCompilationUnit parseFile(const std::string& inputFile)
{
    yyscan_t scanner;
    struct LexerInstanceData extra = {
        KAMA_LEXERINSTANCE_DEFAULT_LINE_ONE,
        KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE,
        nullptr,
        std::make_shared<CodeGenContext>(std::make_shared<std::string>(inputFile)),
        nullptr
    };

    yylex_init_extra(&extra, &scanner);

    FILE* input = fopen(inputFile.c_str(), "r");
    if (!input) {
        fprintf(stderr, "kama: error: cannot open input file '%s'\n", inputFile.c_str());
        yylex_destroy(scanner);
        return nullptr;
    }
    yy_switch_to_buffer(yy_create_buffer(input, YY_BUF_SIZE, scanner), scanner);

    int rc = yyparse(scanner);
    yylex_destroy(scanner);
    fclose(input);

    if (rc != 0 || extra.codeGenContext->errorCount() > 0)
        return nullptr;
    return extra.compilationUnit;
}

// The implicit prelude (Optional/Result, the language-level contracts, the primitive retro-impls,
// Chars/Split) is embedded from prelude/global.kama into KAMA_PRELUDE_SRC (see kama.prelude.h and
// tools/embed_prelude.sh), so it ships inside bin/kama even for a --no-std install. The namespaced
// built-in triad (prelude/std/memory/*.kama) is embedded as KAMA_PRELUDE_MODULES.

// Parse an in-memory kama source string into a CompilationUnit (flex string buffer). nullptr on error.
SharedCompilationUnit parseString(const char* src, const std::string& name)
{
    yyscan_t scanner;
    struct LexerInstanceData extra = {
        KAMA_LEXERINSTANCE_DEFAULT_LINE_ONE,
        KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE,
        nullptr,
        std::make_shared<CodeGenContext>(std::make_shared<std::string>(name)),
        nullptr
    };
    yylex_init_extra(&extra, &scanner);
    yy_scan_string(src, scanner);
    int rc = yyparse(scanner);
    yylex_destroy(scanner);
    if (rc != 0 || extra.codeGenContext->errorCount() > 0) return nullptr;
    return extra.compilationUnit;
}

SharedCompilationUnit preludeUnit() { return parseString(KAMA_PRELUDE_SRC, "<prelude>"); }

// The namespaced built-in modules (the smart-pointer triad, std::memory) — embedded like the prelude
// so they're always in scope with no `import std::memory`, in both the full and `--no-std` installs.
// Each keeps its own `namespace`/`export`; the emitter collects them under that scope + an implicit
// `using` (see CEmitter::collectProgram / ctxOf). nullptr units (a parse failure) are dropped.
std::vector<SharedCompilationUnit> preludeModuleUnits()
{
    std::vector<SharedCompilationUnit> units;
    for (int i = 0; i < KAMA_PRELUDE_MODULE_COUNT; ++i) {
        SharedCompilationUnit u = parseString(KAMA_PRELUDE_MODULES[i].src, "<prelude-module>");
        if (u) units.push_back(u);
    }
    return units;
}

// Emit an already-parsed unit to a single `.c` (`srcPath` drives #line). Returns 0 on success.
int transpileUnitToFile(SharedCompilationUnit unit, const std::string& srcPath,
                        const std::string& outPath, bool emitLines, bool* externsMathH = nullptr,
                        bool* externsNetWeb = nullptr, bool* externsApp = nullptr)
{
    std::ofstream out(outPath);
    if (!out) {
        fprintf(stderr, "kama: error: cannot write '%s'\n", outPath.c_str());
        return 1;
    }
    CEmitter emitter(out, srcPath, emitLines);
    emitter.setPrelude(preludeUnit());   // Optional/Result available implicitly
    for (auto& m : preludeModuleUnits()) emitter.addPreludeModule(m);   // the always-in-scope triad
    int unsupported = emitter.emit(unit);
    if (externsMathH) *externsMathH = emitter.externsHeader("<math.h>");   // -> the driver appends -lm
    if (externsNetWeb) *externsNetWeb = emitter.externsHeader("kama_net_web.h");   // -> wasm --js-library
    if (externsApp) *externsApp = emitter.externsHeader("kama_app.h");   // std::app -> wasm -sEXIT_RUNTIME=1
    out.close();
    if (unsupported > 0) {
        fprintf(stderr, "kama: %d unlowered construct(s) — see the warnings above.\n", unsupported);
        return 1;   // a construct kama couldn't lower (incl. a safety-gate violation) is a hard error
    }
    return 0;
}

// Transpile `inputFile` to C, writing to `outPath`. Returns 0 on success.
int transpileToFile(const std::string& inputFile, const std::string& outPath, bool emitLines)
{
    SharedCompilationUnit unit = parseFile(inputFile);
    if (!unit) return 1;
    return transpileUnitToFile(unit, absolutePath(inputFile), outPath, emitLines);
}

// Emit a multi-file program from already-parsed `units` (parallel to `sourcePaths`): one
// shared header (`headerPath`, included as `headerName`) + one `.c` per unit (`cPaths`).
// Returns 0 on success.
int emitProgramUnits(const std::vector<SharedCompilationUnit>& units,
                     const std::vector<std::string>& sourcePaths,
                     const std::string& headerPath, const std::string& headerName,
                     const std::vector<std::string>& cPaths, bool emitLines,
                     bool* externsMathH = nullptr,   // link hint: did the program `extern "<math.h>";`?
                     bool* externsNetWeb = nullptr,  // link hint: did it `extern "kama_net_web.h";`?
                     bool* externsApp = nullptr)     // link hint: did it `extern "kama_app.h";`? (std::app)
{
    std::ofstream header(headerPath);
    if (!header) { fprintf(stderr, "kama: error: cannot write '%s'\n", headerPath.c_str()); return 1; }

    std::vector<std::unique_ptr<std::ofstream>> moduleFiles;
    std::vector<std::ostream*> moduleStreams;
    for (auto& cp : cPaths) {
        auto f = std::unique_ptr<std::ofstream>(new std::ofstream(cp));
        if (!*f) { fprintf(stderr, "kama: error: cannot write '%s'\n", cp.c_str()); return 1; }
        moduleStreams.push_back(f.get());
        moduleFiles.push_back(std::move(f));
    }

    CEmitter emitter(header, "", emitLines);
    emitter.setPrelude(preludeUnit());   // Optional/Result available implicitly
    for (auto& m : preludeModuleUnits()) emitter.addPreludeModule(m);   // the always-in-scope triad
    int unsupported = emitter.emitProgram(units, headerName, header, moduleStreams, sourcePaths);
    if (externsMathH) *externsMathH = emitter.externsHeader("<math.h>");   // -> the driver appends -lm
    if (externsNetWeb) *externsNetWeb = emitter.externsHeader("kama_net_web.h");   // -> wasm --js-library
    if (externsApp) *externsApp = emitter.externsHeader("kama_app.h");   // std::app -> wasm -sEXIT_RUNTIME=1
    header.close();
    for (auto& f : moduleFiles) f->close();

    if (unsupported > 0) {
        fprintf(stderr, "kama: %d unlowered construct(s) — see the warnings above.\n", unsupported);
        return 1;
    }
    return 0;
}

// Transpile a multi-unit program (a file plus every module it `import`s) into ONE self-contained .c.
// Emits the normal shared-header + per-unit layout to temp siblings, then folds it into a single file:
// the header verbatim (its include guard + runtime includes stay), then each unit's body with its
// `#include "<name>.gen.h"` line dropped (the header is already inlined). Keeps `transpile`'s one-file
// contract while supporting the module system (a single translation unit for downstream tools).
int transpileProgramToSingleFile(const std::vector<SharedCompilationUnit>& units,
                                 const std::vector<std::string>& unitPaths,
                                 const std::string& outPath, bool emitLines)
{
    std::string dir = dirName(outPath);
    std::string stem = stripExtension(baseName(outPath));
    std::string headerName = stem + ".gen.h";
    std::string headerPath = dir + "/" + headerName;
    std::vector<std::string> cPaths;
    for (size_t i = 0; i < units.size(); ++i)
        cPaths.push_back(dir + "/" + stem + "__u" + std::to_string(i) + ".c.tmp");
    if (emitProgramUnits(units, unitPaths, headerPath, headerName, cPaths, emitLines) != 0) return 1;

    std::ofstream out(outPath);
    if (!out) { fprintf(stderr, "kama: error: cannot write '%s'\n", outPath.c_str()); return 1; }
    { std::ifstream h(headerPath); out << h.rdbuf(); }
    out << "\n";
    std::string incLine = "#include \"" + headerName + "\"";
    for (auto& cp : cPaths) {
        std::ifstream u(cp);
        std::string line;
        while (std::getline(u, line)) {
            if (line.find(incLine) != std::string::npos) continue;   // header already inlined above
            out << line << "\n";
        }
    }
    out.close();
    remove(headerPath.c_str());
    for (auto& cp : cPaths) remove(cp.c_str());
    return 0;
}

int runCmd(const std::string& cmd)
{
    int rc = system(cmd.c_str());
    if (rc == -1) return 1;
#if defined(_WIN32)
    return rc;                 // Windows: system() returns the child's exit code directly
#else
    return WEXITSTATUS(rc);    // POSIX: extract it from the wait status (<sys/wait.h>)
#endif
}

// `kama update [--version vX.Y.Z]` — self-update by re-running the canonical installer,
// which re-detects a C compiler (slim vs bundled zig) so the install flavor stays consistent.
int cmdUpdate(const std::string& pinned)
{
#ifdef _WIN32
    std::string env = pinned.empty() ? "" : "$env:KAMA_VERSION='" + pinned + "'; ";
    std::string cmd = "powershell -NoProfile -Command \"" + env +
                      "irm https://kama-lang.org/install.ps1 | iex\"";
#else
    std::string env = pinned.empty() ? "" : "KAMA_VERSION=" + pinned + " ";
    std::string cmd = env + "curl -fsSL https://kama-lang.org/install.sh | sh";
#endif
    return runCmd(cmd);   // the installer prints old->new; verify with `kama --version`
}

void usage()
{
    fprintf(stderr,
        "usage:\n"
        "  kama transpile <in.kama> [-o out.c] [--no-line]\n"
        "  kama build     <in.kama>... [-o out] [--target native|wasm] [--release|--debug]\n"
        "                             [--link <lib>]... [--webgpu] [--cc <compiler>] [--no-line] [--keep-c]\n"
        "                  (pass multiple .kama files to build a multi-file program)\n"
        "  kama update    [--version vX.Y.Z]   self-update via the installer\n"
        "  kama --version\n");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("kama %s\n", KAMA_VERSION);
        return 0;
    }
    if (argc < 2) { usage(); return 2; }

    std::string subcommand = argv[1];

    if (subcommand == "update") {
        std::string pinned;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--version" && i + 1 < argc) pinned = argv[++i];
            else { fprintf(stderr, "kama update: unexpected arg '%s'\n", a.c_str()); return 2; }
        }
        return cmdUpdate(pinned);
    }

    std::vector<std::string> inputs;      // one or more .kama source files
    std::string output;
    std::string cc;                       // empty => pick default per target
    std::string target     = "native";    // native | wasm
    std::vector<std::string> links;        // -l libraries (FFI)
    bool        emitLines  = true;
    bool        keepC      = false;
    bool        webgpu     = false;
    bool        release    = false;        // debug by default

    // Options may appear in any order, before or after the input file.
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc)            output = argv[++i];
        else if (a == "--cc" && i + 1 < argc)     cc = argv[++i];
        else if (a == "--link" && i + 1 < argc)   links.push_back(argv[++i]);
        else if (a == "--target" && i + 1 < argc) target = argv[++i];
        else if (a == "--no-line")                emitLines = false;
        else if (a == "--keep-c")                 keepC = true;
        else if (a == "--webgpu")                 webgpu = true;
        else if (a == "--release")                release = true;
        else if (a == "--debug")                  release = false;
        else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "kama: unknown option '%s'\n", a.c_str()); usage(); return 2;
        }
        else                                      inputs.push_back(a);
    }

    if (inputs.empty()) { fprintf(stderr, "kama: no input file\n"); usage(); return 2; }
    const std::string& input = inputs[0];   // first input drives default output naming

    if (target != "native" && target != "wasm") {
        fprintf(stderr, "kama: unknown --target '%s' (expected native|wasm)\n", target.c_str());
        return 2;
    }
    const bool wasm = (target == "wasm");

    // Release builds strip debug info and #line, optimize, and define NDEBUG.
    if (release) emitLines = false;

    // Where kama_runtime.h lives — resolved so an installed binary works from any
    // cwd ($KAMA_HOME, else <exe>/../include, else <exe>, else ".").
    std::string runtimeDir = resolveRuntimeDir(argv[0]);

    if (subcommand == "transpile") {
        // Transpile to ONE .c. A file with no imports stays on the single-unit fast path; anything that
        // `import`s modules pulls in every transitive unit (loadProgramUnits, as `build` does) and folds
        // them into one self-contained translation unit.
        std::string outPath = output.empty() ? (stripExtension(input) + ".c") : output;
        std::vector<SharedCompilationUnit> units;
        std::vector<std::string> unitPaths;
        if (!loadProgramUnits(inputs, argv[0], units, unitPaths)) return 1;
        int rc = (units.size() == 1)
               ? transpileUnitToFile(units[0], unitPaths[0], outPath, emitLines)
               : transpileProgramToSingleFile(units, unitPaths, outPath, emitLines);
        if (rc == 0) fprintf(stderr, "kama: wrote %s\n", outPath.c_str());
        return rc;
    }

    if (subcommand == "build") {
        // Compiler: native uses a bundled `zig cc` if present else system clang; wasm uses
        // emcc (emcc keys output format off the -o extension). --cc / $EMCC override.
        std::string compiler = cc;
        if (compiler.empty()) {
            if (wasm) {
                const char* env = getenv("EMCC");
                compiler = env ? env : "emcc";
            } else {
                compiler = resolveCCompiler(argv[0]);   // bundled zig cc, else system clang
            }
        }

        // Default output: native -> bare exe name; wasm -> an HTML harness
        // (emcc also emits the .js + .wasm alongside it).
        std::string defaultOut = wasm ? (stripExtension(input) + ".html") : stripExtension(input);
        std::string outPath    = output.empty() ? defaultOut : output;

        // Transpile to one or more .c (multi-file emits a shared header too).
        // Generated files land in the output directory; cleaned unless --keep-c.
        std::string genDir = dirName(outPath);
        std::vector<std::string> cFiles;     // .c to compile
        std::vector<std::string> genFiles;   // generated files to remove afterwards
        std::string headerDir;

        // Parse the CLI inputs and transitively pull in every imported module. A single
        // file with no imports stays on the fast path (one .c, no shared header); anything
        // that drags in more units (multiple inputs, or `import`s) uses the multi-file path.
        std::vector<SharedCompilationUnit> units;
        std::vector<std::string> unitPaths;
        if (!loadProgramUnits(inputs, argv[0], units, unitPaths)) return 1;

        bool needsLibm = false;   // set if the program `extern "<math.h>";`'s (std::math / libm) -> link -lm
        bool needsNetWeb = false; // set if the program `extern "kama_net_web.h";`'s (std::net::web) -> --js-library
        bool needsApp = false;    // set if the program `extern "kama_app.h";`'s (std::app) -> wasm -sEXIT_RUNTIME=1
        if (units.size() == 1) {
            std::string cPath = stripExtension(input) + ".c";
            if (transpileUnitToFile(units[0], unitPaths[0], cPath, emitLines, &needsLibm, &needsNetWeb, &needsApp) != 0) return 1;
            cFiles.push_back(cPath);
            genFiles.push_back(cPath);
        } else {
            std::string headerName = baseName(stripExtension(outPath)) + ".gen.h";
            std::string headerPath = genDir + "/" + headerName;
            headerDir = genDir;
            std::vector<std::string> cPaths;   // one per unit; index-suffixed so distinct dirs never collide
            for (size_t i = 0; i < units.size(); ++i)
                cPaths.push_back(genDir + "/" + stripExtension(baseName(unitPaths[i])) + "_" + std::to_string(i) + ".c");
            if (emitProgramUnits(units, unitPaths, headerPath, headerName, cPaths, emitLines, &needsLibm, &needsNetWeb, &needsApp) != 0) return 1;
            cFiles   = cPaths;
            genFiles = cPaths;
            genFiles.push_back(headerPath);
        }

        std::ostringstream cmd;
        // Promote two silent-UB classes to hard errors (the front end has no return-path
        // / definite-assignment analysis yet): a non-void function that falls off the end,
        // and a read of an uninitialized local. The #line directives map these back to the
        // .kama source. (Audit Step 2 — "no silent surprises".)
        cmd << compiler << " -std=c11 -Werror=return-type -Werror=uninitialized ";
        if (release) {
            // Optimized, no debug info, asserts off. Native uses -O3 (max speed — matches Rust's release
            // default); wasm uses -Oz (size — download cost dominates). -ffunction/data-sections +
            // --gc-sections let the linker drop unused (std)library code — the
            // "pay for what you use" pruning lever. Native also strips symbols.
            cmd << (wasm ? "-Oz " : "-O3 ") << "-DNDEBUG -ffunction-sections -fdata-sections ";
            if (!wasm) {
#ifdef __APPLE__
                cmd << "-Wl,-dead_strip ";
#else
                cmd << "-Wl,--gc-sections ";
#endif
                cmd << "-s ";
            }
        } else {
            // Debug: faithful stepping + breakpoints in .kama via #line (DWARF `-g`). We intentionally do
            // NOT pass `-gsource-map` on wasm: it makes the generated JS load a `.wasm.map` at startup via a
            // chain of runtime symbols (`addRunDependency`, `UTF8ArrayToString`, …) that newer emcc (≥6) does
            // not include by default when the `.js` is run under node — only a browser provides them — which
            // crashes bare-node execution (the test harness + CI). The DWARF info still supports in-browser
            // debugging; browser-devtools .kama source-mapping is a deferred nicety if it's ever wanted back.
            cmd << "-g -O0 ";
        }
        // Numeric safety — no arithmetic UB (Rust's model). Divide-by-zero, shift-past-width, and
        // out-of-range float->int all TRAP in EVERY build (they're always bugs). Signed overflow TRAPS in
        // debug (catch the accidental-overflow bug in dev) and WRAPS (defined two's-complement, -fwrapv,
        // zero-cost) in release. `-fsanitize-trap` lowers to `__builtin_trap` — a clean abort with NO
        // sanitizer-runtime dependency. `shift-exponent` only (not `shift-base`), so `1 << 31` (setting the
        // sign bit) stays legal. Intentional signed wrap is opt-in (unsigned math, or a `wrapping*` helper).
        cmd << "-fsanitize=integer-divide-by-zero,shift-exponent,float-cast-overflow "
               "-fsanitize-trap=integer-divide-by-zero,shift-exponent,float-cast-overflow ";
        // Signed overflow: debug TRAPS it; release `-fwrapv`-WRAPS `+`/`-`/`*` (defined). `INT_MIN / -1` is
        // NOT defined by `-fwrapv`, so we ALSO keep the overflow-trap in release — it wraps the ordinary
        // ops (trap suppressed by `-fwrapv`) but still traps that one pathological division. Net: no
        // arithmetic UB in either build.
        cmd << "-fsanitize=signed-integer-overflow -fsanitize-trap=signed-integer-overflow ";
        if (release) cmd << "-fwrapv ";
        cmd << "-I" << runtimeDir << " -I" << dirName(absolutePath(input)) << " -I. ";
        if (!headerDir.empty()) cmd << "-I" << headerDir << " ";   // the shared generated header
        if (wasm && webgpu) cmd << "--use-port=emdawnwebgpu ";   // emscripten WebGPU port
        // std::net::web (WebSocket / WebTransport) is a thin JS-glue --js-library, linked only when the
        // program actually externs its header. Both the header (compile) and the glue (link) live next to the
        // module in the stdlib. The glue moves bytes across the wasm/JS boundary via the exported heap views.
        if (needsNetWeb) {
            std::string webdir = resolveStdlibDir(argv[0]) + "/std/net/web";
            cmd << "-I\"" << webdir << "\" ";                          // find kama_net_web.h at compile
            if (wasm)
                cmd << "--js-library \"" << webdir << "/kama_net_web.js\" "
                    << "-sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8 ";
        }
        // std::app's run loop keeps the wasm runtime alive (emscripten_set_main_loop); EXIT_RUNTIME lets
        // its quit() (emscripten_force_exit) shut down cleanly with a real exit code.
        if (wasm && needsApp) cmd << "-sEXIT_RUNTIME=1 ";
        for (auto& cf : cFiles) cmd << "\"" << cf << "\" ";
        for (auto& lib : links) cmd << "-l" << lib << " ";       // FFI link flags
        // Pay-for-what-you-use: link libm only when the program pulls in <math.h> (std::math or any libm
        // FFI). Native only — wasm/emscripten bundles libm. (--gc-sections still prunes unused code.)
        if (needsLibm && !wasm) cmd << "-lm ";
#if defined(_WIN32)
        // std::net uses Winsock (kama_os.h). Link ws2_32 on native Windows builds; harmless (and pruned by
        // --gc-sections) for programs that don't open a socket. POSIX sockets need no extra lib.
        if (!wasm) cmd << "-lws2_32 ";
#endif
        cmd << "-o \"" << outPath << "\"";
        int rc = runCmd(cmd.str());

        if (!keepC) for (auto& gf : genFiles) remove(gf.c_str());
        if (rc != 0) {
            fprintf(stderr, "kama: %s failed (exit %d)\n", compiler.c_str(), rc);
            return rc;
        }
        fprintf(stderr, "kama: built %s\n", outPath.c_str());
        return 0;
    }

    fprintf(stderr, "kama: unknown subcommand '%s'\n", subcommand.c_str());
    usage();
    return 2;
}
