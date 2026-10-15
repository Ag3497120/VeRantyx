// x87_exact.py's reference side: built for x86-64, run under Rosetta 2. Reads (op, x, y) records (80-bit x and y), runs the
// x87 transcendental instruction op names (0 FSIN, 1 FCOS, 2 FPTAN, 3 FPATAN, 4 F2XM1, 5 FYL2X, 6 FYL2XP1, 7 FSINCOS) with
// RC from the environment variable RC, writes (ST(0), ST(1) for the two-result ones, FSW).
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
typedef struct { uint64_t m; uint16_t e; } __attribute__((packed)) f80;
typedef struct { uint32_t op; f80 x, y; } __attribute__((packed)) rec;
typedef struct { f80 r0, r1; uint16_t sw; } __attribute__((packed)) out;
int main(void){ rec r; out o;
 uint16_t cw = (uint16_t)(0x037F | (getenv("RC") ? atoi(getenv("RC")) : 0) << 10);
 while (fread(&r, sizeof r, 1, stdin) == 1) {
  o.r1.m = 0; o.r1.e = 0;
  switch (r.op) {
  case 0: __asm__ volatile("fninit\n fldcw %[cw]\n fldt %2\n fsin\n fnstsw %1\n fstpt %0" : "=m"(o.r0), "=m"(o.sw) : "m"(r.x), [cw] "m"(cw)); break;
  case 1: __asm__ volatile("fninit\n fldcw %[cw]\n fldt %2\n fcos\n fnstsw %1\n fstpt %0" : "=m"(o.r0), "=m"(o.sw) : "m"(r.x), [cw] "m"(cw)); break;
  case 2: __asm__ volatile("fninit\n fldcw %[cw]\n fldt %3\n fptan\n fnstsw %2\n fstpt %1\n fstpt %0" : "=m"(o.r0), "=m"(o.r1), "=m"(o.sw) : "m"(r.x), [cw] "m"(cw)); break;
  case 3: __asm__ volatile("fninit\n fldcw %[cw]\n fldt %2\n fldt %3\n fpatan\n fnstsw %1\n fstpt %0" : "=m"(o.r0), "=m"(o.sw) : "m"(r.y), "m"(r.x), [cw] "m"(cw)); break;   // atan(y/x)
  case 4: __asm__ volatile("fninit\n fldcw %[cw]\n fldt %2\n f2xm1\n fnstsw %1\n fstpt %0" : "=m"(o.r0), "=m"(o.sw) : "m"(r.x), [cw] "m"(cw)); break;
  case 5: __asm__ volatile("fninit\n fldcw %[cw]\n fldt %2\n fldt %3\n fyl2x\n fnstsw %1\n fstpt %0" : "=m"(o.r0), "=m"(o.sw) : "m"(r.y), "m"(r.x), [cw] "m"(cw)); break;   // y*log2(x)
  case 6: __asm__ volatile("fninit\n fldcw %[cw]\n fldt %2\n fldt %3\n fyl2xp1\n fnstsw %1\n fstpt %0" : "=m"(o.r0), "=m"(o.sw) : "m"(r.y), "m"(r.x), [cw] "m"(cw)); break;
  case 7: __asm__ volatile("fninit\n fldcw %[cw]\n fldt %3\n fsincos\n fnstsw %2\n fstpt %1\n fstpt %0" : "=m"(o.r0), "=m"(o.r1), "=m"(o.sw) : "m"(r.x), [cw] "m"(cw)); break;
  }
  fwrite(&o, sizeof o, 1, stdout);
 }
}
