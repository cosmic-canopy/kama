#ifndef __KAMA_DIAGNOSTIC_H__
#define __KAMA_DIAGNOSTIC_H__

// A structured diagnostic — the query-surface (LSP) form of an error/warning. The compiler still prints
// its usual `file:line:col: … error: msg` to stderr for the CLI; in ADDITION, both the parser
// (CodeGenContext::handleError) and the semantic pass (CEmitter::unsupported) now ACCUMULATE these into a
// list so a non-emitting `analyze()` run can hand back structured diagnostics without anyone scraping
// stderr. Source ranges start as a single (line,column) point; end positions are filled once the lexer
// carries them (T3) — until then `endLine/endColumn == 0` means "unknown, treat as the start point".

#include <string>
#include <vector>

enum class DiagSeverity { Error, Warning, Information, Hint };

struct Diagnostic {
    int          line      = 0;   // 1-based start line
    int          column    = 0;   // 1-based start column (0 = whole line / unknown)
    int          endLine   = 0;   // T3: end line   (0 = unknown -> == start)
    int          endColumn = 0;   // T3: end column (0 = unknown -> == start)
    DiagSeverity severity  = DiagSeverity::Error;
    std::string  code;            // short stable id, e.g. "unsupported", "parse"
    std::string  message;         // human-readable text
    std::string  file;            // source path / module name the position is relative to
};

#endif // __KAMA_DIAGNOSTIC_H__
