// DXBC (shader model 4 and 5 bytecode, as fxc and d3dcompiler produce it) to Metal Shading Language.
//
// A DXBC container holds chunks: the input and output signatures (ISGN, OSGN) and the program (SHDR or SHEX), a
// stream of 32-bit tokens: declarations, then instructions, each an opcode token followed by operand tokens.
// Registers are untyped 4-component values; the translation keeps every register as a float4 and reinterprets
// its bits (as_type) where an instruction works on integers. Control flow maps one to one onto structured MSL.
//
// The generated function takes the stage's inputs through a [[stage_in]] struct (vertex attributes by input
// register, or the interpolated values of the previous stage), constant buffers as `constant float4 *cbN
// [[buffer(N)]]`, textures [[texture(N)]] and samplers [[sampler(N)]]. Values passed between stages are named
// after their register (user(rN)), which is how Direct3D links stages too.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dxbc.h"

// ---- text building ------------------------------------------------------------------------------------------------------
typedef struct { char *s; size_t len, cap; } text;
static void put(text *t, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(t->s ? t->s + t->len : NULL, t->s ? t->cap - t->len : 0, fmt, ap);
        va_end(ap);
        if (t->s && t->len + (size_t)n < t->cap) { t->len += (size_t)n; return; }
        t->cap = (t->cap + (size_t)n + 1) * 2;
        t->s = realloc(t->s, t->cap);
        if (!t->len) t->s[0] = 0;
    }
}

// ---- the container ---------------------------------------------------------------------------------------------------------
static const uint8_t *chunk(const uint8_t *c, size_t size, const char *four, uint32_t *len) {
    if (size < 32 || memcmp(c, "DXBC", 4)) return NULL;
    uint32_t count = *(const uint32_t *)(c + 28);
    for (uint32_t i = 0; i < count && 32 + i * 4 + 4 <= size; i++) {
        uint32_t off = *(const uint32_t *)(c + 32 + i * 4);
        if (off + 8 > size) continue;
        if (!memcmp(c + off, four, 4)) { *len = *(const uint32_t *)(c + off + 4); return c + off + 8; }
    }
    return NULL;
}

// ISGN/OSGN elements are 24 bytes; OSG5 has a stream index first; ISG1/OSG1 add precision fields.
static int signature(const uint8_t *data, uint32_t len, int stride, int skip, dxbc_element *out, int max) {
    if (len < 8) return 0;
    uint32_t count = *(const uint32_t *)data;
    int n = 0;
    for (uint32_t i = 0; i < count && n < max; i++) {
        const uint8_t *e = data + 8 + i * (uint32_t)stride + (uint32_t)skip;
        if (e + 24 > data + len) break;
        const uint32_t *f = (const uint32_t *)e;
        dxbc_element *el = &out[n++];
        snprintf(el->semantic, sizeof el->semantic, "%s", f[0] < len ? (const char *)data + f[0] : "");
        el->index = f[1]; el->sysval = f[2]; el->component_type = f[3]; el->reg = f[4]; el->mask = e[20];
    }
    return n;
}
static int find_signature(const uint8_t *c, size_t size, int output, dxbc_element *out, int max) {
    uint32_t len;
    const uint8_t *d;
    if (!output) {
        if ((d = chunk(c, size, "ISGN", &len))) return signature(d, len, 24, 0, out, max);
        if ((d = chunk(c, size, "ISG1", &len))) return signature(d, len, 32, 4, out, max);
    } else {
        if ((d = chunk(c, size, "OSGN", &len))) return signature(d, len, 24, 0, out, max);
        if ((d = chunk(c, size, "OSG5", &len))) return signature(d, len, 28, 4, out, max);
        if ((d = chunk(c, size, "OSG1", &len))) return signature(d, len, 32, 4, out, max);
    }
    return 0;
}
int dxbc_input_signature(const void *code, size_t size, dxbc_element *elements, int max) {
    return find_signature(code, size, 0, elements, max);
}

// ---- tokens --------------------------------------------------------------------------------------------------------------------
enum {
    OP_ADD = 0, OP_AND = 1, OP_BREAK = 2, OP_BREAKC = 3, OP_CASE = 6, OP_CONTINUE = 7, OP_CONTINUEC = 8, OP_DEFAULT = 10,
    OP_DERIV_RTX = 11, OP_DERIV_RTY = 12, OP_DISCARD = 13, OP_DIV = 14, OP_DP2 = 15, OP_DP3 = 16, OP_DP4 = 17, OP_ELSE = 18,
    OP_ENDIF = 21, OP_ENDLOOP = 22, OP_ENDSWITCH = 23, OP_EQ = 24, OP_EXP = 25, OP_FRC = 26, OP_FTOI = 27, OP_FTOU = 28,
    OP_GE = 29, OP_IADD = 30, OP_IF = 31, OP_IEQ = 32, OP_IGE = 33, OP_ILT = 34, OP_IMAD = 35, OP_IMAX = 36, OP_IMIN = 37,
    OP_IMUL = 38, OP_INE = 39, OP_INEG = 40, OP_ISHL = 41, OP_ISHR = 42, OP_ITOF = 43, OP_LD = 45, OP_LD_MS = 46, OP_LOG = 47,
    OP_LOOP = 48, OP_LT = 49, OP_MAD = 50, OP_MIN = 51, OP_MAX = 52, OP_CUSTOMDATA = 53, OP_MOV = 54, OP_MOVC = 55, OP_MUL = 56,
    OP_NE = 57, OP_NOP = 58, OP_NOT = 59, OP_OR = 60, OP_RESINFO = 61, OP_RET = 62, OP_RETC = 63, OP_ROUND_NE = 64,
    OP_ROUND_NI = 65, OP_ROUND_PI = 66, OP_ROUND_Z = 67, OP_RSQ = 68, OP_SAMPLE = 69, OP_SAMPLE_C = 70, OP_SAMPLE_C_LZ = 71,
    OP_SAMPLE_L = 72, OP_SAMPLE_D = 73, OP_SAMPLE_B = 74, OP_SQRT = 75, OP_SWITCH = 76, OP_SINCOS = 77, OP_UDIV = 78,
    OP_ULT = 79, OP_UGE = 80, OP_UMUL = 81, OP_UMAD = 82, OP_UMAX = 83, OP_UMIN = 84, OP_USHR = 85, OP_UTOF = 86, OP_XOR = 87,
    OP_DCL_RESOURCE = 88, OP_DCL_CONSTANT_BUFFER = 89, OP_DCL_SAMPLER = 90, OP_DCL_INDEX_RANGE = 91, OP_DCL_INPUT = 95,
    OP_DCL_INPUT_SGV = 96, OP_DCL_INPUT_SIV = 97, OP_DCL_INPUT_PS = 98, OP_DCL_INPUT_PS_SGV = 99, OP_DCL_INPUT_PS_SIV = 100,
    OP_DCL_OUTPUT = 101, OP_DCL_OUTPUT_SGV = 102, OP_DCL_OUTPUT_SIV = 103, OP_DCL_TEMPS = 104, OP_DCL_INDEXABLE_TEMP = 105,
    OP_DCL_GLOBAL_FLAGS = 106, OP_LOD = 108, OP_GATHER4 = 109, OP_DERIV_RTX_COARSE = 122, OP_DERIV_RTX_FINE = 123,
    OP_DERIV_RTY_COARSE = 124, OP_DERIV_RTY_FINE = 125, OP_RCP = 129, OP_F32TOF16 = 130, OP_F16TOF32 = 131, OP_COUNTBITS = 134,
    OP_FIRSTBIT_HI = 135, OP_FIRSTBIT_LO = 136, OP_FIRSTBIT_SHI = 137, OP_UBFE = 138, OP_IBFE = 139, OP_BFI = 140, OP_BFREV = 141,
};
enum {
    OPD_TEMP = 0, OPD_INPUT = 1, OPD_OUTPUT = 2, OPD_INDEXABLE_TEMP = 3, OPD_IMM32 = 4, OPD_IMM64 = 5, OPD_SAMPLER = 6,
    OPD_RESOURCE = 7, OPD_CB = 8, OPD_ICB = 9, OPD_INPUT_PRIMITIVEID = 11, OPD_OUTPUT_DEPTH = 12, OPD_NULL = 13,
    OPD_OUTPUT_COVERAGE = 15, OPD_INPUT_COVERAGE = 35, OPD_OUTPUT_DEPTH_GE = 38, OPD_OUTPUT_DEPTH_LE = 39,
};

