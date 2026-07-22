# math — Vec4/Mat4/Quat throughput, mirrors bench/src/kama/math.kama (exact-integer, so checksum matches).
import sys

def v4add(a, b):   return (a[0]+b[0], a[1]+b[1], a[2]+b[2], a[3]+b[3])
def v4sub(a, b):   return (a[0]-b[0], a[1]-b[1], a[2]-b[2], a[3]-b[3])
def v4scale(a, s): return (a[0]*s, a[1]*s, a[2]*s, a[3]*s)
def v4dot(a, b):   return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3]

def m4transform(m, v):
    c0, c1, c2, c3 = m
    return (
        c0[0]*v[0] + c1[0]*v[1] + c2[0]*v[2] + c3[0]*v[3],
        c0[1]*v[0] + c1[1]*v[1] + c2[1]*v[2] + c3[1]*v[3],
        c0[2]*v[0] + c1[2]*v[1] + c2[2]*v[2] + c3[2]*v[3],
        c0[3]*v[0] + c1[3]*v[1] + c2[3]*v[2] + c3[3]*v[3])

def m4mul(a, b):
    return (m4transform(a, b[0]), m4transform(a, b[1]), m4transform(a, b[2]), m4transform(a, b[3]))

def quat_mul(a, b):  # Hamilton product (x,y,z,w)
    return (
        a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1],
        a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0],
        a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3],
        a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2])

m = ((1.0, 1.0, 0.0, 0.0), (0.0, 1.0, 1.0, 0.0), (0.0, 0.0, 1.0, 1.0), (1.0, 0.0, 0.0, 1.0))
total = 0
for i in range(2000000):
    s = float(i % 8)
    a = (s,       s + 1.0, s + 2.0, s + 3.0)
    b = (s + 2.0, s + 3.0, s + 4.0, s + 5.0)
    c = v4add(a, b)
    e = v4scale(c, 3.0)
    f = v4sub(e, b)
    dp = v4dot(a, b)
    mv = m4transform(m, a)
    mm = m4mul(m, m)
    q1 = (s,       s + 1.0, s + 2.0, s + 3.0)
    q2 = (s + 1.0, s,       s + 3.0, s + 2.0)
    qq = quat_mul(q1, q2)
    qdot = qq[0]*qq[0] + qq[1]*qq[1] + qq[2]*qq[2] + qq[3]*qq[3]
    acc = (f[0] + f[1] + f[2] + f[3]) \
        + dp \
        + (mv[0] + mv[1] + mv[2] + mv[3]) \
        + (mm[0][0] + mm[1][1] + mm[2][2] + mm[3][3]) \
        + qdot
    total += int(acc)
sys.exit(total % 256)
