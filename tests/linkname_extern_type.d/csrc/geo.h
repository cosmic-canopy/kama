#ifndef GEO_H
#define GEO_H
/* A C API whose struct is spelled the C way, `vec2_t`, which kama would not choose for a type. */
typedef struct vec2_t { int x; int y; } vec2_t;
int vec2_dot(const vec2_t* a, const vec2_t* b);
void vec2_scale(vec2_t* v, int k);
#endif
