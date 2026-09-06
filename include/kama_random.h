#ifndef KAMA_RANDOM_H
#define KAMA_RANDOM_H

// kama OS-entropy binding — the FFI boundary for `std::random`.
//
// ONE primitive: fill a buffer from the operating system's cryptographically secure generator. That is
// the whole seam, and it is deliberately the ONLY cryptographic-grade thing `std::random` touches: the
// module's `Rng` is a deterministic, seedable xoshiro256** written in kama — right for a game replay, a
// shuffle, a test, and WRONG for a key, a nonce or a session token, which should come from this seam
// (`std::random::entropy`) or from libsodium. The header sits apart from kama_os.h for kama_time.h's
// reason: a program that seeds one generator from the OS should not pay for <winsock2.h>.
//
//   kama_entropy      — fill `n` bytes; 0 on success, -1 if the OS refused. Never partial: a refusal
//                       leaves the caller's buffer unspecified and the kama layer panics, because an
//                       entropy source that fails is an environment fault, not a program state to
//                       handle (Go 1.24 made crypto/rand.Read infallible for the same reason).
//   kama_entropy_u64  — one word through the same path, for seeding without a byte buffer.
//
// The sources, one per branch and nothing else:
//   POSIX + wasm   getentropy(2) — glibc >= 2.25, musl >= 1.1.20 (emscripten's libc, over
//                  crypto.getRandomValues), macOS >= 10.12, the BSDs. It caps one call at 256 bytes
//                  (EIO above), so the loop chunks. There is no /dev/urandom fallback: getentropy
//                  needs Linux >= 3.17 (2014), and a build for anything older is a build nobody makes.
//   Windows        BCryptGenRandom with the system-preferred algorithm — the documented CNG entry
//                  point (RtlGenRandom is the undocumented one). It lives in bcrypt.dll, which the
//                  driver links (`-lbcrypt`) for every Windows target, pruned by --gc-sections when
//                  unused, exactly as ws2_32 is.
//
// Freestanding-friendly: <stdint.h>/<stddef.h> plus the one platform header (guarded), matching the
// kama_os.h `static inline` (single-TU, pruned-if-unused) convention.

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
static inline int32_t kama_entropy(uint8_t* buf, size_t n) {
    // The length is a ULONG (32-bit); chunk anything larger rather than truncate it.
    while (n > 0) {
        ULONG chunk = n > 0x7FFFFFFFu ? 0x7FFFFFFFu : (ULONG)n;
        // STATUS_SUCCESS is 0 and BCryptGenRandom returns no informational status, so a plain `!= 0`
        // is the failure test — and it spares this header the NTSTATUS typedef's header politics.
        if (BCryptGenRandom(NULL, buf, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return -1;
        buf += chunk;
        n   -= chunk;
    }
    return 0;
}

#else

// POSIX (Linux/macOS/BSD/iOS/Android) and emscripten. <sys/random.h> declares getentropy on every
// libc above without a feature-test macro, which is why this header needs no _DEFAULT_SOURCE restatement
// of its own (kama_runtime.h sets it first regardless).
#include <sys/random.h>
static inline int32_t kama_entropy(uint8_t* buf, size_t n) {
    while (n > 0) {
        size_t chunk = n > 256 ? 256 : n;   // getentropy's per-call ceiling
        if (getentropy(buf, chunk) != 0) return -1;
        buf += chunk;
        n   -= chunk;
    }
    return 0;
}

#endif

static inline int32_t kama_entropy_u64(uint64_t* v) {
    return kama_entropy((uint8_t*)v, sizeof *v);
}

#endif // KAMA_RANDOM_H
