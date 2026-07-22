// math — Vec4/Mat4/Quat throughput, mirrors bench/src/kama/math.kama exactly (f32, exact-integer).
#[derive(Clone, Copy)]
struct V4 { x: f32, y: f32, z: f32, w: f32 }
#[derive(Clone, Copy)]
struct M4 { c0: V4, c1: V4, c2: V4, c3: V4 }

fn v4_add(a: V4, b: V4) -> V4 { V4{ x:a.x+b.x, y:a.y+b.y, z:a.z+b.z, w:a.w+b.w } }
fn v4_sub(a: V4, b: V4) -> V4 { V4{ x:a.x-b.x, y:a.y-b.y, z:a.z-b.z, w:a.w-b.w } }
fn v4_scale(a: V4, s: f32) -> V4 { V4{ x:a.x*s, y:a.y*s, z:a.z*s, w:a.w*s } }
fn v4_dot(a: V4, b: V4) -> f32 { a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w }

fn m4_transform(m: M4, v: V4) -> V4 {
    V4{
        x: m.c0.x*v.x + m.c1.x*v.y + m.c2.x*v.z + m.c3.x*v.w,
        y: m.c0.y*v.x + m.c1.y*v.y + m.c2.y*v.z + m.c3.y*v.w,
        z: m.c0.z*v.x + m.c1.z*v.y + m.c2.z*v.z + m.c3.z*v.w,
        w: m.c0.w*v.x + m.c1.w*v.y + m.c2.w*v.z + m.c3.w*v.w,
    }
}
fn m4_mul(a: M4, b: M4) -> M4 {
    M4{ c0: m4_transform(a, b.c0), c1: m4_transform(a, b.c1),
        c2: m4_transform(a, b.c2), c3: m4_transform(a, b.c3) }
}
fn quat_mul(a: V4, b: V4) -> V4 {   // Hamilton product (x,y,z,w)
    V4{
        x: a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        y: a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        z: a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        w: a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
    }
}

fn main(){
    let m = M4{ c0:V4{x:1.0,y:1.0,z:0.0,w:0.0}, c1:V4{x:0.0,y:1.0,z:1.0,w:0.0},
                c2:V4{x:0.0,y:0.0,z:1.0,w:1.0}, c3:V4{x:1.0,y:0.0,z:0.0,w:1.0} };
    let mut sum: f64 = 0.0;
    for i in 0u64..2000000 {
        let s = (i % 8) as f32;
        let a = V4{ x:s,     y:s+1.0, z:s+2.0, w:s+3.0 };
        let b = V4{ x:s+2.0, y:s+3.0, z:s+4.0, w:s+5.0 };
        let c = v4_add(a, b);
        let e = v4_scale(c, 3.0);
        let f = v4_sub(e, b);
        let dp = v4_dot(a, b);
        let mv = m4_transform(m, a);
        let mm = m4_mul(m, m);
        let q1 = V4{ x:s,     y:s+1.0, z:s+2.0, w:s+3.0 };
        let q2 = V4{ x:s+1.0, y:s,     z:s+3.0, w:s+2.0 };
        let qq = quat_mul(q1, q2);
        let qdot = qq.x*qq.x + qq.y*qq.y + qq.z*qq.z + qq.w*qq.w;
        let acc = (f.x + f.y + f.z + f.w)
                + dp
                + (mv.x + mv.y + mv.z + mv.w)
                + (mm.c0.x + mm.c1.y + mm.c2.z + mm.c3.w)
                + qdot;
        sum += acc as f64;
    }
    std::process::exit(((sum as u64) % 256) as i32);
}
