/* winargv_probe.c — kama's command-line splitter (kama__cmdline_split, include/kama_runtime.h) against
 * CommandLineToArgvW, the function it replaced. Driven by tools/check-winargv.sh, Windows only: the
 * oracle IS Windows. The cases are the ones every argv-splitting bug report is made of — backslash runs
 * before a quote, `""` inside quotes, an unterminated quote, tabs, empty arguments, a quoted argv[0],
 * non-BMP text — plus a lone surrogate, where both sides substitute U+FFFD.
 *
 * The empty command line is deliberately absent: CommandLineToArgvW answers it with the program's own
 * path (it calls GetModuleFileNameW), a special case kama never reaches, since GetCommandLineW is never
 * empty for a process the CRT started. */
#include "kama_runtime.h"
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int    kama_argc;
char** kama_argv;
int    kama__argv_state;

static void show(const char* label, int argc, char** v) {
    printf("    %s argc=%d:", label, argc);
    for (int i = 0; i < argc; ++i) printf(" [%s]", v[i]);
    printf("\n");
}

static int check(const wchar_t* cmd) {
    kama__u8sink count = { (char*)0, 0 };
    int argc = kama__cmdline_split(cmd, &count, (char**)0);
    char** ours = (char**)malloc(((size_t)argc + 1) * sizeof(char*) + count.n);
    kama__u8sink w = { (char*)(ours + argc + 1), 0 };
    kama__cmdline_split(cmd, &w, ours);
    ours[argc] = (char*)0;

    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(cmd, &wargc);
    char** theirs = (char**)malloc(((size_t)wargc + 1) * sizeof(char*));
    for (int i = 0; i < wargc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, (char*)0, 0, (const char*)0, (int*)0);
        theirs[i] = (char*)malloc((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, theirs[i], n, (const char*)0, (int*)0);
    }

    int same = argc == wargc;
    for (int i = 0; same && i < argc; ++i) same = strcmp(ours[i], theirs[i]) == 0;

    char shown[512]; int n = WideCharToMultiByte(CP_UTF8, 0, cmd, -1, shown, (int)sizeof shown, (const char*)0, (int*)0);
    if (n <= 0) strcpy(shown, "(unrenderable)");
    printf("  %s: %s\n", same ? "ok" : "MISMATCH", shown);
    if (!same) { show("kama  ", argc, ours); show("win32 ", wargc, theirs); }
    LocalFree(wargv);
    return same;
}

int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    static const wchar_t* cases[] = {
        L"prog",
        L"prog a b c",
        L"prog   a\t\tb  ",
        L"\"C:\\Program Files\\x.exe\" a b",
        L"\"C:\\Program Files\\x.exe\"a b",
        L"C:\\x\\y.exe\"quoted in argv0\" b",
        L"prog \"a b\" c",
        L"prog a\"b c\"d",
        L"prog \"\"",
        L"prog \"\" x",
        L"prog x \"\"",
        L"prog a\\\"b",
        L"prog a\\\\\"b c\"",
        L"prog a\\\\\\\"b",
        L"prog \\\\\\\\",
        L"prog \"\\\\\"",
        L"prog \"a\\\\\" b",
        L"prog \"a\"\"b\"",
        L"prog \"a\"\"\"b\"",
        L"prog \"a\"\" b\" c",
        L"prog \"unterminated",
        L"prog \"unterminated a\\\"b",
        L"prog \"",
        L"prog a\"",
        L"prog \\",
        L"prog \\\\a",
        L"prog \\\" x",
        L"prog \u65e5\u672c\u8a9e \"\u041f\u0440\u0438\u0432\u0435\u0442\U0001F600\" \U0001F600x",
        L"prog lone\xD800surrogate \xDC00",
        L"\"\" a",
        L"\"unterminated argv0",
    };
    int bad = 0;
    const int total = (int)(sizeof cases / sizeof cases[0]);
    for (int i = 0; i < total; ++i) if (!check(cases[i])) ++bad;
    printf("winargv: %d of %d cases agree with CommandLineToArgvW\n", total - bad, total);
    return bad ? 1 : 0;
}
