/* A callback inside a DESCRIPTOR STRUCT — the shape every real C API uses (WebGPU, CoreAudio and
   miniaudio all take their callback this way). `c_submit` is what C does with one: reach into the
   struct and call the kama function through it. */
typedef int (*OpFn)(int);

typedef struct Desc { OpFn f; int n; } Desc;
typedef struct Info { OpFn cb; int mode; } Info;

static inline int c_submit(Desc d) { return d.f(d.n); }
