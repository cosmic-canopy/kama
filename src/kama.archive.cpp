// kama.archive.cpp — the deterministic .tar.gz writer; kama.archive.h says why it exists.
//
// Three layers, each written from its specification: POSIX ustar headers with a pax record where a name
// does not fit (IEEE Std 1003.1), deflate (RFC 1951, section numbers cited inline), and the gzip frame
// (RFC 1952). Nothing here reads a clock, the environment or the filesystem, and nothing depends on an
// unspecified library order: every sort that can meet equal keys is a stable one, because std::sort's
// arrangement of ties differs between libc++ and libstdc++ — and a tie decides a Huffman code length.
#include "kama.archive.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// ---- tar -------------------------------------------------------------------------------------------------

// 1980-01-01T00:00:00Z. Fixed, so it is not the publish time; not 0, because zip-era readers treat the epoch
// as "no date".
const unsigned long kMtime = 315532800UL;

void tarHeader(std::string& out, const std::string& name, char type, size_t size, unsigned mode,
               const std::string& link)
{
    char h[512];
    memset(h, 0, sizeof h);
    memcpy(h, name.data(), std::min<size_t>(name.size(), 100));   // a longer name travels in a pax record
    snprintf(h + 100, 8, "%07o", mode);
    snprintf(h + 108, 8, "%07o", 0u);                               // uid
    snprintf(h + 116, 8, "%07o", 0u);                               // gid
    snprintf(h + 124, 12, "%011llo", (unsigned long long)size);
    snprintf(h + 136, 12, "%011lo", kMtime);
    h[156] = type;
    memcpy(h + 157, link.data(), std::min<size_t>(link.size(), 100));
    memcpy(h + 257, "ustar", 6);                                    // magic, with its NUL
    memcpy(h + 263, "00", 2);                                       // version
    // uname and gname stay empty: what they held was the publisher's account.
    memset(h + 148, ' ', 8);                                        // summed as spaces, then written over
    unsigned sum = 0;
    for (char c : h) sum += (unsigned char)c;
    snprintf(h + 148, 8, "%06o", sum);                              // six digits, a NUL, and the space left
    out.append(h, sizeof h);
}

void pad512(std::string& out) { out.append((512 - out.size() % 512) % 512, '\0'); }

// One pax record, "<len> <key>=<value>\n", where <len> counts every byte including its own digits.
std::string paxRecord(const std::string& key, const std::string& value)
{
    const size_t body = key.size() + value.size() + 3;   // ' ', '=', '\n'
    size_t len = body + 1;
    while (len != body + std::to_string(len).size()) len = body + std::to_string(len).size();
    return std::to_string(len) + " " + key + "=" + value + "\n";
}

// ---- deflate (RFC 1951) -------------------------------------------------------------------------------

// Bits leave least significant first (3.1.1).
struct BitWriter {
    std::string out;
    uint64_t acc = 0;
    int n = 0;
    void put(uint32_t v, int count) {
        acc |= (uint64_t)v << n;
        n += count;
        while (n >= 8) { out.push_back((char)(acc & 0xFF)); acc >>= 8; n -= 8; }
    }
    // A Huffman code goes out from its most significant bit (3.1.1), so reverse it into put's order.
    void code(uint32_t c, int len) {
        uint32_t r = 0;
        for (int i = 0; i < len; ++i) r |= ((c >> i) & 1u) << (len - 1 - i);
        put(r, len);
    }
    void toByte() { if (n > 0) put(0, 8 - n); }
};

// 3.2.5: the length and distance codes, as a base and a count of extra bits.
const uint16_t kLenBase[29]  = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t  kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t  kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7,
                                 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// The code whose range holds `v`. 258 lands on code 28 (symbol 285), which 3.2.5 requires.
template <size_t N> int codeFor(const uint16_t (&base)[N], int v)
{
    return (int)(std::upper_bound(base, base + N, v) - base) - 1;
}

// One LZ77 token: a literal byte (dist == 0) or a back-reference of `len` bytes, `dist` behind.
struct Token { uint16_t len; uint16_t dist; };

