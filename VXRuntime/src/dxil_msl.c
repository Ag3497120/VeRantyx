// DXIL -> Metal Shading Language (dxil_msl.h). Every SSA value becomes a local variable; the control flow is rebuilt
// as if/else and loops from the dominator trees (DXC emits structured control flow, and derivatives in pixel shaders
// need lanes to meet again where the source's branches met), and a function whose graph does not come out that way is
// written as a state machine instead. dx.op intrinsics are written out one by one; an intrinsic not done yet is an
// error naming it, so a game's shaders say what is missing.
#include "dxil_msl.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- text ---------------------------------------------------------------------------------------------------------

typedef struct { char *s; size_t n, cap; } sb;

static void sbv(sb *b, const char *fmt, va_list ap) {
    va_list cp;
    va_copy(cp, ap);
    int k = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (k < 0) return;
    if (b->n + k + 1 > b->cap) { b->cap = (b->n + k + 1) * 2; b->s = realloc(b->s, b->cap); }
    vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap);
    b->n += k;
}
static void put(sb *b, const char *fmt, ...) { va_list ap; va_start(ap, fmt); sbv(b, fmt, ap); va_end(ap); }

// ---- translation state --------------------------------------------------------------------------------------------

typedef struct { uint8_t cls, kind, comp; uint32_t slot; uint32_t stride; int heap, sampler_heap; uint8_t cmp; } handle_info;

typedef struct {
    const dxil_module *m;
    const dxil_function *f;
    dxil_msl *out;
    sb body, decls, prelude, types;
    int failed;
    int indent;
    // what the body needed
    int need_lane, need_lanes, need_vid, need_iid, need_front, need_sample_id, need_prim, need_tid, need_gid, need_ltid, need_lidx;
    // per value
    uint32_t nvals;
    handle_info *handles;       // by local index: the resource a handle value stands for
    uint8_t *is_handle;
    uint8_t *ptr_space;         // pointer values: 1 thread, 2 threadgroup, 3 constant, 4 device
    uint8_t *declared_types;    // struct and array types emitted
    // signatures (by element id)
    const dxil_md *insig, *outsig;
    // the control flow
    uint32_t nb;
    uint32_t *term;             // the terminator instruction of each block
    uint32_t *first;            // the first instruction of each block
    int *idom, *ipdom, *rpo_index, *loop_header_of, *loop_exit;
    uint8_t *emitted;
    uint32_t *loops;            // innermost loop stack while emitting: header blocks
    uint32_t nloops;
    char views[64][32];
    uint32_t nviews;
} tr;

static void err(tr *t, const char *fmt, ...) {
    if (t->failed) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->out->error, sizeof t->out->error, fmt, ap);
    va_end(ap);
    t->failed = 1;
}

static void line(tr *t, const char *fmt, ...) {
    for (int i = 0; i < t->indent; i++) put(&t->body, "    ");
    va_list ap;
    va_start(ap, fmt);
    sbv(&t->body, fmt, ap);
    va_end(ap);
    put(&t->body, "\n");
}

static const dxil_type *ty(tr *t, uint32_t id) { return id < t->m->ntypes ? &t->m->types[id] : NULL; }
static const dxil_value *val(tr *t, uint32_t id) { return dxil_value_at(t->m, t->f, id); }
static uint32_t local(tr *t, uint32_t id) { return id - t->m->nmodule_values; }

static int is_float_kind(int k) { return k == DXT_FLOAT || k == DXT_HALF || k == DXT_DOUBLE; }

// ---- types ---------------------------------------------------------------------------------------------------------

static void type_name(tr *t, uint32_t id, char *out, size_t cap);

static void declare_type(tr *t, uint32_t id) {
    const dxil_type *y = ty(t, id);
    if (!y || t->declared_types[id]) return;
    if (y->kind == DXT_STRUCT) {
        if (y->name && !strcmp(y->name, "dx.types.Handle")) return;
        t->declared_types[id] = 1;
        for (uint32_t i = 0; i < y->count; i++) declare_type(t, y->members[i]);
        put(&t->types, "struct vx_t%u {", id);
        for (uint32_t i = 0; i < y->count; i++) {
            char n[96];
            type_name(t, y->members[i], n, sizeof n);
            put(&t->types, " %s f%u;", n, i);
        }
        put(&t->types, " };%s%s\n", y->name ? "  // " : "", y->name ? y->name : "");
    } else if (y->kind == DXT_ARRAY) {
        t->declared_types[id] = 1;
        declare_type(t, y->elem);
        char n[96];
        type_name(t, y->elem, n, sizeof n);
        put(&t->types, "struct vx_t%u { %s e[%u]; };\n", id, n, y->count ? y->count : 1);
    }
}

static void type_name(tr *t, uint32_t id, char *out, size_t cap) {
    const dxil_type *y = ty(t, id);
    if (!y) { snprintf(out, cap, "uint"); return; }
    switch (y->kind) {
    case DXT_VOID: snprintf(out, cap, "void"); return;
    case DXT_HALF: snprintf(out, cap, "half"); return;
    case DXT_FLOAT: snprintf(out, cap, "float"); return;
    case DXT_DOUBLE: snprintf(out, cap, "float"); return;      // (Metal has no double: float, as the GPU would do)
    case DXT_INT:
        snprintf(out, cap, y->bits == 1 ? "bool" : y->bits <= 8 ? "uchar" : y->bits <= 16 ? "ushort" : y->bits <= 32 ? "uint" : "ulong");
        return;
    case DXT_VECTOR: {
        char e[32];
        type_name(t, y->elem, e, sizeof e);
        snprintf(out, cap, "%s%u", e, y->count);
        return;
    }
    case DXT_STRUCT:
        if (y->name && !strcmp(y->name, "dx.types.Handle")) { snprintf(out, cap, "device const VxDesc *"); return; }
        declare_type(t, id);
        snprintf(out, cap, "vx_t%u", id);
        return;
    case DXT_ARRAY: declare_type(t, id); snprintf(out, cap, "vx_t%u", id); return;
    default: snprintf(out, cap, "uint"); return;
    }
}

static int int_bits(tr *t, uint32_t id) {
    const dxil_type *y = ty(t, id);
    if (y && y->kind == DXT_VECTOR) y = ty(t, y->elem);
    return y && y->kind == DXT_INT ? (int)y->bits : 0;
}
static int is_float(tr *t, uint32_t id) {
    const dxil_type *y = ty(t, id);
    if (y && y->kind == DXT_VECTOR) y = ty(t, y->elem);
    return y && is_float_kind(y->kind);
}
static const char *signed_name(int bits) { return bits <= 8 ? "char" : bits <= 16 ? "short" : bits <= 32 ? "int" : "long"; }

// ---- values ---------------------------------------------------------------------------------------------------------

static void float_literal(double v, int half, char *out, size_t cap) {
    if (v != v || v - v != 0) {                                   // NaN, inf: by bits
        float f = (float)v;
        uint32_t u;
        memcpy(&u, &f, 4);
        snprintf(out, cap, "as_type<float>(0x%08xu)", u);
        return;
    }
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%.9g", v);
    if (!strpbrk(tmp, ".e")) strcat(tmp, ".0");
    snprintf(out, cap, half ? "half(%s)" : "%sf", tmp);
}

static int64_t const_int(tr *t, uint32_t id, int *ok);

// The MSL text of a value (constants written out).
static void ref(tr *t, uint32_t id, char *out, size_t cap) {
    const dxil_value *v = val(t, id);
    if (!v) { snprintf(out, cap, "v%u", local(t, id)); return; }
    char tn[96];
    switch (v->kind) {
    case DXV_CONST_INT: {
        int bits = int_bits(t, v->type);
        if (bits == 1) snprintf(out, cap, v->u.i ? "true" : "false");
        else if (bits > 32) snprintf(out, cap, "%lluul", (unsigned long long)v->u.i);
        else {
            uint32_t u = (uint32_t)v->u.i;
            if (bits && bits < 32) {
                u &= (1u << bits) - 1;
                type_name(t, v->type, tn, sizeof tn);
                snprintf(out, cap, "%s(%u)", tn, u);
            } else snprintf(out, cap, "%uu", u);
        }
        return;
    }
    case DXV_CONST_FLOAT: {
        const dxil_type *y = ty(t, v->type);
        float_literal(v->u.f, y && y->kind == DXT_HALF, out, cap);
        return;
    }
    case DXV_CONST_NULL: case DXV_CONST_UNDEF: {
        const dxil_type *y = ty(t, v->type);
        if (y && y->kind == DXT_INT && y->bits == 1) { snprintf(out, cap, "false"); return; }
        if (y && (y->kind == DXT_STRUCT || y->kind == DXT_ARRAY)) { type_name(t, v->type, tn, sizeof tn); snprintf(out, cap, "%s{}", tn); return; }
        if (y && y->kind == DXT_POINTER) { snprintf(out, cap, "nullptr"); return; }
        type_name(t, v->type, tn, sizeof tn);
        snprintf(out, cap, "%s(0)", tn);
        return;
    }
    case DXV_GLOBAL: snprintf(out, cap, "g%u", v->u.index); return;
    case DXV_CONST_EXPR: {
        // (getelementptr into a global, a cast of one: the address written out)
        if (v->u.expr.op == DXI_CAST && v->u.expr.n == 1) { ref(t, v->u.expr.ops[0], out, cap); return; }
        if (v->u.expr.op == DXI_GEP && v->u.expr.n >= 2) {
            char base[64], path[256];
            ref(t, v->u.expr.ops[1], base, sizeof base);
            const dxil_value *bv = val(t, v->u.expr.ops[1]);
            uint32_t tid = v->u.expr.ops[0];
            if (tid == UINT32_MAX && bv && bv->kind == DXV_GLOBAL) tid = t->m->globals[bv->u.index].type;
            snprintf(path, sizeof path, "(%s)", base);
            for (uint32_t i = 3; i < v->u.expr.n; i++) {
                const dxil_type *y = ty(t, tid);
                int ok;
                int64_t k = const_int(t, v->u.expr.ops[i], &ok);
                size_t l = strlen(path);
                if (y && y->kind == DXT_STRUCT) { snprintf(path + l, sizeof path - l, ".f%lld", (long long)k); tid = (uint32_t)k < y->count ? y->members[k] : 0; }
                else if (y && y->kind == DXT_ARRAY) { snprintf(path + l, sizeof path - l, ".e[%lld]", (long long)k); tid = y->elem; }
                else { snprintf(path + l, sizeof path - l, "[%lld]", (long long)k); tid = y ? y->elem : 0; }
            }
            snprintf(out, cap, "(&%s)", path);
            return;
        }
        err(t, "a constant expression (%u)", v->u.expr.op);
        snprintf(out, cap, "0");
        return;
    }
    case DXV_INST: case DXV_ARG: default: snprintf(out, cap, "v%u", local(t, id)); return;
    }
}

static int64_t const_int(tr *t, uint32_t id, int *ok) {
    const dxil_value *v = val(t, id);
    if (v && v->kind == DXV_CONST_INT) { if (ok) *ok = 1; return v->u.i; }
    if (v && (v->kind == DXV_CONST_NULL)) { if (ok) *ok = 1; return 0; }
    if (ok) *ok = 0;
    return 0;
}
static int is_undef(tr *t, uint32_t id) { const dxil_value *v = val(t, id); return v && v->kind == DXV_CONST_UNDEF; }