typedef struct operand {
    int type, ncomp, mode;
    uint8_t mask, swz[4];
    int dims;
    uint32_t index[3];
    struct operand *rel[3];       // a relative part of each index, or NULL
    uint32_t value[4];
    int neg, abs;
} operand;

typedef struct { const uint32_t *p, *end; int bad; } reader;
static uint32_t next(reader *r) { if (r->p >= r->end) { r->bad = 1; return 0; } return *r->p++; }

// Operands live in a small pool per instruction (relative indices are operands too).
typedef struct { operand items[16]; int used; } pool;
static operand *alloc_operand(pool *pl) { return pl->used < 16 ? &pl->items[pl->used++] : &pl->items[15]; }

static void read_operand(reader *r, pool *pl, operand *o) {
    memset(o, 0, sizeof *o);
    uint32_t t = next(r);
    o->ncomp = (int)(t & 3);
    o->type = (int)((t >> 12) & 0xff);
    o->dims = (int)((t >> 20) & 3);
    if (o->ncomp == 2) {                                        // four components
        o->mode = (int)((t >> 2) & 3);
        if (o->mode == 0) { o->mask = (uint8_t)((t >> 4) & 0xf); for (int i = 0; i < 4; i++) o->swz[i] = (uint8_t)i; }
        else if (o->mode == 1) for (int i = 0; i < 4; i++) o->swz[i] = (uint8_t)((t >> (4 + 2 * i)) & 3);
        else { uint8_t c = (uint8_t)((t >> 4) & 3); for (int i = 0; i < 4; i++) o->swz[i] = c; }
    } else {
        o->mask = 1;
        for (int i = 0; i < 4; i++) o->swz[i] = 0;
    }
    if (t & 0x80000000u) {                                      // extended operand: modifiers
        uint32_t ext = next(r);
        if ((ext & 0x3f) == 1) { uint32_t m = (ext >> 6) & 0xff; o->neg = m == 1 || m == 3; o->abs = m == 2 || m == 3; }
    }
    if (o->type == OPD_IMM32) {
        int n = o->ncomp == 2 ? 4 : 1;
        for (int i = 0; i < n; i++) o->value[i] = next(r);
        if (n == 1) o->value[1] = o->value[2] = o->value[3] = o->value[0];
    } else if (o->type == OPD_IMM64) {
        int n = o->ncomp == 2 ? 4 : 1;
        for (int i = 0; i < n * 2; i++) next(r);                // doubles are not supported
    }
    for (int d = 0; d < o->dims; d++) {
        uint32_t rep = (t >> (22 + 3 * d)) & 7;
        if (rep == 0 || rep == 3) o->index[d] = next(r);
        else if (rep == 1 || rep == 4) { o->index[d] = next(r); next(r); }      // 64-bit: low word first
        if (rep == 2 || rep == 3 || rep == 4) { o->rel[d] = alloc_operand(pl); read_operand(r, pl, o->rel[d]); }
    }
}

// ---- generation state ------------------------------------------------------------------------------------------------------------
typedef struct {
    dxbc_shader *sh;
    text body;
    int indent;
    int temps;
    struct { uint32_t reg, size; } xt[32];
    int nxt;
    uint8_t in_decl[32], out_decl[32];
    uint32_t in_sv[32], out_sv[32], in_interp[32];
    uint8_t in_mask[32];
    int out_depth, in_primid, in_coverage, out_coverage;
    uint8_t tex_dim[128], tex_ret[128], tex_cmp[128];
    uint32_t cb_size[16];
    uint32_t *icb, icb_n;
    char why[200];
} gen;