std::vector<Token> lz77(const std::string& s)
{
    // Window and match limits are deflate's (3.2.5); the search parameters sit between zlib's levels 6 and
    // 9. A 3-byte match further back than kTooFar costs more bits than the literals it replaces (zlib's rule).
    const size_t kWindow = 32768;
    const int kHashBits = 15, kMaxChain = 1024, kMaxMatch = 258, kTooFar = 4096;
    const size_t n = s.size();
    const unsigned char* d = (const unsigned char*)s.data();
    std::vector<int32_t> head((size_t)1 << kHashBits, -1), prev(kWindow, -1);
    auto hash = [&](size_t i) {
        uint32_t v = ((uint32_t)d[i] << 16) | ((uint32_t)d[i + 1] << 8) | d[i + 2];
        return (v * 2654435761u) >> (32 - kHashBits);
    };
    auto insert = [&](size_t i) {
        if (i + 3 > n) return;
        uint32_t h = hash(i);
        prev[i & (kWindow - 1)] = head[h];
        head[h] = (int32_t)i;
    };
    // The longest earlier match for position i. Called BEFORE i is inserted, so it never finds itself; every
    // position on a chain is then >= i - kWindow, whose ring slot no later position has overwritten yet.
    auto longest = [&](size_t i, int& bestLen, int& bestDist) {
        bestLen = 0; bestDist = 0;
        if (i + 3 > n) return;
        const int maxLen = (int)std::min<size_t>(kMaxMatch, n - i);
        int32_t c = head[hash(i)];
        for (int chain = 0; c >= 0 && chain < kMaxChain; ++chain) {
            const size_t dist = i - (size_t)c;
            if (dist > kWindow) break;
            // A candidate can only beat the best so far if it also matches at the best's length.
            if (bestLen == 0 || d[c + bestLen] == d[i + bestLen]) {
                int l = 0;
                while (l < maxLen && d[c + l] == d[i + l]) ++l;
                if (l > bestLen) { bestLen = l; bestDist = (int)dist; if (l == maxLen) break; }
            }
            const int32_t next = prev[(size_t)c & (kWindow - 1)];
            if (next >= c) break;
            c = next;
        }
        if (bestLen < 3 || (bestLen == 3 && bestDist > kTooFar)) { bestLen = 0; bestDist = 0; }
    };

    // Lazy matching: a match found at i is held back one byte, and taken only if the match at i+1 is no
    // longer; otherwise i goes out as a literal and the longer match takes its place.
    std::vector<Token> out;
    size_t i = 0;
    bool pending = false;
    int pLen = 0, pDist = 0;
    while (i < n) {
        int len, dist;
        longest(i, len, dist);
        insert(i);
        if (pending && pLen >= 3 && pLen >= len) {
            out.push_back({(uint16_t)pLen, (uint16_t)pDist});
            const size_t end = i - 1 + (size_t)pLen;
            for (size_t j = i + 1; j < end; ++j) insert(j);
            i = end;
            pending = false;
            continue;
        }
        if (pending) out.push_back({d[i - 1], 0});
        pending = true; pLen = len; pDist = dist;
        ++i;
    }
    if (pending) out.push_back({d[n - 1], 0});   // the last byte: no match can start there
    return out;
}

// Code lengths for `freq`, none longer than `limit`, by package-merge (optimal under the limit). Symbols of
// frequency 0 get length 0. The caller guarantees at least two nonzero symbols, which makes every code
// COMPLETE — the one shape every inflater accepts (zlib refuses an incomplete one of more than one code).
std::vector<uint8_t> codeLengths(const std::vector<uint32_t>& freq, int limit)
{
    struct Node { uint64_t weight; int symbol; int a, b; };   // symbol < 0: a package of nodes a and b
    std::vector<Node> pool;
    std::vector<int> leaves;
    for (int s = 0; s < (int)freq.size(); ++s)
        if (freq[s]) { pool.push_back({freq[s], s, -1, -1}); leaves.push_back((int)pool.size() - 1); }
    auto lighter = [&](int x, int y) { return pool[x].weight < pool[y].weight; };
    std::stable_sort(leaves.begin(), leaves.end(), lighter);   // ties stay in symbol order
    std::vector<int> list = leaves;
    for (int level = 1; level < limit; ++level) {
        std::vector<int> packages;
        for (size_t k = 0; k + 1 < list.size(); k += 2) {
            Node p{pool[list[k]].weight + pool[list[k + 1]].weight, -1, list[k], list[k + 1]};
            pool.push_back(p);
            packages.push_back((int)pool.size() - 1);
        }
        std::vector<int> merged(leaves.size() + packages.size());
        std::merge(leaves.begin(), leaves.end(), packages.begin(), packages.end(), merged.begin(), lighter);
        list.swap(merged);
    }
    // A symbol's length is the number of times it occurs among the 2n-2 lightest items.
    std::vector<uint8_t> len(freq.size(), 0);
    std::vector<int> stack;
    for (size_t k = 0; k < 2 * leaves.size() - 2; ++k) {
        stack.push_back(list[k]);
        while (!stack.empty()) {
            const Node& x = pool[stack.back()];
            stack.pop_back();
            if (x.symbol >= 0) ++len[x.symbol];
            else { stack.push_back(x.a); stack.push_back(x.b); }
        }
    }
    return len;
}