// The address space of a pointer value: 1 thread, 2 threadgroup, 3 constant.
static uint8_t space_of(tr *t, uint32_t id) {
    const dxil_value *v = val(t, id);
    if (!v) return 1;
    if (v->kind == DXV_GLOBAL) { const dxil_global *g = &t->m->globals[v->u.index]; return g->addrspace == 3 ? 2 : g->constant ? 3 : 1; }
    if (v->kind == DXV_CONST_EXPR && v->u.expr.n >= 1) return space_of(t, v->u.expr.op == DXI_GEP ? v->u.expr.ops[1] : v->u.expr.ops[0]);
    if (id >= t->m->nmodule_values && local(t, id) < t->nvals && t->ptr_space[local(t, id)]) return t->ptr_space[local(t, id)];
    return 1;
}

// ---- metadata: resources and signatures ---------------------------------------------------------------------------

static int md_i(tr *t, const dxil_md *node, uint32_t i, int64_t def) {
    int64_t v;
    return dxil_md_int(t->m, dxil_md_op(t->m, node, i), &v) ? (int)v : (int)def;
}

// Extended properties of a resource: tag/value pairs (0: element type, 1: structure stride).
static int64_t md_tag(tr *t, const dxil_md *list, int tag, int64_t def) {
    if (!list || list->kind != DXM_NODE) return def;
    for (uint32_t i = 0; i + 1 < list->n; i += 2) {
        int64_t k, v;
        if (dxil_md_int(t->m, dxil_md_op(t->m, list, i), &k) && k == tag && dxil_md_int(t->m, dxil_md_op(t->m, list, i + 1), &v)) return v;
    }
    return def;
}

static void read_resources(tr *t) {
    const dxil_named_md *nm = dxil_named(t->m, "dx.resources");
    if (!nm || !nm->n) return;
    const dxil_md *lists = &t->m->md[nm->nodes[0]];
    for (int cls = 0; cls < 4; cls++) {
        const dxil_md *list = dxil_md_op(t->m, lists, cls);
        for (uint32_t i = 0; list && i < list->n; i++) {
            const dxil_md *r = dxil_md_op(t->m, list, i);
            if (!r || t->out->nslots >= 64) continue;
            dxil_msl_slot *s = &t->out->slots[t->out->nslots++];
            memset(s, 0, sizeof *s);
            s->cls = (uint8_t)cls;
            s->space = (uint32_t)md_i(t, r, 3, 0);
            s->lower = (uint32_t)md_i(t, r, 4, 0);
            s->count = (uint32_t)md_i(t, r, 5, 1);
            if (cls == DXIL_SRV || cls == DXIL_UAV) {
                s->kind = (uint8_t)md_i(t, r, 6, 0);
                const dxil_md *ext = dxil_md_op(t->m, r, cls == DXIL_SRV ? 8 : 10);
                s->comp = (uint8_t)md_tag(t, ext, 0, 9);
                s->stride = (uint32_t)md_tag(t, ext, 1, 0);
            } else if (cls == DXIL_CBV) {
                s->kind = 13;
            } else {
                s->kind = 14;
                s->comp = (uint8_t)md_i(t, r, 6, 0);              // (1: a comparison sampler)
            }
        }
    }
}

// The slot of a resource range by class and range id (createHandle: the range id is its place in its class's list).
static int slot_by_range(tr *t, int cls, uint32_t range) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < t->out->nslots; i++) if (t->out->slots[i].cls == cls) { if (k == range) return (int)i; k++; }
    return -1;
}
static int slot_by_binding(tr *t, int cls, uint32_t space, uint32_t reg) {
    for (uint32_t i = 0; i < t->out->nslots; i++) {
        const dxil_msl_slot *s = &t->out->slots[i];
        if (s->cls == cls && s->space == space && reg >= s->lower && (s->count == UINT32_MAX || reg - s->lower < s->count)) return (int)i;
    }
    return -1;
}

static const dxil_md *signature_element(const dxil_md *sig, tr *t, uint32_t id) {
    for (uint32_t i = 0; sig && i < sig->n; i++) {
        const dxil_md *e = dxil_md_op(t->m, sig, i);
        if (e && md_i(t, e, 0, -1) == (int)id) return e;
    }
    return NULL;
}

static const char *md_str(tr *t, const dxil_md *node, uint32_t i) {
    const dxil_md *s = dxil_md_op(t->m, node, i);
    return s && s->kind == DXM_STRING ? s->str : "";
}

static void read_signatures(tr *t) {
    const dxil_named_md *nm = dxil_named(t->m, "dx.entryPoints");
    if (!nm || !nm->n) return;
    const dxil_md *ep = &t->m->md[nm->nodes[0]];
    const dxil_md *sigs = dxil_md_op(t->m, ep, 2);
    t->insig = dxil_md_op(t->m, sigs, 0);
    t->outsig = dxil_md_op(t->m, sigs, 1);
    const dxil_md *props = dxil_md_op(t->m, ep, 4);
    t->out->threads[0] = t->out->threads[1] = t->out->threads[2] = 1;
    for (uint32_t i = 0; props && i + 1 < props->n; i += 2) {
        int64_t tag;
        if (!dxil_md_int(t->m, dxil_md_op(t->m, props, i), &tag)) continue;
        if (tag == 4) {                                            // NumThreads
            const dxil_md *n = dxil_md_op(t->m, props, i + 1);
            for (int k = 0; k < 3; k++) t->out->threads[k] = (uint32_t)md_i(t, n, k, 1);
        }
    }
}

// The MSL member name and type of a signature element row: "i<id>_<row>" / "o<id>_<row>".
static void element_type(tr *t, const dxil_md *e, char *out, size_t cap) {
    int comp = md_i(t, e, 2, 9), cols = md_i(t, e, 7, 4);
    const char *base = comp == 1 ? "bool" : comp == 2 ? "short" : comp == 3 ? "ushort" : comp == 4 ? "int" : comp == 5 ? "uint"
                     : comp == 8 ? "half" : "float";
    if (cols <= 1) snprintf(out, cap, "%s", base);
    else snprintf(out, cap, "%s%d", base, cols);
}

// ---- handles ------------------------------------------------------------------------------------------------------

static const char *tex_type(int kind, int comp, int uav) {
    static char out[96];
    const char *c = comp == 4 || comp == 2 ? "int" : comp == 5 || comp == 3 ? "uint" : comp == 8 ? "half" : "float";
    const char *dim = kind == 1 ? "texture1d" : kind == 2 ? "texture2d" : kind == 3 ? "texture2d_ms" : kind == 4 ? "texture3d" : kind == 5 ? "texturecube"
                    : kind == 6 ? "texture1d_array" : kind == 7 ? "texture2d_array" : kind == 8 ? "texture2d_ms_array" : kind == 9 ? "texturecube_array"
                    : kind == 10 ? "texture_buffer" : "texture2d";
    snprintf(out, sizeof out, "%s<%s%s>", dim, c, uav ? ", access::read_write" : "");
    return out;
}

// The view struct a handle is read through: the descriptor with its resource id as the right Metal type.
static const char *view(tr *t, const handle_info *h) {
    char key[32];
    if (h->cls == DXIL_SAMPLER) snprintf(key, sizeof key, "VxS");
    else snprintf(key, sizeof key, "VxV%d_%d_%d", h->kind, h->comp, h->cls == DXIL_UAV);
    for (uint32_t i = 0; i < t->nviews; i++) if (!strcmp(t->views[i], key)) return t->views[i];
    if (t->nviews >= 64) { err(t, "too many resource views"); return "VxDesc"; }
    strcpy(t->views[t->nviews], key);
    if (h->cls == DXIL_SAMPLER) put(&t->prelude, "struct VxS { ulong va; sampler s; uint size, stride, fmt, flags; };\n");
    else put(&t->prelude, "struct %s { ulong va; %s t; uint size, stride, fmt, flags; };\n", key, tex_type(h->kind, h->comp, h->cls == DXIL_UAV));
    return t->views[t->nviews++];
}

static handle_info *hinfo(tr *t, uint32_t id) {
    uint32_t l = local(t, id);
    if (id < t->m->nmodule_values || l >= t->nvals || !t->is_handle[l]) return NULL;
    return &t->handles[l];
}

// The texture expression of a handle: ((device const VxV.. *)h)->t
static void tex_of(tr *t, uint32_t id, char *out, size_t cap) {
    handle_info *h = hinfo(t, id);
    char r[64];
    ref(t, id, r, sizeof r);
    if (!h) { err(t, "a resource handle that does not come from createHandle"); snprintf(out, cap, "0"); return; }
    snprintf(out, cap, "((device const %s *)%s)->%s", view(t, h), r, h->cls == DXIL_SAMPLER ? "s" : "t");
}

// ---- instructions ---------------------------------------------------------------------------------------------------

static void args(tr *t, const dxil_inst *in, uint32_t from, uint32_t n, char (*a)[96]) {
    for (uint32_t i = 0; i < n; i++) {
        if (from + i < in->n) ref(t, in->ops[from + i], a[i], 96);
        else snprintf(a[i], 96, "0");
    }
}

static int coord_count(int kind) {
    return kind == 1 ? 1 : kind == 2 || kind == 3 ? 2 : kind == 6 ? 1 : kind == 7 || kind == 8 ? 2 : kind == 4 || kind == 5 ? 3 : kind == 9 ? 3 : 1;
}
static int is_array_kind(int kind) { return kind == 6 || kind == 7 || kind == 8 || kind == 9; }

// coordinates as a vector "float2(a, b)" (and the array index separately for array kinds)
static void coords(tr *t, const dxil_inst *in, uint32_t from, int kind, const char *ctype, char *out, size_t cap, char *layer, size_t lcap) {
    char a[4][96];
    args(t, in, from, 4, a);
    int n = coord_count(kind);
    if (n == 1) snprintf(out, cap, "%s(%s)", ctype, a[0]);
    else if (n == 2) snprintf(out, cap, "%s2(%s, %s)", ctype, a[0], a[1]);
    else snprintf(out, cap, "%s3(%s, %s, %s)", ctype, a[0], a[1], a[2]);
    if (layer) {
        if (is_array_kind(kind)) snprintf(layer, lcap, ", uint(%s)", a[n]);
        else layer[0] = 0;
    }
}

static void offsets(tr *t, const dxil_inst *in, uint32_t from, int kind, char *out, size_t cap) {
    int n = kind == 4 ? 3 : coord_count(kind) >= 2 ? 2 : 0;
    out[0] = 0;
    if (kind == 5 || kind == 9 || n == 0) return;               // (no offsets on cubes, nor in Metal's 1D sampling)
    int any = 0;
    int v[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        if (from + i >= in->n || is_undef(t, in->ops[from + i])) continue;
        int ok;
        v[i] = (int)const_int(t, in->ops[from + i], &ok);
        if (!ok) { err(t, "texture offsets that are not constants"); return; }
        any |= v[i] != 0;
    }
    if (!any) return;
    if (n == 2) snprintf(out, cap, ", int2(%d, %d)", v[0], v[1]); else snprintf(out, cap, ", int3(%d, %d, %d)", v[0], v[1], v[2]);
}

