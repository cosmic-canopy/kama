// math — Vec4/Mat4/Quat throughput, mirrors bench/src/kama/math.kama exactly (float32, exact-integer).
package main

import "os"

type V4 struct{ x, y, z, w float32 }
type M4 struct{ c0, c1, c2, c3 V4 }

func v4add(a, b V4) V4   { return V4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w} }
func v4sub(a, b V4) V4   { return V4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w} }
func v4scale(a V4, s float32) V4 { return V4{a.x * s, a.y * s, a.z * s, a.w * s} }
func v4dot(a, b V4) float32 { return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w }

func m4transform(m M4, v V4) V4 {
	return V4{
		m.c0.x*v.x + m.c1.x*v.y + m.c2.x*v.z + m.c3.x*v.w,
		m.c0.y*v.x + m.c1.y*v.y + m.c2.y*v.z + m.c3.y*v.w,
		m.c0.z*v.x + m.c1.z*v.y + m.c2.z*v.z + m.c3.z*v.w,
		m.c0.w*v.x + m.c1.w*v.y + m.c2.w*v.z + m.c3.w*v.w,
	}
}
func m4mul(a, b M4) M4 {
	return M4{m4transform(a, b.c0), m4transform(a, b.c1), m4transform(a, b.c2), m4transform(a, b.c3)}
}
func quatMul(a, b V4) V4 { // Hamilton product (x,y,z,w)
	return V4{
		a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
		a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
		a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
		a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
	}
}

func main() {
	m := M4{V4{1, 1, 0, 0}, V4{0, 1, 1, 0}, V4{0, 0, 1, 1}, V4{1, 0, 0, 1}}
	var sum float64 = 0
	for i := uint64(0); i < 2000000; i++ {
		s := float32(i % 8)
		a := V4{s, s + 1, s + 2, s + 3}
		b := V4{s + 2, s + 3, s + 4, s + 5}
		c := v4add(a, b)
		e := v4scale(c, 3.0)
		f := v4sub(e, b)
		dp := v4dot(a, b)
		mv := m4transform(m, a)
		mm := m4mul(m, m)
		q1 := V4{s, s + 1, s + 2, s + 3}
		q2 := V4{s + 1, s, s + 3, s + 2}
		qq := quatMul(q1, q2)
		qdot := qq.x*qq.x + qq.y*qq.y + qq.z*qq.z + qq.w*qq.w
		acc := (f.x + f.y + f.z + f.w) +
			dp +
			(mv.x + mv.y + mv.z + mv.w) +
			(mm.c0.x + mm.c1.y + mm.c2.z + mm.c3.w) +
			qdot
		sum += float64(acc)
	}
	os.Exit(int(uint64(sum) % 256))
}