// 3.2.2: the canonical code for each length.
std::vector<uint16_t> canonicalCodes(const std::vector<uint8_t>& len)
{
    int count[16] = {0};
    for (uint8_t l : len) if (l) ++count[l];
    uint16_t next[16] = {0};
    uint16_t code = 0;
    for (int bits = 1; bits < 16; ++bits) { code = (uint16_t)((code + count[bits - 1]) << 1); next[bits] = code; }
    std::vector<uint16_t> codes(len.size(), 0);
    for (size_t s = 0; s < len.size(); ++s) if (len[s]) codes[s] = next[len[s]]++;
    return codes;
}

std::vector<uint32_t> atLeastTwo(std::vector<uint32_t> f)
{
    int nonzero = 0;
    for (uint32_t x : f) if (x) ++nonzero;
    for (size_t s = 0; nonzero < 2 && s < f.size(); ++s) if (!f[s]) { f[s] = 1; ++nonzero; }
    return f;
}

// One block of tokens covering `raw[0..rawLen)`, written as whichever of stored, fixed-code or dynamic-code
// (3.2.3) is smallest. Ties go to the simpler form.
void writeBlock(BitWriter& bw, const Token* tok, size_t nTok, const unsigned char* raw, size_t rawLen, bool last)
{
    std::vector<uint32_t> lf(286, 0), df(30, 0);
    uint64_t extra = 0;
    for (size_t k = 0; k < nTok; ++k) {
        if (tok[k].dist == 0) { ++lf[tok[k].len]; continue; }
        const int ls = codeFor(kLenBase, tok[k].len), ds = codeFor(kDistBase, tok[k].dist);
        ++lf[257 + ls]; ++df[ds];
        extra += kLenExtra[ls] + kDistExtra[ds];
    }
    lf[256] = 1;   // end of block

    // Dynamic: the two codes, then the code-length sequence run-length coded with 16/17/18 (3.2.7).
    const std::vector<uint8_t> ll = codeLengths(atLeastTwo(lf), 15), dl = codeLengths(atLeastTwo(df), 15);
    int hlit = 286; while (hlit > 257 && ll[hlit - 1] == 0) --hlit;
    int hdist = 30; while (hdist > 1 && dl[hdist - 1] == 0) --hdist;
    std::vector<uint8_t> seq(ll.begin(), ll.begin() + hlit);
    seq.insert(seq.end(), dl.begin(), dl.begin() + hdist);   // one sequence: a run may cross between them
    struct Run { uint8_t sym, extra; };
    std::vector<Run> runs;
    for (size_t k = 0; k < seq.size();) {
        const uint8_t v = seq[k];
        size_t run = 1;
        while (k + run < seq.size() && seq[k + run] == v) ++run;
        size_t left = run;
        if (v == 0) {
            while (left >= 3) {
                const size_t r = std::min<size_t>(left, 138);
                runs.push_back(r >= 11 ? Run{18, (uint8_t)(r - 11)} : Run{17, (uint8_t)(r - 3)});
                left -= r;
            }
        } else if (run >= 4) {
            runs.push_back({v, 0});
            --left;
            while (left >= 3) { const size_t r = std::min<size_t>(left, 6); runs.push_back({16, (uint8_t)(r - 3)}); left -= r; }
        }
        while (left--) runs.push_back({v, 0});
        k += run;
    }
    std::vector<uint32_t> cf(19, 0);
    for (const Run& r : runs) ++cf[r.sym];
    const std::vector<uint8_t> cl = codeLengths(atLeastTwo(cf), 7);
    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int hclen = 19; while (hclen > 4 && cl[kOrder[hclen - 1]] == 0) --hclen;

    static const uint8_t kRunExtra[19] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7};
    uint64_t dynBits = 3 + 5 + 5 + 4 + 3 * (uint64_t)hclen + extra;
    for (const Run& r : runs) dynBits += cl[r.sym] + kRunExtra[r.sym];
    for (int s = 0; s < 286; ++s) dynBits += (uint64_t)lf[s] * ll[s];
    for (int s = 0; s < 30; ++s)  dynBits += (uint64_t)df[s] * dl[s];

    // Fixed (3.2.6): lengths 8/9/7/8 by symbol range, every distance 5 bits.
    std::vector<uint8_t> fl(288), fd(30, 5);
    for (int s = 0; s < 288; ++s) fl[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
    uint64_t fixBits = 3 + extra;
    for (int s = 0; s < 286; ++s) fixBits += (uint64_t)lf[s] * fl[s];
    for (int s = 0; s < 30; ++s)  fixBits += (uint64_t)df[s] * 5;

    // Stored (3.2.4): at most 65535 bytes a block, each with a header, padding to a byte, LEN and NLEN.
    const size_t chunks = rawLen == 0 ? 1 : (rawLen + 65534) / 65535;
    const uint64_t storedBits = chunks * (3 + 7 + 32) + 8 * (uint64_t)rawLen;

    if (storedBits < fixBits && storedBits < dynBits) {
        for (size_t c = 0, off = 0; c < chunks; ++c) {
            const size_t len = std::min<size_t>(65535, rawLen - off);
            bw.put(last && c + 1 == chunks, 1);
            bw.put(0, 2);
            bw.toByte();
            bw.put((uint32_t)len, 16);
            bw.put((uint32_t)(~len & 0xFFFF), 16);
            bw.out.append((const char*)raw + off, len);   // aligned: toByte, then two whole bytes twice
            off += len;
        }
        return;
    }
    const bool fixed = fixBits <= dynBits;
    const std::vector<uint8_t>& L = fixed ? fl : ll;
    const std::vector<uint8_t>& D = fixed ? fd : dl;
    const std::vector<uint16_t> lc = canonicalCodes(L), dc = canonicalCodes(D);
    bw.put(last, 1);
    bw.put(fixed ? 1 : 2, 2);
    if (!fixed) {
        bw.put((uint32_t)(hlit - 257), 5);
        bw.put((uint32_t)(hdist - 1), 5);
        bw.put((uint32_t)(hclen - 4), 4);
        for (int k = 0; k < hclen; ++k) bw.put(cl[kOrder[k]], 3);
        const std::vector<uint16_t> cc = canonicalCodes(cl);
        for (const Run& r : runs) { bw.code(cc[r.sym], cl[r.sym]); bw.put(r.extra, kRunExtra[r.sym]); }
    }
    for (size_t k = 0; k < nTok; ++k) {
        const Token& t = tok[k];
        if (t.dist == 0) { bw.code(lc[t.len], L[t.len]); continue; }
        const int ls = codeFor(kLenBase, t.len), ds = codeFor(kDistBase, t.dist);
        bw.code(lc[257 + ls], L[257 + ls]);
        bw.put((uint32_t)(t.len - kLenBase[ls]), kLenExtra[ls]);
        bw.code(dc[ds], D[ds]);
        bw.put((uint32_t)(t.dist - kDistBase[ds]), kDistExtra[ds]);
    }
    bw.code(lc[256], L[256]);
}