// A ResRet-style result: "T{ x, y, z, w, 0 }" from a 4-vector expression.
static void resret(tr *t, const dxil_inst *in, const char *vec4, int status) {
    char tn[96], r[32];
    type_name(t, in->type, tn, sizeof tn);
    ref(t, in->result, r, sizeof r);
    const dxil_type *y = ty(t, in->type);
    char e[4][32];
    for (int i = 0; i < 4; i++) {
        char mt[32];
        type_name(t, y && y->count > (uint32_t)i ? y->members[i] : 0, mt, sizeof mt);
        snprintf(e[i], sizeof e[i], "%s", mt);
    }
    line(t, "{ auto vx_r = %s; %s = %s{ %s(vx_r.x), %s(vx_r.y), %s(vx_r.z), %s(vx_r.w)%s }; }", vec4, r, tn, e[0], e[1], e[2], e[3],
         y && y->count > 4 ? (status ? ", 1u" : ", 0u") : "");
}

static int buffer_kind(int kind) { return kind == 11 ? 1 : kind == 12 ? 2 : kind == 10 || kind == 15 ? 3 : 0; }   // raw, structured, typed

static void call(tr *t, const dxil_inst *in) {
    int op = dxil_dxop(t->m, t->f, in);
    const char *callee = dxil_value_name(t->m, in->ops[0]);
    if (op < 0) { err(t, "a call to %s", callee ? callee : "?"); return; }
    char a[12][96], r[32] = "", tn[96] = "";
    args(t, in, 2, in->n > 2 ? (in->n - 2 < 12 ? in->n - 2 : 12) : 0, a);
    if (in->result != UINT32_MAX) { ref(t, in->result, r, sizeof r); type_name(t, in->type, tn, sizeof tn); }
    int fp = in->result != UINT32_MAX && is_float(t, in->type);
    int bits = in->result != UINT32_MAX ? int_bits(t, in->type) : 0;
    switch (op) {
    case 4: {                                                      // LoadInput(id, row, col, axis)
        int ok, okc;
        uint32_t id = (uint32_t)const_int(t, in->ops[2], &ok), col = (uint32_t)const_int(t, in->ops[4], &okc);
        const dxil_md *e = signature_element(t->insig, t, id);
        if (!e) { err(t, "input %u is not in the signature", id); return; }
        int kind = md_i(t, e, 3, 0), cols = md_i(t, e, 7, 4);
        int okr;
        uint32_t row = (uint32_t)const_int(t, in->ops[3], &okr);
        if (!okr) { err(t, "an input row chosen at run time"); return; }
        if (kind == 1) { t->need_vid = 1; line(t, "%s = %s(vx_vid);", r, tn); return; }
        if (kind == 2) { t->need_iid = 1; line(t, "%s = %s(vx_iid);", r, tn); return; }
        if (kind == 13) { t->need_front = 1; line(t, "%s = %s(vx_front);", r, tn); return; }
        if (kind == 12) { t->need_sample_id = 1; line(t, "%s = %s(vx_sample);", r, tn); return; }
        if (kind == 10) { t->need_prim = 1; line(t, "%s = %s(vx_prim);", r, tn); return; }
        if (cols <= 1) line(t, "%s = %s(vx_in.i%u_%u);", r, tn, id, row);
        else line(t, "%s = %s(vx_in.i%u_%u[%u]);", r, tn, id, row, okc ? col : 0);
        return;
    }
    case 5: {                                                      // StoreOutput(id, row, col, value)
        int ok, okc, okr;
        uint32_t id = (uint32_t)const_int(t, in->ops[2], &ok), row = (uint32_t)const_int(t, in->ops[3], &okr), col = (uint32_t)const_int(t, in->ops[4], &okc);
        const dxil_md *e = signature_element(t->outsig, t, id);
        if (!e || !okr) { err(t, "output %u is not in the signature", id); return; }
        char et[32];
        element_type(t, e, et, sizeof et);
        int cols = md_i(t, e, 7, 4);
        // (the element's own type: float for SV_Target, uint for an integer output)
        char base[16];
        snprintf(base, sizeof base, "%.*s", (int)strcspn(et, "0123456789"), et);
        if (cols <= 1) line(t, "vx_out.o%u_%u = %s(%s);", id, row, base, a[3]);
        else line(t, "vx_out.o%u_%u[%u] = %s(%s);", id, row, okc ? col : 0, base, a[3]);
        return;
    }
    case 6: line(t, "%s = %s;", r, fp ? (snprintf(a[11], 96, "fabs(%s)", a[0]), a[11]) : (snprintf(a[11], 96, "%s(abs(as_type<%s>(%s)))", tn, signed_name(bits), a[0]), a[11])); return;
    case 7: line(t, "%s = saturate(%s);", r, a[0]); return;
    case 8: line(t, "%s = isnan(%s);", r, a[0]); return;
    case 9: line(t, "%s = isinf(%s);", r, a[0]); return;
    case 10: line(t, "%s = isfinite(%s);", r, a[0]); return;
    case 11: line(t, "%s = isnormal(%s);", r, a[0]); return;
    case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24: case 25:
    case 26: case 27: case 28: case 29: {
        static const char *fn[] = { "cos", "sin", "tan", "acos", "asin", "atan", "cosh", "sinh", "tanh", "exp2", "fract", "log2", "sqrt", "rsqrt",
                                    "rint", "floor", "ceil", "trunc" };
        line(t, "%s = %s(%s);", r, fn[op - 12], a[0]);
        return;
    }
    case 30: line(t, "%s = reverse_bits(%s);", r, a[0]); return;
    case 31: line(t, "%s = %s(popcount(%s));", r, tn, a[0]); return;
    case 32: line(t, "%s = %s(%s == 0 ? 0xFFFFFFFFu : uint(ctz(%s)));", r, tn, a[0], a[0]); return;
    case 33: line(t, "%s = %s(%s == 0 ? 0xFFFFFFFFu : uint(clz(%s)));", r, tn, a[0], a[0]); return;
    case 34: line(t, "{ int vx_s = as_type<int>(uint(%s)); uint vx_u = vx_s < 0 ? ~uint(vx_s) : uint(vx_s); %s = %s(vx_u == 0 ? 0xFFFFFFFFu : uint(clz(vx_u))); }", a[0], r, tn); return;
    case 35: line(t, "%s = fmax(%s, %s);", r, a[0], a[1]); return;
    case 36: line(t, "%s = fmin(%s, %s);", r, a[0], a[1]); return;
    case 37: line(t, "%s = %s(max(as_type<%s>(%s), as_type<%s>(%s)));", r, tn, signed_name(bits), a[0], signed_name(bits), a[1]); return;
    case 38: line(t, "%s = %s(min(as_type<%s>(%s), as_type<%s>(%s)));", r, tn, signed_name(bits), a[0], signed_name(bits), a[1]); return;
    case 39: line(t, "%s = max(%s, %s);", r, a[0], a[1]); return;
    case 40: line(t, "%s = min(%s, %s);", r, a[0], a[1]); return;
    case 46: case 47: line(t, "%s = fma(%s, %s, %s);", r, a[0], a[1], a[2]); return;
    case 48: line(t, "%s = %s(as_type<int>(%s) * as_type<int>(%s) + as_type<int>(%s));", r, tn, a[0], a[1], a[2]); return;
    case 49: line(t, "%s = %s * %s + %s;", r, a[0], a[1], a[2]); return;
    case 51: line(t, "%s = %s(extract_bits(as_type<int>(%s), uint(%s) & 31, uint(%s) & 31));", r, tn, a[2], a[1], a[0]); return;
    case 52: line(t, "%s = extract_bits(%s, uint(%s) & 31, uint(%s) & 31);", r, a[2], a[1], a[0]); return;
    case 53: line(t, "%s = insert_bits(%s, %s, uint(%s) & 31, uint(%s) & 31);", r, a[3], a[2], a[1], a[0]); return;
    case 54: line(t, "%s = dot(float2(%s, %s), float2(%s, %s));", r, a[0], a[1], a[2], a[3]); return;
    case 55: line(t, "%s = dot(float3(%s, %s, %s), float3(%s, %s, %s));", r, a[0], a[1], a[2], a[3], a[4], a[5]); return;
    case 56: line(t, "%s = dot(float4(%s, %s, %s, %s), float4(%s, %s, %s, %s));", r, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); return;
    case 57: case 217: case 218: {                                 // CreateHandle, CreateHandleFromBinding, CreateHandleFromHeap
        handle_info *h = &t->handles[local(t, in->result)];
        t->is_handle[local(t, in->result)] = 1;
        memset(h, 0, sizeof *h);
        if (op == 218) {
            int ok;
            int sampler = (int)const_int(t, in->ops[3], &ok);
            h->heap = !sampler;
            h->sampler_heap = sampler;
            h->cls = sampler ? DXIL_SAMPLER : DXIL_SRV;
            if (sampler) t->out->uses_sampler_heap = 1; else t->out->uses_heap = 1;
            line(t, "%s = (device const VxDesc *)(vx_root.%s) + uint(%s);", r, sampler ? "samplers" : "heap", a[0]);
            return;                                                // (its kind comes with annotateHandle)
        }
        int slot = -1;
        char index[96];
        if (op == 57) {
            int ok1, ok2;
            int cls = (int)const_int(t, in->ops[2], &ok1);
            uint32_t range = (uint32_t)const_int(t, in->ops[3], &ok2);
            slot = slot_by_range(t, cls, range);
            snprintf(index, sizeof index, "%s", a[2]);
        } else {
            const dxil_value *bind = val(t, in->ops[2]);           // ResBind { lower, upper, space, class }
            if (!bind || bind->kind != DXV_CONST_AGGREGATE || bind->u.list.n < 4) { err(t, "createHandleFromBinding without a constant binding"); return; }
            int ok;
            uint32_t lower = (uint32_t)const_int(t, bind->u.list.ids[0], &ok), space = (uint32_t)const_int(t, bind->u.list.ids[2], &ok);
            int cls = (int)const_int(t, bind->u.list.ids[3], &ok);
            slot = slot_by_binding(t, cls, space, lower);
            snprintf(index, sizeof index, "%s", a[1]);
        }
        if (slot < 0) { err(t, "a handle for a resource not in dx.resources"); return; }
        const dxil_msl_slot *s = &t->out->slots[slot];
        h->cls = s->cls; h->kind = s->kind; h->comp = s->comp; h->slot = (uint32_t)slot; h->stride = s->stride;
        line(t, "%s = (device const VxDesc *)(vx_root.slot[%d]) + (uint(%s) - %uu);", r, slot, index, s->lower);
        return;
    }
    case 216: {                                                    // AnnotateHandle(res, props)
        handle_info *src = hinfo(t, in->ops[2]);
        handle_info *h = &t->handles[local(t, in->result)];
        t->is_handle[local(t, in->result)] = 1;
        if (src) *h = *src; else memset(h, 0, sizeof *h);
        const dxil_value *p = val(t, in->ops[3]);
        if (p && p->kind == DXV_CONST_AGGREGATE && p->u.list.n >= 2) {
            int ok;
            uint32_t basic = (uint32_t)const_int(t, p->u.list.ids[0], &ok), extra = (uint32_t)const_int(t, p->u.list.ids[1], &ok);
            h->kind = basic & 0xFF;
            if (h->cls != DXIL_SAMPLER) h->cls = (basic >> 12) & 1 ? DXIL_UAV : (h->kind == 13 ? DXIL_CBV : h->kind == 14 ? DXIL_SAMPLER : DXIL_SRV);
            if (h->kind == 12) h->stride = extra; else if (h->kind != 11 && h->kind != 13) h->comp = extra & 0xFF;
            if (h->kind == 14) h->cls = DXIL_SAMPLER;
        }
        line(t, "%s = %s;", r, a[0]);
        return;
    }
    case 59: {                                                     // CBufferLoadLegacy(handle, row)
        const dxil_type *y = ty(t, in->type);
        int f32 = y && y->count && is_float(t, y->members[0]) && ty(t, y->members[0])->kind == DXT_FLOAT;
        int h16 = y && y->count == 8;
        if (h16) { err(t, "16-bit constant buffer loads"); return; }
        char vec[160];
        snprintf(vec, sizeof vec, "vx_cb(%s, uint(%s))", a[0], a[1]);
        if (f32) resret(t, in, vec, 0);
        else {
            char e[96];
            snprintf(e, sizeof e, "as_type<uint4>(%s)", vec);
            resret(t, in, e, 0);
        }
        return;
    }
    case 60: case 61: case 62: case 63: case 64: case 65: {       // Sample, SampleBias, SampleLevel, SampleGrad, SampleCmp, SampleCmpLevelZero
        handle_info *h = hinfo(t, in->ops[2]);
        if (!h) { err(t, "sampling a handle that does not come from createHandle"); return; }
        if (op == 64 || op == 65) { err(t, "comparison sampling (SampleCmp) is not done yet"); return; }
        char tex[160], smp[160], c[256], layer[128], off[64], opt[256] = "";
        tex_of(t, in->ops[2], tex, sizeof tex);
        tex_of(t, in->ops[3], smp, sizeof smp);
        coords(t, in, 4, h->kind, "float", c, sizeof c, layer, sizeof layer);
        offsets(t, in, 8, h->kind, off, sizeof off);
        int n = in->n - 2;
        if (op == 61) snprintf(opt, sizeof opt, ", bias(%s)", a[9]);
        if (op == 62) snprintf(opt, sizeof opt, ", level(%s)", a[9]);
        if (op == 63) {
            int k = coord_count(h->kind);
            const char *g = h->kind == 5 || h->kind == 9 ? "gradientcube" : k == 3 ? "gradient3d" : "gradient2d";
            if (k == 3) snprintf(opt, sizeof opt, ", %s(float3(%s, %s, %s), float3(%s, %s, %s))", g, a[9], a[10], a[11], a[12 < n ? 11 : 11], a[11], a[11]);
            else snprintf(opt, sizeof opt, ", %s(float2(%s, %s), float2(%s, %s))", g, a[9], a[10], a[11 + 1 < 12 ? 11 : 11], a[11]);
            if (k == 3 || h->kind == 5) { err(t, "SampleGrad on 3D and cube textures is not done yet"); return; }
            char b[4][96];
            args(t, in, 2 + 9, 4, b);
            snprintf(opt, sizeof opt, ", gradient2d(float2(%s, %s), float2(%s, %s))", b[0], b[2 - 0 > 0 ? 1 : 1], b[2], b[3]);
            // (DXIL: ddx0, ddx1, ddx2, ddy0, ddy1, ddy2)
            char d[6][96];
            args(t, in, 2 + 9, 6, d);
            snprintf(opt, sizeof opt, ", gradient2d(float2(%s, %s), float2(%s, %s))", d[0], d[1], d[3], d[4]);
        }
        if ((op == 60 || op == 61) && in->n > 2 + 10 && !is_undef(t, in->ops[2 + (op == 60 ? 9 : 10)])) {
            char cl[96];
            ref(t, in->ops[2 + (op == 60 ? 9 : 10)], cl, sizeof cl);
            if (h->kind == 2 || h->kind == 7 || h->kind == 4 || h->kind == 5) { size_t l = strlen(opt); snprintf(opt + l, sizeof opt - l, ", min_lod_clamp(%s)", cl); }
        }
        if (h->kind == 1 || h->kind == 6) opt[0] = 0;              // (Metal's 1D textures take no level options)
        char e[700];
        snprintf(e, sizeof e, "float4(%s.sample(%s, %s%s%s%s))", tex, smp, c, layer, opt, off);
        resret(t, in, e, 1);
        return;
    }
    case 66: {                                                     // TextureLoad(srv, mip or sample, c0, c1, c2, o0, o1, o2)
        handle_info *h = hinfo(t, in->ops[2]);
        if (!h) { err(t, "a load from a handle that does not come from createHandle"); return; }
        char tex[160], c[256], layer[128];
        tex_of(t, in->ops[2], tex, sizeof tex);
        coords(t, in, 4, h->kind, "uint", c, sizeof c, layer, sizeof layer);
        char e[700];
        if (h->kind == 10) snprintf(e, sizeof e, "float4(%s.read(uint(%s)))", tex, a[2]);   // (typed buffer as a texture buffer)
        else if (h->kind == 3 || h->kind == 8) snprintf(e, sizeof e, "float4(%s.read(%s%s, uint(%s)))", tex, c, layer, a[1]);
        else if (h->cls == DXIL_UAV || h->kind == 1 || h->kind == 6) snprintf(e, sizeof e, "float4(%s.read(%s%s))", tex, c, layer);
        else snprintf(e, sizeof e, "float4(%s.read(%s%s, uint(%s)))", tex, c, layer, a[1]);
        int ints = h->comp == 4 || h->comp == 5 || h->comp == 2 || h->comp == 3;
        if (ints) { char *p = strstr(e, "float4("); if (p) memcpy(p, "uint4( ", 7); }
        resret(t, in, e, 1);
        return;
    }
    case 67: {                                                     // TextureStore(uav, c0, c1, c2, v0..v3, mask)
        handle_info *h = hinfo(t, in->ops[2]);
        if (!h) { err(t, "a store to a handle that does not come from createHandle"); return; }
        char tex[160], c[256], layer[128];
        tex_of(t, in->ops[2], tex, sizeof tex);
        coords(t, in, 3, h->kind, "uint", c, sizeof c, layer, sizeof layer);
        int ints = h->comp == 4 || h->comp == 5 || h->comp == 2 || h->comp == 3;
        const char *vt = h->comp == 4 || h->comp == 2 ? "int4" : ints ? "uint4" : h->comp == 8 ? "half4" : "float4";
        line(t, "%s.write(%s(%s, %s, %s, %s), %s%s);", tex, vt, a[4], a[5], a[6], a[7], c, layer);
        return;
    }
    case 68: case 139: {                                           // BufferLoad(res, index, wot), RawBufferLoad(res, index, offset, mask, align)
        handle_info *h = hinfo(t, in->ops[2]);
        if (!h) { err(t, "a buffer load from a handle that does not come from createHandle"); return; }
        int bk = buffer_kind(h->kind);
        char e[400];
        if (bk == 3) {
            char tex[160];
            tex_of(t, in->ops[2], tex, sizeof tex);
            int ints = h->comp == 4 || h->comp == 5 || h->comp == 2 || h->comp == 3;
            snprintf(e, sizeof e, "%s(%s.read(uint(%s)))", ints ? "uint4" : "float4", tex, a[1]);
        } else {
            char addr[256];
            if (bk == 2 && !(op == 139 && is_undef(t, in->ops[4]))) snprintf(addr, sizeof addr, "uint(%s) * %s->stride + uint(%s)", a[1], a[0], a[2]);
            else snprintf(addr, sizeof addr, "uint(%s)", a[1]);
            snprintf(e, sizeof e, "vx_ld4(%s, %s)", a[0], addr);
            int f = in->result != UINT32_MAX && ty(t, in->type) && ty(t, in->type)->count && is_float(t, ty(t, in->type)->members[0]);
            if (f) { char e2[420]; snprintf(e2, sizeof e2, "as_type<float4>(%s)", e); snprintf(e, sizeof e, "%s", e2); }
        }
        resret(t, in, e, 1);
        return;
    }
    case 69: case 140: {                                           // BufferStore(uav, c0, c1, v0..v3, mask), RawBufferStore(... mask, align)
        handle_info *h = hinfo(t, in->ops[2]);
        if (!h) { err(t, "a buffer store to a handle that does not come from createHandle"); return; }
        int bk = buffer_kind(h->kind), ok;
        int mask = (int)const_int(t, in->ops[2 + 7], &ok);
        if (!ok) mask = 15;
        if (bk == 3) {
            char tex[160];
            tex_of(t, in->ops[2], tex, sizeof tex);
            int ints = h->comp == 4 || h->comp == 5 || h->comp == 2 || h->comp == 3;
            line(t, "%s.write(%s(%s, %s, %s, %s), uint(%s));", tex, ints ? "uint4" : "float4", a[3], a[4], a[5], a[6], a[1]);
            return;
        }
        char addr[256];
        if (bk == 2 && !is_undef(t, in->ops[4])) snprintf(addr, sizeof addr, "uint(%s) * %s->stride + uint(%s)", a[1], a[0], a[2]);
        else snprintf(addr, sizeof addr, "uint(%s)", a[1]);
        for (int k = 0; k < 4; k++) {
            if (!(mask & (1 << k))) continue;
            int f = is_float(t, val(t, in->ops[5 + k]) ? val(t, in->ops[5 + k])->type : 0);
            line(t, "vx_st(%s, %s + %du, %s);", a[0], addr, 4 * k, f ? (snprintf(a[11], 96, "as_type<uint>(%s)", a[3 + k]), a[11]) : a[3 + k]);
        }
        return;
    }
    case 78: case 79: {                                            // AtomicBinOp(h, op, c0, c1, c2, v), AtomicCompareExchange(h, c0, c1, c2, cmp, v)
        handle_info *h = hinfo(t, in->ops[2]);
        if (!h) { err(t, "an atomic on a handle that does not come from createHandle"); return; }
        int bk = buffer_kind(h->kind);
        if (!bk || bk == 3) { err(t, "atomics on textures are not done yet"); return; }
        uint32_t c0 = op == 78 ? 4 : 3;
        char c[3][96];
        for (int k = 0; k < 3; k++) ref(t, in->ops[c0 + k], c[k], 96);
        char addr[256];
        if (bk == 2) snprintf(addr, sizeof addr, "uint(%s) * %s->stride + uint(%s)", c[0], a[0], c[1]);
        else snprintf(addr, sizeof addr, "uint(%s)", c[0]);
        if (op == 78) {
            int ok;
            int aop = (int)const_int(t, in->ops[3], &ok);
            static const char *fn[] = { "add", "and", "or", "xor", "min", "max", "min", "max", "exchange" };
            if (!ok || aop > 8) { err(t, "atomic operation %d", aop); return; }
            int sgn = aop == 4 || aop == 5;
            char v[96];
            ref(t, in->ops[7], v, sizeof v);
            line(t, "%s%s%s(atomic_%s_explicit((device atomic_%s *)(%s->va + (%s)), %s(%s), memory_order_relaxed));", r[0] ? r : "", r[0] ? " = " : "",
                 r[0] ? tn : "", aop == 8 ? "exchange" : (snprintf(a[11], 96, "fetch_%s", fn[aop]), a[11]), sgn ? "int" : "uint", a[0], addr, sgn ? "as_type<int>" : "uint", v);
        } else {
            char cmp[96], v[96];
            ref(t, in->ops[6], cmp, sizeof cmp);
            ref(t, in->ops[7], v, sizeof v);
            line(t, "{ uint vx_e = %s; atomic_compare_exchange_weak_explicit((device atomic_uint *)(%s->va + (%s)), &vx_e, uint(%s), memory_order_relaxed, memory_order_relaxed); %s%s vx_e; }",
                 cmp, a[0], addr, v, r[0] ? r : "(void)", r[0] ? " =" : "");
        }
        return;
    }
    case 80: {                                                     // Barrier(mode): 1 sync, 2 UAV global, 4 UAV group, 8 groupshared
        int ok;
        int mode = (int)const_int(t, in->ops[2], &ok);
        const char *flags = (mode & 2) || (mode & 4) ? ((mode & 8) ? "mem_flags::mem_device | mem_flags::mem_threadgroup" : "mem_flags::mem_device")
                          : (mode & 8) ? "mem_flags::mem_threadgroup" : "mem_flags::mem_none";
        if (mode & 1) line(t, "threadgroup_barrier(%s);", flags);
        else line(t, "atomic_thread_fence(%s, memory_order_seq_cst);", flags);
        return;
    }
    case 82: line(t, "if (%s) discard_fragment();", a[0]); return;
    case 83: case 85: line(t, "%s = dfdx(%s);", r, a[0]); return;
    case 84: case 86: line(t, "%s = dfdy(%s);", r, a[0]); return;
    case 93: case 94: case 95: {
        int ok;
        int c = (int)const_int(t, in->ops[2], &ok);
        const char *src = op == 93 ? "vx_tid" : op == 94 ? "vx_gid" : "vx_ltid";
        if (op == 93) t->need_tid = 1; else if (op == 94) t->need_gid = 1; else t->need_ltid = 1;
        line(t, "%s = %s[%d];", r, src, c);
        return;
    }
    case 96: t->need_lidx = 1; line(t, "%s = vx_lidx;", r); return;
    case 110: line(t, "%s = simd_is_first();", r); return;
    case 111: t->need_lane = 1; line(t, "%s = vx_lane;", r); return;
    case 112: t->need_lanes = 1; line(t, "%s = vx_lanes;", r); return;
    case 113: line(t, "%s = simd_any(%s);", r, a[0]); return;
    case 114: line(t, "%s = simd_all(%s);", r, a[0]); return;
    case 115: line(t, "%s = simd_all(%s == simd_broadcast_first(%s));", r, a[0], a[0]); return;
    case 117: line(t, "%s = simd_shuffle(%s, ushort(%s));", r, a[0], a[1]); return;
    case 118: line(t, "%s = simd_broadcast_first(%s);", r, a[0]); return;
    case 119: case 121: {                                          // WaveActiveOp / WavePrefixOp(value, op, sop)
        int ok;
        int wop = (int)const_int(t, in->ops[3], &ok), sop = (int)const_int(t, in->ops[4], &ok);
        static const char *fn[] = { "sum", "product", "min", "max" };
        if (wop > 3) { err(t, "wave operation %d", wop); return; }
        if (op == 121 && wop > 1) { err(t, "wave prefix min/max"); return; }
        const char *pre = op == 121 ? "simd_prefix_exclusive_" : "simd_";
        if (!fp && !sop && (wop == 2 || wop == 3)) line(t, "%s = %s(%s%s(as_type<%s>(%s)));", r, tn, pre, fn[wop], signed_name(bits), a[0]);
        else line(t, "%s = %s%s(%s);", r, pre, fn[wop], a[0]);
        return;
    }
    case 120: {                                                    // WaveActiveBit(value, op): and, or, xor
        int ok;
        int bop = (int)const_int(t, in->ops[3], &ok);
        static const char *fn[] = { "simd_and", "simd_or", "simd_xor" };
        line(t, "%s = %s(%s);", r, bop < 3 ? fn[bop] : "simd_or", a[0]);
        return;
    }
    case 122: line(t, "%s = quad_shuffle(%s, ushort(%s));", r, a[0], a[1]); return;
    case 123: {
        int ok;
        int q = (int)const_int(t, in->ops[3], &ok);
        line(t, "%s = quad_shuffle_xor(%s, ushort(%d));", r, a[0], q == 0 ? 1 : q == 1 ? 2 : 3);
        return;
    }
    case 124: case 125: case 126: case 127: line(t, "%s = as_type<%s>(%s);", r, tn, a[0]); return;
    case 130: line(t, "%s = uint(as_type<ushort>(half(%s)));", r, a[0]); return;
    case 131: line(t, "%s = float(as_type<half>(ushort(%s)));", r, a[0]); return;
    case 135: line(t, "{ simd_vote vx_v = simd_ballot(%s); %s = popcount(uint(ulong(vx_v))) + popcount(uint(ulong(vx_v) >> 32)); }", a[0], r); return;
    case 116: line(t, "{ ulong vx_v = ulong(simd_ballot(%s)); %s = %s{ uint(vx_v), uint(vx_v >> 32), 0u, 0u }; }", a[0], r, tn); return;
    default: {
        const char *name = callee ? callee : "?";
        err(t, "DXIL intrinsic %d (%s) is not done yet", op, name);
        return;
    }
    }
    (void)bits;
}

