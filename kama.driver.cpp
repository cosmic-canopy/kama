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
    // Collections -> a JSON array. The element write recurses (primitive or nested Serialize type).
    if (type->value && type->genericArg &&
        (*type->value == "List" || *type->value == "Array" || *type->value == "Fixed")) {
        std::string elemTy   = typeToStr(type->genericArg);
        std::string elemWrite = serializeWriteStmt(type->genericArg, "__e");
        if (elemWrite.empty()) return "";
        return "w.beginArray(count: " + access + ".length());\n"
               "        foreach (" + elemTy + " __e in " + access + ") { " + elemWrite + " }\n"
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

// The default value for a field local before deserialization (also the value for a field absent from the
// JSON). "" => an unsupported field type (nested/collection/Optional deserialize is v1-deferred).
// The initial value for a field local (also the value for a field absent from the JSON): a scalar default,
// or the natural empty for a container (`Optional::None`, empty `List()`). "" => unsupported field type.
static std::string deserializeDefault(SharedIdentifier type)
{
    if (!type) return "";
    if (type->value && *type->value == "Optional" && type->genericArg) return "Optional::None";
    if (type->value && type->genericArg && *type->value == "List")     return "List()";
    switch (type->builtInVal) {
        case IDENTIFIER_INT8_VAL:    return "0i8";
        case IDENTIFIER_INT16_VAL:   return "0i16";
        case IDENTIFIER_INT32_VAL:   return "0";
        case IDENTIFIER_INT64_VAL:   return "0i64";
        case IDENTIFIER_UINT8_VAL:   return "0ui8";
        case IDENTIFIER_UINT16_VAL:  return "0ui16";
        case IDENTIFIER_UINT32_VAL:  return "0ui32";
        case IDENTIFIER_UINT64_VAL:  return "0ui64";
        case IDENTIFIER_BOOL_VAL:    return "false";
        case IDENTIFIER_FLOAT32_VAL: return "0.0f32";
        case IDENTIFIER_FLOAT64_VAL: return "0.0";
        case IDENTIFIER_CHAR_VAL:    return "cast<char>(0ui32)";
        case IDENTIFIER_STRING_VAL:  return "\"\"";
        default: return "";
    }
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

// The statement(s) that read a value of `type` into local `dst`. Scalar => assign; `Optional<E>` => null→None
// else Some(read E); `List<E>` => read a JSON array element-by-element. "" => unsupported (nested element).
static std::string deserializeReadInto(SharedIdentifier type, const std::string& dst)
{
    if (!type) return "";
    if (type->value && *type->value == "Optional" && type->genericArg) {
        std::string re = deserializeReadScalar(type->genericArg);
        if (re.empty()) return "";   // Optional<nested> — deferred
        return "if (r.readNull()) { " + dst + " = Optional::None; } "
               "else { " + dst + " = Optional::Some(value: " + re + "); }";
    }
    if (type->value && type->genericArg && *type->value == "List") {
        std::string re = deserializeReadScalar(type->genericArg);
        if (re.empty()) return "";   // List<nested> — deferred
        return "r.beginArray(); while (r.moreElems()) { " + dst + ".add(item: " + re + "); }";
    }
    std::string rs = deserializeReadScalar(type);
    if (rs.empty()) return "";
    return dst + " = " + rs + ";";
}

// True for a field type the ctor must receive with `give` (it owns heap): string, a collection, or an
// Optional wrapping one.
static bool isOwnedFieldType(SharedIdentifier type)
{
    if (!type) return false;
    if (type->builtInVal == IDENTIFIER_STRING_VAL) return true;
    if (type->value && (*type->value == "List" || *type->value == "Array" || *type->value == "Fixed")) return true;
    if (type->value && *type->value == "Optional" && type->genericArg) return isOwnedFieldType(type->genericArg);
    return false;
}

// The body of the synthesized static `deserialize`: a field local per @field (with a default), a read loop
// that dispatches each JSON key to the matching local, then construction via the type's ctor (named args =
// the @field field names). Returns "" if any @field has an unsupported (non-scalar) type — v1 deserialize
// covers flat scalar/string structs; the caller then skips the Deserialize merge for that type.
static std::string buildDeserializeBody(ClassDeclarationNode* cd)
{
    if (!cd || !cd->name || !cd->name->value || !cd->members) return "";
    if (cd->typeParams && !cd->typeParams->empty()) return "";
    std::string tn = *cd->name->value;
    bool resKind = cd->typeKind && *cd->typeKind == "resource";
    std::string locals, chain, ctorArgs;
    bool first = true; int idx = 0;
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
            std::string fname = *d->name->value, wire = rename.empty() ? fname : rename;
            std::string local = "__f_" + fname, tyStr = typeToStr(fd->type);
            std::string def = deserializeDefault(fd->type), readInto = deserializeReadInto(fd->type, local);
            if (def.empty() || readInto.empty()) return "";        // unsupported field type
            locals += "        " + tyStr + " " + local + " = " + def + ";\n";
            chain  += std::string("            ") + (first ? "if" : "else if")
                    + " (__key == \"" + wire + "\") { " + readInto + " }\n";
            first = false;
            ctorArgs += (idx ? ", " : "") + fname + ": " + (isOwnedFieldType(fd->type) ? "give " : "") + local;
            idx++;
        }
    }
    if (idx == 0) return "";
    return locals
         + "        r.beginObject();\n"
           "        while (r.moreFields()) {\n"
           "            string __key = r.fieldName();\n"
         + chain +
           "            else { r.skipValue(); }\n"
           "        }\n"
           "        if (r.failed()) { return Result::Err(error: DeError::Malformed); }\n"
           "        " + tn + " __result = " + tn + "(" + ctorArgs + ");\n"
           "        return Result::Ok(value: " + std::string(resKind ? "give " : "") + "__result);\n";
}

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