static void line(gen *g, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    put(&g->body, "%*s%s\n", 4 + g->indent * 4, "", buf);
}
static void fail(gen *g, const char *fmt, ...) {
    if (g->why[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->why, sizeof g->why, fmt, ap);
    va_end(ap);
}

enum { K_FLOAT, K_INT, K_UINT };
static const char *type_name(int kind, int n) {
    static const char *names[3][5] = { { "", "float", "float2", "float3", "float4" }, { "", "int", "int2", "int3", "int4" },
                                       { "", "uint", "uint2", "uint3", "uint4" } };
    return names[kind][n < 1 ? 1 : n > 4 ? 4 : n];
}
static const char CH[] = "xyzw";

// A destination's enabled components.
static int dest_comps(const operand *d, int *comps) {
    int n = 0;
    if (d->ncomp != 2) { comps[n++] = 0; return n; }
    for (int c = 0; c < 4; c++) if (d->mask & (1 << c)) comps[n++] = c;
    return n;
}
static int scalar_register(const operand *o) {
    return o->type == OPD_OUTPUT_DEPTH || o->type == OPD_OUTPUT_DEPTH_GE || o->type == OPD_OUTPUT_DEPTH_LE ||
           o->type == OPD_OUTPUT_COVERAGE || o->type == OPD_INPUT_COVERAGE || o->type == OPD_INPUT_PRIMITIVEID;
}

static void src(gen *g, const operand *o, const int *comps, int n, int kind, char *out, size_t cap);

// The index of a register: an immediate, plus a relative part read as an integer.
static void index_text(gen *g, const operand *o, int d, char *out, size_t cap) {
    if (!o->rel[d]) { snprintf(out, cap, "%u", o->index[d]); return; }
    char rel[256];
    int c0 = 0;
    src(g, o->rel[d], &c0, 1, K_INT, rel, sizeof rel);
    if (o->index[d]) snprintf(out, cap, "(%u + %s)", o->index[d], rel);
    else snprintf(out, cap, "%s", rel);
}

// The float4 (or scalar) a register operand names.
static void reg_text(gen *g, const operand *o, char *out, size_t cap) {
    char i0[300], i1[300];
    switch (o->type) {
    case OPD_TEMP: snprintf(out, cap, "r%u", o->index[0]); break;
    case OPD_INPUT: snprintf(out, cap, "v%u", o->index[o->dims - 1]); break;
    case OPD_OUTPUT: snprintf(out, cap, "o%u", o->index[0]); break;
    case OPD_INDEXABLE_TEMP: index_text(g, o, 1, i1, sizeof i1); snprintf(out, cap, "x%u[%s]", o->index[0], i1); break;
    case OPD_CB: index_text(g, o, 1, i1, sizeof i1); snprintf(out, cap, "cb%u[%s]", o->index[0], i1); break;
    case OPD_ICB: index_text(g, o, 0, i0, sizeof i0); snprintf(out, cap, "icb[%s]", i0); break;
    case OPD_INPUT_PRIMITIVEID: snprintf(out, cap, "vPrim"); break;
    case OPD_INPUT_COVERAGE: snprintf(out, cap, "vCoverage"); break;
    case OPD_OUTPUT_DEPTH: case OPD_OUTPUT_DEPTH_GE: case OPD_OUTPUT_DEPTH_LE: snprintf(out, cap, "oDepth"); break;
    case OPD_OUTPUT_COVERAGE: snprintf(out, cap, "oMask"); break;
    default: fail(g, "operand type %d is not supported", o->type); snprintf(out, cap, "r0"); break;
    }
}

// A source operand as a value of `n` components of the given kind, its swizzle taken at `comps`.
static void src(gen *g, const operand *o, const int *comps, int n, int kind, char *out, size_t cap) {
    if (o->type == OPD_IMM32) {
        char vals[128] = "";
        size_t k = 0;
        for (int i = 0; i < n; i++) k += (size_t)snprintf(vals + k, sizeof vals - k, "%s0x%xu", i ? ", " : "", o->value[o->ncomp == 2 ? comps[i] : 0]);
        if (kind == K_UINT) snprintf(out, cap, "%s(%s)", type_name(K_UINT, n), vals);
        else snprintf(out, cap, "as_type<%s>(%s(%s))", type_name(kind, n), type_name(K_UINT, n), vals);
        return;
    }
    char reg[400], sw[5] = "";
    reg_text(g, o, reg, sizeof reg);
    char base[512];
    if (scalar_register(o)) {
        if (n == 1) snprintf(base, sizeof base, "%s", reg);
        else snprintf(base, sizeof base, "float%d(%s)", n, reg);
    } else {
        for (int i = 0; i < n; i++) sw[i] = CH[o->mode == 0 ? comps[i] : o->swz[comps[i]]];
        sw[n] = 0;
        snprintf(base, sizeof base, "%s.%s", reg, sw);
    }
    if (kind == K_FLOAT) {
        char t[600];
        snprintf(t, sizeof t, o->abs ? "abs(%s)" : "%s", base);
        snprintf(out, cap, o->neg ? "(-%s)" : "%s", t);
    } else {
        char t[600];
        snprintf(t, sizeof t, "as_type<%s>(%s)", type_name(kind, n), base);
        if (o->abs && kind == K_INT) { char a[620]; snprintf(a, sizeof a, "abs(%s)", t); snprintf(t, sizeof t, "%s", a); }
        snprintf(out, cap, o->neg ? "(-%s)" : "%s", t);
    }
}

// Assigns an expression of `n` components and the given kind to a destination.
static void assign(gen *g, const operand *d, const char *expr, int kind, int n, int saturate) {
    if (d->type == OPD_NULL) return;
    char reg[400], lhs[420];
    reg_text(g, d, reg, sizeof reg);
    if (scalar_register(d)) snprintf(lhs, sizeof lhs, "%s", reg);
    else {
        int comps[4], m = dest_comps(d, comps);
        char sw[5];
        for (int i = 0; i < m; i++) sw[i] = CH[comps[i]];
        sw[m] = 0;
        snprintf(lhs, sizeof lhs, "%s.%s", reg, sw);
    }
    if (d->type == OPD_OUTPUT_COVERAGE) { line(g, "%s = as_type<uint>(%s);", lhs, expr); return; }
    if (kind == K_FLOAT) line(g, saturate ? "%s = saturate(%s);" : "%s = %s;", lhs, expr);
    else line(g, "%s = as_type<%s>(%s);", lhs, type_name(K_FLOAT, n), expr);
}

// ---- instructions --------------------------------------------------------------------------------------------------------------
typedef struct { uint32_t op, token; int saturate, test_nonzero; int offset[3]; operand ops[8]; int nops; uint32_t ret_type; } instr;

// Component-wise operations: the template gets the sources as %1$s, %2$s, %3$s (each `n` components of `kind`).
static void componentwise(gen *g, const instr *in, int kind, int result_kind, const char *fmt) {
    int comps[4], n = dest_comps(&in->ops[0], comps);
    if (in->ops[0].type == OPD_NULL) return;
    char a[700] = "", b[700] = "", c[700] = "", expr[2400];
    if (in->nops > 1) src(g, &in->ops[1], comps, n, kind, a, sizeof a);
    if (in->nops > 2) src(g, &in->ops[2], comps, n, kind, b, sizeof b);
    if (in->nops > 3) src(g, &in->ops[3], comps, n, kind, c, sizeof c);
    snprintf(expr, sizeof expr, fmt, a, b, c, type_name(result_kind, n), type_name(K_UINT, n));
    assign(g, &in->ops[0], expr, result_kind, n, in->saturate);
}
// Comparisons give all ones or zero per component.
static void compare(gen *g, const instr *in, int kind, const char *op) {
    char fmt[128];
    snprintf(fmt, sizeof fmt, "select(%%5$s(0u), %%5$s(0xffffffffu), %%1$s %s %%2$s)", op);
    componentwise(g, in, kind, K_UINT, fmt);
}
static void dot(gen *g, const instr *in, int width) {
    int comps[4] = { 0, 1, 2, 3 }, dc[4], n = dest_comps(&in->ops[0], dc);
    char a[700], b[700], expr[1500];
    src(g, &in->ops[1], comps, width, K_FLOAT, a, sizeof a);
    src(g, &in->ops[2], comps, width, K_FLOAT, b, sizeof b);
    snprintf(expr, sizeof expr, n > 1 ? "%s(dot(%s, %s))" : "%.0sdot(%s, %s)", type_name(K_FLOAT, n), a, b);
    assign(g, &in->ops[0], expr, K_FLOAT, n, in->saturate);
}
// The condition of if/breakc/continuec/retc/discard: one component tested against zero.
static void condition(gen *g, const instr *in, const operand *o, char *out, size_t cap) {
    char v[700];
    int c0 = 0;
    src(g, o, &c0, 1, K_UINT, v, sizeof v);
    snprintf(out, cap, in->test_nonzero ? "%s != 0u" : "%s == 0u", v);
}

// Texture coordinates and the sampling call, by resource dimension.
enum { DIM_BUFFER = 1, DIM_1D = 2, DIM_2D = 3, DIM_2DMS = 4, DIM_3D = 5, DIM_CUBE = 6, DIM_1DARRAY = 7, DIM_2DARRAY = 8,
       DIM_2DMSARRAY = 9, DIM_CUBEARRAY = 10 };
static void coords(gen *g, const operand *o, int dim, int kind, char *xy, size_t cap, char *slice, size_t scap) {
    int c4[4] = { 0, 1, 2, 3 };
    int n = dim == DIM_1D || dim == DIM_1DARRAY || dim == DIM_BUFFER ? 1 : dim == DIM_3D || dim == DIM_CUBE || dim == DIM_CUBEARRAY ? 3 : 2;
    src(g, o, c4, n, kind, xy, cap);
    slice[0] = 0;
    int slice_comp = dim == DIM_1DARRAY ? 1 : dim == DIM_2DARRAY || dim == DIM_2DMSARRAY ? 2 : dim == DIM_CUBEARRAY ? 3 : -1;
    if (slice_comp >= 0) {
        char s[700];
        src(g, o, &slice_comp, 1, kind, s, sizeof s);
        snprintf(slice, scap, kind == K_FLOAT ? "uint(rint(%s))" : "uint(%s)", s);
    }
}
static void sample(gen *g, const instr *in) {
    const operand *dst = &in->ops[0], *addr = &in->ops[1], *res = &in->ops[2], *smp = in->nops > 3 ? &in->ops[3] : NULL;
    uint32_t t = res->index[0];
    int dim = t < 128 ? g->tex_dim[t] : DIM_2D;
    char xy[700], slice[720], extra[1600] = "", a[700], b[700];
    int c0 = 0, c4[4] = { 0, 1, 2, 3 };
    coords(g, addr, dim, K_FLOAT, xy, sizeof xy, slice, sizeof slice);
    char args[2400];
    snprintf(args, sizeof args, "s%u, %s%s%s", smp ? smp->index[0] : 0, xy, slice[0] ? ", " : "", slice);
    int gn = dim == DIM_3D || dim == DIM_CUBE || dim == DIM_CUBEARRAY ? 3 : 2;
    const char *grad = dim == DIM_3D ? "gradient3d" : dim == DIM_CUBE || dim == DIM_CUBEARRAY ? "gradientcube" : "gradient2d";
    int compare = 0;
    switch (in->op) {
    case OP_SAMPLE_L: src(g, &in->ops[4], &c0, 1, K_FLOAT, a, sizeof a); snprintf(extra, sizeof extra, ", level(%s)", a); break;
    case OP_SAMPLE_B: src(g, &in->ops[4], &c0, 1, K_FLOAT, a, sizeof a); snprintf(extra, sizeof extra, ", bias(%s)", a); break;
    case OP_SAMPLE_D:
        src(g, &in->ops[4], c4, gn, K_FLOAT, a, sizeof a); src(g, &in->ops[5], c4, gn, K_FLOAT, b, sizeof b);
        snprintf(extra, sizeof extra, ", %s(%s, %s)", grad, a, b);
        break;
    case OP_SAMPLE_C: case OP_SAMPLE_C_LZ:
        compare = 1;
        g->tex_cmp[t] = 1;
        src(g, &in->ops[4], &c0, 1, K_FLOAT, a, sizeof a);
        snprintf(extra, sizeof extra, ", %s%s", a, in->op == OP_SAMPLE_C_LZ ? ", level(0)" : "");
        break;
    }
    if ((in->offset[0] || in->offset[1] || in->offset[2]) && dim != DIM_CUBE && dim != DIM_CUBEARRAY) {
        size_t k = strlen(extra);
        if (dim == DIM_3D) snprintf(extra + k, sizeof extra - k, ", int3(%d, %d, %d)", in->offset[0], in->offset[1], in->offset[2]);
        else snprintf(extra + k, sizeof extra - k, ", int2(%d, %d)", in->offset[0], in->offset[1]);
    }
    char call[4200];
    if (compare) snprintf(call, sizeof call, "float4(t%u.sample_compare(%s%s))", t, args, extra);
    else snprintf(call, sizeof call, "t%u.sample(%s%s)", t, args, extra);
    // The resource operand's swizzle picks from the result; the destination takes its components.
    int comps[4], n = dest_comps(dst, comps);
    char sw[5];
    for (int i = 0; i < n; i++) sw[i] = CH[res->swz[comps[i]]];
    sw[n] = 0;
    char expr[4300];
    int kind = g->tex_ret[t] == 3 ? K_INT : g->tex_ret[t] == 4 ? K_UINT : K_FLOAT;
    snprintf(expr, sizeof expr, "%s.%s", call, sw);
    assign(g, dst, expr, kind, n, in->saturate);
}
static void load(gen *g, const instr *in) {
    const operand *dst = &in->ops[0], *addr = &in->ops[1], *res = &in->ops[2];
    uint32_t t = res->index[0];
    int dim = t < 128 ? g->tex_dim[t] : DIM_2D;
    char xy[700], slice[720], mip[700] = "0", call[2400];
    coords(g, addr, dim, K_INT, xy, sizeof xy, slice, sizeof slice);
    int c3 = 3;
    if (dim != DIM_BUFFER && dim != DIM_2DMS && dim != DIM_2DMSARRAY) src(g, addr, &c3, 1, K_INT, mip, sizeof mip);
    const char *ut = dim == DIM_1D || dim == DIM_1DARRAY || dim == DIM_BUFFER ? "uint" : dim == DIM_3D ? "uint3" : "uint2";
    if (dim == DIM_BUFFER) snprintf(call, sizeof call, "t%u.read(uint(%s))", t, xy);
    else if (dim == DIM_2DMS || dim == DIM_2DMSARRAY) {
        char s[700]; int c0 = 0;
        src(g, &in->ops[3], &c0, 1, K_INT, s, sizeof s);
        snprintf(call, sizeof call, "t%u.read(%s(%s)%s%s, uint(%s))", t, ut, xy, slice[0] ? ", " : "", slice, s);
    } else snprintf(call, sizeof call, "t%u.read(%s(%s)%s%s, uint(%s))", t, ut, xy, slice[0] ? ", " : "", slice, mip);
    if (g->tex_cmp[t]) { char c[2500]; snprintf(c, sizeof c, "float4(%s)", call); snprintf(call, sizeof call, "%s", c); }
    int comps[4], n = dest_comps(dst, comps);
    char sw[5];
    for (int i = 0; i < n; i++) sw[i] = CH[res->swz[comps[i]]];
    sw[n] = 0;
    char expr[2500];
    int kind = g->tex_ret[t] == 3 ? K_INT : g->tex_ret[t] == 4 ? K_UINT : K_FLOAT;
    snprintf(expr, sizeof expr, "%s.%s", call, sw);
    assign(g, dst, expr, kind, n, in->saturate);
}
// resinfo: width, height, depth or array size, mip count, as floats (or 1/x, or uints).
static void resinfo(gen *g, const instr *in) {
    const operand *dst = &in->ops[0], *res = &in->ops[2];
    uint32_t t = res->index[0];
    int dim = t < 128 ? g->tex_dim[t] : DIM_2D;
    char mip[700]; int c0 = 0;
    src(g, &in->ops[1], &c0, 1, K_UINT, mip, sizeof mip);
    char v[4][300];
    snprintf(v[0], 300, "t%u.get_width(%s)", t, dim == DIM_BUFFER ? "" : mip);
    snprintf(v[1], 300, dim == DIM_1D || dim == DIM_1DARRAY || dim == DIM_BUFFER ? "0u" : "t%u.get_height(%s)", t, mip);
    if (dim == DIM_3D) snprintf(v[2], 300, "t%u.get_depth(%s)", t, mip);
    else if (dim == DIM_1DARRAY || dim == DIM_2DARRAY || dim == DIM_CUBEARRAY || dim == DIM_2DMSARRAY) snprintf(v[2], 300, "t%u.get_array_size()", t);
    else snprintf(v[2], 300, "0u");
    snprintf(v[3], 300, dim == DIM_BUFFER || dim == DIM_2DMS || dim == DIM_2DMSARRAY ? "1u" : "t%u.get_num_mip_levels()", t);
    if (dim == DIM_BUFFER) snprintf(v[0], 300, "t%u.get_width()", t);
    line(g, "{ uint4 vx_info = uint4(%s, %s, %s, %s);", v[0], v[1], v[2], v[3]);
    int comps[4], n = dest_comps(dst, comps);
    char sw[5];
    for (int i = 0; i < n; i++) sw[i] = CH[res->swz[comps[i]]];
    sw[n] = 0;
    char expr[200];
    uint32_t rt = in->ret_type;
    if (rt == 2) { snprintf(expr, sizeof expr, "vx_info.%s", sw); assign(g, dst, expr, K_UINT, n, 0); }
    else if (rt == 1) { snprintf(expr, sizeof expr, "1.0 / %s(vx_info.%s)", type_name(K_FLOAT, n), sw); assign(g, dst, expr, K_FLOAT, n, in->saturate); }
    else { snprintf(expr, sizeof expr, "%s(vx_info.%s)", type_name(K_FLOAT, n), sw); assign(g, dst, expr, K_FLOAT, n, in->saturate); }
    line(g, "}");
}
// Bit-field instructions take per-component offsets and widths: one statement per component.
static void bitfield(gen *g, const instr *in) {
    int comps[4], n = dest_comps(&in->ops[0], comps);
    for (int i = 0; i < n; i++) {
        operand d = in->ops[0];
        d.mask = (uint8_t)(1 << comps[i]);
        char w[700], o[700], v[700], b[700], expr[3000];
        int c = comps[i];
        src(g, &in->ops[1], &c, 1, K_UINT, w, sizeof w);
        src(g, &in->ops[2], &c, 1, K_UINT, o, sizeof o);
        if (in->op == OP_BFI) {
            src(g, &in->ops[3], &c, 1, K_UINT, v, sizeof v);
            src(g, &in->ops[4], &c, 1, K_UINT, b, sizeof b);
            snprintf(expr, sizeof expr, "insert_bits(%s, %s, (%s) & 31u, (%s) & 31u)", b, v, o, w);
            assign(g, &d, expr, K_UINT, 1, 0);
        } else {
            int k = in->op == OP_IBFE ? K_INT : K_UINT;
            src(g, &in->ops[3], &c, 1, k, v, sizeof v);
            snprintf(expr, sizeof expr, "((%s) & 31u) == 0u ? %s : extract_bits(%s, (%s) & 31u, (%s) & 31u)", w, v, v, o, w);
            assign(g, &d, expr, k, 1, 0);
        }
    }
}

static void translate(gen *g, const instr *in) {
    switch (in->op) {
    case OP_MOV: componentwise(g, in, K_FLOAT, K_FLOAT, "%1$s"); break;
    case OP_ADD: componentwise(g, in, K_FLOAT, K_FLOAT, "%1$s + %2$s"); break;
    case OP_MUL: componentwise(g, in, K_FLOAT, K_FLOAT, "%1$s * %2$s"); break;
    case OP_DIV: componentwise(g, in, K_FLOAT, K_FLOAT, "%1$s / %2$s"); break;
    case OP_MAD: componentwise(g, in, K_FLOAT, K_FLOAT, "fma(%1$s, %2$s, %3$s)"); break;
    case OP_MIN: componentwise(g, in, K_FLOAT, K_FLOAT, "fmin(%1$s, %2$s)"); break;
    case OP_MAX: componentwise(g, in, K_FLOAT, K_FLOAT, "fmax(%1$s, %2$s)"); break;
    case OP_FRC: componentwise(g, in, K_FLOAT, K_FLOAT, "fract(%1$s)"); break;
    case OP_EXP: componentwise(g, in, K_FLOAT, K_FLOAT, "exp2(%1$s)"); break;
    case OP_LOG: componentwise(g, in, K_FLOAT, K_FLOAT, "log2(%1$s)"); break;
    case OP_SQRT: componentwise(g, in, K_FLOAT, K_FLOAT, "sqrt(%1$s)"); break;
    case OP_RSQ: componentwise(g, in, K_FLOAT, K_FLOAT, "rsqrt(%1$s)"); break;
    case OP_RCP: componentwise(g, in, K_FLOAT, K_FLOAT, "(1.0 / %1$s)"); break;
    case OP_ROUND_NE: componentwise(g, in, K_FLOAT, K_FLOAT, "rint(%1$s)"); break;
    case OP_ROUND_NI: componentwise(g, in, K_FLOAT, K_FLOAT, "floor(%1$s)"); break;
    case OP_ROUND_PI: componentwise(g, in, K_FLOAT, K_FLOAT, "ceil(%1$s)"); break;
    case OP_ROUND_Z: componentwise(g, in, K_FLOAT, K_FLOAT, "trunc(%1$s)"); break;
    case OP_DERIV_RTX: case OP_DERIV_RTX_COARSE: case OP_DERIV_RTX_FINE: componentwise(g, in, K_FLOAT, K_FLOAT, "dfdx(%1$s)"); break;
    case OP_DERIV_RTY: case OP_DERIV_RTY_COARSE: case OP_DERIV_RTY_FINE: componentwise(g, in, K_FLOAT, K_FLOAT, "dfdy(%1$s)"); break;
    case OP_EQ: compare(g, in, K_FLOAT, "=="); break;
    case OP_NE: compare(g, in, K_FLOAT, "!="); break;
    case OP_LT: compare(g, in, K_FLOAT, "<"); break;
    case OP_GE: compare(g, in, K_FLOAT, ">="); break;
    case OP_IEQ: compare(g, in, K_INT, "=="); break;
    case OP_INE: compare(g, in, K_INT, "!="); break;
    case OP_ILT: compare(g, in, K_INT, "<"); break;
    case OP_IGE: compare(g, in, K_INT, ">="); break;
    case OP_ULT: compare(g, in, K_UINT, "<"); break;
    case OP_UGE: compare(g, in, K_UINT, ">="); break;
    case OP_IADD: componentwise(g, in, K_INT, K_INT, "%1$s + %2$s"); break;
    case OP_INEG: componentwise(g, in, K_INT, K_INT, "(-%1$s)"); break;
    case OP_IMAD: componentwise(g, in, K_INT, K_INT, "%1$s * %2$s + %3$s"); break;
    case OP_UMAD: componentwise(g, in, K_UINT, K_UINT, "%1$s * %2$s + %3$s"); break;
    case OP_IMIN: componentwise(g, in, K_INT, K_INT, "min(%1$s, %2$s)"); break;
    case OP_IMAX: componentwise(g, in, K_INT, K_INT, "max(%1$s, %2$s)"); break;
    case OP_UMIN: componentwise(g, in, K_UINT, K_UINT, "min(%1$s, %2$s)"); break;
    case OP_UMAX: componentwise(g, in, K_UINT, K_UINT, "max(%1$s, %2$s)"); break;
    case OP_AND: componentwise(g, in, K_UINT, K_UINT, "%1$s & %2$s"); break;
    case OP_OR: componentwise(g, in, K_UINT, K_UINT, "%1$s | %2$s"); break;
    case OP_XOR: componentwise(g, in, K_UINT, K_UINT, "%1$s ^ %2$s"); break;
    case OP_NOT: componentwise(g, in, K_UINT, K_UINT, "~%1$s"); break;
    case OP_ISHL: componentwise(g, in, K_INT, K_INT, "%1$s << (as_type<%5$s>(%2$s) & 31u)"); break;
    case OP_ISHR: componentwise(g, in, K_INT, K_INT, "%1$s >> (as_type<%5$s>(%2$s) & 31u)"); break;
    case OP_USHR: componentwise(g, in, K_UINT, K_UINT, "%1$s >> (%2$s & 31u)"); break;
    case OP_ITOF: componentwise(g, in, K_INT, K_FLOAT, "%4$s(%1$s)"); break;
    case OP_UTOF: componentwise(g, in, K_UINT, K_FLOAT, "%4$s(%1$s)"); break;
    case OP_FTOI: componentwise(g, in, K_FLOAT, K_INT, "%4$s(%1$s)"); break;
    case OP_FTOU: componentwise(g, in, K_FLOAT, K_UINT, "%4$s(%1$s)"); break;
    case OP_COUNTBITS: componentwise(g, in, K_UINT, K_UINT, "popcount(%1$s)"); break;
    case OP_BFREV: componentwise(g, in, K_UINT, K_UINT, "reverse_bits(%1$s)"); break;
    case OP_FIRSTBIT_LO: componentwise(g, in, K_UINT, K_UINT, "select(ctz(%1$s), %4$s(0xffffffffu), %1$s == 0u)"); break;
    case OP_FIRSTBIT_HI: componentwise(g, in, K_UINT, K_UINT, "select(%4$s(31u) - clz(%1$s), %4$s(0xffffffffu), %1$s == 0u)"); break;
    case OP_FIRSTBIT_SHI: componentwise(g, in, K_INT, K_UINT, "select(%4$s(31u) - clz(as_type<%4$s>(select(%1$s, ~%1$s, %1$s < 0))), %4$s(0xffffffffu), %1$s == 0 || %1$s == -1)"); break;
    case OP_F32TOF16: componentwise(g, in, K_FLOAT, K_UINT, "%4$s(as_type<ushort>(half(%1$s)))"); break;
    case OP_F16TOF32: componentwise(g, in, K_UINT, K_FLOAT, "float(as_type<half>(ushort(%1$s & 0xffffu)))"); break;
    case OP_UBFE: case OP_IBFE: case OP_BFI: bitfield(g, in); break;
    case OP_MOVC: {
        int comps[4], n = dest_comps(&in->ops[0], comps);
        char c[700], a[700], b[700], expr[2200];
        src(g, &in->ops[1], comps, n, K_UINT, c, sizeof c);
        src(g, &in->ops[2], comps, n, K_FLOAT, a, sizeof a);
        src(g, &in->ops[3], comps, n, K_FLOAT, b, sizeof b);
        snprintf(expr, sizeof expr, "select(%s, %s, %s != 0u)", b, a, c);
        assign(g, &in->ops[0], expr, K_FLOAT, n, in->saturate);
        break;
    }
    case OP_DP2: dot(g, in, 2); break;
    case OP_DP3: dot(g, in, 3); break;
    case OP_DP4: dot(g, in, 4); break;
    case OP_SINCOS: {                                         // dst_sin, dst_cos, src
        for (int k = 0; k < 2; k++) {
            if (in->ops[k].type == OPD_NULL) continue;
            int comps[4], n = dest_comps(&in->ops[k], comps);
            char a[700], expr[800];
            src(g, &in->ops[2], comps, n, K_FLOAT, a, sizeof a);
            snprintf(expr, sizeof expr, k ? "cos(%s)" : "sin(%s)", a);
            assign(g, &in->ops[k], expr, K_FLOAT, n, in->saturate);
        }
        break;
    }
    case OP_IMUL: case OP_UMUL: {                              // dst_hi, dst_lo, a, b
        int kind = in->op == OP_IMUL ? K_INT : K_UINT;
        for (int k = 0; k < 2; k++) {
            if (in->ops[k].type == OPD_NULL) continue;
            int comps[4], n = dest_comps(&in->ops[k], comps);
            char a[700], b[700], expr[1600];
            src(g, &in->ops[2], comps, n, kind, a, sizeof a);
            src(g, &in->ops[3], comps, n, kind, b, sizeof b);
            snprintf(expr, sizeof expr, k ? "%s * %s" : "mulhi(%s, %s)", a, b);
            assign(g, &in->ops[k], expr, kind, n, 0);
        }
        break;
    }
    case OP_UDIV: {                                            // dst_quotient, dst_remainder, a, b; x / 0 = all ones
        for (int k = 0; k < 2; k++) {
            if (in->ops[k].type == OPD_NULL) continue;
            int comps[4], n = dest_comps(&in->ops[k], comps);
            char a[700], b[700], expr[2000];
            src(g, &in->ops[2], comps, n, K_UINT, a, sizeof a);
            src(g, &in->ops[3], comps, n, K_UINT, b, sizeof b);
            snprintf(expr, sizeof expr, "select(%s %c max(%s, %s(1u)), %s(0xffffffffu), %s == 0u)", a, k ? '%' : '/', b, type_name(K_UINT, n), type_name(K_UINT, n), b);
            assign(g, &in->ops[k], expr, K_UINT, n, 0);
        }
        break;
    }
    case OP_SAMPLE: case OP_SAMPLE_L: case OP_SAMPLE_B: case OP_SAMPLE_D: case OP_SAMPLE_C: case OP_SAMPLE_C_LZ: sample(g, in); break;
    case OP_LD: case OP_LD_MS: load(g, in); break;
    case OP_RESINFO: resinfo(g, in); break;
    case OP_IF: { char c[800]; condition(g, in, &in->ops[0], c, sizeof c); line(g, "if (%s) {", c); g->indent++; break; }
    case OP_ELSE: g->indent--; line(g, "} else {"); g->indent++; break;
    case OP_ENDIF: case OP_ENDLOOP: case OP_ENDSWITCH: g->indent--; line(g, "}"); break;
    case OP_LOOP: line(g, "while (true) {"); g->indent++; break;
    case OP_BREAK: line(g, "break;"); break;
    case OP_CONTINUE: line(g, "continue;"); break;
    case OP_BREAKC: { char c[800]; condition(g, in, &in->ops[0], c, sizeof c); line(g, "if (%s) break;", c); break; }
    case OP_CONTINUEC: { char c[800]; condition(g, in, &in->ops[0], c, sizeof c); line(g, "if (%s) continue;", c); break; }
    case OP_SWITCH: { char v[700]; int c0 = 0; src(g, &in->ops[0], &c0, 1, K_INT, v, sizeof v); line(g, "switch (%s) {", v); g->indent++; break; }
    case OP_CASE: line(g, "case %d:", (int)in->ops[0].value[0]); break;
    case OP_DEFAULT: line(g, "default:"); break;
    case OP_RET: line(g, "return vx_finish();"); break;
    case OP_RETC: { char c[800]; condition(g, in, &in->ops[0], c, sizeof c); line(g, "if (%s) return vx_finish();", c); break; }
    case OP_DISCARD: { char c[800]; condition(g, in, &in->ops[0], c, sizeof c); line(g, "if (%s) discard_fragment();", c); break; }
    case OP_NOP: break;
    default: fail(g, "instruction %u is not supported yet", in->op); break;
    }
}

// ---- declarations ---------------------------------------------------------------------------------------------------------------
static void declare(gen *g, uint32_t op, uint32_t token, reader *r, pool *pl) {
    operand o;
    switch (op) {
    case OP_DCL_TEMPS: g->temps = (int)next(r); break;
    case OP_DCL_INDEXABLE_TEMP: {
        uint32_t reg = next(r), size = next(r);
        next(r);                                              // components
        if (g->nxt < 32) { g->xt[g->nxt].reg = reg; g->xt[g->nxt].size = size; g->nxt++; }
        break;
    }
    case OP_DCL_INPUT: case OP_DCL_INPUT_PS: case OP_DCL_INPUT_SGV: case OP_DCL_INPUT_SIV: case OP_DCL_INPUT_PS_SGV: case OP_DCL_INPUT_PS_SIV:
        read_operand(r, pl, &o);
        if (o.type == OPD_INPUT && o.dims >= 1) {
            uint32_t reg = o.index[o.dims - 1];
            if (reg < 32) {
                g->in_decl[reg] = 1;
                g->in_mask[reg] |= o.mask;
                if (op == OP_DCL_INPUT_PS || op == OP_DCL_INPUT_PS_SIV) g->in_interp[reg] = (token >> 11) & 0xf;
                if (op == OP_DCL_INPUT_SGV || op == OP_DCL_INPUT_SIV || op == OP_DCL_INPUT_PS_SGV || op == OP_DCL_INPUT_PS_SIV) g->in_sv[reg] = next(r);
            }
        } else if (o.type == OPD_INPUT_PRIMITIVEID) g->in_primid = 1;
        else if (o.type == OPD_INPUT_COVERAGE) g->in_coverage = 1;
        break;
    case OP_DCL_OUTPUT: case OP_DCL_OUTPUT_SGV: case OP_DCL_OUTPUT_SIV:
        read_operand(r, pl, &o);
        if (o.type == OPD_OUTPUT && o.index[0] < 32) {
            g->out_decl[o.index[0]] = 1;
            if (op != OP_DCL_OUTPUT) g->out_sv[o.index[0]] = next(r);
        } else if (o.type == OPD_OUTPUT_DEPTH || o.type == OPD_OUTPUT_DEPTH_GE || o.type == OPD_OUTPUT_DEPTH_LE) g->out_depth = 1;
        else if (o.type == OPD_OUTPUT_COVERAGE) g->out_coverage = 1;
        break;
    case OP_DCL_CONSTANT_BUFFER:
        read_operand(r, pl, &o);
        if (o.index[0] < 16) { g->sh->cbuffers |= 1u << o.index[0]; g->cb_size[o.index[0]] = o.index[1]; }
        break;
    case OP_DCL_SAMPLER:
        read_operand(r, pl, &o);
        if (o.index[0] < 32) g->sh->samplers |= 1u << o.index[0];
        break;
    case OP_DCL_RESOURCE: {
        read_operand(r, pl, &o);
        uint32_t ret = next(r);
        if (o.index[0] < 128) {
            g->sh->textures[o.index[0] / 64] |= 1ull << (o.index[0] % 64);
            g->tex_dim[o.index[0]] = (uint8_t)((token >> 11) & 0x1f);
            g->tex_ret[o.index[0]] = (uint8_t)(ret & 0xf);   // 1 unorm, 2 snorm, 3 sint, 4 uint, 5 float
        }
        break;
    }
    default: break;                                          // flags, index ranges and the like change nothing here
    }
}

// ---- the whole program ----------------------------------------------------------------------------------------------------------
static const char *interp_qualifier(uint32_t mode) {
    switch (mode) {
    case 1: return " [[flat]]";
    case 3: return " [[centroid_perspective]]";
    case 4: return " [[center_no_perspective]]";
    case 5: return " [[centroid_no_perspective]]";
    case 6: return " [[sample_perspective]]";
    case 7: return " [[sample_no_perspective]]";
    default: return "";
    }
}
static uint32_t component_type(const dxbc_element *els, int n, uint32_t reg) {
    for (int i = 0; i < n; i++) if (els[i].reg == reg) return els[i].component_type;
    return 3;
}
static const char *vec_type(uint32_t component_type) { return component_type == 1 ? "uint4" : component_type == 2 ? "int4" : "float4"; }
static int first_comp(uint8_t mask) { for (int c = 0; c < 4; c++) if (mask & (1 << c)) return c; return 0; }

static char *compose(gen *g) {
    dxbc_shader *sh = g->sh;
    text t = { 0 };
    int vs = sh->type == DXBC_VERTEX, ps = sh->type == DXBC_PIXEL;
    put(&t, "#include <metal_stdlib>\nusing namespace metal;\n\n");
    if (g->icb_n) {
        put(&t, "constant uint4 icb_bits[%u] = {", g->icb_n / 4);
        for (uint32_t i = 0; i + 3 < g->icb_n; i += 4)
            put(&t, "%s\n    uint4(0x%xu, 0x%xu, 0x%xu, 0x%xu)", i ? "," : "", g->icb[i], g->icb[i + 1], g->icb[i + 2], g->icb[i + 3]);
        put(&t, "\n};\n#define icb as_type<constant float4 *>(icb_bits)\n\n");
    }
    // Stage inputs.
    int has_in = 0;
    put(&t, "struct VxIn {\n");
    for (int r = 0; r < 32; r++) {
        if (!g->in_decl[r]) continue;
        if (vs && !g->in_sv[r]) { put(&t, "    %s v%d [[attribute(%d)]];\n", vec_type(component_type(sh->inputs, sh->ninputs, (uint32_t)r)), r, r); has_in = 1; }
        if (ps && g->in_sv[r] == 1) { put(&t, "    float4 v%d [[position]];\n", r); has_in = 1; }
        if (ps && !g->in_sv[r]) { put(&t, "    float4 v%d [[user(r%d)]]%s;\n", r, r, interp_qualifier(g->in_interp[r])); has_in = 1; }
    }
    put(&t, "};\n");
    // Stage outputs.
    put(&t, "struct VxOut {\n");
    char finish[2048] = "";
    size_t fk = 0;
    for (int r = 0; r < 32; r++) {
        if (!g->out_decl[r]) continue;
        if (vs) put(&t, g->out_sv[r] == 1 ? "    float4 o%d [[position]];\n" : "    float4 o%d [[user(r%d)]];\n", r, r);
        if (ps) put(&t, "    %s o%d [[color(%d)]];\n", vec_type(component_type(sh->outputs, sh->noutputs, (uint32_t)r)), r, r);
        fk += (size_t)snprintf(finish + fk, sizeof finish - fk, "%sas_type<%s>(o%d)", fk ? ", " : "",
                               ps ? vec_type(component_type(sh->outputs, sh->noutputs, (uint32_t)r)) : "float4", r);
    }
    if (g->out_depth) { put(&t, "    float oDepth [[depth(any)]];\n"); fk += (size_t)snprintf(finish + fk, sizeof finish - fk, "%soDepth", fk ? ", " : ""); }
    if (g->out_coverage) { put(&t, "    uint oMask [[sample_mask]];\n"); fk += (size_t)snprintf(finish + fk, sizeof finish - fk, "%soMask", fk ? ", " : ""); }
    if (!fk) put(&t, "    float4 vx_unused [[user(unused)]];\n");
    put(&t, "};\n#define vx_finish() (VxOut{%s})\n\n", fk ? finish : "float4(0)");
    // The function.
    put(&t, "%s VxOut vx_main(%s", vs ? "vertex" : "fragment", has_in ? "VxIn vin [[stage_in]], " : "");
    if (vs) put(&t, "uint vx_vertex_id [[vertex_id]], uint vx_instance_id [[instance_id]], uint vx_base_instance [[base_instance]]");
    if (ps) put(&t, "bool vx_front_facing [[front_facing]], uint vx_sample_id [[sample_id]], uint vx_coverage [[sample_mask]], uint vx_primitive [[primitive_id]]");
    for (int i = 0; i < 16; i++) if (sh->cbuffers & (1u << i)) put(&t, ", constant float4 *cb%d [[buffer(%d)]]", i, i);
    for (int i = 0; i < 128; i++) {
        if (!(sh->textures[i / 64] & (1ull << (i % 64)))) continue;
        const char *rt = g->tex_ret[i] == 3 ? "int" : g->tex_ret[i] == 4 ? "uint" : "float";
        const char *tt;
        switch (g->tex_dim[i]) {
        case DIM_BUFFER: tt = "texture_buffer"; break;
        case DIM_1D: tt = "texture1d"; break;
        case DIM_1DARRAY: tt = "texture1d_array"; break;
        case DIM_2DMS: tt = g->tex_cmp[i] ? "depth2d_ms" : "texture2d_ms"; break;
        case DIM_2DMSARRAY: tt = g->tex_cmp[i] ? "depth2d_ms_array" : "texture2d_ms_array"; break;
        case DIM_3D: tt = "texture3d"; break;
        case DIM_CUBE: tt = g->tex_cmp[i] ? "depthcube" : "texturecube"; break;
        case DIM_CUBEARRAY: tt = g->tex_cmp[i] ? "depthcube_array" : "texturecube_array"; break;
        case DIM_2DARRAY: tt = g->tex_cmp[i] ? "depth2d_array" : "texture2d_array"; break;
        default: tt = g->tex_cmp[i] ? "depth2d" : "texture2d"; break;
        }
        put(&t, ", %s<%s> t%d [[texture(%d)]]", tt, g->tex_cmp[i] ? "float" : rt, i, i);
    }
    for (int i = 0; i < 32; i++) if (sh->samplers & (1u << i)) put(&t, ", sampler s%d [[sampler(%d)]]", i, i);
    put(&t, ") {\n");
    for (int i = 0; i < g->temps; i++) put(&t, "    float4 r%d = 0;\n", i);
    for (int i = 0; i < g->nxt; i++) put(&t, "    float4 x%u[%u];\n", g->xt[i].reg, g->xt[i].size);
    for (int r = 0; r < 32; r++) {
        if (g->out_decl[r]) put(&t, "    float4 o%d = 0;\n", r);
        if (!g->in_decl[r]) continue;
        uint32_t sv = g->in_sv[r];
        int c = first_comp(g->in_mask[r]);
        if (vs && sv == 6) put(&t, "    float4 v%d = 0; v%d.%c = as_type<float>(vx_vertex_id);\n", r, r, CH[c]);
        else if (vs && sv == 8) put(&t, "    float4 v%d = 0; v%d.%c = as_type<float>(vx_instance_id - vx_base_instance);\n", r, r, CH[c]);
        else if (ps && sv == 9) put(&t, "    float4 v%d = 0; v%d.%c = as_type<float>(vx_front_facing ? 0xffffffffu : 0u);\n", r, r, CH[c]);
        else if (ps && sv == 10) put(&t, "    float4 v%d = 0; v%d.%c = as_type<float>(vx_sample_id);\n", r, r, CH[c]);
        else if (ps && sv == 7) put(&t, "    float4 v%d = 0; v%d.%c = as_type<float>(vx_primitive);\n", r, r, CH[c]);
        else if (sv && sv != 1) put(&t, "    float4 v%d = 0;\n", r);
        else put(&t, "    float4 v%d = as_type<float4>(vin.v%d);\n", r, r);
    }
    if (g->out_depth) put(&t, "    float oDepth = 0;\n");
    if (g->out_coverage) put(&t, "    uint oMask = 0xffffffffu;\n");
    if (g->in_primid) put(&t, "    float vPrim = as_type<float>(vx_primitive);\n");
    if (g->in_coverage) put(&t, "    float vCoverage = as_type<float>(vx_coverage);\n");
    put(&t, "%s    return vx_finish();\n}\n", g->body.s ? g->body.s : "");
    return t.s;
}

int dxbc_translate(const void *code, size_t size, dxbc_shader *out) {
    memset(out, 0, sizeof *out);
    uint32_t len;
    const uint8_t *prog = chunk(code, size, "SHEX", &len);
    if (!prog) prog = chunk(code, size, "SHDR", &len);
    if (!prog || len < 8) { snprintf(out->error, sizeof out->error, "no shader program in the container"); return -1; }
    out->ninputs = find_signature(code, size, 0, out->inputs, 32);
    out->noutputs = find_signature(code, size, 1, out->outputs, 32);
    const uint32_t *tok = (const uint32_t *)prog;
    out->type = (int)(tok[0] >> 16); out->major = (int)((tok[0] >> 4) & 0xf); out->minor = (int)(tok[0] & 0xf);
    out->entry = "vx_main";
    if (out->type != DXBC_VERTEX && out->type != DXBC_PIXEL) { snprintf(out->error, sizeof out->error, "shader type %d is not supported yet", out->type); return -1; }
    uint32_t count = tok[1];
    if (count * 4 > len) count = len / 4;
    gen g;
    memset(&g, 0, sizeof g);
    g.sh = out;
    reader r = { tok + 2, tok + count, 0 };
    while (r.p < r.end && !g.why[0]) {
        const uint32_t *start = r.p;
        uint32_t token = next(&r), op = token & 0x7ff;
        uint32_t length = (token >> 24) & 0x7f;
        if (op == OP_CUSTOMDATA) {
            uint32_t total = next(&r), cls = token >> 11;
            if (cls == 3 && total >= 2) {                       // the immediate constant buffer
                g.icb_n = total - 2;
                g.icb = malloc(sizeof(uint32_t) * (g.icb_n ? g.icb_n : 1));
                memcpy(g.icb, r.p, sizeof(uint32_t) * g.icb_n);
            }
            r.p = start + (total ? total : 2);
            continue;
        }
        if (!length) { snprintf(out->error, sizeof out->error, "bad instruction length"); free(g.icb); return -1; }
        const uint32_t *end = start + length;
        pool pl = { .used = 0 };
        instr in;
        memset(&in, 0, sizeof in);
        in.op = op; in.token = token;
        in.saturate = (token >> 13) & 1;
        in.test_nonzero = (token >> 18) & 1;
        in.ret_type = (token >> 11) & 3;
        uint32_t ext = token;
        while (ext & 0x80000000u) {                              // extended opcode tokens: texel offsets
            ext = next(&r);
            if ((ext & 0x3f) == 1) {
                for (int k = 0; k < 3; k++) { int v = (int)((ext >> (9 + 4 * k)) & 0xf); in.offset[k] = v >= 8 ? v - 16 : v; }
            }
        }
        if (op >= OP_DCL_RESOURCE && op <= OP_DCL_GLOBAL_FLAGS) declare(&g, op, token, &r, &pl);
        else if (op >= 143 && op <= 162) { /* SM5 declarations (streams, tessellation, UAVs): later */ }
        else {
            while (r.p < end && in.nops < 8) read_operand(&r, &pl, &in.ops[in.nops++]);
            translate(&g, &in);
        }
        r.p = end;
    }
    if (g.why[0] || r.bad) {
        snprintf(out->error, sizeof out->error, "%s", g.why[0] ? g.why : "truncated program");
        free(g.body.s); free(g.icb);
        return -1;
    }
    out->msl = compose(&g);
    free(g.body.s); free(g.icb);
    return 0;
}
void dxbc_free(dxbc_shader *s) { free(s->msl); s->msl = NULL; }
