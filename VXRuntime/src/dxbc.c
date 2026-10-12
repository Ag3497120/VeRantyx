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
    OP_ADD = 0, OP_AND = 1, OP_BREAK = 2, OP_BREAKC = 3, OP_CALL = 4, OP_CALLC = 5, OP_CASE = 6, OP_CONTINUE = 7, OP_CONTINUEC = 8, OP_DEFAULT = 10,
    OP_DERIV_RTX = 11, OP_DERIV_RTY = 12, OP_DISCARD = 13, OP_DIV = 14, OP_DP2 = 15, OP_DP3 = 16, OP_DP4 = 17, OP_ELSE = 18,
    OP_ENDIF = 21, OP_ENDLOOP = 22, OP_ENDSWITCH = 23, OP_EQ = 24, OP_EXP = 25, OP_FRC = 26, OP_FTOI = 27, OP_FTOU = 28,
    OP_GE = 29, OP_IADD = 30, OP_IF = 31, OP_IEQ = 32, OP_IGE = 33, OP_ILT = 34, OP_IMAD = 35, OP_IMAX = 36, OP_IMIN = 37,
    OP_IMUL = 38, OP_INE = 39, OP_INEG = 40, OP_ISHL = 41, OP_ISHR = 42, OP_ITOF = 43, OP_LABEL = 44, OP_LD = 45, OP_LD_MS = 46, OP_LOG = 47,
    OP_LOOP = 48, OP_LT = 49, OP_MAD = 50, OP_MIN = 51, OP_MAX = 52, OP_CUSTOMDATA = 53, OP_MOV = 54, OP_MOVC = 55, OP_MUL = 56,
    OP_NE = 57, OP_NOP = 58, OP_NOT = 59, OP_OR = 60, OP_RESINFO = 61, OP_RET = 62, OP_RETC = 63, OP_ROUND_NE = 64,
    OP_ROUND_NI = 65, OP_ROUND_PI = 66, OP_ROUND_Z = 67, OP_RSQ = 68, OP_SAMPLE = 69, OP_SAMPLE_C = 70, OP_SAMPLE_C_LZ = 71,
    OP_SAMPLE_L = 72, OP_SAMPLE_D = 73, OP_SAMPLE_B = 74, OP_SQRT = 75, OP_SWITCH = 76, OP_SINCOS = 77, OP_UDIV = 78,
    OP_ULT = 79, OP_UGE = 80, OP_UMUL = 81, OP_UMAD = 82, OP_UMAX = 83, OP_UMIN = 84, OP_USHR = 85, OP_UTOF = 86, OP_XOR = 87,
    OP_DCL_RESOURCE = 88, OP_DCL_CONSTANT_BUFFER = 89, OP_DCL_SAMPLER = 90, OP_DCL_INDEX_RANGE = 91, OP_DCL_INPUT = 95,
    OP_DCL_INPUT_SGV = 96, OP_DCL_INPUT_SIV = 97, OP_DCL_INPUT_PS = 98, OP_DCL_INPUT_PS_SGV = 99, OP_DCL_INPUT_PS_SIV = 100,
    OP_DCL_OUTPUT = 101, OP_DCL_OUTPUT_SGV = 102, OP_DCL_OUTPUT_SIV = 103, OP_DCL_TEMPS = 104, OP_DCL_INDEXABLE_TEMP = 105,
    OP_DCL_GLOBAL_FLAGS = 106, OP_LOD = 108, OP_GATHER4 = 109, OP_SAMPLE_POS = 110, OP_SAMPLE_INFO = 111,
    OP_GATHER4_C = 126, OP_GATHER4_PO = 127, OP_GATHER4_PO_C = 128, OP_UADDC = 132, OP_USUBB = 133, OP_SWAPC = 142,
    OP_EVAL_SNAPPED = 203, OP_EVAL_SAMPLE_INDEX = 204, OP_EVAL_CENTROID = 205, OP_MSAD = 213,
    OP_DADD = 191, OP_DMAX = 192, OP_DMIN = 193, OP_DMUL = 194, OP_DEQ = 195, OP_DGE = 196, OP_DLT = 197, OP_DNE = 198,
    OP_DMOV = 199, OP_DMOVC = 200, OP_DTOF = 201, OP_FTOD = 202, OP_DDIV = 210, OP_DFMA = 211, OP_DRCP = 212,
    OP_DTOI = 214, OP_DTOU = 215, OP_ITOD = 216, OP_UTOD = 217, OP_DERIV_RTX_COARSE = 122, OP_DERIV_RTX_FINE = 123,
    OP_BUFINFO = 121, OP_DERIV_RTY_COARSE = 124, OP_DERIV_RTY_FINE = 125, OP_RCP = 129, OP_F32TOF16 = 130, OP_F16TOF32 = 131, OP_COUNTBITS = 134,
    OP_FIRSTBIT_HI = 135, OP_FIRSTBIT_LO = 136, OP_FIRSTBIT_SHI = 137, OP_UBFE = 138, OP_IBFE = 139, OP_BFI = 140, OP_BFREV = 141,
    OP_DCL_THREAD_GROUP = 155, OP_DCL_UAV_TYPED = 156, OP_DCL_UAV_RAW = 157, OP_DCL_UAV_STRUCTURED = 158, OP_DCL_TGSM_RAW = 159,
    OP_DCL_TGSM_STRUCTURED = 160, OP_DCL_RESOURCE_RAW = 161, OP_DCL_RESOURCE_STRUCTURED = 162, OP_LD_UAV_TYPED = 163,
    OP_STORE_UAV_TYPED = 164, OP_LD_RAW = 165, OP_STORE_RAW = 166, OP_LD_STRUCTURED = 167, OP_STORE_STRUCTURED = 168,
    OP_ATOMIC_AND = 169, OP_ATOMIC_OR = 170, OP_ATOMIC_XOR = 171, OP_ATOMIC_CMP_STORE = 172, OP_ATOMIC_IADD = 173,
    OP_ATOMIC_IMAX = 174, OP_ATOMIC_IMIN = 175, OP_ATOMIC_UMAX = 176, OP_ATOMIC_UMIN = 177, OP_IMM_ATOMIC_ALLOC = 178,
    OP_IMM_ATOMIC_CONSUME = 179, OP_IMM_ATOMIC_IADD = 180, OP_IMM_ATOMIC_AND = 181, OP_IMM_ATOMIC_OR = 182, OP_IMM_ATOMIC_XOR = 183,
    OP_IMM_ATOMIC_EXCH = 184, OP_IMM_ATOMIC_CMP_EXCH = 185, OP_IMM_ATOMIC_IMAX = 186, OP_IMM_ATOMIC_IMIN = 187,
    OP_IMM_ATOMIC_UMAX = 188, OP_IMM_ATOMIC_UMIN = 189, OP_SYNC = 190,
};
enum {
    OPD_TEMP = 0, OPD_INPUT = 1, OPD_OUTPUT = 2, OPD_INDEXABLE_TEMP = 3, OPD_IMM32 = 4, OPD_IMM64 = 5, OPD_SAMPLER = 6,
    OPD_RESOURCE = 7, OPD_CB = 8, OPD_ICB = 9, OPD_INPUT_PRIMITIVEID = 11, OPD_OUTPUT_DEPTH = 12, OPD_NULL = 13,
    OPD_RASTERIZER = 14, OPD_OUTPUT_COVERAGE = 15, OPD_UAV = 30, OPD_TGSM = 31, OPD_THREAD_ID = 32, OPD_THREAD_GROUP_ID = 33,
    OPD_THREAD_ID_IN_GROUP = 34, OPD_INPUT_COVERAGE = 35, OPD_THREAD_INDEX_IN_GROUP = 36, OPD_OUTPUT_DEPTH_GE = 38, OPD_OUTPUT_DEPTH_LE = 39,
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
        // One or two doubles. Metal has no doubles: a double is kept as a float in its low word (high word 0).
        int n = o->ncomp == 2 ? 2 : 1;
        for (int i = 0; i < n; i++) {
            uint64_t bits = next(r); bits |= (uint64_t)next(r) << 32;
            double d; memcpy(&d, &bits, 8);
            float f = (float)d; memcpy(&o->value[2 * i], &f, 4);
            o->value[2 * i + 1] = 0;
        }
        if (n == 1) { o->value[2] = o->value[0]; o->value[3] = 0; }
        o->type = OPD_IMM32;
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
    uint8_t in_mask[32], in_eval[32];      // in_eval: read through an interpolant (eval_* instructions)
    int uses_msad;
    const uint32_t *labels[64], *prog_end;    // subroutine bodies, the program's end
    int sub_depth, nesting;                   // subroutines being inlined; loops and switches open in this one
    int out_depth, in_primid, in_coverage, out_coverage;
    int vp_reg, vp_comp, rt_reg, rt_comp, clip_reg; uint8_t clip_mask;   // vertex outputs: viewport/layer index, clip distances
    uint8_t tex_dim[128], tex_ret[128], tex_cmp[128];
    uint32_t cb_size[16];
    uint32_t *icb, icb_n;
    struct { uint32_t reg, words, stride; } tgsm[16];
    int ntgsm;
    uint8_t uav_dim[8], uav_ret[8];
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
static int first_comp(uint8_t mask);

// A destination's enabled components.
static int dest_comps(const operand *d, int *comps) {
    int n = 0;
    if (d->ncomp != 2) { comps[n++] = 0; return n; }
    for (int c = 0; c < 4; c++) if (d->mask & (1 << c)) comps[n++] = c;
    return n;
}
static int scalar_register(const operand *o) {
    return o->type == OPD_OUTPUT_DEPTH || o->type == OPD_OUTPUT_DEPTH_GE || o->type == OPD_OUTPUT_DEPTH_LE ||
           o->type == OPD_OUTPUT_COVERAGE || o->type == OPD_INPUT_COVERAGE || o->type == OPD_INPUT_PRIMITIVEID ||
           o->type == OPD_THREAD_INDEX_IN_GROUP;
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
    case OPD_THREAD_ID: snprintf(out, cap, "vx_thread_id"); break;
    case OPD_THREAD_GROUP_ID: snprintf(out, cap, "vx_group_id"); break;
    case OPD_THREAD_ID_IN_GROUP: snprintf(out, cap, "vx_thread_in_group"); break;
    case OPD_THREAD_INDEX_IN_GROUP: snprintf(out, cap, "vx_thread_index"); break;
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
    else if (g->tex_cmp[t]) snprintf(call, sizeof call, "float4(t%u.sample(%s%s), 0, 0, 1)", t, args, extra);   // a depth texture
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
    int uav = res->type == OPD_UAV;                          // a UAV texture: one mip level (the view's)
    int dim = uav ? g->uav_dim[t & 7] : t < 128 ? g->tex_dim[t] : DIM_2D;
    char name[16];
    snprintf(name, sizeof name, "%c%u", uav ? 'u' : 't', uav ? t & 7 : t);
    char mip[700]; int c0 = 0;
    src(g, &in->ops[1], &c0, 1, K_UINT, mip, sizeof mip);
    char v[4][300];
    const char *lod = dim == DIM_BUFFER || dim == DIM_2DMS || dim == DIM_2DMSARRAY || uav ? "" : mip;   // multisampled textures have one level
    snprintf(v[0], 300, "%s.get_width(%s)", name, lod);
    snprintf(v[1], 300, dim == DIM_1D || dim == DIM_1DARRAY || dim == DIM_BUFFER ? "0u" : "%s.get_height(%s)", name, lod);
    if (dim == DIM_3D) snprintf(v[2], 300, "%s.get_depth(%s)", name, lod);
    else if (dim == DIM_1DARRAY || dim == DIM_2DARRAY || dim == DIM_CUBEARRAY || dim == DIM_2DMSARRAY) snprintf(v[2], 300, "%s.get_array_size()", name);
    else snprintf(v[2], 300, "0u");
    snprintf(v[3], 300, dim == DIM_BUFFER || dim == DIM_2DMS || dim == DIM_2DMSARRAY || uav ? "1u" : "%s.get_num_mip_levels()", name);
    if (dim == DIM_BUFFER) snprintf(v[0], 300, "%s.get_width()", name);
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

// ---- memory: raw and structured buffers, shared memory, typed UAVs, atomics -----------------------------------------------------
// The memory an operand names: a UAV buffer (device), a raw/structured SRV buffer (const device) or thread-group
// shared memory. Elements are 32-bit words; `stride` is the structure size (0 for raw memory).
static int memory(gen *g, const operand *o, char *name, size_t cap, uint32_t *stride, int *shared) {
    *shared = 0; *stride = 0;
    uint32_t i = o->index[0];
    if (o->type == OPD_UAV && i < 8) { snprintf(name, cap, "u%u", i); *stride = g->sh->uav_stride[i]; return 1; }
    if (o->type == OPD_RESOURCE && i < 64) { snprintf(name, cap, "t%u", i); *stride = g->sh->srv_stride[i]; return 1; }
    if (o->type == OPD_TGSM) {
        for (int k = 0; k < g->ntgsm; k++) if (g->tgsm[k].reg == i) *stride = g->tgsm[k].stride;
        snprintf(name, cap, "g%u", i); *shared = 1; return 1;
    }
    fail(g, "memory operand type %d is not supported", o->type);
    return 0;
}
// The word address of a raw byte address, or of a structure index and byte offset.
static void word_address(gen *g, const operand *index, int ci, const operand *offset, int co, uint32_t stride, char *out, size_t cap) {
    char a[700], b[700];
    src(g, index, &ci, 1, K_UINT, a, sizeof a);
    if (!offset) { snprintf(out, cap, "((%s) >> 2)", a); return; }
    src(g, offset, &co, 1, K_UINT, b, sizeof b);
    snprintf(out, cap, "((%s * %uu + %s) >> 2)", a, stride, b);
}
static void memory_access(gen *g, const instr *in) {
    int structured = in->op == OP_LD_STRUCTURED || in->op == OP_STORE_STRUCTURED;
    int store = in->op == OP_STORE_RAW || in->op == OP_STORE_STRUCTURED;
    const operand *res = store ? &in->ops[0] : &in->ops[structured ? 3 : 2];
    char m[64], addr[1600];
    uint32_t stride; int shared;
    if (!memory(g, res, m, sizeof m, &stride, &shared)) return;
    word_address(g, &in->ops[1], 0, structured ? &in->ops[2] : NULL, 0, stride, addr, sizeof addr);
    line(g, "{ uint vx_a = %s;", addr);
    if (store) {
        const operand *val = &in->ops[structured ? 3 : 2];
        int comps[4], n = dest_comps(res, comps);
        for (int i = 0; i < n; i++) {
            char v[700];
            int c = comps[i];
            src(g, val, &c, 1, K_UINT, v, sizeof v);
            line(g, "    %s[vx_a + %d] = %s;", m, c, v);
        }
    } else {
        int comps[4], n = dest_comps(&in->ops[0], comps);
        char vals[400] = "";
        size_t k = 0;
        for (int i = 0; i < n; i++) k += (size_t)snprintf(vals + k, sizeof vals - k, "%s%s[vx_a + %d]", i ? ", " : "", m, res->swz[comps[i]]);
        char expr[500];
        snprintf(expr, sizeof expr, "%s(%s)", type_name(K_UINT, n), vals);
        g->indent++;
        assign(g, &in->ops[0], expr, K_UINT, n, 0);
        g->indent--;
    }
    line(g, "}");
}
static void typed_uav(gen *g, const instr *in) {
    int store = in->op == OP_STORE_UAV_TYPED;
    const operand *u = store ? &in->ops[0] : &in->ops[2], *addr = &in->ops[1];
    uint32_t i = u->index[0] & 7;
    int dim = g->uav_dim[i], kind = g->uav_ret[i] == 3 ? K_INT : g->uav_ret[i] == 4 ? K_UINT : K_FLOAT;
    char xy[700], slice[720];
    coords(g, addr, dim, K_UINT, xy, sizeof xy, slice, sizeof slice);
    const char *ut = dim == DIM_1D || dim == DIM_1DARRAY || dim == DIM_BUFFER ? "uint" : dim == DIM_3D ? "uint3" : "uint2";
    char at[800];
    snprintf(at, sizeof at, "%s(%s)%s%s", ut, xy, slice[0] ? ", " : "", slice);
    if (store) {
        int c4[4] = { 0, 1, 2, 3 };
        char v[700];
        src(g, &in->ops[2], c4, 4, kind, v, sizeof v);
        line(g, "u%u.write(%s, %s);", i, v, at);
        return;
    }
    g->sh->uav_read[i] = 1;
    int comps[4], n = dest_comps(&in->ops[0], comps);
    char sw[5];
    for (int k = 0; k < n; k++) sw[k] = CH[u->swz[comps[k]]];
    sw[n] = 0;
    char expr[1000];
    snprintf(expr, sizeof expr, "u%u.read(%s).%s", i, at, sw);
    assign(g, &in->ops[0], expr, kind, n, 0);
}
// bufinfo: the element count (structured), byte size (raw) or width (typed) of a buffer view. Sizes in bytes
// come from a table the runtime binds: u0..u7, then t0..
static void bufinfo(gen *g, const instr *in) {
    const operand *r = &in->ops[1];
    uint32_t i = r->index[0];
    int comps[4], n = dest_comps(&in->ops[0], comps);
    char v[200], expr[300];
    if (r->type == OPD_UAV && (i & 7) == i && g->sh->uav_kind[i] == DXBC_UAV_TYPED) snprintf(v, sizeof v, "u%u.get_width()", i);
    else if (r->type == OPD_RESOURCE && !(g->sh->srv_buffers & (1ull << (i & 63)))) snprintf(v, sizeof v, "t%u.get_width()", i);
    else {
        uint32_t stride = r->type == OPD_UAV ? g->sh->uav_stride[i & 7] : g->sh->srv_stride[i & 63];
        g->sh->uses_sizes = 1;
        snprintf(v, sizeof v, "(vx_sizes[%u]%s)", r->type == OPD_UAV ? i : 8 + i, stride ? "" : "");
        if (stride) { char t[250]; snprintf(t, sizeof t, "(%s / %uu)", v, stride); snprintf(v, sizeof v, "%s", t); }
    }
    snprintf(expr, sizeof expr, "%s(%s)", type_name(K_UINT, n), v);
    assign(g, &in->ops[0], expr, K_UINT, n, 0);
}
static void atomic(gen *g, const instr *in) {
    int imm = in->op >= OP_IMM_ATOMIC_ALLOC;
    if (in->op == OP_IMM_ATOMIC_ALLOC || in->op == OP_IMM_ATOMIC_CONSUME) {   // a UAV's hidden counter: dst, u#
        uint32_t u = in->ops[1].index[0] & 7;
        g->sh->uav_counters |= (uint8_t)(1u << u);
        g->sh->uses_sizes = 1;
        char call[300];
        snprintf(call, sizeof call, in->op == OP_IMM_ATOMIC_ALLOC ? "atomic_fetch_add_explicit(vx_counter(%u), 1u, memory_order_relaxed)"
                                                                  : "(atomic_fetch_sub_explicit(vx_counter(%u), 1u, memory_order_relaxed) - 1u)", u);
        assign(g, &in->ops[0], call, K_UINT, 1, 0);
        return;
    }
    const operand *res = &in->ops[imm ? 1 : 0], *addr = &in->ops[imm ? 2 : 1];
    const operand *v1 = &in->ops[imm ? 3 : 2], *v2 = in->nops > (imm ? 4 : 3) ? &in->ops[imm ? 4 : 3] : NULL;
    uint32_t op = imm ? in->op - (OP_IMM_ATOMIC_IADD - OP_ATOMIC_IADD) : in->op;
    if (in->op == OP_IMM_ATOMIC_EXCH) op = 0;
    if (in->op == OP_IMM_ATOMIC_CMP_EXCH) op = OP_ATOMIC_CMP_STORE;
    if (in->op == OP_IMM_ATOMIC_AND) op = OP_ATOMIC_AND;
    if (in->op == OP_IMM_ATOMIC_OR) op = OP_ATOMIC_OR;
    if (in->op == OP_IMM_ATOMIC_XOR) op = OP_ATOMIC_XOR;
    if (res->type == OPD_UAV && g->sh->uav_kind[res->index[0] & 7] == DXBC_UAV_TYPED) {   // a texture's atomics (Metal 3.1)
        uint32_t i = res->index[0] & 7;
        int dim = g->uav_dim[i], tk = g->uav_ret[i] == 3 ? K_INT : K_UINT;
        if (g->uav_ret[i] != 3 && g->uav_ret[i] != 4) { fail(g, "atomics on a float typed UAV"); return; }
        g->sh->uav_read[i] = 1;
        char xy[700], slice[720], at[1500], v[700], w[700];
        coords(g, addr, dim, K_UINT, xy, sizeof xy, slice, sizeof slice);
        const char *ut = dim == DIM_1D || dim == DIM_1DARRAY || dim == DIM_BUFFER ? "uint" : dim == DIM_3D ? "uint3" : "uint2";
        snprintf(at, sizeof at, "%s(%s)%s%s", ut, xy, slice[0] ? ", " : "", slice);
        int c0 = 0;
        src(g, v1, &c0, 1, tk, v, sizeof v);
        const char *vec = type_name(tk, 4);
        if (op == OP_ATOMIC_CMP_STORE) {
            src(g, v2, &c0, 1, tk, w, sizeof w);
            line(g, "{ %s vx_e = %s(%s);", vec, vec, v);
            line(g, "  while (!u%u.atomic_compare_exchange_weak(%s, &vx_e, %s(%s)) && vx_e.x == %s) {}", i, at, vec, w, v);
            if (imm) { g->indent++; assign(g, &in->ops[0], "vx_e.x", tk, 1, 0); g->indent--; }
            line(g, "}");
            return;
        }
        const char *fn = op == 0 ? "atomic_exchange" : op == OP_ATOMIC_AND ? "atomic_fetch_and" : op == OP_ATOMIC_OR ? "atomic_fetch_or" :
                         op == OP_ATOMIC_XOR ? "atomic_fetch_xor" : op == OP_ATOMIC_IADD ? "atomic_fetch_add" :
                         op == OP_ATOMIC_IMAX || op == OP_ATOMIC_UMAX ? "atomic_fetch_max" : "atomic_fetch_min";
        char call[2600];
        snprintf(call, sizeof call, "u%u.%s(%s, %s(%s)).x", i, fn, at, op == 0 ? vec : "", v);
        if (imm) assign(g, &in->ops[0], call, tk, 1, 0);
        else line(g, "%s;", call);
        return;
    }
    char m[64], a[1600];
    uint32_t stride; int shared;
    if (!memory(g, res, m, sizeof m, &stride, &shared)) return;
    word_address(g, addr, 0, stride ? addr : NULL, 1, stride, a, sizeof a);
    int is_signed = op == OP_ATOMIC_IMAX || op == OP_ATOMIC_IMIN;
    int kind = is_signed ? K_INT : K_UINT, c0 = 0;
    char ptr[1800], v[700], w[700];
    snprintf(ptr, sizeof ptr, "(%s %s *)&%s[%s]", shared ? "threadgroup" : "device", is_signed ? "atomic_int" : "atomic_uint", m, a);
    src(g, v1, &c0, 1, kind, v, sizeof v);
    if (op == OP_ATOMIC_CMP_STORE) {
        src(g, v2, &c0, 1, K_UINT, w, sizeof w);
        line(g, "{ uint vx_e = %s;", v);
        line(g, "  while (!atomic_compare_exchange_weak_explicit(%s, &vx_e, %s, memory_order_relaxed, memory_order_relaxed) && vx_e == %s) {}", ptr, w, v);
        if (imm) { g->indent++; assign(g, &in->ops[0], "vx_e", K_UINT, 1, 0); g->indent--; }
        line(g, "}");
        return;
    }
    const char *fn = op == 0 ? "atomic_exchange_explicit" : op == OP_ATOMIC_AND ? "atomic_fetch_and_explicit" :
                     op == OP_ATOMIC_OR ? "atomic_fetch_or_explicit" : op == OP_ATOMIC_XOR ? "atomic_fetch_xor_explicit" :
                     op == OP_ATOMIC_IADD ? "atomic_fetch_add_explicit" : op == OP_ATOMIC_IMAX || op == OP_ATOMIC_UMAX ? "atomic_fetch_max_explicit" :
                     "atomic_fetch_min_explicit";
    char call[2600];
    snprintf(call, sizeof call, "%s(%s, %s, memory_order_relaxed)", fn, ptr, v);
    if (imm) assign(g, &in->ops[0], call, kind, 1, 0);
    else line(g, "%s;", call);
}

// gather4 (dst, addr, res, smp), gather4_c (+ ref), gather4_po (dst, addr, offset, res, smp), gather4_po_c (+ ref).
// The sampler operand's swizzle picks the channel; programmable offsets move the coordinate by whole texels.
static void gather(gen *g, const instr *in) {
    int po = in->op == OP_GATHER4_PO || in->op == OP_GATHER4_PO_C, cmp = in->op == OP_GATHER4_C || in->op == OP_GATHER4_PO_C;
    const operand *dst = &in->ops[0], *addr = &in->ops[1], *res = &in->ops[2 + po], *smp = &in->ops[3 + po];
    uint32_t t = res->index[0];
    int dim = t < 128 ? g->tex_dim[t] : DIM_2D;
    int cube = dim == DIM_CUBE || dim == DIM_CUBEARRAY;
    char xy[700], slice[720], args[2400], extra[800] = "", ref[700] = "";
    int c0 = 0, c4[4] = { 0, 1, 2, 3 };
    coords(g, addr, dim, K_FLOAT, xy, sizeof xy, slice, sizeof slice);
    if (po && !cube) {
        char off[700], moved[1600];
        src(g, &in->ops[2], c4, 2, K_INT, off, sizeof off);
        snprintf(moved, sizeof moved, "(%s + float2(%s) / float2(t%u.get_width(), t%u.get_height()))", xy, off, t, t);
        snprintf(xy, sizeof xy, "%s", moved);
    } else if ((in->offset[0] || in->offset[1]) && !cube) snprintf(extra, sizeof extra, ", int2(%d, %d)", in->offset[0], in->offset[1]);
    snprintf(args, sizeof args, "s%u, %s%s%s", smp->index[0], xy, slice[0] ? ", " : "", slice);
    char call[4000];
    if (cmp) {
        src(g, &in->ops[4 + po], &c0, 1, K_FLOAT, ref, sizeof ref);
        snprintf(call, sizeof call, "t%u.gather_compare(%s, %s%s)", t, args, ref, extra);
    } else if (g->tex_cmp[t]) snprintf(call, sizeof call, "t%u.gather(%s%s)", t, args, extra);
    else {
        static const char *comp[4] = { "component::x", "component::y", "component::z", "component::w" };
        if (cube) snprintf(call, sizeof call, "t%u.gather(%s, %s)", t, args, comp[smp->swz[0] & 3]);
        else snprintf(call, sizeof call, "t%u.gather(%s%s, %s)", t, args, extra[0] ? extra : ", int2(0)", comp[smp->swz[0] & 3]);
    }
    int comps[4], n = dest_comps(dst, comps);
    char sw[5];
    for (int i = 0; i < n; i++) sw[i] = CH[res->swz[comps[i]]];
    sw[n] = 0;
    char expr[4200];
    int kind = g->tex_ret[t] == 3 && !g->tex_cmp[t] ? K_INT : g->tex_ret[t] == 4 && !g->tex_cmp[t] ? K_UINT : K_FLOAT;
    snprintf(expr, sizeof expr, "%s.%s", call, sw);
    assign(g, dst, expr, kind, n, in->saturate);
}
// lod: (clamped, unclamped, 0, 0) levels of detail, in fragment functions.
static void lod(gen *g, const instr *in) {
    const operand *dst = &in->ops[0], *addr = &in->ops[1], *res = &in->ops[2], *smp = &in->ops[3];
    uint32_t t = res->index[0];
    int dim = t < 128 ? g->tex_dim[t] : DIM_2D;
    char v[1800] = "float4(0)";
    if (g->sh->type == DXBC_PIXEL && dim != DIM_1D && dim != DIM_1DARRAY && dim != DIM_BUFFER) {
        int c4[4] = { 0, 1, 2, 3 }, nc = dim == DIM_3D || dim == DIM_CUBE || dim == DIM_CUBEARRAY ? 3 : 2;
        char xy[700];
        src(g, addr, c4, nc, K_FLOAT, xy, sizeof xy);
        snprintf(v, sizeof v, "float4(t%u.calculate_clamped_lod(s%u, %s), t%u.calculate_unclamped_lod(s%u, %s), 0, 0)", t, smp->index[0], xy, t, smp->index[0], xy);
    }
    int comps[4], n = dest_comps(dst, comps);
    char sw[5], expr[1900];
    for (int i = 0; i < n; i++) sw[i] = CH[res->swz[comps[i]]];
    sw[n] = 0;
    snprintf(expr, sizeof expr, "%s.%s", v, sw);
    assign(g, dst, expr, K_FLOAT, n, in->saturate);
}
// sample_info (the sample count of the rasterizer or a texture) and samplepos (a sample's position from the pixel's
// center), as floats or (sample_info_uint) uints.
static void sample_query(gen *g, const instr *in) {
    const operand *dst = &in->ops[0], *res = &in->ops[1];
    int frag = g->sh->type == DXBC_PIXEL;
    char count[300], v[1200];
    if (res->type == OPD_RASTERIZER) snprintf(count, sizeof count, frag ? "get_num_samples()" : "1u");
    else {
        uint32_t t = res->index[0];
        int dim = t < 128 ? g->tex_dim[t] : DIM_2D;
        if (dim == DIM_2DMS || dim == DIM_2DMSARRAY) snprintf(count, sizeof count, "t%u.get_num_samples()", t);
        else snprintf(count, sizeof count, "1u");
    }
    int as_uint = in->op == OP_SAMPLE_INFO && (in->token >> 11 & 1);
    if (in->op == OP_SAMPLE_INFO) snprintf(v, sizeof v, as_uint ? "uint4(%s, 0u, 0u, 0u)" : "float4(float(%s), 0, 0, 0)", count);
    else {
        char i[700]; int c0 = 0;
        src(g, &in->ops[2], &c0, 1, K_UINT, i, sizeof i);
        if (frag) snprintf(v, sizeof v, "float4(get_sample_position(%s) - 0.5, 0, 0)", i);
        else snprintf(v, sizeof v, "float4(0)");
    }
    int comps[4], n = dest_comps(dst, comps);
    char sw[5], expr[1300];
    for (int k = 0; k < n; k++) sw[k] = CH[res->swz[comps[k]]];
    sw[n] = 0;
    snprintf(expr, sizeof expr, "%s.%s", v, sw);
    assign(g, dst, expr, as_uint ? K_UINT : K_FLOAT, n, in->saturate);
}
// eval_sample_index, eval_centroid, eval_snapped: an input interpolated at a sample, the centroid, or an offset
// from the pixel's center in sixteenths of a pixel (the input becomes a Metal interpolant).
static void evaluate(gen *g, const instr *in) {
    const operand *dst = &in->ops[0], *v = &in->ops[1];
    if (v->type != OPD_INPUT || g->sh->type != DXBC_PIXEL) { fail(g, "eval of operand type %d", v->type); return; }
    uint32_t reg = v->index[v->dims - 1] & 31;
    g->in_eval[reg] = 1;
    char call[1200], a[700];
    int c0 = 0, c2[2] = { 0, 1 };
    if (in->op == OP_EVAL_CENTROID) snprintf(call, sizeof call, "vin.v%u.interpolate_at_centroid()", reg);
    else if (in->op == OP_EVAL_SAMPLE_INDEX) { src(g, &in->ops[2], &c0, 1, K_UINT, a, sizeof a); snprintf(call, sizeof call, "vin.v%u.interpolate_at_sample(%s)", reg, a); }
    else { src(g, &in->ops[2], c2, 2, K_INT, a, sizeof a); snprintf(call, sizeof call, "vin.v%u.interpolate_at_offset(float2(0.5) + float2(clamp(%s, -8, 7)) / 16.0)", reg, a); }
    int comps[4], n = dest_comps(dst, comps);
    char sw[5], expr[1300];
    for (int k = 0; k < n; k++) sw[k] = CH[v->mode == 0 ? comps[k] : v->swz[comps[k]]];
    sw[n] = 0;
    snprintf(expr, sizeof expr, "%s.%s", call, sw);
    char t[1400];
    if (v->neg || v->abs) { snprintf(t, sizeof t, "%s%s%s%s", v->neg ? "-" : "", v->abs ? "abs(" : "", expr, v->abs ? ")" : ""); snprintf(expr, sizeof expr, "%s", t); }
    assign(g, dst, expr, K_FLOAT, n, in->saturate);
}
// Two results from the same sources (uaddc, usubb: result and carry or borrow; swapc: both selections): the
// sources are read into temporaries first, as a destination may be one of them.
static void two_results(gen *g, const instr *in) {
    int c4[4] = { 0, 1, 2, 3 };
    char a[700], b[700], c[700];
    src(g, &in->ops[2], c4, 4, K_UINT, a, sizeof a);
    src(g, &in->ops[3], c4, 4, K_UINT, b, sizeof b);
    if (in->op == OP_SWAPC) { src(g, &in->ops[4], c4, 4, K_UINT, c, sizeof c); line(g, "{ uint4 vx_c = %s, vx_a = %s, vx_b = %s;", a, b, c); }
    else line(g, "{ uint4 vx_a = %s, vx_b = %s;", a, b);
    static const char *fmt[3][2] = {
        { "(vx_a + vx_b)", "select(uint4(0u), uint4(1u), vx_a + vx_b < vx_a)" },         // uaddc
        { "(vx_a - vx_b)", "select(uint4(0u), uint4(1u), vx_a < vx_b)" },                // usubb
        { "select(vx_a, vx_b, vx_c != 0u)", "select(vx_b, vx_a, vx_c != 0u)" },          // swapc
    };
    int which = in->op == OP_UADDC ? 0 : in->op == OP_USUBB ? 1 : 2;
    g->indent++;
    for (int k = 0; k < 2; k++) {
        if (in->ops[k].type == OPD_NULL) continue;
        int comps[4], n = dest_comps(&in->ops[k], comps);
        char sw[5], expr[300];
        for (int i = 0; i < n; i++) sw[i] = CH[comps[i]];
        sw[n] = 0;
        snprintf(expr, sizeof expr, "%s.%s", fmt[which][k], sw);
        assign(g, &in->ops[k], expr, K_UINT, n, 0);
    }
    g->indent--;
    line(g, "}");
}

// Double-precision instructions, approximated in single precision (Metal has no doubles): a double register
// pair holds a float in its low word and zero in its high word. Results that are doubles fill destination
// pairs (.xy, .zw); 32-bit results take a component per double.
static void double_op(gen *g, const instr *in) {
    const operand *dst = &in->ops[0];
    if (dst->type == OPD_NULL) return;
    int op = (int)in->op;
    int to_double = op == OP_DADD || op == OP_DMAX || op == OP_DMIN || op == OP_DMUL || op == OP_DMOV || op == OP_DMOVC ||
                    op == OP_FTOD || op == OP_DDIV || op == OP_DFMA || op == OP_DRCP || op == OP_ITOD || op == OP_UTOD;
    // The destination slots: the low component of each pair (doubles) or each component (32-bit results).
    int slot[4], n = 0;
    for (int c = 0; c < 4 && n < 2; c++) {
        if (dst->ncomp != 2) { slot[n++] = 0; break; }
        if (to_double ? !(c & 1) && (dst->mask & (3 << c)) : (dst->mask & (1 << c))) slot[n++] = c;
    }
    char vals[2][1800];
    for (int k = 0; k < n; k++) {
        // The source component that holds this double's value (or this 32-bit input).
        int from32 = op == OP_FTOD || op == OP_ITOD || op == OP_UTOD;
        int pos = to_double ? (from32 ? k : slot[k]) : 2 * k;
        char a[700] = "", b[700] = "", c[700] = "";
        int kin = op == OP_ITOD ? K_INT : op == OP_UTOD ? K_UINT : K_FLOAT;
        int first = op == OP_DMOVC ? 2 : 1;
        if (op == OP_DMOVC) src(g, &in->ops[1], &pos, 1, K_UINT, c, sizeof c);
        src(g, &in->ops[first], &pos, 1, kin, a, sizeof a);
        if (in->nops > first + 1) src(g, &in->ops[first + 1], &pos, 1, K_FLOAT, b, sizeof b);
        char d3[700] = "";
        if (op == OP_DFMA) src(g, &in->ops[3], &pos, 1, K_FLOAT, d3, sizeof d3);
        switch (op) {
        case OP_DADD: snprintf(vals[k], sizeof vals[k], "(%s + %s)", a, b); break;
        case OP_DMUL: snprintf(vals[k], sizeof vals[k], "(%s * %s)", a, b); break;
        case OP_DDIV: snprintf(vals[k], sizeof vals[k], "(%s / %s)", a, b); break;
        case OP_DMAX: snprintf(vals[k], sizeof vals[k], "fmax(%s, %s)", a, b); break;
        case OP_DMIN: snprintf(vals[k], sizeof vals[k], "fmin(%s, %s)", a, b); break;
        case OP_DFMA: snprintf(vals[k], sizeof vals[k], "fma(%s, %s, %s)", a, b, d3); break;
        case OP_DRCP: snprintf(vals[k], sizeof vals[k], "(1.0 / %s)", a); break;
        case OP_DMOVC: snprintf(vals[k], sizeof vals[k], "select(%s, %s, %s != 0u)", b, a, c); break;
        case OP_DEQ: snprintf(vals[k], sizeof vals[k], "as_type<float>(select(0u, 0xffffffffu, %s == %s))", a, b); break;
        case OP_DNE: snprintf(vals[k], sizeof vals[k], "as_type<float>(select(0u, 0xffffffffu, %s != %s))", a, b); break;
        case OP_DLT: snprintf(vals[k], sizeof vals[k], "as_type<float>(select(0u, 0xffffffffu, %s < %s))", a, b); break;
        case OP_DGE: snprintf(vals[k], sizeof vals[k], "as_type<float>(select(0u, 0xffffffffu, %s >= %s))", a, b); break;
        case OP_DTOI: snprintf(vals[k], sizeof vals[k], "as_type<float>(int(%s))", a); break;
        case OP_DTOU: snprintf(vals[k], sizeof vals[k], "as_type<float>(uint(%s))", a); break;
        case OP_ITOD: case OP_UTOD: snprintf(vals[k], sizeof vals[k], "float(%s)", a); break;
        default: snprintf(vals[k], sizeof vals[k], "%s", a); break;      // dmov, dtof, ftod
        }
    }
    line(g, "{");
    g->indent++;
    for (int k = 0; k < n; k++) line(g, "float vx_d%d = %s;", k, vals[k]);
    for (int k = 0; k < n; k++) {
        operand d = *dst;
        char e[32];
        snprintf(e, sizeof e, "vx_d%d", k);
        d.mask = (uint8_t)(1 << slot[k]);
        assign(g, &d, e, K_FLOAT, 1, to_double ? 0 : in->saturate);
        if (to_double && dst->ncomp == 2) { d.mask = (uint8_t)(1 << (slot[k] + 1)); assign(g, &d, "0u", K_UINT, 1, 0); }
    }
    g->indent--;
    line(g, "}");
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
    case OP_GATHER4: case OP_GATHER4_C: case OP_GATHER4_PO: case OP_GATHER4_PO_C: gather(g, in); break;
    case OP_LOD: lod(g, in); break;
    case OP_SAMPLE_INFO: case OP_SAMPLE_POS: sample_query(g, in); break;
    case OP_EVAL_SNAPPED: case OP_EVAL_SAMPLE_INDEX: case OP_EVAL_CENTROID: evaluate(g, in); break;
    case OP_UADDC: case OP_USUBB: case OP_SWAPC: two_results(g, in); break;
    case OP_MSAD: g->uses_msad = 1; componentwise(g, in, K_UINT, K_UINT, "vx_msad(%1$s, %2$s, %3$s)"); break;
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
    case OP_RET: line(g, "VX_RETURN;"); break;
    case OP_RETC: { char c[800]; condition(g, in, &in->ops[0], c, sizeof c); line(g, "if (%s) VX_RETURN;", c); break; }
    case OP_DISCARD: { char c[800]; condition(g, in, &in->ops[0], c, sizeof c); line(g, "if (%s) discard_fragment();", c); break; }
    case OP_NOP: break;
    case OP_LD_RAW: case OP_LD_STRUCTURED: case OP_STORE_RAW: case OP_STORE_STRUCTURED: memory_access(g, in); break;
    case OP_LD_UAV_TYPED: case OP_STORE_UAV_TYPED: typed_uav(g, in); break;
    case OP_BUFINFO: bufinfo(g, in); break;
    case OP_SYNC:
        // Thread-group synchronization; a fence alone (no thread sync) needs nothing here: Apple GPUs keep a
        // thread's own accesses in order, and other threads only see results after a barrier anyway.
        if ((in->token >> 11) & 1) line(g, "threadgroup_barrier(%s);", ((in->token >> 11) & 0xc) ? "mem_flags::mem_threadgroup | mem_flags::mem_device" : "mem_flags::mem_threadgroup");
        break;
    default:
        if (in->op >= OP_ATOMIC_AND && in->op <= OP_IMM_ATOMIC_UMIN) { atomic(g, in); break; }
        if ((in->op >= OP_DADD && in->op <= OP_FTOD) || (in->op >= OP_DDIV && in->op <= OP_DRCP) || (in->op >= OP_DTOI && in->op <= OP_UTOD)) { double_op(g, in); break; }
        fail(g, "instruction %u is not supported yet", in->op);
        break;
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
            if (op != OP_DCL_OUTPUT) {
                uint32_t sv = next(r);
                if (sv == 5) { g->vp_reg = (int)o.index[0]; g->vp_comp = first_comp(o.mask); }        // SV_ViewportArrayIndex
                else if (sv == 4) { g->rt_reg = (int)o.index[0]; g->rt_comp = first_comp(o.mask); }   // SV_RenderTargetArrayIndex
                else if (sv == 2 && g->clip_reg < 0) { g->clip_reg = (int)o.index[0]; g->clip_mask = o.mask; }   // SV_ClipDistance
                else g->out_sv[o.index[0]] = sv;
            }
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
    case OP_DCL_THREAD_GROUP:
        for (int i = 0; i < 3; i++) g->sh->threads[i] = next(r);
        break;
    case OP_DCL_UAV_TYPED: case OP_DCL_UAV_RAW: case OP_DCL_UAV_STRUCTURED: {
        read_operand(r, pl, &o);
        uint32_t u = o.index[0];
        if (u >= 8) { fail(g, "UAV u%u: only u0..u7 are supported", u); break; }
        g->sh->uavs |= 1u << u;
        g->sh->uav_kind[u] = op == OP_DCL_UAV_TYPED ? DXBC_UAV_TYPED : op == OP_DCL_UAV_RAW ? DXBC_UAV_RAW : DXBC_UAV_STRUCTURED;
        if (op == OP_DCL_UAV_TYPED) { g->uav_dim[u] = (uint8_t)((token >> 11) & 0x1f); g->uav_ret[u] = (uint8_t)(next(r) & 0xf); }
        if (op == OP_DCL_UAV_STRUCTURED) g->sh->uav_stride[u] = next(r);
        break;
    }
    case OP_DCL_TGSM_RAW: case OP_DCL_TGSM_STRUCTURED: {
        read_operand(r, pl, &o);
        uint32_t stride = 0, words;
        if (op == OP_DCL_TGSM_RAW) words = next(r) / 4;
        else { stride = next(r); words = stride * next(r) / 4; }
        if (g->ntgsm < 16) { g->tgsm[g->ntgsm].reg = o.index[0]; g->tgsm[g->ntgsm].words = words; g->tgsm[g->ntgsm].stride = stride; g->ntgsm++; }
        break;
    }
    case OP_DCL_RESOURCE_RAW: case OP_DCL_RESOURCE_STRUCTURED:
        read_operand(r, pl, &o);
        if (o.index[0] < 22) {
            g->sh->srv_buffers |= 1ull << o.index[0];
            g->sh->srv_stride[o.index[0]] = op == OP_DCL_RESOURCE_STRUCTURED ? next(r) : 0;
        } else fail(g, "buffer t%u: only t0..t21 are supported", o.index[0]);
        break;
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

// Constant buffers, textures, samplers, buffer SRVs and UAVs as function parameters (shared by all stages).
static void resource_params(gen *g, text *t) {
    dxbc_shader *sh = g->sh;
    for (int i = 0; i < 16; i++) if (sh->cbuffers & (1u << i)) put(t, ", constant float4 *cb%d [[buffer(%d)]]", i, i);
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
        put(t, ", %s<%s> t%d [[texture(%d)]]", tt, g->tex_cmp[i] ? "float" : rt, i, i);
    }
    for (int i = 0; i < 32; i++) if (sh->samplers & (1u << i)) put(t, ", sampler s%d [[sampler(%d)]]", i, i);
    for (int i = 0; i < 22; i++) if (sh->srv_buffers & (1ull << i)) put(t, ", const device uint *t%d [[buffer(%d)]]", i, DXBC_SRV_BUFFER_BASE + i);
    for (int i = 0; i < 8; i++) {
        if (!(sh->uavs & (1u << i))) continue;
        if (sh->uav_kind[i] != DXBC_UAV_TYPED) { put(t, ", device uint *u%d [[buffer(%d)]]", i, DXBC_UAV_BUFFER_BASE + i); continue; }
        const char *rt = g->uav_ret[i] == 3 ? "int" : g->uav_ret[i] == 4 ? "uint" : "float";
        const char *tt = g->uav_dim[i] == DIM_BUFFER ? "texture_buffer" : g->uav_dim[i] == DIM_1D ? "texture1d" : g->uav_dim[i] == DIM_1DARRAY ? "texture1d_array" :
                         g->uav_dim[i] == DIM_2DARRAY ? "texture2d_array" : g->uav_dim[i] == DIM_3D ? "texture3d" : "texture2d";
        put(t, ", %s<%s, access::%s> u%d [[texture(%d)]]", tt, rt, sh->uav_read[i] ? "read_write" : "write", i, DXBC_UAV_TEXTURE_BASE + i);
    }
    if (sh->uses_sizes) put(t, ", constant uint *vx_sizes [[buffer(%d)]]", DXBC_SIZES_BUFFER);
}
static void locals(gen *g, text *t) {
    for (int i = 0; i < g->temps; i++) put(t, "    float4 r%d = 0;\n", i);
    for (int i = 0; i < g->nxt; i++) put(t, "    float4 x%u[%u];\n", g->xt[i].reg, g->xt[i].size);
}

static char *compose(gen *g) {
    dxbc_shader *sh = g->sh;
    text t = { 0 };
    int vs = sh->type == DXBC_VERTEX, ps = sh->type == DXBC_PIXEL, cs = sh->type == DXBC_COMPUTE;
    put(&t, "#include <metal_stdlib>\nusing namespace metal;\n\n");
    if (g->sh->uav_counters)
        put(&t, "struct VxCounters { device atomic_uint *c[8]; };\n#define vx_counter(u) ((*reinterpret_cast<constant VxCounters *>(vx_sizes + 32)).c[u])\n\n");
    if (g->uses_msad)
        put(&t, "template <typename T> T vx_msad(T r, T s, T a) {\n"
                "    for (uint i = 0; i < 32; i += 8) { T rb = (r >> i) & 0xffu, sb = (s >> i) & 0xffu; a += select(T(0u), absdiff(rb, sb), rb != 0u); }\n"
                "    return a;\n}\n\n");
    if (g->icb_n) {
        put(&t, "constant uint4 icb_bits[%u] = {", g->icb_n / 4);
        for (uint32_t i = 0; i + 3 < g->icb_n; i += 4)
            put(&t, "%s\n    uint4(0x%xu, 0x%xu, 0x%xu, 0x%xu)", i ? "," : "", g->icb[i], g->icb[i + 1], g->icb[i + 2], g->icb[i + 3]);
        put(&t, "\n};\n#define icb reinterpret_cast<constant float4 *>(icb_bits)\n\n");
    }
    if (cs) {
        put(&t, "#define VX_RETURN return\n\n");
        put(&t, "kernel void vx_main(uint3 vx_tid [[thread_position_in_grid]], uint3 vx_gid [[threadgroup_position_in_grid]], "
                "uint3 vx_ltid [[thread_position_in_threadgroup]], uint vx_lidx [[thread_index_in_threadgroup]]");
        resource_params(g, &t);
        put(&t, ") {\n");
        locals(g, &t);
        for (int i = 0; i < g->ntgsm; i++) put(&t, "    threadgroup uint g%u[%u];\n", g->tgsm[i].reg, g->tgsm[i].words ? g->tgsm[i].words : 1);
        put(&t, "    float4 vx_thread_id = as_type<float4>(uint4(vx_tid, 0u));\n");
        put(&t, "    float4 vx_group_id = as_type<float4>(uint4(vx_gid, 0u));\n");
        put(&t, "    float4 vx_thread_in_group = as_type<float4>(uint4(vx_ltid, 0u));\n");
        put(&t, "    float vx_thread_index = as_type<float>(vx_lidx);\n");
        put(&t, "%s}\n", g->body.s ? g->body.s : "");
        return t.s;
    }
    // Stage inputs.
    int has_in = 0;
    put(&t, "struct VxIn {\n");
    for (int r = 0; r < 32; r++) {
        if (!g->in_decl[r]) continue;
        if (vs && !g->in_sv[r]) { put(&t, "    %s v%d [[attribute(%d)]];\n", vec_type(component_type(sh->inputs, sh->ninputs, (uint32_t)r)), r, r); has_in = 1; }
        if (ps && g->in_sv[r] == 1) { put(&t, "    float4 v%d [[position]];\n", r); has_in = 1; }
        if (ps && !g->in_sv[r] && g->in_eval[r]) {
            uint32_t m = g->in_interp[r];
            put(&t, "    interpolant<float4, interpolation::%s> v%d [[user(r%d)]];\n", m == 4 || m == 5 || m == 7 ? "no_perspective" : "perspective", r, r);
            has_in = 1;
        } else if (ps && !g->in_sv[r]) { put(&t, "    float4 v%d [[user(r%d)]]%s;\n", r, r, interp_qualifier(g->in_interp[r])); has_in = 1; }
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
    int has_position = 0;
    for (int r = 0; r < 32; r++) if (g->out_decl[r] && g->out_sv[r] == 1) has_position = 1;
    if (vs && !has_position) {                                  // a vertex shader for stream output only: Metal wants a position
        put(&t, "    float4 vx_position [[position]];\n");
        fk += (size_t)snprintf(finish + fk, sizeof finish - fk, "%sfloat4(0)", fk ? ", " : "");
    }
    if (vs && g->vp_reg >= 0) { put(&t, "    uint vx_viewport [[viewport_array_index]];\n"); fk += (size_t)snprintf(finish + fk, sizeof finish - fk, "%sas_type<uint>(o%d.%c)", fk ? ", " : "", g->vp_reg, CH[g->vp_comp]); }
    if (vs && g->rt_reg >= 0) { put(&t, "    uint vx_layer [[render_target_array_index]];\n"); fk += (size_t)snprintf(finish + fk, sizeof finish - fk, "%sas_type<uint>(o%d.%c)", fk ? ", " : "", g->rt_reg, CH[g->rt_comp]); }
    if (vs && g->clip_reg >= 0) {                              // the first register of clip distances (up to four)
        int n = 0; char list[128] = ""; size_t lk = 0;
        for (int c = 0; c < 4; c++) if (g->clip_mask & (1 << c)) { lk += (size_t)snprintf(list + lk, sizeof list - lk, "%so%d.%c", n ? ", " : "", g->clip_reg, CH[c]); n++; }
        put(&t, "    float vx_clip [[clip_distance]] [%d];\n", n);
        fk += (size_t)snprintf(finish + fk, sizeof finish - fk, "%s{%s}", fk ? ", " : "", list);
    }
    if (g->out_depth) { put(&t, "    float oDepth [[depth(any)]];\n"); fk += (size_t)snprintf(finish + fk, sizeof finish - fk, "%soDepth", fk ? ", " : ""); }
    if (g->out_coverage) { put(&t, "    uint oMask [[sample_mask]];\n"); fk += (size_t)snprintf(finish + fk, sizeof finish - fk, "%soMask", fk ? ", " : ""); }
    if (!fk) put(&t, "    float4 vx_unused [[user(unused)]];\n");
    put(&t, "};\n#define VX_RETURN return (VxOut{%s})\n\n", fk ? finish : "float4(0)");
    // The function.
    put(&t, "%s VxOut vx_main(%s", vs ? "vertex" : "fragment", has_in ? "VxIn vin [[stage_in]], " : "");
    if (vs) put(&t, "uint vx_vertex_id [[vertex_id]], uint vx_instance_id [[instance_id]], uint vx_base_instance [[base_instance]]");
    if (ps) put(&t, "bool vx_front_facing [[front_facing]], uint vx_sample_id [[sample_id]], uint vx_coverage [[sample_mask]], uint vx_primitive [[primitive_id]]");
    resource_params(g, &t);
    put(&t, ") {\n");
    locals(g, &t);
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
        else if (ps && g->in_eval[r]) {
            uint32_t m = g->in_interp[r];
            put(&t, "    float4 v%d = vin.v%d.%s;\n", r, r, m == 3 || m == 5 ? "interpolate_at_centroid()" : m == 6 || m == 7 ? "interpolate_at_sample(vx_sample_id)" : "interpolate_at_center()");
        }
        else put(&t, "    float4 v%d = as_type<float4>(vin.v%d);\n", r, r);
    }
    if (g->out_depth) put(&t, "    float oDepth = 0;\n");
    if (g->out_coverage) put(&t, "    uint oMask = 0xffffffffu;\n");
    if (g->in_primid) put(&t, "    float vPrim = as_type<float>(vx_primitive);\n");
    if (g->in_coverage) put(&t, "    float vCoverage = as_type<float>(vx_coverage);\n");
    put(&t, "%s    VX_RETURN;\n}\n", g->body.s ? g->body.s : "");
    return t.s;
}

// Translates instructions from `p` to the end of the function they belong to (the program's end, or the next
// label). A call inlines the subroutine's body (Direct3D has no recursion) in a one-pass loop, its returns
// becoming breaks. Returns nonzero for a truncated program.
static int run(gen *g, const uint32_t *p) {
    reader r = { p, g->prog_end, 0 };
    while (r.p < r.end && !g->why[0]) {
        const uint32_t *start = r.p;
        uint32_t token = next(&r), op = token & 0x7ff;
        uint32_t length = (token >> 24) & 0x7f;
        if (op == OP_CUSTOMDATA) {
            uint32_t total = next(&r), cls = token >> 11;
            if (cls == 3 && total >= 2 && !g->icb) {            // the immediate constant buffer
                g->icb_n = total - 2;
                g->icb = malloc(sizeof(uint32_t) * (g->icb_n ? g->icb_n : 1));
                memcpy(g->icb, r.p, sizeof(uint32_t) * g->icb_n);
            }
            r.p = start + (total ? total : 2);
            continue;
        }
        if (!length) { fail(g, "bad instruction length"); return 1; }
        if (op == OP_LABEL) break;
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
        if ((op >= OP_DCL_RESOURCE && op <= OP_DCL_GLOBAL_FLAGS) || (op >= OP_DCL_THREAD_GROUP && op <= OP_DCL_RESOURCE_STRUCTURED))
            declare(g, op, token, &r, &pl);
        else if (op >= 143 && op <= 154) { /* streams, interfaces, tessellation: later */ }
        else if (op == OP_CALL || op == OP_CALLC) {
            while (r.p < end && in.nops < 8) read_operand(&r, &pl, &in.ops[in.nops++]);
            const operand *label = &in.ops[op == OP_CALLC ? 1 : 0];
            const uint32_t *body = label->index[0] < 64 ? g->labels[label->index[0]] : NULL;
            if (!body || g->sub_depth >= 8) { fail(g, "call to an unknown or too deeply nested subroutine"); return 0; }
            if (op == OP_CALLC) { char c[800]; condition(g, &in, &in.ops[0], c, sizeof c); line(g, "if (%s) {", c); g->indent++; }
            line(g, "for (int vx_sub%d = 0; vx_sub%d < 1; vx_sub%d++) {", g->sub_depth, g->sub_depth, g->sub_depth);
            g->indent++;
            int saved = g->nesting;
            g->nesting = 0; g->sub_depth++;
            if (run(g, body)) return 1;
            g->sub_depth--; g->nesting = saved;
            g->indent--;
            line(g, "}");
            if (op == OP_CALLC) { g->indent--; line(g, "}"); }
        } else {
            while (r.p < end && in.nops < 8) read_operand(&r, &pl, &in.ops[in.nops++]);
            if (op == OP_LOOP || op == OP_SWITCH) g->nesting++;
            if (op == OP_ENDLOOP || op == OP_ENDSWITCH) g->nesting--;
            if (g->sub_depth && (op == OP_RET || op == OP_RETC)) {
                if (g->nesting) { fail(g, "a return inside a loop or switch of a subroutine"); return 0; }
                if (op == OP_RET) line(g, "break;");
                else { char c[800]; condition(g, &in, &in.ops[0], c, sizeof c); line(g, "if (%s) break;", c); }
            } else translate(g, &in);
        }
        r.p = end;
    }
    return r.bad;
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
    if (out->type != DXBC_VERTEX && out->type != DXBC_PIXEL && out->type != DXBC_COMPUTE) { snprintf(out->error, sizeof out->error, "shader type %d is not supported yet", out->type); return -1; }
    uint32_t count = tok[1];
    if (count * 4 > len) count = len / 4;
    gen g;
    memset(&g, 0, sizeof g);
    g.vp_reg = g.rt_reg = g.clip_reg = -1;
    g.sh = out;
    // First pass: textures sampled or gathered with a comparison are depth textures in Metal.
    for (reader q = { tok + 2, tok + count, 0 }; q.p < q.end && !q.bad;) {
        const uint32_t *start = q.p;
        uint32_t token = next(&q), op = token & 0x7ff, length = (token >> 24) & 0x7f;
        if (op == OP_CUSTOMDATA) { uint32_t total = next(&q); q.p = start + (total ? total : 2); continue; }
        if (!length) break;
        if (op == OP_SAMPLE_C || op == OP_SAMPLE_C_LZ || op == OP_GATHER4_C || op == OP_GATHER4_PO_C) {
            pool pl = { .used = 0 };
            operand ops[4];
            uint32_t ext = token;
            while (ext & 0x80000000u) ext = next(&q);
            int want = op == OP_GATHER4_PO_C ? 4 : 3;
            for (int k = 0; k < want && q.p < start + length; k++) read_operand(&q, &pl, &ops[k]);
            if (ops[want - 1].type == OPD_RESOURCE && ops[want - 1].index[0] < 128) g.tex_cmp[ops[want - 1].index[0]] = 1;
        }
        q.p = start + length;
    }
    // Subroutines: where each label's body starts (the main program ends at the first label).
    for (reader q = { tok + 2, tok + count, 0 }; q.p < q.end && !q.bad;) {
        const uint32_t *start = q.p;
        uint32_t token = next(&q), op = token & 0x7ff, length = (token >> 24) & 0x7f;
        if (op == OP_CUSTOMDATA) { uint32_t total = next(&q); q.p = start + (total ? total : 2); continue; }
        if (!length) break;
        if (op == OP_LABEL) {
            pool pl = { .used = 0 };
            operand o;
            read_operand(&q, &pl, &o);
            if (o.index[0] < 64) g.labels[o.index[0]] = start + length;
        }
        q.p = start + length;
    }
    g.prog_end = tok + count;
    int bad = run(&g, tok + 2);
    if (g.why[0] || bad) {
        snprintf(out->error, sizeof out->error, "%s", g.why[0] ? g.why : "truncated program");
        free(g.body.s); free(g.icb);
        return -1;
    }
    out->msl = compose(&g);
    free(g.body.s); free(g.icb);
    return 0;
}
void dxbc_free(dxbc_shader *s) { free(s->msl); s->msl = NULL; }