std::string deflate(const std::string& in)
{
    const std::vector<Token> tok = lz77(in);
    const unsigned char* raw = (const unsigned char*)in.data();
    const size_t kBlockTokens = 16384;   // zlib's default buffer: a new code per block adapts to the data
    BitWriter bw;
    if (tok.empty()) writeBlock(bw, nullptr, 0, raw, 0, true);
    for (size_t k = 0, pos = 0; k < tok.size(); k += kBlockTokens) {
        const size_t nTok = std::min(kBlockTokens, tok.size() - k);
        size_t bytes = 0;
        for (size_t j = k; j < k + nTok; ++j) bytes += tok[j].dist ? tok[j].len : 1;
        writeBlock(bw, &tok[k], nTok, raw + pos, bytes, k + nTok == tok.size());
        pos += bytes;
    }
    bw.toByte();
    return bw.out;
}

// ---- gzip (RFC 1952) ---------------------------------------------------------------------------------

uint32_t crc32(const std::string& s)
{
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (char b : s) c = table[(c ^ (unsigned char)b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string gzip(const std::string& in)
{
    // ID1 ID2, CM = deflate, no flags (so no name), MTIME 0, XFL 0, OS 255 "unknown": the bytes do not
    // depend on which OS wrote them, so claiming one would be false.
    static const unsigned char kHeader[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255};
    std::string out((const char*)kHeader, sizeof kHeader);
    out += deflate(in);
    const uint32_t crc = crc32(in), size = (uint32_t)in.size();   // ISIZE is the size mod 2^32
    for (int i = 0; i < 4; ++i) out.push_back((char)(crc >> (8 * i)));
    for (int i = 0; i < 4; ++i) out.push_back((char)(size >> (8 * i)));
    return out;
}

} // namespace

std::string kamaTarGz(const std::string& root, const std::vector<ArchiveEntry>& entries)
{
    std::string tar;
    for (const ArchiveEntry& e : entries) {
        const std::string name = root + "/" + e.path;
        const bool link = e.kind == ArchiveEntry::Symlink;
        std::string pax;
        if (name.size() > 100) pax += paxRecord("path", name);
        if (link && e.data.size() > 100) pax += paxRecord("linkpath", e.data);
        if (!pax.empty()) {
            tarHeader(tar, "PaxHeader", 'x', pax.size(), 0644, "");
            tar += pax;
            pad512(tar);
        }
        const unsigned mode = link ? 0777 : e.kind == ArchiveEntry::Executable ? 0755 : 0644;
        tarHeader(tar, name, link ? '2' : '0', link ? 0 : e.data.size(), mode, link ? e.data : std::string());
        if (!link) { tar += e.data; pad512(tar); }
    }
    tar.append(1024, '\0');   // the end: two zero blocks
    return gzip(tar);
}