// Scan every unit for `@generate` types and give each the generated `serialize`/`deserialize` method +
// `implements Serialize`/`Deserialize` clause, MERGED INTO THE TYPE ITSELF (a normal conformance with a
// fat-pointer vtable — a retroactive block is static-dispatch-only and couldn't be a contract value).
void synthesizeSerialization(std::vector<SharedCompilationUnit>& units)
{
    for (auto& u : units) {
        if (!u || !u->codeDeclarationList) continue;
        std::vector<SharedStatement> generated;   // top-level retro-impls to append (deserialize)
        for (auto& decl : *u->codeDeclarationList) {
            auto* cd = dynamic_cast<ClassDeclarationNode*>(decl.get());
            if (!cd || !cd->name || !cd->name->value) continue;
            bool ser = false, de = false;
            generateDirections(cd, ser, de);
            // Serialize is used dynamically (behind the `Serialize` interface, in `toString`), so it's MERGED
            // into the type as a normal conformance (fat-pointer vtable).
            if (ser) {
                std::string body = buildSerializeBody(cd);
                if (!(body.empty() && cd->typeParams && !cd->typeParams->empty()))
                    mergeGeneratedInto(cd,
                        "type value __KamaGenSer implements Serialize {\n"
                        "    public fn void serialize(ref Serializer w) {\n"
                        "        w.beginObject();\n" + body +
                        "        w.endObject();\n"
                        "    }\n"
                        "}\n");
            }
            // Deserialize is a marker (method-less) contract, used only statically (`T::deserialize` via the
            // bound in `tryParse`). Merge the static `deserialize` factory + the marker into the type: the
            // empty marker means the vtable has no method slots (no `This` to resolve), and a merged static
            // member keeps its `static` (a retro impl would drop it).
            if (de) {
                std::string body = buildDeserializeBody(cd);
                if (!body.empty())   // skip types with fields deserialize can't yet build (non-scalar)
                    mergeGeneratedInto(cd,
                        "type value __KamaGenDe implements Deserialize {\n"
                        "    public static fn Result<" + *cd->name->value + ", DeError> deserialize(Deserializer r) {\n"
                        + body +
                        "    }\n"
                        "}\n");
            }
        }
        (void)generated;
    }
}