static const char *fcmp_op(int p, int *negate) {
    *negate = 0;
    switch (p) {
    case 1: return "=="; case 2: return ">"; case 3: return ">="; case 4: return "<"; case 5: return "<="; case 6: return "!=";
    case 9: *negate = 1; return "!="; case 10: *negate = 1; return "<="; case 11: *negate = 1; return "<"; case 12: *negate = 1; return ">=";
    case 13: *negate = 1; return ">"; case 14: return "!=";
    }
    return NULL;
}

static void inst(tr *t, const dxil_inst *in) {
    char r[32] = "", tn[96] = "", a[4][96];
    if (in->result != UINT32_MAX) { ref(t, in->result, r, sizeof r); type_name(t, in->type, tn, sizeof tn); }
    for (uint32_t i = 0; i < 4; i++) { if (i < in->n) ref(t, in->ops[i], a[i], 96); else a[i][0] = 0; }
    switch (in->op) {
    case DXI_BINOP: {
        int fp = is_float(t, in->type), bits = int_bits(t, in->type);
        static const char *ops[] = { "+", "-", "*", "/", "/", "%", "%", "<<", ">>", ">>", "&", "|", "^" };
        if (in->sub > 12) { err(t, "binary operation %u", in->sub); return; }
        if (fp) {
            if (in->sub == 6) line(t, "%s = fmod(%s, %s);", r, a[0], a[1]);
            else line(t, "%s = %s %s %s;", r, a[0], ops[in->sub], a[1]);
        } else if (bits == 1) {
            line(t, "%s = %s %s %s;", r, a[0], in->sub == 10 ? "&&" : in->sub == 11 ? "||" : in->sub == 12 ? "!=" : ops[in->sub], a[1]);
        } else if (in->sub == 4 || in->sub == 6 || in->sub == 9) {      // signed: sdiv, srem, ashr
            const char *s = signed_name(bits);
            line(t, "%s = %s(as_type<%s>(%s) %s %s(%s));", r, tn, s, a[0], ops[in->sub], in->sub == 9 ? "uint" : s, a[1]);
        } else if (in->sub == 7 || in->sub == 8) {
            line(t, "%s = %s(%s %s (%s & %du));", r, tn, a[0], ops[in->sub], a[1], bits - 1);   // (shifts take the count's low bits, as D3D does)
        } else {
            line(t, "%s = %s(%s %s %s);", r, tn, a[0], ops[in->sub], a[1]);
        }
        return;
    }
    case DXI_CAST: {
        const dxil_value *sv = val(t, in->ops[0]);
        uint32_t st = sv ? sv->type : 0;
        int sbits = int_bits(t, st), dbits = int_bits(t, in->type);
        switch (in->sub) {
        case 0: line(t, "%s = %s(%s);", r, tn, a[0]); return;                          // trunc
        case 1: line(t, "%s = %s(%s);", r, tn, a[0]); return;                          // zext
        case 2:                                                                         // sext
            if (sbits == 1) line(t, "%s = %s(%s ? -1 : 0);", r, tn, a[0]);
            else line(t, "%s = %s(%s(as_type<%s>(%s)));", r, tn, signed_name(dbits), signed_name(sbits), a[0]);
            return;
        case 3: line(t, "%s = %s(%s);", r, tn, a[0]); return;                          // fptoui
        case 4: line(t, "%s = %s(%s(%s));", r, tn, signed_name(dbits), a[0]); return;  // fptosi
        case 5: line(t, "%s = %s(%s);", r, tn, a[0]); return;                          // uitofp
        case 6: line(t, "%s = %s(as_type<%s>(%s));", r, tn, signed_name(sbits), a[0]); return;   // sitofp
        case 7: case 8: line(t, "%s = %s(%s);", r, tn, a[0]); return;                  // fptrunc, fpext
        case 11: line(t, "%s = as_type<%s>(%s);", r, tn, a[0]); return;                // bitcast
        }
        err(t, "cast %u", in->sub);
        return;
    }
    case DXI_CMP: {
        if (in->sub >= 32) {
            static const char *ops[] = { "==", "!=", ">", ">=", "<", "<=", ">", ">=", "<", "<=" };
            int p = in->sub - 32;
            if (p > 9) { err(t, "compare %u", in->sub); return; }
            const dxil_value *lv = val(t, in->ops[0]);
            int bits = lv ? int_bits(t, lv->type) : 32;
            if (p >= 6) line(t, "%s = as_type<%s>(%s) %s as_type<%s>(%s);", r, signed_name(bits), a[0], ops[p], signed_name(bits), a[1]);
            else line(t, "%s = %s %s %s;", r, a[0], ops[p], a[1]);
        } else {
            int neg;
            const char *o = fcmp_op(in->sub, &neg);
            if (in->sub == 0 || in->sub == 15) { line(t, "%s = %s;", r, in->sub ? "true" : "false"); return; }
            if (in->sub == 7) { line(t, "%s = !isnan(%s) && !isnan(%s);", r, a[0], a[1]); return; }
            if (in->sub == 8) { line(t, "%s = isnan(%s) || isnan(%s);", r, a[0], a[1]); return; }
            if (!o) { err(t, "compare %u", in->sub); return; }
            line(t, neg ? "%s = !(%s %s %s);" : "%s = %s %s %s;", r, a[0], o, a[1]);
        }
        return;
    }
    case DXI_SELECT: line(t, "%s = %s ? %s : %s;", r, a[0], a[1], a[2]); return;
    case DXI_EXTRACTELT: line(t, "%s = %s[%s];", r, a[0], a[1]); return;
    case DXI_INSERTELT: line(t, "%s = %s; %s[%s] = %s;", r, a[0], r, a[2], a[1]); return;
    case DXI_EXTRACTVAL: case DXI_INSERTVAL: {
        char path[256] = "";
        uint32_t tid = val(t, in->ops[0]) ? val(t, in->ops[0])->type : 0;
        for (uint32_t i = in->nvals; i < in->n; i++) {
            const dxil_type *y = ty(t, tid);
            size_t l = strlen(path);
            if (y && y->kind == DXT_STRUCT) { snprintf(path + l, sizeof path - l, ".f%u", in->ops[i]); tid = in->ops[i] < y->count ? y->members[in->ops[i]] : 0; }
            else if (y && y->kind == DXT_ARRAY) { snprintf(path + l, sizeof path - l, ".e[%u]", in->ops[i]); tid = y->elem; }
            else { snprintf(path + l, sizeof path - l, "[%u]", in->ops[i]); tid = y ? y->elem : 0; }
        }
        if (in->op == DXI_EXTRACTVAL) line(t, "%s = %s%s;", r, a[0], path);
        else line(t, "%s = %s; %s%s = %s;", r, a[0], r, path, a[1]);
        return;
    }
    case DXI_ALLOCA: return;                                       // (the variable is declared with the others)
    case DXI_GEP: {
        // [source type, base, first index, indices...]: &(base[first]).e[i]...
        char path[512];
        uint32_t tid = in->ops[0];
        if (in->n < 3) { err(t, "getelementptr without indices"); return; }
        char base[96], first[96];
        ref(t, in->ops[1], base, sizeof base);
        ref(t, in->ops[2], first, sizeof first);
        int ok;
        int64_t f0 = const_int(t, in->ops[2], &ok);
        const dxil_value *bv = val(t, in->ops[1]);
        int base_is_object = bv && (bv->kind == DXV_GLOBAL || (bv->kind == DXV_INST && t->f->insts[bv->u.index].op == DXI_ALLOCA));
        if (base_is_object && ok && f0 == 0) snprintf(path, sizeof path, "(%s)", base);
        else snprintf(path, sizeof path, "(%s%s)[%s]", base_is_object ? "&" : "", base, first);
        for (uint32_t i = 3; i < in->n; i++) {
            const dxil_type *y = ty(t, tid);
            char ix[96];
            ref(t, in->ops[i], ix, sizeof ix);
            size_t l = strlen(path);
            if (y && y->kind == DXT_STRUCT) {
                int k = (int)const_int(t, in->ops[i], &ok);
                snprintf(path + l, sizeof path - l, ".f%d", k);
                tid = (uint32_t)k < y->count ? y->members[k] : 0;
            } else if (y && y->kind == DXT_ARRAY) {
                snprintf(path + l, sizeof path - l, ".e[%s]", ix);
                tid = y->elem;
            } else {
                snprintf(path + l, sizeof path - l, "[%s]", ix);
                tid = y ? y->elem : 0;
            }
        }
        line(t, "%s = &%s;", r, path);
        return;
    }
    case DXI_LOAD: line(t, "%s = *(%s);", r, a[0]); return;
    case DXI_STORE: line(t, "*(%s) = %s;", a[0], a[1]); return;
    case DXI_ATOMICRMW: {
        static const char *fn[] = { "exchange", "fetch_add", "fetch_sub", "fetch_and", "fetch_nand", "fetch_or", "fetch_xor", "fetch_max", "fetch_min", "fetch_max", "fetch_min" };
        if (in->sub > 10 || in->sub == 4) { err(t, "atomicrmw %u", in->sub); return; }
        uint8_t sp = space_of(t, in->ops[0]);
        const char *space = sp == 2 ? "threadgroup" : "device";
        int sgn = in->sub == 7 || in->sub == 8;
        line(t, "%s = %s(atomic_%s_explicit((%s atomic_%s *)(%s), %s(%s), memory_order_relaxed));", r, tn, fn[in->sub], space, sgn ? "int" : "uint", a[0], sgn ? "as_type<int>" : "uint", a[1]);
        return;
    }
    case DXI_CMPXCHG: {
        uint8_t sp = space_of(t, in->ops[0]);
        line(t, "{ uint vx_e = %s; bool vx_ok = atomic_compare_exchange_weak_explicit((%s atomic_uint *)(%s), &vx_e, %s, memory_order_relaxed, memory_order_relaxed); %s = %s{ vx_e, vx_ok }; }",
             a[1], sp == 2 ? "threadgroup" : "device", a[0], a[2], r, tn);
        return;
    }
    case DXI_CALL: call(t, in); return;
    case DXI_FENCE: line(t, "atomic_thread_fence(mem_flags::mem_device | mem_flags::mem_threadgroup, memory_order_seq_cst);"); return;
    case DXI_PHI: return;                                          // (assigned on the edges into its block)
    }
    err(t, "instruction %u", in->op);
}

