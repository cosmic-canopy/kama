-- math — Vec4/Mat4/Quat throughput, mirrors bench/src/kama/math.kama (exact-integer, so checksum matches).
local function v4add(a,b) return {x=a.x+b.x, y=a.y+b.y, z=a.z+b.z, w=a.w+b.w} end
local function v4sub(a,b) return {x=a.x-b.x, y=a.y-b.y, z=a.z-b.z, w=a.w-b.w} end
local function v4scale(a,s) return {x=a.x*s, y=a.y*s, z=a.z*s, w=a.w*s} end
local function v4dot(a,b) return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w end
local function m4transform(m,v) return {
  x = m.c0.x*v.x + m.c1.x*v.y + m.c2.x*v.z + m.c3.x*v.w,
  y = m.c0.y*v.x + m.c1.y*v.y + m.c2.y*v.z + m.c3.y*v.w,
  z = m.c0.z*v.x + m.c1.z*v.y + m.c2.z*v.z + m.c3.z*v.w,
  w = m.c0.w*v.x + m.c1.w*v.y + m.c2.w*v.z + m.c3.w*v.w } end
local function m4mul(a,b) return { c0=m4transform(a,b.c0), c1=m4transform(a,b.c1),
                                   c2=m4transform(a,b.c2), c3=m4transform(a,b.c3) } end
local function quat_mul(a,b) return {   -- Hamilton product (x,y,z,w)
  x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
  y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
  z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
  w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z } end

local m = { c0={x=1,y=1,z=0,w=0}, c1={x=0,y=1,z=1,w=0}, c2={x=0,y=0,z=1,w=1}, c3={x=1,y=0,z=0,w=1} }
local sum = 0.0
for i=0,1999999 do
  local s = i % 8
  local a = {x=s,   y=s+1, z=s+2, w=s+3}
  local b = {x=s+2, y=s+3, z=s+4, w=s+5}
  local c = v4add(a,b)
  local e = v4scale(c,3.0)
  local f = v4sub(e,b)
  local dp = v4dot(a,b)
  local mv = m4transform(m,a)
  local mm = m4mul(m,m)
  local q1 = {x=s,   y=s+1, z=s+2, w=s+3}
  local q2 = {x=s+1, y=s,   z=s+3, w=s+2}
  local qq = quat_mul(q1,q2)
  local qdot = qq.x*qq.x + qq.y*qq.y + qq.z*qq.z + qq.w*qq.w
  local acc = (f.x + f.y + f.z + f.w)
            + dp
            + (mv.x + mv.y + mv.z + mv.w)
            + (mm.c0.x + mm.c1.y + mm.c2.z + mm.c3.w)
            + qdot
  sum = sum + acc
end
os.exit(math.floor(sum) % 256)
