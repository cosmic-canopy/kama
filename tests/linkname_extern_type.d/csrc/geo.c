#include "geo.h"
int vec2_dot(const vec2_t* a, const vec2_t* b) { return a->x * b->x + a->y * b->y; }
void vec2_scale(vec2_t* v, int k) { v->x *= k; v->y *= k; }