// Parse the CLI inputs, then transitively resolve + parse imported modules. Dedup by
// absolute path so cycles load exactly once. `std`/`core` are reserved roots (stdlib only);
// other modules search the importing file's dir, then $KAMA_PATH, then the stdlib. Returns
// false on any parse/resolution failure. `units`/`paths` come back parallel, in load order.
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
    for (auto& in : cliInputs) {
        std::string abs = absolutePath(in);
        if (!seen.insert(abs).second) continue;
        SharedCompilationUnit u = parseFile(in);
        if (!u) return false;
        units.push_back(u); paths.push_back(abs);
        std::string k = nsKey(u); if (!k.empty()) provided.insert(k);
    }
    for (size_t i = 0; i < units.size(); ++i) {          // grows as imports are discovered (BFS)
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

// The implicit prelude — library sum types available to every program without an import.
// Parsed from source (dogfooding the parser), collected before user code, with an empty (global)
// namespace so `Optional`/`Result` resolve unqualified everywhere (like the builtin collections).
static const char* PRELUDE_SRC =
    "enum Optional<T> { Some(T value), None }\n"
    "enum Result<T, E> { Ok(T value), Err(E error) }\n"
    // The empty value — the payload for a fallible op that succeeds with nothing to return
    // (`Result<Unit, E>`, the analogue of Rust's `Result<(), E>`). Keeps ONE error convention
    // (always Result) whether or not there is a value; no dead/placeholder payload. A single-variant
    // enum (like `None`) so it lives in the prelude without needing value-type ctor emission.
    "enum Unit { Unit }\n"
    // Auto-deref opt-in: a type implementing Deref<T> forwards member access to its pointee `T`
    // (`ptr.method()`/`ptr.field` -> the T). The contract is the gate (explicit, nominal); the smart
    // pointers become ordinary kama types over this instead of compiler intrinsics.
    "type contract Deref<T> for both { fn ref T deref(); }\n"
    // Heap-owner opt-in: a type implementing HeapOwner<T> can be a `new T(args)` target. `new`
    // placement-constructs T on the heap (0 copies) and hands the raw Ptr<T> to `adopt`, which wraps it.
    // `new` stays valid ONLY into such an owner, so it can never leak a bare raw pointer.
    "type contract HeapOwner<T> for resource { static fn This adopt(Ptr<T> raw); }\n"
    // Ownership capability markers (compiler-recognized). `Movable` is implicit on every `resource`
    // (`!Movable` subtracts it → copy-only). `Copyable` = a public `copy()` returning `This`; a resource
    // that implements it is duplicable. Together, `Copyable, !Movable` = shared-ownership (retain on copy).
    "type contract Movable for resource { }\n"
    "type contract Copyable for resource { fn This copy(); }\n"
    // Hashing + equality opt-in (the `Map`/`Set` key protocol, nominal). A generic bound `<K: Hashable +
    // Equatable>` is checked STRUCTURALLY (the type must expose a public `hash()` / `equals(This)`), so
    // `string` satisfies `Equatable` through its built-in `equals` with no impl block; `Hashable` it gets
    // from the pure-kama `implements` below. Both live in the prelude because they are language-level bounds.
    "type contract Hashable for both { fn uint64 hash(); }\n"
    "type contract Equatable for both { fn bool equals(This other); }\n"
    // Iteration opt-in (the `foreach` protocol, nominal). An iterator declares which it provides;
    // `foreach` verifies the declaration and emits DIRECT (monomorphized) calls — no vtable, zero-cost.
    // `Iterator<T>` yields each element BY VALUE (a copy); `IteratorMut<T>` yields a mutable place
    // (`ref T`) so `foreach (ref T x in c)` can write through it (Optional can't carry a place, so the
    // two are parallel — Rust's iter()/iter_mut() split). A container hands one out via a nullary
    // `iterator()` / `iterMut()` factory method.
    "type contract Iterator<T> for both { fn Optional<T> next(); }\n"
    "type contract IteratorMut<T> for both { fn bool hasNext(); fn ref T next(); }\n"
    // Container-side opt-in: a type that `implements Iterable<T>` hands out a by-value iterator via a
    // nullary `iterator()`; `IterableMut<T>` hands out a mutable iterator via `iterMut()`. `foreach`
    // requires the container to declare the matching one (nominal on both sides). A type that is its OWN
    // iterator (implements `Iterator<T>` and is iterated directly) needs no `Iterable`.
    "type contract Iterable<T> for both { fn Iterator<T> iterator(); }\n"
    "type contract IterableMut<T> for both { fn IteratorMut<T> iterMut(); }\n"
    // Serialization capability contracts (the `@generate` protocol). Like the iteration contracts these are
    // language-level bounds, so they live in the prelude: `Serializer` is the pluggable output sink a backend
    // (std::json, …) implements; `Serialize` is what a `@generate(Serialize)` type gets (compiler-synthesized)
    // or a custom serializer hand-writes. Per-width scalar methods let a binary backend pack tight while text
    // backends widen. Declaration-only — zero cost unless a program actually serializes.
    "type contract Serializer for resource {\n"
    "    fn void beginObject(); fn void endObject(); fn void fieldName(string name);\n"
    "    fn void beginArray(usize count); fn void endArray();\n"
    "    fn void writeI8(int8 v); fn void writeI16(int16 v); fn void writeI32(int32 v); fn void writeI64(int64 v);\n"
    "    fn void writeU8(uint8 v); fn void writeU16(uint16 v); fn void writeU32(uint32 v); fn void writeU64(uint64 v);\n"
    "    fn void writeF32(float32 v); fn void writeF64(float64 v);\n"
    "    fn void writeBool(bool v); fn void writeChar(char v); fn void writeString(ref string v); fn void writeNull();\n"
    "}\n"
    "type contract Serialize for both { fn void serialize(ref Serializer w); }\n"
    // Deserialization: `Deserializer` is the pluggable input source (a backend's reader implements it, using
    // a STICKY error flag — a read on malformed input sets `failed()` and returns a default, so the generated
    // `deserialize` needs no per-read branching; `tryParse` checks `failed()` once at the end). `Deserialize`
    // is the per-type capability the codegen synthesizes: a static factory `deserialize(ref Deserializer)`.
    "enum DeError { Malformed, UnexpectedEnd, TypeMismatch, MissingField }\n"
    "type contract Deserializer for resource {\n"
    "    fn void beginObject(); fn bool moreFields(); fn string fieldName();\n"
    "    fn void beginArray(); fn bool moreElems();\n"
    "    fn int8 readI8(); fn int16 readI16(); fn int32 readI32(); fn int64 readI64();\n"
    "    fn uint8 readU8(); fn uint16 readU16(); fn uint32 readU32(); fn uint64 readU64();\n"
    "    fn float32 readF32(); fn float64 readF64();\n"
    "    fn bool readBool(); fn char readChar(); fn string readString();\n"
    "    fn bool readNull(); fn void skipValue(); fn bool failed();\n"
    "}\n"
    // A marker contract (like Movable): the `@generate(Deserialize)` codegen supplies the static factory
    // `deserialize(ref Deserializer) -> Result<This, DeError>` via a retro impl, and `tryParse<T: Deserialize>`
    // calls `T::deserialize(...)` statically. Kept method-less so its signature needn't name `This` in a
    // return position (which the generic-instance machinery can't resolve outside a type).
    "type contract Deserialize for both { }\n"
    // The `.chars()` codepoint iterator over a string's UTF-8 bytes. Decodes one Unicode scalar value
    // per `next()`; the compiler constructs it from a string's bytes (a borrow — valid while the string
    // is). Assumes well-formed UTF-8 (string literals/concat are); a truncated trailing sequence is
    // clamped to the available bytes rather than reading past the end.
    "type value Chars implements Iterator<char> {\n"
    "    Ptr<uint8> data; int32 len; int32 pos;\n"
    "    public fn Optional<char> next() {\n"
    "        if (this.pos >= this.len) { return Optional::None; }\n"
    "        uint32 c0 = 0; unsafe { c0 = cast<uint32>(this.data[this.pos]); }\n"
    "        uint32 cp = c0; int32 n = 1;\n"
    "        if (c0 >= 240ui32) { cp = c0 & 7ui32; n = 4; }\n"
    "        else if (c0 >= 224ui32) { cp = c0 & 15ui32; n = 3; }\n"
    "        else if (c0 >= 192ui32) { cp = c0 & 31ui32; n = 2; }\n"
    "        if (this.pos + n > this.len) { n = this.len - this.pos; }\n"
    "        int32 i = 1;\n"
    "        while (i < n) {\n"
    "            uint32 cc = 0; unsafe { cc = cast<uint32>(this.data[this.pos + i]); }\n"
    "            cp = (cp << 6ui32) | (cc & 63ui32); i = i + 1;\n"
    "        }\n"
    "        this.pos = this.pos + n;\n"
    "        return Optional::Some(value: cast<char>(cp));\n"
    "    }\n"
    "}\n"
    // The `.split(separator:)` iterator over a string's UTF-8 bytes. Like `Chars` it borrows the source
    // bytes (a raw `Ptr<uint8>`, so `Split` stays a POD `value` type) — valid while the source string is —
    // but here it ALSO borrows the separator's bytes, and yields each piece as an OWNED string. The
    // byte-range copy is done by a tiny runtime helper (kama can't allocate a string from raw bytes on its
    // own). Byte semantics (a literal separator), Go `strings.Split` behaviour: consecutive/trailing
    // separators yield "" pieces; an empty separator yields the whole string once.
    "extern fn string kama_string_from_raw(Ptr<uint8> src, int32 start, int32 len);\n"
    "type value Split implements Iterator<string> {\n"
    "    Ptr<uint8> data; int32 len; Ptr<uint8> sep; int32 seplen; int32 pos; bool done;\n"
    "    public fn Optional<string> next() {\n"
    "        if (this.done) { return Optional::None; }\n"
    "        if (this.seplen == 0) {\n"
    "            this.done = true;\n"
    "            return Optional::Some(value: kama_string_from_raw(src: this.data, start: this.pos, len: this.len - this.pos));\n"
    "        }\n"
    "        int32 i = this.pos;\n"
    "        while (i + this.seplen <= this.len) {\n"
    "            bool m = true; int32 k = 0;\n"
    "            while (k < this.seplen) {\n"
    "                uint8 a = 0ui8; uint8 b = 0ui8;\n"
    "                unsafe { a = this.data[i + k]; b = this.sep[k]; }\n"
    "                if (a != b) { m = false; break; }\n"
    "                k = k + 1;\n"
    "            }\n"
    "            if (m) {\n"
    "                int32 st = this.pos;\n"
    "                this.pos = i + this.seplen;\n"
    "                return Optional::Some(value: kama_string_from_raw(src: this.data, start: st, len: i - st));\n"
    "            }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        this.done = true;\n"
    "        return Optional::Some(value: kama_string_from_raw(src: this.data, start: this.pos, len: this.len - this.pos));\n"
    "    }\n"
    "}\n";

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

SharedCompilationUnit preludeUnit() { return parseString(PRELUDE_SRC, "<prelude>"); }

// Emit an already-parsed unit to a single `.c` (`srcPath` drives #line). Returns 0 on success.
int transpileUnitToFile(SharedCompilationUnit unit, const std::string& srcPath,
                        const std::string& outPath, bool emitLines, bool* externsMathH = nullptr)
{
    std::ofstream out(outPath);
    if (!out) {
        fprintf(stderr, "kama: error: cannot write '%s'\n", outPath.c_str());
        return 1;
    }
    CEmitter emitter(out, srcPath, emitLines);
    emitter.setPrelude(preludeUnit());   // Optional/Result available implicitly
    int unsupported = emitter.emit(unit);
    if (externsMathH) *externsMathH = emitter.externsHeader("<math.h>");   // -> the driver appends -lm
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
                     bool* externsMathH = nullptr)   // link hint: did the program `extern "<math.h>";`?
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
    int unsupported = emitter.emitProgram(units, headerName, header, moduleStreams, sourcePaths);
    if (externsMathH) *externsMathH = emitter.externsHeader("<math.h>");   // -> the driver appends -lm
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
        if (units.size() == 1) {
            std::string cPath = stripExtension(input) + ".c";
            if (transpileUnitToFile(units[0], unitPaths[0], cPath, emitLines, &needsLibm) != 0) return 1;
            cFiles.push_back(cPath);
            genFiles.push_back(cPath);
        } else {
            std::string headerName = baseName(stripExtension(outPath)) + ".gen.h";
            std::string headerPath = genDir + "/" + headerName;
            headerDir = genDir;
            std::vector<std::string> cPaths;   // one per unit; index-suffixed so distinct dirs never collide
            for (size_t i = 0; i < units.size(); ++i)
                cPaths.push_back(genDir + "/" + stripExtension(baseName(unitPaths[i])) + "_" + std::to_string(i) + ".c");
            if (emitProgramUnits(units, unitPaths, headerPath, headerName, cPaths, emitLines, &needsLibm) != 0) return 1;
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
            // Debug: faithful stepping + breakpoints in .kama via #line.
            cmd << (wasm ? "-g -gsource-map -O0 " : "-g -O0 ");
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