// ---- control flow ---------------------------------------------------------------------------------------------------

static uint32_t nsucc(tr *t, uint32_t b, uint32_t *s) {
    const dxil_inst *in = &t->f->insts[t->term[b]];
    uint32_t n = 0;
    if (in->op == DXI_BR) { s[n++] = in->ops[0]; if (in->n == 3) s[n++] = in->ops[1]; }
    else if (in->op == DXI_SWITCH) { s[n++] = in->ops[1]; for (uint32_t i = 3; i < in->n && n < 64; i += 2) s[n++] = in->ops[i]; }
    return n;
}

// Phi assignments for the edge from -> to (through temporaries when the block has several phis).
static void edge(tr *t, uint32_t from, uint32_t to) {
    if (to >= t->nb) return;
    uint32_t nphi = 0;
    for (uint32_t i = t->first[to]; i < t->f->ninsts && t->f->insts[i].block == to && t->f->insts[i].op == DXI_PHI; i++) nphi++;
    if (!nphi) return;
    char v[96], r[32];
    for (uint32_t i = t->first[to], k = 0; k < nphi; i++, k++) {
        const dxil_inst *p = &t->f->insts[i];
        for (uint32_t j = 0; j + 1 < p->n; j += 2) {
            if (p->ops[j + 1] != from) continue;
            ref(t, p->ops[j], v, sizeof v);
            if (nphi == 1) { ref(t, p->result, r, sizeof r); line(t, "%s = %s;", r, v); }
            else line(t, "vx_phi%u = %s;", local(t, p->result), v);
            break;
        }
    }
    if (nphi > 1) for (uint32_t i = t->first[to], k = 0; k < nphi; i++, k++) {
        ref(t, t->f->insts[i].result, r, sizeof r);
        line(t, "%s = vx_phi%u;", r, local(t, t->f->insts[i].result));
    }
}

