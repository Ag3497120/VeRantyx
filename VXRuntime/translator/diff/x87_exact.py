#!/usr/bin/env python3
"""x87 transcendental results against exact ones.

usage: x87_exact.py <x87_ref> <x87_ours> [cases per instruction and mode] [seed]

Random arguments for FSIN, FCOS, FPTAN, FPATAN, F2XM1, FYL2X, FYL2XP1 and FSINCOS go to Rosetta 2 (x87_ref, an x86-64
program) and to VXRuntime's x87 (x87_ours); their results are compared with exact values computed in decimal arithmetic
(110 digits), rounded to 64-bit significands in each rounding mode. FSIN, FCOS, FPTAN and FSINCOS reduce their argument by
Intel's 66-bit value of pi, as x86 processors do: the exact values here do the same. Printed: for each instruction and mode,
how many results are correctly rounded, and how far (units in the last place) the others are.
"""
import os, random, struct, subprocess, sys
from decimal import Decimal as D, getcontext, ROUND_HALF_EVEN, ROUND_FLOOR, ROUND_CEILING

NAMES = ["FSIN", "FCOS", "FPTAN", "FPATAN", "F2XM1", "FYL2X", "FYL2XP1", "FSINCOS"]


def pi(digits):
    getcontext().prec = digits + 10
    def atan_inv(n):
        x = D(1) / n; s = D(0); t = x; k = 1; sign = 1
        while abs(t) > D(10) ** -(digits + 5):
            s += sign * t / k; t /= n * n; k += 2; sign = -sign
        return s
    return 4 * (4 * atan_inv(5) - atan_inv(239))


PI = pi(400)
PI66 = (PI * D(2) ** 64).to_integral_value() / D(2) ** 64          # pi rounded to 66 bits
getcontext().prec = 110
LN2 = D(2).ln()


def value(m, e):                                                     # an 80-bit number
    sign = -1 if e >> 15 else 1
    E = e & 0x7FFF
    return sign * D(m) * D(2) ** ((E if E else 1) - 16383 - 63)


def rounded(v, rc):                                                  # to 80 bits: (significand, sign and exponent)
    if v == 0:
        return (0, 0)
    sign = 0x8000 if v < 0 else 0
    a = abs(v)
    e = int(a.ln() / LN2) - 2
    while a >= D(2) ** (e + 1): e += 1
    while a < D(2) ** e: e -= 1
    mode = ROUND_HALF_EVEN if rc == 0 else (ROUND_CEILING if sign else ROUND_FLOOR) if rc == 1 else \
           (ROUND_FLOOR if sign else ROUND_CEILING) if rc == 2 else ROUND_FLOOR
    m = int((a / D(2) ** (e - 63)).to_integral_value(rounding=mode))
    if m == 1 << 64:
        m >>= 1; e += 1
    return (m, sign | (e + 16383))


def series_sin_cos(r):
    s = D(0); t = r; n = 1
    while abs(t) > D(10) ** -105:
        s += t; t = -t * r * r / ((n + 1) * (n + 2)); n += 2
    c = D(0); t = D(1); n = 0
    while abs(t) > D(10) ** -105:
        c += t; t = -t * r * r / ((n + 1) * (n + 2)); n += 2
    return s, c


def sin_cos(x):                                                      # Intel's model: x - k * pi66/2, then the quadrant
    getcontext().prec = 400
    k = (x / (PI66 / 2)).to_integral_value()
    r = x - k * (PI66 / 2)
    getcontext().prec = 110
    s, c = series_sin_cos(+r)
    return [(s, c), (c, -s), (-s, -c), (-c, s)][int(k) % 4]


def atan(z):
    neg = z < 0; z = abs(z); inv = z > 1
    if inv: z = 1 / z
    k = 0
    while z > D("0.1"):
        z = z / (1 + (1 + z * z).sqrt()); k += 1
    s = D(0); t = z; n = 1; sign = 1
    while abs(t) > D(10) ** -105:
        s += sign * t / n; t *= z * z; n += 2; sign = -sign
    r = s * 2 ** k
    if inv: r = PI / 2 - r
    return -r if neg else r


def atan2(y, x):
    if x > 0: return atan(y / x)
    if x < 0: return atan(y / x) + (PI if y >= 0 else -PI)
    return PI / 2 if y > 0 else -PI / 2


def exact(op, x, y):
    if op == 0: return [sin_cos(x)[0]]
    if op == 1: return [sin_cos(x)[1]]
    if op == 2: s, c = sin_cos(x); return [s / c, D(1)]
    if op == 3: return [atan2(y, x)]
    if op == 4: return [(x * LN2).exp() - 1]
    if op == 5: return [y * x.ln() / LN2]
    if op == 6: return [y * (1 + x).ln() / LN2]
    s, c = sin_cos(x); return [s, c]


def number(lo, hi, positive=False):                                  # an 80-bit number with an exponent in [lo, hi]
    m = random.getrandbits(64) | 1 << 63
    s = 0 if positive else random.getrandbits(1) << 15
    return (m, s | (16383 + random.randint(lo, hi)))


def arguments(op, i):
    if op in (0, 1, 2, 7): return (number(-20, 62) if i % 3 else number(-3, 1)), (0, 0)
    if op == 3: return number(-30, 30), number(-30, 30)
    if op == 4: return number(-40, -1), (0, 0)
    if op == 5: return number(-60, 60, True), number(-10, 10)
    return number(-40, -3), number(-10, 10)


def run(prog, recs, rc):
    data = b"".join(struct.pack("<IQHQH", op, x[0], x[1], y[0], y[1]) for op, x, y in recs)
    out = subprocess.run(prog, input=data, capture_output=True, env=dict(os.environ, RC=str(rc)), check=True).stdout
    return [struct.unpack_from("<QHQH", out, 22 * i) for i in range(len(recs))]


def ulps(a, b):                                                      # distance of two 80-bit numbers of one sign
    def key(m, e): return (e & 0x7FFF) << 63 | (m & ~(1 << 63))
    if (a[1] ^ b[1]) & 0x8000: return None
    return abs(key(*a) - key(*b))


def main():
    ref, ours = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 500
    random.seed(int(sys.argv[4]) if len(sys.argv) > 4 else 1)
    print(f"{'':10} {'RC':>2}   {'Rosetta 2: correct, else ulps':40} VXRuntime: correct, else ulps")
    for op in range(8):
        recs = [(op,) + arguments(op, i) for i in range(n)]
        want = [exact(op, value(*x), value(*y)) for _, x, y in recs]
        for rc in range(4):
            line = f"{NAMES[op]:10} {rc:>2}  "
            for prog in (["arch", "-x86_64", ref], [ours]):
                res = run(prog, recs, rc)
                ok = 0; far = {}
                for w, r in zip(want, res):
                    got = [(r[0], r[1]), (r[2], r[3])][:len(w)]
                    for wv, g in zip(w, got):
                        cr = rounded(wv, rc)
                        if cr == g: ok += 1
                        else:
                            d = ulps(cr, g); k = "far" if d is None or d > 8 else d
                            far[k] = far.get(k, 0) + 1
                total = sum(len(w) for w in want)
                line += f" {ok:5}/{total:<5} {' '.join(f'{k}:{v}' for k, v in sorted(far.items(), key=str)):28}"
            print(line)


if __name__ == "__main__":
    main()
