// math — Vec4/Mat4/Quat throughput, mirrors bench/src/kama/math.kama (exact-integer, so checksum matches).
interface V4 { x: number; y: number; z: number; w: number; }
interface M4 { c0: V4; c1: V4; c2: V4; c3: V4; }

function v4add(a: V4, b: V4): V4 { return {x:a.x+b.x, y:a.y+b.y, z:a.z+b.z, w:a.w+b.w}; }
function v4sub(a: V4, b: V4): V4 { return {x:a.x-b.x, y:a.y-b.y, z:a.z-b.z, w:a.w-b.w}; }
function v4scale(a: V4, s: number): V4 { return {x:a.x*s, y:a.y*s, z:a.z*s, w:a.w*s}; }
function v4dot(a: V4, b: V4): number { return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w; }
function m4transform(m: M4, v: V4): V4 { return {
  x: m.c0.x*v.x + m.c1.x*v.y + m.c2.x*v.z + m.c3.x*v.w,
  y: m.c0.y*v.x + m.c1.y*v.y + m.c2.y*v.z + m.c3.y*v.w,
  z: m.c0.z*v.x + m.c1.z*v.y + m.c2.z*v.z + m.c3.z*v.w,
  w: m.c0.w*v.x + m.c1.w*v.y + m.c2.w*v.z + m.c3.w*v.w }; }
function m4mul(a: M4, b: M4): M4 { return { c0:m4transform(a,b.c0), c1:m4transform(a,b.c1),
                                            c2:m4transform(a,b.c2), c3:m4transform(a,b.c3) }; }
function quatMul(a: V4, b: V4): V4 { return {   // Hamilton product (x,y,z,w)
  x: a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
  y: a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
  z: a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
  w: a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z }; }

const m: M4 = { c0:{x:1,y:1,z:0,w:0}, c1:{x:0,y:1,z:1,w:0}, c2:{x:0,y:0,z:1,w:1}, c3:{x:1,y:0,z:0,w:1} };
let sum = 0;
for(let i=0;i<2000000;i++){
  const s = i % 8;
  const a: V4 = {x:s,   y:s+1, z:s+2, w:s+3};
  const b: V4 = {x:s+2, y:s+3, z:s+4, w:s+5};
  const c = v4add(a,b);
  const e = v4scale(c,3.0);
  const f = v4sub(e,b);
  const dp = v4dot(a,b);
  const mv = m4transform(m,a);
  const mm = m4mul(m,m);
  const q1: V4 = {x:s,   y:s+1, z:s+2, w:s+3};
  const q2: V4 = {x:s+1, y:s,   z:s+3, w:s+2};
  const qq = quatMul(q1,q2);
  const qdot = qq.x*qq.x + qq.y*qq.y + qq.z*qq.z + qq.w*qq.w;
  const acc = (f.x + f.y + f.z + f.w)
            + dp
            + (mv.x + mv.y + mv.z + mv.w)
            + (mm.c0.x + mm.c1.y + mm.c2.z + mm.c3.w)
            + qdot;
  sum += acc;
}
process.exit(sum % 256);

export {};