static void block_body(tr *t, uint32_t b) {
    for (uint32_t i = t->first[b]; i < t->term[b] && !t->failed; i++) inst(t, &t->f->insts[i]);
}

static void ret_line(tr *t) {
    int stage = t->out->stage;
    line(t, stage == 5 ? "return;" : "return vx_out;");
}

// The state machine: every block a case of a switch in a loop.
static void emit_machine(tr *t) {
    line(t, "uint vx_b = 0;");
    line(t, "while (true) {");
    t->indent++;
    line(t, "switch (vx_b) {");
    for (uint32_t b = 0; b < t->nb && !t->failed; b++) {
        line(t, "case %u: {", b);
        t->indent++;
        block_body(t, b);
        const dxil_inst *in = &t->f->insts[t->term[b]];
        char c[96];
        if (in->op == DXI_RET) ret_line(t);
        else if (in->op == DXI_UNREACHABLE) ret_line(t);
        else if (in->op == DXI_BR && in->n == 1) { edge(t, b, in->ops[0]); line(t, "vx_b = %u; continue;", in->ops[0]); }
        else if (in->op == DXI_BR) {
            ref(t, in->ops[2], c, sizeof c);
            line(t, "if (%s) {", c); t->indent++; edge(t, b, in->ops[0]); line(t, "vx_b = %u; continue;", in->ops[0]); t->indent--;
            line(t, "} else {"); t->indent++; edge(t, b, in->ops[1]); line(t, "vx_b = %u; continue;", in->ops[1]); t->indent--; line(t, "}");
        } else if (in->op == DXI_SWITCH) {
            ref(t, in->ops[0], c, sizeof c);
            for (uint32_t i = 2; i + 1 < in->n; i += 2) {
                char v[96];
                ref(t, in->ops[i], v, sizeof v);
                const dxil_value *cv = dxil_value_at(t->m, t->f, in->ops[i]);
                if (cv && cv->kind == DXV_CONST_INT) snprintf(v, sizeof v, "%uu", (uint32_t)cv->u.i);
                line(t, "if (%s == %s) {", c, v); t->indent++; edge(t, b, in->ops[i + 1]); line(t, "vx_b = %u; continue;", in->ops[i + 1]); t->indent--; line(t, "}");
            }
            edge(t, b, in->ops[1]);
            line(t, "vx_b = %u; continue;", in->ops[1]);
        }
        t->indent--;
        line(t, "}");
    }
    line(t, "default: %s", t->out->stage == 5 ? "return;" : "return vx_out;");
    line(t, "}");
    t->indent--;
    line(t, "}");
}

// Dominators (or post-dominators, on the reversed graph with a virtual exit): Cooper, Harvey and Kennedy's iteration.
static void dominators(tr *t, int post, int *dom) {
    uint32_t n = t->nb + 1, exitn = t->nb;
    uint32_t (*succ)[64] = calloc(n, sizeof *succ), *ns = calloc(n, sizeof *ns);
    uint32_t (*pred)[64] = calloc(n, sizeof *pred), *np = calloc(n, sizeof *np);
    for (uint32_t b = 0; b < t->nb; b++) {
        uint32_t s[64], k = nsucc(t, b, s);
        if (k == 0) s[k++] = exitn;
        for (uint32_t i = 0; i < k; i++) {
            uint32_t from = post ? s[i] : b, to = post ? b : s[i];
            if (ns[from] < 64) succ[from][ns[from]++] = to;
            if (np[to] < 64) pred[to][np[to]++] = from;
        }
    }
    uint32_t start = post ? exitn : 0;
    // reverse post order from start
    uint32_t *order = calloc(n, sizeof *order), *stack = calloc(n * 65, sizeof *stack), *it = calloc(n, sizeof *it), no = 0, sp = 0;
    int *seen = calloc(n, sizeof *seen), *rpo = calloc(n, sizeof *rpo);
    stack[sp++] = start; seen[start] = 1;
    while (sp) {
        uint32_t b = stack[sp - 1];
        if (it[b] < ns[b]) { uint32_t s2 = succ[b][it[b]++]; if (!seen[s2]) { seen[s2] = 1; stack[sp++] = s2; } }
        else { order[no++] = b; sp--; }
    }
    for (uint32_t i = 0; i < n; i++) rpo[i] = -1;
    for (uint32_t i = 0; i < no; i++) rpo[order[i]] = (int)(no - 1 - i);
    for (uint32_t i = 0; i < n; i++) dom[i] = -1;
    dom[start] = (int)start;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = (int)no - 1; i >= 0; i--) {
            uint32_t b = order[i];
            if (b == start) continue;
            int nd = -1;
            for (uint32_t k = 0; k < np[b]; k++) {
                int p = (int)pred[b][k];
                if (dom[p] < 0) continue;
                if (nd < 0) { nd = p; continue; }
                int x = p, y = nd;
                while (x != y) { while (rpo[x] > rpo[y]) x = dom[x]; while (rpo[y] > rpo[x]) y = dom[y]; }
                nd = x;
            }
            if (nd >= 0 && dom[b] != nd) { dom[b] = nd; changed = 1; }
        }
    }
    if (!post) for (uint32_t i = 0; i < t->nb; i++) t->rpo_index[i] = rpo[i];
    free(succ); free(ns); free(pred); free(np); free(order); free(stack); free(it); free(seen); free(rpo);
}

static int dominates(tr *t, uint32_t a, uint32_t b) {
    for (int x = (int)b; ; x = t->idom[x]) { if ((uint32_t)x == a) return 1; if (x < 0 || t->idom[x] == x) return (uint32_t)x == a; }
}

static int in_loop(tr *t, uint32_t header, uint32_t b);

