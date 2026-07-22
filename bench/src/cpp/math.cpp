// math — Vec4/Mat4/Quat throughput, mirrors bench/src/kama/math.kama exactly (float32, exact-integer).
#include <cstdint>

struct V4 { float x, y, z, w; };
struct M4 { V4 c0, c1, c2, c3; };

static V4 v4_add(V4 a, V4 b){ return {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}; }
static V4 v4_sub(V4 a, V4 b){ return {a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w}; }
static V4 v4_scale(V4 a, float s){ return {a.x*s, a.y*s, a.z*s, a.w*s}; }
static float v4_dot(V4 a, V4 b){ return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w; }

static V4 m4_transform(M4 m, V4 v){
  return {
    m.c0.x*v.x + m.c1.x*v.y + m.c2.x*v.z + m.c3.x*v.w,
    m.c0.y*v.x + m.c1.y*v.y + m.c2.y*v.z + m.c3.y*v.w,
    m.c0.z*v.x + m.c1.z*v.y + m.c2.z*v.z + m.c3.z*v.w,
    m.c0.w*v.x + m.c1.w*v.y + m.c2.w*v.z + m.c3.w*v.w };
}
static M4 m4_mul(M4 a, M4 b){
  return { m4_transform(a, b.c0), m4_transform(a, b.c1),
           m4_transform(a, b.c2), m4_transform(a, b.c3) };
}
static V4 quat_mul(V4 a, V4 b){   // Hamilton product (x,y,z,w)
  return {
    a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
    a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
    a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
    a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
}

int main(){
  M4 m = { {1,1,0,0}, {0,1,1,0}, {0,0,1,1}, {1,0,0,1} };
  double sum = 0.0;
  for(uint64_t i=0;i<2000000ULL;i++){
    float s = (float)(i % 8);
    V4 a = { s,   s+1, s+2, s+3 };
    V4 b = { s+2, s+3, s+4, s+5 };
    V4 c = v4_add(a, b);
    V4 e = v4_scale(c, 3.0f);
    V4 f = v4_sub(e, b);
    float dp = v4_dot(a, b);
    V4 mv = m4_transform(m, a);
    M4 mm = m4_mul(m, m);
    V4 q1 = { s,   s+1, s+2, s+3 };
    V4 q2 = { s+1, s,   s+3, s+2 };
    V4 qq = quat_mul(q1, q2);
    float qdot = qq.x*qq.x + qq.y*qq.y + qq.z*qq.z + qq.w*qq.w;
    float acc = (f.x + f.y + f.z + f.w)
              + dp
              + (mv.x + mv.y + mv.z + mv.w)
              + (mm.c0.x + mm.c1.y + mm.c2.z + mm.c3.w)
              + qdot;
    sum += acc;
  }
  return (int)((uint64_t)sum % 256);
}
