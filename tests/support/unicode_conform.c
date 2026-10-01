/* Conformance for include/kama_unicode.h (tools/check-unicode.sh) against the UCD's NormalizationTest.txt, read from stdin.
 * Every line of Part 0..3 is five sequences c1..c5, and UAX #15 states what each form must make of each:
 *   NFC:  c2 == NFC(c1) == NFC(c2) == NFC(c3);   c4 == NFC(c4) == NFC(c5)
 *   NFD:  c3 == NFD(c1) == NFD(c2) == NFD(c3);   c5 == NFD(c4) == NFD(c5)
 *   NFKC: c4 == NFKC(c1) == ... == NFKC(c5)
 *   NFKD: c5 == NFKD(c1) == ... == NFKD(c5)
 * and every code point NOT listed in Part 1 is unchanged by all four. Exits 0 when every check holds. */
#include "kama_unicode.h"
#include <stdio.h>
/* what a kama program defines and the runtime header refers to */
void (*kama_panic_hook)(void) = 0;
int kama_in_panic_hook = 0;
void kama__stdio_write_failed(void) {}
#include <stdlib.h>
#include <string.h>

static size_t enc(const char* field, uint8_t* out) {          /* "1E0A 0323" -> UTF-8 */
    size_t n = 0;
    const char* p = field;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        char* end;
        unsigned long cp = strtoul(p, &end, 16);
        n += kama__uni_utf8((uint32_t)cp, out + n);
        p = end;
    }
    return n;
}

static int failures = 0, checks = 0;
static void expect(const char* what, long line, int form, const uint8_t* in, size_t inn, const uint8_t* want, size_t wn) {
    uint8_t got[1024];
    const ptrdiff_t g = kama_unicode_normalize(in, inn, form, got, sizeof got);
    ++checks;
    if (g < 0 || (size_t)g != wn || memcmp(got, want, wn) != 0) {
        if (failures++ < 10) fprintf(stderr, "line %ld: %s (form %d) differs\n", line, what, form);
    }
}

static unsigned char listed[0x110000];

int main(void) {
    char line[4096];
    long ln = 0;
    int part1 = 0;
    while (fgets(line, sizeof line, stdin)) {
        ++ln;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (line[0] == '@') { part1 = strncmp(line, "@Part1", 6) == 0; continue; }
        char* f[5];
        char* p = line;
        for (int i = 0; i < 5; ++i) { f[i] = p; p = strchr(p, ';'); if (!p) return 2; *p++ = 0; }
        uint8_t c[5][512]; size_t cn[5];
        for (int i = 0; i < 5; ++i) cn[i] = enc(f[i], c[i]);
        if (part1) listed[strtoul(f[0], NULL, 16)] = 1;
        for (int i = 0; i < 3; ++i) { expect("NFC", ln, 0, c[i], cn[i], c[1], cn[1]); expect("NFD", ln, 1, c[i], cn[i], c[2], cn[2]); }
        for (int i = 3; i < 5; ++i) { expect("NFC", ln, 0, c[i], cn[i], c[3], cn[3]); expect("NFD", ln, 1, c[i], cn[i], c[4], cn[4]); }
        for (int i = 0; i < 5; ++i) { expect("NFKC", ln, 2, c[i], cn[i], c[3], cn[3]); expect("NFKD", ln, 3, c[i], cn[i], c[4], cn[4]); }
    }
    for (uint32_t cp = 0; cp < 0x110000; ++cp) {
        if (listed[cp] || (cp >= 0xD800 && cp <= 0xDFFF)) continue;
        uint8_t u[4]; const size_t n = kama__uni_utf8(cp, u);
        for (int form = 0; form < 4; ++form) expect("unlisted code point", (long)cp, form, u, n, u, n);
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
