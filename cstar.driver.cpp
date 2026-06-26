// cstar driver: parse cstar source and transpile to portable C, optionally
// invoking a C compiler to produce a native executable.
//
//   cstar transpile <in.cstar> [-o out.c] [--no-line]
//   cstar build     <in.cstar> [-o out] [--target native|wasm] [--webgpu]
//                              [--cc <compiler>] [--no-line] [--keep-c]
//
// native builds invoke clang; wasm builds invoke emcc (Emscripten), keying the
// output format off the -o extension (.html harness by default).
// (LLVM is gone; the backend is cstar.cemit.*.)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>

#include <limits.h>
#ifdef _WIN32
  #include <stdlib.h>          // _fullpath, _MAX_PATH
  #ifndef PATH_MAX
    #define PATH_MAX _MAX_PATH
  #endif
#else
  #include <unistd.h>
#endif

#include "cstar.parser.hpp"
#include "cstar.lexer.hpp"
#include "cstar.context.h"
#include "cstar.cemit.h"

#ifndef CSTAR_VERSION
#define CSTAR_VERSION "0.0.0-dev"
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

// Where cstar_runtime.h lives, resolved so an INSTALLED binary finds it from any
// cwd: $CSTAR_HOME, else <exeDir>/../include (bin/cstar -> ../include), else
// <exeDir> (repo root layout), else ".".
std::string resolveRuntimeDir(const char* argv0)
{
    if (const char* home = getenv("CSTAR_HOME")) return home;
    std::string exeDir = dirName(absolutePath(argv0 ? argv0 : "cstar"));
    if (fileExists(exeDir + "/../include/cstar_runtime.h")) return exeDir + "/../include";
    if (fileExists(exeDir + "/cstar_runtime.h"))            return exeDir;
    return ".";
}