// Natural loops: a back edge u -> h (h dominates u). Each header's exit: the one block outside the loop that a block
// inside branches to (DXC's structured loops have one); none or several: not structurable here.
static int find_loops(tr *t) {
    for (uint32_t b = 0; b < t->nb; b++) { t->loop_header_of[b] = -1; t->loop_exit[b] = -2; }
    for (uint32_t u = 0; u < t->nb; u++) {
        uint32_t s[64], k = nsucc(t, u, s);
        for (uint32_t i = 0; i < k; i++) if (t->rpo_index[s[i]] >= 0 && t->rpo_index[u] >= 0 && dominates(t, s[i], u)) t->loop_header_of[s[i]] = (int)s[i];
    }
    for (uint32_t h = 0; h < t->nb; h++) {
        if (t->loop_header_of[h] != (int)h) continue;
        int exit = -1;
        for (uint32_t b = 0; b < t->nb; b++) {
            if (!in_loop(t, h, b)) continue;
            uint32_t s[64], k = nsucc(t, b, s);
            for (uint32_t i = 0; i < k; i++) {
                if (in_loop(t, h, s[i])) continue;
                if (exit >= 0 && exit != (int)s[i]) return 0;      // (several exits)
                exit = (int)s[i];
            }
        }
        t->loop_exit[h] = exit;                                    // (-1: the loop never ends but by return)
    }
    return 1;
}

// b is in the loop headed by h: h dominates b and b reaches a back edge to h without leaving h's dominance.
static int in_loop(tr *t, uint32_t h, uint32_t b) {
    if (!dominates(t, h, b)) return 0;
    if (b == h) return 1;
    // (reaches h again: a depth-first walk over blocks h dominates)
    uint8_t *seen = calloc(t->nb, 1);
    uint32_t *stack = malloc(t->nb * 64 * sizeof *stack), sp = 0;
    int found = 0;
    stack[sp++] = b;
    seen[b] = 1;
    while (sp && !found) {
        uint32_t x = stack[--sp], s[64], k = nsucc(t, x, s);
        for (uint32_t i = 0; i < k; i++) {
            if (s[i] == h) { found = 1; break; }
            if (!seen[s[i]] && dominates(t, h, s[i])) { seen[s[i]] = 1; stack[sp++] = s[i]; }
        }
    }
    free(seen);
    free(stack);
    return found;
}

static int emit_region(tr *t, uint32_t b, int stop);

// The jump of an edge: into the innermost loop's header (continue), its exit (break), the region's end, or on.
static int emit_edge(tr *t, uint32_t from, uint32_t to, int stop) {
    edge(t, from, to);
    if (t->nloops) {
        uint32_t h = t->loops[t->nloops - 1];
        if (to == h) { line(t, "continue;"); return 0; }
        if ((int)to == t->loop_exit[h]) { line(t, "break;"); return 0; }
        for (uint32_t i = 0; i + 1 < t->nloops; i++)               // (out of an outer loop: not without labels)
            if (to == t->loops[i] || (int)to == t->loop_exit[t->loops[i]]) return -1;
    }
    if ((int)to == stop) return 0;
    return emit_region(t, to, stop);
}

// Emits the blocks from b up to (not including) stop, in the source's structure. -1: not structurable.
static int emit_region(tr *t, uint32_t b, int stop) {
    while (!t->failed) {
        if ((int)b == stop) return 0;
        if (b >= t->nb) return -1;
        int header = t->loop_header_of[b] == (int)b;
        int open = 0;
        for (uint32_t i = 0; i < t->nloops; i++) if (t->loops[i] == b) open = 1;
        if (header && !open) {
            if (t->emitted[b]) return -1;
            int exit = t->loop_exit[b];
            line(t, "while (true) {");
            t->indent++;
            t->loops[t->nloops++] = b;
            int rc = emit_region(t, b, -3);                        // (the body ends in continue/break/return)
            t->nloops--;
            t->indent--;
            line(t, "}");
            if (rc) return rc;
            if (exit < 0) return 0;
            b = (uint32_t)exit;
            continue;
        }
        if (t->emitted[b]) return -1;
        t->emitted[b] = 1;
        block_body(t, b);
        const dxil_inst *in = &t->f->insts[t->term[b]];
        if (in->op == DXI_RET || in->op == DXI_UNREACHABLE) { ret_line(t); return 0; }
        if (in->op == DXI_BR && in->n == 1) return emit_edge(t, b, in->ops[0], stop);
        // a two-way branch or a switch: the arms up to where they meet (the post-dominator), then on from there
        int merge = t->ipdom[b];
        if (merge == (int)t->nb) merge = -1;                       // (they meet only at the exit: each arm returns)
        if (t->nloops && merge >= 0 && !in_loop(t, t->loops[t->nloops - 1], (uint32_t)merge)) merge = -1;   // (they meet past the loop)
        int stop_here = merge >= 0 ? merge : stop;
        char c[96];
        if (in->op == DXI_BR) {
            ref(t, in->ops[2], c, sizeof c);
            line(t, "if (%s) {", c);
            t->indent++;
            if (emit_edge(t, b, in->ops[0], stop_here)) return -1;
            t->indent--;
            line(t, "} else {");
            t->indent++;
            if (emit_edge(t, b, in->ops[1], stop_here)) return -1;
            t->indent--;
            line(t, "}");
        } else {
            ref(t, in->ops[0], c, sizeof c);
            // (an if/else chain, not a switch: a break in an arm must leave the loop, not the switch)
            uint32_t ncase = (in->n - 2) / 2;
            uint8_t *done = calloc(ncase + 1, 1);
            int first = 1;
            for (uint32_t i = 0; i < ncase; i++) {
                if (done[i]) continue;
                uint32_t target = in->ops[3 + 2 * i];
                char cond[512] = "";
                for (uint32_t j = i; j < ncase; j++) {
                    if (in->ops[3 + 2 * j] != target) continue;
                    done[j] = 1;
                    const dxil_value *cv = dxil_value_at(t->m, t->f, in->ops[2 + 2 * j]);
                    size_t l = strlen(cond);
                    snprintf(cond + l, sizeof cond - l, "%s%s == %uu", l ? " || " : "", c, cv && cv->kind == DXV_CONST_INT ? (uint32_t)cv->u.i : 0);
                }
                line(t, "%sif (%s) {", first ? "" : "} else ", cond);
                first = 0;
                t->indent++;
                if (emit_edge(t, b, target, stop_here)) { free(done); return -1; }
                t->indent--;
            }
            free(done);
            if (!first) line(t, "} else {");
            t->indent += !first;
            if (emit_edge(t, b, in->ops[1], stop_here)) return -1;
            t->indent -= !first;
            if (!first) line(t, "}");
        }
        if (merge < 0) return 0;                                   // (nothing after: the arms ended the region)
        b = (uint32_t)merge;
    }
    return -1;
}

// ---- the entry point -----------------------------------------------------------------------------------------------

static void declare_values(tr *t) {
    const dxil_function *f = t->f;
    char tn[96];
    for (uint32_t i = 0; i < f->ninsts; i++) {
        const dxil_inst *in = &f->insts[i];
        if (in->result == UINT32_MAX) continue;
        uint32_t l = local(t, in->result);
        const dxil_type *y = ty(t, in->type);
        if (in->op == DXI_ALLOCA) {
            const dxil_type *p = y;
            uint32_t et = p && p->kind == DXT_POINTER ? p->elem : in->type;
            type_name(t, et, tn, sizeof tn);
            put(&t->decls, "    %s vx_a%u = {};\n    thread %s *v%u = &vx_a%u;\n", tn, l, tn, l, l);
            t->ptr_space[l] = 1;
            continue;
        }
        if (y && y->kind == DXT_POINTER) {
            // (its address space: groupshared (3) or the object it points into)
            uint32_t et = y->elem;
            type_name(t, et, tn, sizeof tn);
            int space = y->bits == 3 ? 2 : 1;
            if (in->op == DXI_GEP && in->n > 1) space = space_of(t, in->ops[1]);
            t->ptr_space[l] = (uint8_t)space;
            put(&t->decls, "    %s %s *v%u = nullptr;\n", space == 2 ? "threadgroup" : space == 3 ? "constant" : "thread", tn, l);
            continue;
        }
        type_name(t, in->type, tn, sizeof tn);
        if (y && y->kind == DXT_STRUCT && y->name && !strcmp(y->name, "dx.types.Handle")) put(&t->decls, "    device const VxDesc *v%u = nullptr;\n", l);
        else put(&t->decls, "    %s v%u = {};\n", tn, l);
        if (in->op == DXI_PHI) {
            uint32_t nphi = 0;
            for (uint32_t k = t->first[in->block]; k < f->ninsts && f->insts[k].block == in->block && f->insts[k].op == DXI_PHI; k++) nphi++;
            if (nphi > 1) put(&t->decls, "    %s vx_phi%u = {};\n", tn, l);
        }
    }
}

// Global variables: groupshared (threadgroup, in the kernel), constant arrays (file scope), others (the thread's).
static void declare_globals(tr *t, sb *file, sb *local_decls) {
    for (uint32_t i = 0; i < t->m->nglobals; i++) {
        const dxil_global *g = &t->m->globals[i];
        char tn[96];
        type_name(t, g->type, tn, sizeof tn);
        if (g->addrspace == 3) { put(local_decls, "    threadgroup %s g%u;\n", tn, i); continue; }
        // (an initializer: written out element by element)
        sb init = { 0 };
        const dxil_value *iv = g->init != UINT32_MAX ? &t->m->values[g->init] : NULL;
        if (iv && (iv->kind == DXV_CONST_DATA || iv->kind == DXV_CONST_AGGREGATE)) {
            const dxil_type *y = ty(t, g->type);
            int fp = y && y->kind == DXT_ARRAY && is_float(t, y->elem);
            put(&init, " = { {");
            uint32_t n = iv->u.list.n;
            for (uint32_t k = 0; k < n; k++) {
                char v[64];
                if (iv->kind == DXV_CONST_DATA) {
                    if (fp) { float f; uint32_t u = (uint32_t)iv->data[k]; memcpy(&f, &u, 4); float_literal(f, 0, v, sizeof v); }
                    else snprintf(v, sizeof v, "%uu", (uint32_t)iv->data[k]);
                } else ref(t, iv->u.list.ids[k], v, sizeof v);
                put(&init, "%s%s", k ? ", " : " ", v);
            }
            put(&init, " } }");
        }
        if (g->constant) put(file, "constant %s g%u%s;\n", tn, i, init.s ? init.s : " = {}");
        else put(local_decls, "    %s g%u%s;\n", tn, i, init.s ? init.s : " = {}");
        free(init.s);
    }
}

