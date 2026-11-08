#ifndef KAMA_ASSET_H
#define KAMA_ASSET_H

/* std::asset (KR-283 step 3): the record of the project's `assets`, and an Android app's reads of them.

   The record is every name in the directory `kama.json`'s `assets` names, sorted bytewise, which the emitter defines
   beside `main` for a program that imports std::asset. A name is looked up in it exactly on every host, as Go's
   `embed.FS` looks up the names its build recorded. So a name whose case differs from the file's is not found on
   Windows or macOS either, whose file systems would have opened it, just as Android's asset manager does not.

   In an Android app (`-DKAMA_ANDROID_APP`) the files are the APK's `assets/`, which only the NDK's AAssetManager reads.
   Everywhere else std::asset reads them as files, through std::fs, and these calls answer "no asset manager". */

#include <stdint.h>
#include <string.h>

extern const char* const kama__asset_names[];
extern const int kama__asset_count;

/* 1 when the `len` bytes at `name` are a name in the record, exactly; the order is std::sort's over std::string. */
static inline int32_t kama_asset_known(const uint8_t* name, intptr_t len) {
    int lo = 0, hi = kama__asset_count;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        const char* m = kama__asset_names[mid];
        const size_t mlen = strlen(m), n = (size_t)len;
        int c = memcmp(m, name, mlen < n ? mlen : n);
        if (c == 0) c = mlen < n ? -1 : mlen > n ? 1 : 0;
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

#if defined(KAMA_ANDROID_APP)
#include <android/asset_manager.h>
#include <android_native_app_glue.h>

extern struct android_app* kama__android_app;   /* the entry's (kama_android_app.h) */

static inline int32_t kama_asset_manager(void) { return kama__android_app != NULL; }
/* The asset `name` (`len` bytes), opened for random access, or NULL. The name is copied to the stack for its NUL; one
   past PATH_MAX is no asset a device could hold, and is not found. */
static inline void* kama_asset_open(const uint8_t* name, intptr_t len) {
    char z[4096];
    if (len < 0 || (size_t)len >= sizeof z) return NULL;
    memcpy(z, name, (size_t)len); z[len] = '\0';
    return AAssetManager_open(kama__android_app->activity->assetManager, z, AASSET_MODE_RANDOM);
}
static inline intptr_t kama_asset_read(void* a, uint8_t* into, intptr_t n) { return AAsset_read((AAsset*)a, into, (size_t)n); }
static inline int64_t kama_asset_seek(void* a, int64_t off, int32_t whence) { return AAsset_seek64((AAsset*)a, off, whence); }
static inline int64_t kama_asset_length(void* a) { return AAsset_getLength64((AAsset*)a); }
static inline void kama_asset_close(void* a) { AAsset_close((AAsset*)a); }
#else
static inline int32_t kama_asset_manager(void) { return 0; }
static inline void* kama_asset_open(const uint8_t* name, intptr_t len) { (void)name; (void)len; return NULL; }
static inline intptr_t kama_asset_read(void* a, uint8_t* into, intptr_t n) { (void)a; (void)into; (void)n; return -1; }
static inline int64_t kama_asset_seek(void* a, int64_t off, int32_t whence) { (void)a; (void)off; (void)whence; return -1; }
static inline int64_t kama_asset_length(void* a) { (void)a; return -1; }
static inline void kama_asset_close(void* a) { (void)a; }
#endif

#endif /* KAMA_ASSET_H */