// Parse one cstar file into a CompilationUnit. Returns nullptr on failure.
SharedCompilationUnit parseFile(const std::string& inputFile)
{
    yyscan_t scanner;
    struct LexerInstanceData extra = {
        CSTAR_LEXERINSTANCE_DEFAULT_LINE_ONE,
        CSTAR_LEXERINSTANCE_DEFAULT_COLUMN_ONE,
        nullptr,
        std::make_shared<CodeGenContext>(std::make_shared<std::string>(inputFile)),
        nullptr
    };

    yylex_init_extra(&extra, &scanner);

    FILE* input = fopen(inputFile.c_str(), "r");
    if (!input) {
        fprintf(stderr, "cstar: error: cannot open input file '%s'\n", inputFile.c_str());
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

// Transpile `inputFile` to C, writing to `outPath`. Returns 0 on success.
int transpileToFile(const std::string& inputFile, const std::string& outPath, bool emitLines)
{
    SharedCompilationUnit unit = parseFile(inputFile);
    if (!unit) return 1;

    std::ofstream out(outPath);
    if (!out) {
        fprintf(stderr, "cstar: error: cannot write '%s'\n", outPath.c_str());
        return 1;
    }

    CEmitter emitter(out, absolutePath(inputFile), emitLines);
    int unsupported = emitter.emit(unit);
    out.close();

    if (unsupported > 0)
        fprintf(stderr, "cstar: %d construct(s) not yet lowered; generated C may be incomplete.\n",
                unsupported);
    return 0;
}

int runCmd(const std::string& cmd)
{
    int rc = system(cmd.c_str());
    return (rc == -1) ? 1 : WEXITSTATUS(rc);
}

void usage()
{
    fprintf(stderr,
        "usage:\n"
        "  cstar transpile <in.cstar> [-o out.c] [--no-line]\n"
        "  cstar build     <in.cstar> [-o out] [--target native|wasm] [--release|--debug]\n"
        "                             [--webgpu] [--cc <compiler>] [--no-line] [--keep-c]\n");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("cstar %s\n", CSTAR_VERSION);
        return 0;
    }
    if (argc < 2) { usage(); return 2; }

    std::string subcommand = argv[1];
    std::string input;                    // first positional after the subcommand
    std::string output;
    std::string cc;                       // empty => pick default per target
    std::string target     = "native";    // native | wasm
    bool        emitLines  = true;
    bool        keepC      = false;
    bool        webgpu     = false;
    bool        release    = false;        // debug by default

    // Options may appear in any order, before or after the input file.
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc)            output = argv[++i];
        else if (a == "--cc" && i + 1 < argc)     cc = argv[++i];
        else if (a == "--target" && i + 1 < argc) target = argv[++i];
        else if (a == "--no-line")                emitLines = false;
        else if (a == "--keep-c")                 keepC = true;
        else if (a == "--webgpu")                 webgpu = true;
        else if (a == "--release")                release = true;
        else if (a == "--debug")                  release = false;
        else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "cstar: unknown option '%s'\n", a.c_str()); usage(); return 2;
        }
        else if (input.empty())                   input = a;
        else { fprintf(stderr, "cstar: unexpected extra argument '%s'\n", a.c_str()); return 2; }
    }

    if (input.empty()) { fprintf(stderr, "cstar: no input file\n"); usage(); return 2; }

    if (target != "native" && target != "wasm") {
        fprintf(stderr, "cstar: unknown --target '%s' (expected native|wasm)\n", target.c_str());
        return 2;
    }
    const bool wasm = (target == "wasm");

    // Release builds strip debug info and #line, optimize, and define NDEBUG.
    if (release) emitLines = false;

    // Where cstar_runtime.h lives — resolved so an installed binary works from any
    // cwd ($CSTAR_HOME, else <exe>/../include, else <exe>, else ".").
    std::string runtimeDir = resolveRuntimeDir(argv[0]);

    if (subcommand == "transpile") {
        std::string outPath = output.empty() ? (stripExtension(input) + ".c") : output;
        int rc = transpileToFile(input, outPath, emitLines);
        if (rc == 0) fprintf(stderr, "cstar: wrote %s\n", outPath.c_str());
        return rc;
    }

    if (subcommand == "build") {
        // Compiler: native uses clang; wasm uses emcc (emcc keys output format
        // off the -o extension). --cc / $EMCC override.
        std::string compiler = cc;
        if (compiler.empty()) {
            if (wasm) {
                const char* env = getenv("EMCC");
                compiler = env ? env : "emcc";
            } else {
                compiler = "clang";
            }
        }

        // Default output: native -> bare exe name; wasm -> an HTML harness
        // (emcc also emits the .js + .wasm alongside it).
        std::string defaultOut = wasm ? (stripExtension(input) + ".html") : stripExtension(input);
        std::string cPath      = stripExtension(input) + ".c";
        std::string outPath    = output.empty() ? defaultOut : output;

        if (transpileToFile(input, cPath, emitLines) != 0)
            return 1;

        std::ostringstream cmd;
        cmd << compiler << " -std=c11 ";
        if (release) {
            // Optimized, no debug info, asserts off. -ffunction/data-sections +
            // --gc-sections let the linker drop unused (std)library code — the
            // "pay for what you use" pruning lever. Native also strips symbols.
            cmd << (wasm ? "-Oz " : "-O2 ") << "-DNDEBUG -ffunction-sections -fdata-sections ";
            if (!wasm) {
#ifdef __APPLE__
                cmd << "-Wl,-dead_strip ";
#else
                cmd << "-Wl,--gc-sections ";
#endif
                cmd << "-s ";
            }
        } else {
            // Debug: faithful stepping + breakpoints in .cstar via #line.
            cmd << (wasm ? "-g -gsource-map -O0 " : "-g -O0 ");
        }
        cmd << "-I" << runtimeDir << " -I" << dirName(absolutePath(input)) << " -I. ";
        if (wasm && webgpu) cmd << "--use-port=emdawnwebgpu ";   // emscripten WebGPU port
        cmd << "\"" << cPath << "\" -o \"" << outPath << "\"";
        int rc = runCmd(cmd.str());

        if (!keepC) remove(cPath.c_str());
        if (rc != 0) {
            fprintf(stderr, "cstar: %s failed (exit %d)\n", compiler.c_str(), rc);
            return rc;
        }
        fprintf(stderr, "cstar: built %s\n", outPath.c_str());
        return 0;
    }

    fprintf(stderr, "cstar: unknown subcommand '%s'\n", subcommand.c_str());
    usage();
    return 2;
}