static void semantic_attr(tr *t, const dxil_md *e, uint32_t row, char *out, size_t cap) {
    const dxil_md *idx = dxil_md_op(t->m, e, 4);
    int sem = md_i(t, idx, row, (int)row);
    snprintf(out, cap, "%s%d", md_str(t, e, 1), sem);
    for (char *p = out; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
}

static const char *interp(int mode, int is_int) {
    if (is_int || mode == 1) return ", flat";
    switch (mode) {
    case 3: return ", centroid_perspective";
    case 4: return ", center_no_perspective";
    case 5: return ", centroid_no_perspective";
    case 6: return ", sample_perspective";
    case 7: return ", sample_no_perspective";
    }
    return "";
}

static void io_structs(tr *t, sb *s) {
    int stage = t->out->stage;
    // inputs
    if (stage == 0 || stage == 1) {
        put(s, "struct VxIn {\n");
        int any = 0;
        uint32_t attr = 0;
        for (uint32_t i = 0; t->insig && i < t->insig->n; i++) {
            const dxil_md *e = dxil_md_op(t->m, t->insig, i);
            int id = md_i(t, e, 0, 0), kind = md_i(t, e, 3, 0), rows = md_i(t, e, 6, 1), comp = md_i(t, e, 2, 9);
            if (kind == 1 || kind == 2 || kind == 13 || kind == 12 || kind == 10 || kind == 14) continue;   // (function parameters)
            char et[32];
            element_type(t, e, et, sizeof et);
            for (int r = 0; r < rows; r++) {
                char sem[64];
                semantic_attr(t, e, (uint32_t)r, sem, sizeof sem);
                if (stage == 1) {
                    put(s, "    %s i%d_%d [[attribute(%u)]];\n", et, id, r, attr);
                    if (t->out->ninputs < 32) {
                        dxil_msl_element *x = &t->out->inputs[t->out->ninputs++];
                        snprintf(x->name, sizeof x->name, "%s", md_str(t, e, 1));
                        x->index = (uint32_t)md_i(t, dxil_md_op(t->m, e, 4), r, r);
                        x->kind = (uint8_t)kind; x->comp = (uint8_t)comp; x->cols = (uint8_t)md_i(t, e, 7, 4); x->attribute = attr;
                    }
                    attr++;
                } else if (kind == 3) put(s, "    float4 i%d_%d [[position]];\n", id, r);
                else put(s, "    %s i%d_%d [[user(%s)%s]];\n", et, id, r, sem, interp(md_i(t, e, 5, 2), comp != 9 && comp != 8));
                any = 1;
            }
        }
        if (!any) put(s, "    float vx_unused [[user(VX_UNUSED)]];\n");
        put(s, "};\n");
    }
    // outputs
    if (stage == 0 || stage == 1) {
        put(s, "struct VxOut {\n");
        int any = 0;
        for (uint32_t i = 0; t->outsig && i < t->outsig->n; i++) {
            const dxil_md *e = dxil_md_op(t->m, t->outsig, i);
            int id = md_i(t, e, 0, 0), kind = md_i(t, e, 3, 0), rows = md_i(t, e, 6, 1), comp = md_i(t, e, 2, 9);
            char et[32];
            element_type(t, e, et, sizeof et);
            for (int r = 0; r < rows; r++) {
                char sem[64];
                semantic_attr(t, e, (uint32_t)r, sem, sizeof sem);
                if (t->out->noutputs < 32) {
                    dxil_msl_element *x = &t->out->outputs[t->out->noutputs++];
                    snprintf(x->name, sizeof x->name, "%s", md_str(t, e, 1));
                    x->index = (uint32_t)md_i(t, dxil_md_op(t->m, e, 4), r, r);
                    x->kind = (uint8_t)kind; x->comp = (uint8_t)comp; x->cols = (uint8_t)md_i(t, e, 7, 4);
                }
                if (stage == 1) {
                    if (kind == 3) put(s, "    float4 o%d_%d [[position]];\n", id, r);
                    else if (kind == 4) put(s, "    uint o%d_%d [[render_target_array_index]];\n", id, r);
                    else if (kind == 5) put(s, "    uint o%d_%d [[viewport_array_index]];\n", id, r);
                    else put(s, "    %s o%d_%d [[user(%s)]];\n", et, id, r, sem);
                } else {
                    if (kind == 16) put(s, "    %s o%d_%d [[color(%d)]];\n", et, id, r, md_i(t, dxil_md_op(t->m, e, 4), r, r));
                    else if (kind == 17 || kind == 18 || kind == 19) put(s, "    float o%d_%d [[depth(%s)]];\n", id, r, kind == 18 ? "less" : kind == 19 ? "greater" : "any");
                    else if (kind == 14) put(s, "    uint o%d_%d [[sample_mask]];\n", id, r);
                    else if (kind == 20) put(s, "    uint o%d_%d [[stencil]];\n", id, r);
                    else { err(t, "pixel shader output %s", md_str(t, e, 1)); return; }
                }
                (void)comp;
                any = 1;
            }
        }
        if (!any && stage == 0) put(s, "    float4 vx_unused [[color(0)]];\n");
        if (!any && stage == 1) put(s, "    float4 vx_pos [[position]];\n");
        put(s, "};\n");
    }
}

static const char *PRELUDE =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VxDesc { ulong va; ulong res; uint size, stride, fmt, flags; };\n"
    "static inline float4 vx_cb(device const VxDesc *h, uint row) { return row * 16 < h->size ? ((device const float4 *)h->va)[row] : float4(0); }\n"
    "static inline uint vx_ld(device const VxDesc *h, uint off) { return off + 4 <= h->size ? *(device const uint *)(h->va + off) : 0u; }\n"
    "static inline uint4 vx_ld4(device const VxDesc *h, uint off) { return uint4(vx_ld(h, off), vx_ld(h, off + 4), vx_ld(h, off + 8), vx_ld(h, off + 12)); }\n"
    "static inline void vx_st(device const VxDesc *h, uint off, uint v) { if (off + 4 <= h->size) *(device uint *)(h->va + off) = v; }\n";

int dxil_to_msl(const dxil_module *m, dxil_msl *out) {
    memset(out, 0, sizeof *out);
    out->stage = (int)m->shader_kind;
    tr t = { 0 };
    t.m = m;
    t.out = out;
    const dxil_function *f = NULL;
    const dxil_named_md *ep = dxil_named(m, "dx.entryPoints");
    if (ep && ep->n) {
        const dxil_md *e = &m->md[ep->nodes[0]];
        const dxil_md *fv = dxil_md_op(m, e, 0);
        if (fv && fv->kind == DXM_VALUE) for (uint32_t i = 0; i < m->nfunctions; i++) if (m->functions[i].value == fv->value) f = &m->functions[i];
    }
    if (!f) for (uint32_t i = 0; i < m->nfunctions; i++) if (!m->functions[i].is_declaration) { f = &m->functions[i]; break; }
    if (!f) { snprintf(out->error, sizeof out->error, "no function with a body"); return -1; }
    if (out->stage != 0 && out->stage != 1 && out->stage != 5) { snprintf(out->error, sizeof out->error, "shader kind %d is not done yet", out->stage); return -1; }
    t.f = f;
    t.nvals = f->nlocals;
    t.handles = calloc(t.nvals + 1, sizeof *t.handles);
    t.is_handle = calloc(t.nvals + 1, 1);
    t.ptr_space = calloc(t.nvals + 1, 1);
    t.declared_types = calloc(m->ntypes + 1, 1);
    read_resources(&t);
    read_signatures(&t);
    // blocks
    t.nb = f->nblocks ? f->nblocks : 1;
    t.term = calloc(t.nb, sizeof *t.term);
    t.first = calloc(t.nb, sizeof *t.first);
    for (uint32_t b = 0; b < t.nb; b++) t.first[b] = UINT32_MAX;
    for (uint32_t i = 0; i < f->ninsts; i++) {
        uint32_t b = f->insts[i].block;
        if (b >= t.nb) continue;
        if (t.first[b] == UINT32_MAX) t.first[b] = i;
        t.term[b] = i;
    }
    for (uint32_t b = 0; b < t.nb; b++) if (t.first[b] == UINT32_MAX) { snprintf(out->error, sizeof out->error, "an empty block"); goto done; }
    declare_values(&t);
    // the body: structured if the graph allows, else a state machine
    t.idom = calloc(t.nb + 1, sizeof(int));
    t.ipdom = calloc(t.nb + 1, sizeof(int));
    t.rpo_index = calloc(t.nb + 1, sizeof(int));
    t.loop_header_of = calloc(t.nb + 1, sizeof(int));
    t.loop_exit = calloc(t.nb + 1, sizeof(int));
    t.emitted = calloc(t.nb + 1, 1);
    t.loops = calloc(t.nb + 1, sizeof(uint32_t));
    dominators(&t, 0, t.idom);
    dominators(&t, 1, t.ipdom);
    t.indent = 1;
    int structured = find_loops(&t) && emit_region(&t, 0, -1) == 0 && !t.failed;
    if (!structured && !t.failed) {
        free(t.body.s);
        memset(&t.body, 0, sizeof t.body);
        memset(t.emitted, 0, t.nb);
        t.nloops = 0;
        t.indent = 1;
        emit_machine(&t);
    }
    out->structured = structured;
    if (t.failed) goto done;
    // assemble
    {
        sb file = { 0 }, io = { 0 }, gl = { 0 }, glocal = { 0 };
        declare_globals(&t, &gl, &glocal);
        io_structs(&t, &io);
        put(&file, "%s", PRELUDE);
        put(&file, "%s", t.prelude.s ? t.prelude.s : "");
        put(&file, "struct VxRoot { device const uchar *slot[%u]; device const uchar *heap; device const uchar *samplers; };\n", out->nslots ? out->nslots : 1);
        put(&file, "%s", t.types.s ? t.types.s : "");
        put(&file, "%s", gl.s ? gl.s : "");
        put(&file, "%s", io.s ? io.s : "");
        int stage = out->stage;
        if (stage == 5) {
            put(&file, "kernel void vx_main(constant VxRoot &vx_root [[buffer(0)]]");
            if (t.need_tid) put(&file, ", uint3 vx_tid [[thread_position_in_grid]]");
            if (t.need_gid) put(&file, ", uint3 vx_gid [[threadgroup_position_in_grid]]");
            if (t.need_ltid) put(&file, ", uint3 vx_ltid [[thread_position_in_threadgroup]]");
            if (t.need_lidx) put(&file, ", uint vx_lidx [[thread_index_in_threadgroup]]");
        } else if (stage == 1) {
            put(&file, "vertex VxOut vx_main(VxIn vx_in [[stage_in]], constant VxRoot &vx_root [[buffer(0)]]");
            if (t.need_vid) put(&file, ", uint vx_vid [[vertex_id]]");
            if (t.need_iid) put(&file, ", uint vx_iid0 [[instance_id]], uint vx_base_instance [[base_instance]]");
        } else {
            put(&file, "fragment VxOut vx_main(VxIn vx_in [[stage_in]], constant VxRoot &vx_root [[buffer(0)]]");
            if (t.need_front) put(&file, ", bool vx_front [[front_facing]]");
            if (t.need_sample_id) put(&file, ", uint vx_sample [[sample_id]]");
            if (t.need_prim) put(&file, ", uint vx_prim [[primitive_id]]");
        }
        if (t.need_lane) put(&file, ", uint vx_lane [[thread_index_in_simdgroup]]");
        if (t.need_lanes) put(&file, ", uint vx_lanes [[threads_per_simdgroup]]");
        put(&file, ") {\n");
        if (stage != 5) put(&file, "    VxOut vx_out = {};\n");
        if (t.need_iid) put(&file, "    uint vx_iid = vx_iid0 - vx_base_instance;   // (D3D's SV_InstanceID leaves out StartInstanceLocation)\n");
        put(&file, "%s", glocal.s ? glocal.s : "");
        put(&file, "%s", t.decls.s ? t.decls.s : "");
        put(&file, "%s", t.body.s ? t.body.s : "");
        put(&file, "}\n");
        out->msl = file.s;
        free(io.s); free(gl.s); free(glocal.s);
    }
done:
    free(t.body.s); free(t.decls.s); free(t.prelude.s); free(t.types.s);
    free(t.handles); free(t.is_handle); free(t.ptr_space); free(t.declared_types);
    free(t.term); free(t.first); free(t.idom); free(t.ipdom); free(t.rpo_index); free(t.loop_header_of); free(t.loop_exit); free(t.emitted); free(t.loops);
    if (t.failed || !out->msl) { if (!out->error[0]) snprintf(out->error, sizeof out->error, "translation failed"); free(out->msl); out->msl = NULL; return -1; }
    return 0;
}

void dxil_msl_free(dxil_msl *out) { free(out->msl); out->msl = NULL; }
