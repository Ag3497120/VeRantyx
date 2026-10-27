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
#include <strings.h>

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

typedef struct { uint8_t cls, kind, comp, has_counter; uint32_t slot; uint32_t stride; int heap, sampler_heap; uint8_t cmp, access; } handle_info;

typedef struct {
    const dxil_module *m;
    const dxil_function *f;
    dxil_msl *out;
    sb body, decls, prelude, types;
    int failed;
    int indent;
    // what the body needed
    int need_lane, need_lanes, need_vid, need_iid, need_front, need_sample_id, need_prim, need_tid, need_gid, need_ltid, need_lidx;
    int need_coverage;
    // Minimum precision (min16float without -enable-16bit-types): DXIL's half values are kept as float, as desktop
    // GPUs keep them, so that a shader tested there gets its 32-bit results; native 16-bit types stay half.
    int native16;
    int dual_source;            // pixel shader for dual-source blending: SV_Target1 is color(0) index(1)
    // geometry pipelines: 0 a stage function, 1 the vertex shader as a kernel, 2 a geometry shader as a kernel
    int mode;
    const dxil_msl *gs_from;    // (mode 2) the vertex kernel whose outputs are the inputs
    uint32_t gs_inprim, gs_maxvert, gs_topology, gs_instances;
    int16_t gs_slot[64][4];     // (modes 2-4) input element id, row -> the vertex kernel's output slot
    // tessellation: 3 hull control-point phase, 4 hull patch-constant phase, 5 domain shader
    const dxil_function *fn;    // the function to translate (the patch-constant one for mode 4)
    const dxil_md *cpsig, *pcsig;   // output control points, patch constants (hull); patch constants (domain)
    const dxil_msl *hs_from;    // (mode 5) the hull shader whose outputs are read
    int16_t pc_slot[64][4];     // (mode 5) patch-constant element id, row -> the hull's patch slot
    // per value
    uint32_t nvals;
    handle_info *handles;       // by local index: the resource a handle value stands for
    uint8_t *is_handle;
    uint8_t *ptr_space;         // pointer values: 1 thread, 2 threadgroup, 3 constant, 4 device
    uint8_t *declared_types;    // struct and array types emitted
    // signatures (by element id)
    const dxil_md *insig, *outsig;
    uint32_t sample_mask;
    int apply_sample_mask;
    // the control flow
    uint32_t nb;
    uint32_t *term;             // the terminator instruction of each block
    uint32_t *first;            // the first instruction of each block
    int *idom, *ipdom, *rpo_index, *loop_header_of, *loop_exit;
    uint8_t *tail;                         // (small blocks that return: copied into each place that jumps to them)
    uint8_t *emitted;
    uint32_t *npred;            // predecessors of each block
    int *flag;                  // a block reached from several arms of a branch: its flag while the arms are emitted, else -1
    uint32_t nflags;
    uint32_t *loops;            // innermost loop stack while emitting: header blocks
    uint32_t nloops;
    char views[64][32];
    uint32_t nviews;
    uint8_t heap_uav_access;
    uint8_t unresolved_uav_access;
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
static const char *wave_active_predicate(tr *t) {
    return t->out->stage == 0 && !(t->m->shader_feature_flags & DXIL_FEATURE_WAVE_OPS_INCLUDE_HELPER_LANES) ?
        "!simd_is_helper_thread()" : "true";
}
static int wave_int_kind(const dxil_type *type) {
    return type && type->kind == DXT_INT &&
        (type->bits == 1 || type->bits == 8 || type->bits == 16 || type->bits == 32 || type->bits == 64);
}
static int wave_value_type_supported(tr *t, uint32_t type_id) {
    const dxil_type *type = ty(t, type_id);
    const dxil_type *component = type && type->kind == DXT_VECTOR ? ty(t, type->elem) : type;
    if (!type || !component ||
        (type->kind == DXT_VECTOR && (type->count < 2 || type->count > 4))) return 0;
    return component->kind == DXT_FLOAT || component->kind == DXT_HALF || wave_int_kind(component);
}
static int wave_mask_type(tr *t, uint32_t type_id) {
    const dxil_type *type = ty(t, type_id);
    if (type && type->kind == DXT_VECTOR) {
        const dxil_type *element = ty(t, type->elem);
        return type->count == 4 && element && element->kind == DXT_INT && element->bits == 32;
    }
    // DXIL represents uint4 wave masks as the four-field dx.types.fouri32 aggregate.
    if (!type || type->kind != DXT_STRUCT || type->count != 4) return 0;
    for (uint32_t i = 0; i < 4; i++) {
        const dxil_type *element = ty(t, type->members[i]);
        if (!element || element->kind != DXT_INT || element->bits != 32) return 0;
    }
    return 1;
}
static void wave_shuffle_expression(tr *t, uint32_t value_id, const char *value, const char *lane,
                                    char *out, size_t cap) {
    const dxil_value *v = val(t, value_id);
    const dxil_type *value_type = v ? ty(t, v->type) : NULL;
    const dxil_type *component = value_type && value_type->kind == DXT_VECTOR ?
        ty(t, value_type->elem) : value_type;
    if (component && component->kind == DXT_INT && component->bits == 1) {
        char uint_type[24];
        if (value_type->kind == DXT_VECTOR) snprintf(uint_type, sizeof uint_type, "uint%u", value_type->count);
        else snprintf(uint_type, sizeof uint_type, "uint");
        if (value_type->kind == DXT_VECTOR)
            snprintf(out, cap, "(simd_shuffle(%s(%s), ushort(%s)) != %s(0))", uint_type, value, lane, uint_type);
        else
            snprintf(out, cap, "(simd_shuffle(uint(%s), ushort(%s)) != 0u)", value, lane);
    } else {
        snprintf(out, cap, "simd_shuffle(%s, ushort(%s))", value, lane);
    }
}

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
    case DXT_HALF: snprintf(out, cap, t->native16 ? "half" : "float"); return;
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
static const char *unsigned_name(int bits) { return bits <= 8 ? "uchar" : bits <= 16 ? "ushort" : bits <= 32 ? "uint" : "ulong"; }
static int is_float(tr *t, uint32_t id) {
    const dxil_type *y = ty(t, id);
    if (y && y->kind == DXT_VECTOR) y = ty(t, y->elem);
    return y && is_float_kind(y->kind);
}
// Bytes of a scalar (or a vector's element) type in memory: 2 for half and 16-bit integers, 8 for 64-bit ones.
static int elem_bytes(tr *t, uint32_t id) {
    const dxil_type *y = ty(t, id);
    if (y && y->kind == DXT_VECTOR) y = ty(t, y->elem);
    if (!y) return 4;
    if (y->kind == DXT_HALF) return 2;
    if (y->kind == DXT_DOUBLE) return 8;
    if (y->kind == DXT_INT) return y->bits <= 16 ? 2 : y->bits == 64 ? 8 : 4;
    return 4;
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
        float_literal(v->u.f, y && y->kind == DXT_HALF && t->native16, out, cap);
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
    case DXV_GLOBAL: snprintf(out, cap, "(&g%u)", v->u.index); return;   // (a global's value is its address)
    case DXV_CONST_EXPR: {
        // (getelementptr into a global, a cast of one: the address written out)
        if (v->u.expr.op == DXI_CAST && v->u.expr.n == 1) { ref(t, v->u.expr.ops[0], out, cap); return; }
        if (v->u.expr.op == DXI_GEP && v->u.expr.n >= 2) {
            char base[64], path[256];
            const dxil_value *bv = val(t, v->u.expr.ops[1]);
            if (bv && bv->kind == DXV_GLOBAL) snprintf(base, sizeof base, "g%u", bv->u.index);   // (the object itself)
            else { char p[64]; ref(t, v->u.expr.ops[1], p, sizeof p); snprintf(base, sizeof base, "(*%s)", p); }
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

// LLVM's shuffle mask is a fixed vector of i32 constants. Undef mask lanes, and indices outside the two
// concatenated source vectors, produce undef/poison result lanes; zero is a valid lowering for those lanes.
static int shuffle_mask_lane(tr *t, uint32_t mask_id, uint32_t lane, uint32_t *index, int *undefined) {
    const dxil_value *mask = val(t, mask_id);
    const dxil_type *mask_type = mask ? ty(t, mask->type) : NULL;
    if (!mask || !mask_type || mask_type->kind != DXT_VECTOR || lane >= mask_type->count) return 0;
    const dxil_type *element_type = ty(t, mask_type->elem);
    if (!element_type || element_type->kind != DXT_INT || element_type->bits != 32) return 0;
    *undefined = 0;
    if (mask->kind == DXV_CONST_UNDEF) { *undefined = 1; return 1; }
    if (mask->kind == DXV_CONST_NULL) { *index = 0; return 1; }

    if (mask->kind == DXV_CONST_AGGREGATE) {
        if (lane >= mask->u.list.n) return 0;
        const dxil_value *element = val(t, mask->u.list.ids[lane]);
        if (!element || element->type != mask_type->elem) return 0;
        if (element->kind == DXV_CONST_UNDEF || element->kind == DXV_CONST_NULL) {
            *undefined = 1;
            return 1;
        }
        if (element->kind != DXV_CONST_INT) return 0;
        if (element->u.i < 0 || (uint64_t)element->u.i > UINT32_MAX) {
            *undefined = 1;
            return 1;
        }
        *index = (uint32_t)element->u.i;
        return 1;
    }
    if (mask->kind == DXV_CONST_DATA) {
        if (lane >= mask->u.list.n || !mask->data) return 0;
        uint32_t raw = (uint32_t)mask->data[lane];
        int32_t signed_index;
        memcpy(&signed_index, &raw, sizeof signed_index);
        if (signed_index < 0) { *undefined = 1; return 1; }
        *index = raw;
        return 1;
    }
    return 0;
}

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
                s->has_counter = cls == DXIL_UAV && md_i(t, r, 8, 0) != 0;
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

// DXIL handle values usually come directly from CreateHandle/AnnotateHandle. Follow those definitions (and
// handle-valued selects/phis) so a write-only UAV can use Metal's less restrictive access::write type.
static int resource_handle_slots(tr *t, uint32_t id, uint8_t *slots, int *heap, uint32_t depth) {
    if (depth >= 32) return 0;
    const dxil_value *v = val(t, id);
    if (!v || v->kind != DXV_INST || v->u.index >= t->f->ninsts) return 0;
    const dxil_inst *in = &t->f->insts[v->u.index];
    if (in->op == DXI_CALL) {
        int op = dxil_dxop(t->m, t->f, in);
        if (op == 57 && in->n >= 4) {
            int ok_cls = 0, ok_range = 0;
            int cls = (int)const_int(t, in->ops[2], &ok_cls);
            uint32_t range = (uint32_t)const_int(t, in->ops[3], &ok_range);
            int slot = ok_cls && ok_range ? slot_by_range(t, cls, range) : -1;
            if (slot < 0) return 0;
            slots[slot] = 1;
            return 1;
        }
        if (op == 217 && in->n >= 3) {
            const dxil_value *bind = val(t, in->ops[2]);
            if (!bind || bind->kind != DXV_CONST_AGGREGATE || bind->u.list.n < 4) return 0;
            int ok_lower = 0, ok_space = 0, ok_cls = 0;
            uint32_t lower = (uint32_t)const_int(t, bind->u.list.ids[0], &ok_lower);
            uint32_t space = (uint32_t)const_int(t, bind->u.list.ids[2], &ok_space);
            int cls = (int)const_int(t, bind->u.list.ids[3], &ok_cls);
            int slot = ok_lower && ok_space && ok_cls ? slot_by_binding(t, cls, space, lower) : -1;
            if (slot < 0) return 0;
            slots[slot] = 1;
            return 1;
        }
        if (op == 218) {
            *heap = 1;
            return 1;
        }
        if (op == 216 && in->n >= 3)
            return resource_handle_slots(t, in->ops[2], slots, heap, depth + 1);
        return 0;
    }
    if (in->op == DXI_PHI) {
        int complete = in->n != 0;
        for (uint32_t i = 0; i + 1 < in->n; i += 2)
            complete &= resource_handle_slots(t, in->ops[i], slots, heap, depth + 1);
        return complete;
    }
    if (in->op == DXI_SELECT && in->n >= 3)
        return resource_handle_slots(t, in->ops[1], slots, heap, depth + 1) &&
               resource_handle_slots(t, in->ops[2], slots, heap, depth + 1);
    if (in->op == DXI_CAST && in->n >= 1)
        return resource_handle_slots(t, in->ops[0], slots, heap, depth + 1);
    return 0;
}

static void analyze_uav_accesses(tr *t) {
    for (uint32_t i = 0; i < t->f->ninsts; i++) {
        const dxil_inst *in = &t->f->insts[i];
        int op = dxil_dxop(t->m, t->f, in);
        uint8_t access = 0;
        switch (op) {
        case 66: case 68: case 139: access = DXIL_MSL_ACCESS_READ; break;
        case 67: case 69: case 140: access = DXIL_MSL_ACCESS_WRITE; break;
        case 78: case 79: access = DXIL_MSL_ACCESS_READ | DXIL_MSL_ACCESS_WRITE; break;
        default: break;
        }
        if (!access || in->n < 3) continue;
        uint8_t slots[64] = { 0 };
        int heap = 0;
        int complete = resource_handle_slots(t, in->ops[2], slots, &heap, 0);
        for (uint32_t k = 0; k < t->out->nslots; k++)
            if (slots[k] && t->out->slots[k].cls == DXIL_UAV)
                t->out->slots[k].uav_access |= access;
        if (heap) t->heap_uav_access |= access;
        if (!complete) t->unresolved_uav_access |= access;
    }
    if (t->unresolved_uav_access) {
        for (uint32_t i = 0; i < t->out->nslots; i++)
            if (t->out->slots[i].cls == DXIL_UAV) t->out->slots[i].uav_access = DXIL_MSL_ACCESS_READ | DXIL_MSL_ACCESS_WRITE;
        t->heap_uav_access = DXIL_MSL_ACCESS_READ | DXIL_MSL_ACCESS_WRITE;
    }
    for (uint32_t i = 0; i < t->out->nslots; i++)
        if (t->out->slots[i].cls == DXIL_UAV && !t->out->slots[i].uav_access)
            t->out->slots[i].uav_access = DXIL_MSL_ACCESS_READ | DXIL_MSL_ACCESS_WRITE;
    if (!t->heap_uav_access) t->heap_uav_access = DXIL_MSL_ACCESS_READ | DXIL_MSL_ACCESS_WRITE;
}

static const dxil_md *signature_element(const dxil_md *sig, tr *t, uint32_t id) {
    for (uint32_t i = 0; sig && i < sig->n; i++) {
        const dxil_md *e = dxil_md_op(t->m, sig, i);
        if (e && md_i(t, e, 0, -1) == (int)id) return e;
    }
    return NULL;
}

// An element row's slot: its place among the signature's rows, in element order (how the kernels lay rows out).
static int sig_slot(tr *t, const dxil_md *sig, uint32_t id, uint32_t row) {
    int slot = 0;
    for (uint32_t i = 0; sig && i < sig->n; i++) {
        const dxil_md *e = dxil_md_op(t->m, sig, i);
        int rows = md_i(t, e, 6, 1);
        if (md_i(t, e, 0, -1) == (int)id) return row < (uint32_t)rows ? slot + (int)row : -1;
        slot += rows;
    }
    return -1;
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
    t->pcsig = dxil_md_op(t->m, sigs, 2);
    const dxil_md *props = dxil_md_op(t->m, ep, 4);
    t->out->threads[0] = t->out->threads[1] = t->out->threads[2] = 1;
    for (uint32_t i = 0; props && i + 1 < props->n; i += 2) {
        int64_t tag;
        if (!dxil_md_int(t->m, dxil_md_op(t->m, props, i), &tag)) continue;
        if (tag == 0) {                                            // ShaderFlags: 0x800000 UseNativeLowPrecision
            int64_t flags = 0;
            if (dxil_md_int(t->m, dxil_md_op(t->m, props, i + 1), &flags)) t->native16 = (flags & 0x800000) != 0;
        }
        if (tag == 4) {                                            // NumThreads
            const dxil_md *n = dxil_md_op(t->m, props, i + 1);
            for (int k = 0; k < 3; k++) t->out->threads[k] = (uint32_t)md_i(t, n, k, 1);
        }
        if (tag == 2) {                                            // DSState: domain, input control points
            const dxil_md *n = dxil_md_op(t->m, props, i + 1);
            t->out->tess_domain = (uint32_t)md_i(t, n, 0, 0);
            t->out->tess_in_cp = (uint32_t)md_i(t, n, 1, 0);
        }
        if (tag == 3) {                                            // HSState: patch-constant function, in/out control points, domain, partitioning, output, max factor
            const dxil_md *n = dxil_md_op(t->m, props, i + 1);
            const dxil_md *fv = dxil_md_op(t->m, n, 0);
            if (fv && fv->kind == DXM_VALUE && t->mode == 4)
                for (uint32_t k = 0; k < t->m->nfunctions; k++) if (t->m->functions[k].value == fv->value) t->fn = &t->m->functions[k];
            t->out->tess_in_cp = (uint32_t)md_i(t, n, 1, 0);
            t->out->tess_out_cp = (uint32_t)md_i(t, n, 2, 0);
            t->out->tess_domain = (uint32_t)md_i(t, n, 3, 0);
            t->out->tess_partitioning = (uint32_t)md_i(t, n, 4, 0);
            t->out->tess_output = (uint32_t)md_i(t, n, 5, 0);
            const dxil_md *mf = dxil_md_op(t->m, n, 6);
            t->out->tess_max_factor = 64.0f;
            if (mf && mf->kind == DXM_VALUE) {
                const dxil_value *v = &t->m->values[mf->value];
                if (v->kind == DXV_CONST_FLOAT && v->u.f >= 1.0 && v->u.f <= 64.0) t->out->tess_max_factor = (float)v->u.f;
            }
        }
        if (tag == 1) {                                            // GSState: input primitive, max vertices, streams, topology, instances
            const dxil_md *n = dxil_md_op(t->m, props, i + 1);
            t->gs_inprim = (uint32_t)md_i(t, n, 0, 0);
            t->gs_maxvert = (uint32_t)md_i(t, n, 1, 0);
            t->gs_topology = (uint32_t)md_i(t, n, 3, 0);
            t->gs_instances = (uint32_t)md_i(t, n, 4, 1);
        }
    }
}

// The MSL member name and type of a signature element row: "i<id>_<row>" / "o<id>_<row>".
static void element_type(tr *t, const dxil_md *e, char *out, size_t cap) {
    int comp = md_i(t, e, 2, 9), cols = md_i(t, e, 7, 4);
    const char *base = comp == 1 ? "bool" : comp == 2 ? "short" : comp == 3 ? "ushort" : comp == 4 ? "int" : comp == 5 ? "uint"
                     : comp == 8 && t->native16 ? "half" : "float";
    if (cols <= 1) snprintf(out, cap, "%s", base);
    else snprintf(out, cap, "%s%d", base, cols);
}

// ---- handles ------------------------------------------------------------------------------------------------------

static const char *tex_type(int kind, int comp, uint8_t access, int depth) {
    static __thread char out[96];                                   // (pipelines are made on many threads at once)
    const char *c = comp == 4 || comp == 2 ? "int" : comp == 5 || comp == 3 ? "uint" : comp == 8 ? "half" : "float";
    const char *dim = depth ? kind == 2 ? "depth2d" : kind == 7 ? "depth2d_array" :
                               kind == 5 ? "depthcube" : kind == 9 ? "depthcube_array" : "depth2d"
                    : kind == 1 ? "texture1d" : kind == 2 ? "texture2d" : kind == 3 ? "texture2d_ms" : kind == 4 ? "texture3d" : kind == 5 ? "texturecube"
                    : kind == 6 ? "texture1d_array" : kind == 7 ? "texture2d_array" : kind == 8 ? "texture2d_ms_array" : kind == 9 ? "texturecube_array"
                    : kind == 10 || kind == 15 ? "texture_buffer" : "texture2d";
    if (depth) c = "float";
    const char *qualifier = access == DXIL_MSL_ACCESS_READ ? ", access::read" :
        access == DXIL_MSL_ACCESS_WRITE ? ", access::write" :
        access == (DXIL_MSL_ACCESS_READ | DXIL_MSL_ACCESS_WRITE) ? ", access::read_write" : "";
    snprintf(out, sizeof out, "%s<%s%s>", dim, c, qualifier);
    return out;
}

// The view struct a handle is read through: the descriptor with its resource id as the right Metal type.
static const char *view(tr *t, const handle_info *h, int depth) {
    char key[32];
    if (h->cls == DXIL_SAMPLER) snprintf(key, sizeof key, "VxS");
    else snprintf(key, sizeof key, "VxV%d_%d_%d_%d_%d", h->kind, h->comp, h->cls == DXIL_UAV, h->access, depth);
    for (uint32_t i = 0; i < t->nviews; i++) if (!strcmp(t->views[i], key)) return t->views[i];
    if (t->nviews >= 64) { err(t, "too many resource views"); return "VxDesc"; }
    strcpy(t->views[t->nviews], key);
    if (h->cls == DXIL_SAMPLER) put(&t->prelude, "struct VxS { ulong va; sampler s; uint size, stride, fmt, flags; };\n");
    else if (h->kind == 11 || h->kind == 12) put(&t->prelude, "struct %s { ulong va; ulong t; uint size, stride, fmt, flags; };\n", key);
    else put(&t->prelude, "struct %s { ulong va; %s t; uint size, stride, fmt, flags; };\n", key,
             tex_type(h->kind, h->comp, h->cls == DXIL_UAV ? h->access : 0, depth));
    return t->views[t->nviews++];
}

static handle_info *hinfo(tr *t, uint32_t id) {
    uint32_t l = local(t, id);
    if (id < t->m->nmodule_values || l >= t->nvals || !t->is_handle[l]) return NULL;
    return &t->handles[l];
}

// The texture expression of a handle: ((device const VxV.. *)h)->t
static void tex_of(tr *t, uint32_t id, int depth, char *out, size_t cap) {
    handle_info *h = hinfo(t, id);
    char r[64];
    ref(t, id, r, sizeof r);
    if (!h) { err(t, "a resource handle that does not come from createHandle"); snprintf(out, cap, "0"); return; }
    snprintf(out, cap, "((device const %s *)%s)->%s", view(t, h, depth), r, h->cls == DXIL_SAMPLER ? "s" : "t");
}

static void desc_min_lod_of(tr *t, uint32_t id, char *out, size_t cap) {
    char r[64];
    if (!hinfo(t, id)) {
        err(t, "a min-LOD clamp requires a resource handle");
        snprintf(out, cap, "0.0f");
        return;
    }
    ref(t, id, r, sizeof r);
    snprintf(out, cap, "vx_desc_min_lod((device const VxDesc *)%s)", r);
}

static int sample_min_lod_supported(int kind) {
    // Metal 1D textures have no min_lod_clamp overload. Metal 3D, 2D, arrays and cubes do.
    return kind == 2 || kind == 4 || kind == 5 || kind == 7 || kind == 9;
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

static void resret_min_lod_feature(tr *t, const dxil_inst *in, const char *supported_expr,
                                   const char *fallback_expr, int status) {
    line(t, "#if defined(VXRT_HAS_MIN_LOD_CLAMP)");
    resret(t, in, supported_expr, status);
    line(t, "#else");
    resret(t, in, fallback_expr, status);
    line(t, "#endif");
}

static void resret_component_mapping(tr *t, const dxil_inst *in, const char *expression,
                                     const char *descriptor, int status) {
    if (!descriptor) { resret(t, in, expression, status); return; }
    char mapped[1024];
    snprintf(mapped, sizeof mapped, "vx_desc_swizzle(%s, %s)", expression, descriptor);
    resret(t, in, mapped, status);
}

static void resret_min_lod_component_mapping(tr *t, const dxil_inst *in, const char *supported_expression,
                                             const char *fallback_expression, const char *descriptor,
                                             int status) {
    if (!descriptor) {
        resret_min_lod_feature(t, in, supported_expression, fallback_expression, status);
        return;
    }
    char supported[1024], fallback[1024];
    snprintf(supported, sizeof supported, "vx_desc_swizzle(%s, %s)", supported_expression, descriptor);
    snprintf(fallback, sizeof fallback, "vx_desc_swizzle(%s, %s)", fallback_expression, descriptor);
    resret_min_lod_feature(t, in, supported, fallback, status);
}

static int texture_load_offsets(tr *t, const dxil_inst *in, int kind, int64_t offsets[3]) {
    uint32_t count = kind == 1 || kind == 6 ? 1 :
                     kind == 2 || kind == 3 || kind == 7 || kind == 8 ? 2 :
                     kind == 4 ? 3 : 0;
    for (uint32_t i = 0; i < 3; i++) offsets[i] = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t operand = 7 + i;
        if (operand >= in->n || is_undef(t, in->ops[operand])) continue;
        const dxil_value *value = val(t, in->ops[operand]);
        if (value && value->kind == DXV_CONST_NULL) continue;
        if (!value || value->kind != DXV_CONST_INT || int_bits(t, value->type) != 32) {
            err(t, "a TextureLoad offset that is not a static 32-bit integer");
            return 0;
        }
        uint32_t bits = (uint32_t)value->u.i;
        offsets[i] = bits <= INT32_MAX ? (int64_t)bits : (int64_t)bits - INT64_C(0x100000000);
    }
    return 1;
}

// DXIL Load guarantees zero for invalid coordinates, mip levels, array layers, and sample indices. Metal's
// texture read is undefined outside those bounds, so guard it explicitly before issuing the read. Compute the
// coordinate plus the statically encoded DXIL offset in 64 bits to keep signed overflow from wrapping in-bounds.
static void emit_texture_load(tr *t, const dxil_inst *in, const char a[12][96], const handle_info *h) {
    if (h->kind == 5 || h->kind == 9) {
        err(t, "TextureLoad from cube resources is not supported by Direct3D");
        return;
    }
    int64_t offsets[3];
    if (!texture_load_offsets(t, in, h->kind, offsets)) return;

    char tex[160];
    tex_of(t, in->ops[2], 0, tex, sizeof tex);
    uint32_t coordinate_count = (uint32_t)coord_count(h->kind);
    static const char axes[] = { 'x', 'y', 'z' };
    for (uint32_t i = 0; i < coordinate_count; i++)
        line(t, "long vx_texture_load_%u_%c = long(int(%s)) + long(%lld);", in->result, axes[i],
             a[2 + i], (long long)offsets[i]);

    uint32_t valid_id = in->result;
    line(t, "bool vx_texture_load_valid_%u = true;", valid_id);
    int multisample = h->kind == 3 || h->kind == 8;
    int array = h->kind == 6 || h->kind == 7 || h->kind == 8;
    if (multisample) {
        line(t, "vx_texture_load_valid_%u = int(%s) >= 0 && uint(%s) < %s.get_num_samples();",
             valid_id, a[1], a[1], tex);
    } else if (h->cls != DXIL_UAV) {
        line(t, "vx_texture_load_valid_%u = int(%s) >= 0 && uint(%s) < %s.get_num_mip_levels();",
             valid_id, a[1], a[1], tex);
    }

    for (uint32_t i = 0; i < coordinate_count; i++) {
        const char *dimension = i == 0 ? "get_width" : i == 1 ? "get_height" : "get_depth";
        char bound[384];
        if (h->cls == DXIL_UAV || multisample) {
            if (i == 2 && h->kind == 4)
                snprintf(bound, sizeof bound,
                    "vx_texture_load_%u_z >= 0 && ulong(vx_texture_load_%u_z) < ulong(%s->size) && "
                    "ulong(vx_texture_load_%u_z) + ulong(%s->stride) < ulong(%s.get_depth())",
                    valid_id, valid_id, a[0], valid_id, a[0], tex);
            else
                snprintf(bound, sizeof bound, "vx_texture_load_%u_%c >= 0 && "
                         "vx_texture_load_%u_%c < long(%s.%s())",
                         valid_id, axes[i], valid_id, axes[i], tex, dimension);
        } else {
            snprintf(bound, sizeof bound, "vx_texture_load_%u_%c >= 0 && "
                     "vx_texture_load_%u_%c < long(%s.%s(uint(%s)))",
                     valid_id, axes[i], valid_id, axes[i], tex, dimension, a[1]);
        }
        line(t, "if (vx_texture_load_valid_%u) vx_texture_load_valid_%u = %s;", valid_id, valid_id, bound);
    }

    char layer[128] = "";
    if (array) {
        const char *index = a[2 + coordinate_count];
        line(t, "if (vx_texture_load_valid_%u) vx_texture_load_valid_%u = int(%s) >= 0 && "
             "uint(%s) < %s.get_array_size();", valid_id, valid_id, index, index, tex);
        snprintf(layer, sizeof layer, ", uint(%s)", index);
    }

    char coord[256];
    if (coordinate_count == 1)
        snprintf(coord, sizeof coord, "uint(vx_texture_load_%u_x)", valid_id);
    else if (coordinate_count == 2)
        snprintf(coord, sizeof coord, "uint2(uint(vx_texture_load_%u_x), uint(vx_texture_load_%u_y))",
                 valid_id, valid_id);
    else if (h->kind == 4 && h->cls == DXIL_UAV)
        snprintf(coord, sizeof coord, "uint3(uint(vx_texture_load_%u_x), uint(vx_texture_load_%u_y), "
                 "uint(vx_texture_load_%u_z) + %s->stride)", valid_id, valid_id, valid_id, a[0]);
    else
        snprintf(coord, sizeof coord, "uint3(uint(vx_texture_load_%u_x), uint(vx_texture_load_%u_y), "
                 "uint(vx_texture_load_%u_z))", valid_id, valid_id, valid_id);

    char read[512], expression[768];
    if (multisample)
        snprintf(read, sizeof read, "%s.read(%s%s, uint(%s))", tex, coord, layer, a[1]);
    else if (h->cls == DXIL_UAV)
        snprintf(read, sizeof read, "%s.read(%s%s)", tex, coord, layer);
    else
        snprintf(read, sizeof read, "%s.read(%s%s, uint(%s))", tex, coord, layer, a[1]);

    const char *vector_type = h->comp == 2 || h->comp == 4 ? "int4" :
                              h->comp == 3 || h->comp == 5 ? "uint4" : "float4";
    if (h->cls == DXIL_SRV)
        snprintf(expression, sizeof expression, "(vx_texture_load_valid_%u ? vx_desc_swizzle(%s(%s), %s) : %s(0))",
                 valid_id, vector_type, read, a[0], vector_type);
    else
        snprintf(expression, sizeof expression, "(vx_texture_load_valid_%u ? %s(%s) : %s(0))",
                 valid_id, vector_type, read, vector_type);
    resret(t, in, expression, 1);
}

static int buffer_kind(int kind) { return kind == 11 ? 1 : kind == 12 ? 2 : kind == 10 || kind == 15 ? 3 : 0; }   // raw, structured, typed

static int atomic_texture_kind(int kind) {
    return kind == 1 || kind == 2 || kind == 4 || kind == 6 || kind == 7 || kind == 10;
}

static int atomic_texture_arguments(tr *t, const handle_info *h, const char *const c[3], const char *desc,
                                    const char *tex, char *coord, size_t coord_cap, char *bounds, size_t bounds_cap) {
    switch (h->kind) {
    case 1:
        snprintf(coord, coord_cap, "uint(%s)", c[0]);
        snprintf(bounds, bounds_cap, "int(%s) >= 0 && uint(%s) < %s.get_width()", c[0], c[0], tex);
        break;
    case 6:
        snprintf(coord, coord_cap, "uint(%s), uint(%s)", c[0], c[1]);
        snprintf(bounds, bounds_cap, "int(%s) >= 0 && uint(%s) < %s.get_width() && int(%s) >= 0 && uint(%s) < %s.get_array_size()",
                 c[0], c[0], tex, c[1], c[1], tex);
        break;
    case 2:
        snprintf(coord, coord_cap, "uint2(uint(%s), uint(%s))", c[0], c[1]);
        snprintf(bounds, bounds_cap, "int(%s) >= 0 && uint(%s) < %s.get_width() && int(%s) >= 0 && uint(%s) < %s.get_height()",
                 c[0], c[0], tex, c[1], c[1], tex);
        break;
    case 7:
        snprintf(coord, coord_cap, "uint2(uint(%s), uint(%s)), uint(%s)", c[0], c[1], c[2]);
        snprintf(bounds, bounds_cap, "int(%s) >= 0 && uint(%s) < %s.get_width() && int(%s) >= 0 && uint(%s) < %s.get_height() && int(%s) >= 0 && uint(%s) < %s.get_array_size()",
                 c[0], c[0], tex, c[1], c[1], tex, c[2], c[2], tex);
        break;
    case 4:
        snprintf(coord, coord_cap, "uint3(uint(%s), uint(%s), uint(%s))", c[0], c[1], c[2]);
        snprintf(bounds, bounds_cap, "int(%s) >= 0 && uint(%s) < %s.get_width() && int(%s) >= 0 && uint(%s) < %s.get_height() && int(%s) >= 0 && uint(%s) < %s.get_depth()",
                 c[0], c[0], tex, c[1], c[1], tex, c[2], c[2], tex);
        if (h->cls == DXIL_UAV)
            snprintf(coord, coord_cap, "uint3(uint(%s), uint(%s), uint(%s) + %s->stride)", c[0], c[1], c[2], desc);
        if (h->cls == DXIL_UAV)
            snprintf(bounds, bounds_cap, "int(%s) >= 0 && uint(%s) < %s.get_width() && int(%s) >= 0 && uint(%s) < %s.get_height() && int(%s) >= 0 && uint(%s) < %s->size && uint(%s) + %s->stride < %s.get_depth()",
                     c[0], c[0], tex, c[1], c[1], tex, c[2], c[2], desc, c[2], desc, tex);
        break;
    case 10:
        snprintf(coord, coord_cap, "uint(%s->va + uint(%s))", desc, c[0]);
        snprintf(bounds, bounds_cap, "uint(%s) < %s->size", c[0], desc);
        break;
    default:
        err(t, "atomics on DXIL resource kind %u are not supported", h->kind);
        return 0;
    }
    return 1;
}

static void emit_texture_atomic_minmax(tr *t, const char *tex, const char *coord, const char *bounds,
                                       const char *storage_vector_type, int signed_operation,
                                       const char *operation, const char *value, const char *result) {
    const char *type = signed_operation ? "int" : "uint";
    line(t, "if (%s) {", bounds);
    t->indent++;
    line(t, "uint vx_old_bits = as_type<uint>(%s.atomic_load(%s).x);", tex, coord);
    line(t, "bool vx_done = false;");
    line(t, "while (!vx_done) {");
    t->indent++;
    line(t, "%s vx_value = %s(%s);", type, type, value);
    line(t, "%s vx_old = as_type<%s>(vx_old_bits);", type, type);
    line(t, "%s vx_new = %s(vx_old, vx_value);", type, operation);
    line(t, "uint vx_new_bits = as_type<uint>(vx_new);");
    line(t, "%s vx_expected = as_type<%s>(uint4(vx_old_bits));", storage_vector_type, storage_vector_type);
    line(t, "vx_done = %s.atomic_compare_exchange_weak(%s, &vx_expected, as_type<%s>(uint4(vx_new_bits)));",
         tex, coord, storage_vector_type);
    line(t, "if (!vx_done) vx_old_bits = as_type<uint>(vx_expected.x);");
    t->indent--;
    line(t, "}");
    if (result[0]) line(t, "%s = as_type<int>(vx_old_bits);", result);
    else line(t, "(void)vx_old_bits;");
    t->indent--;
    line(t, "} else {");
    t->indent++;
    if (result[0]) line(t, "%s = 0;", result);
    else line(t, "(void)0;");
    t->indent--;
    line(t, "}");
}

static void call(tr *t, const dxil_inst *in) {
    int op = dxil_dxop(t->m, t->f, in);
    const char *callee = dxil_value_name(t->m, in->ops[0]);
    // (lifetime markers and other hints the optimizer left carry no meaning for the translation)
    if (op < 0 && callee && (!strncmp(callee, "llvm.lifetime.", 14) || !strncmp(callee, "llvm.invariant.", 15) ||
                             !strcmp(callee, "llvm.donothing") || !strncmp(callee, "llvm.dbg.", 9))) return;
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
        if (kind == 10 && t->mode == 2) { line(t, "%s = %s(vx_tid.x);", r, tn); return; }
        if (kind == 10 && t->mode >= 3) { line(t, "%s = %s(vx_pid);", r, tn); return; }
        if (kind == 10) { t->need_prim = 1; line(t, "%s = %s(vx_prim);", r, tn); return; }
        if (t->mode >= 2) {                                        // a vertex of the input primitive / patch (VXV: the kernel's)
            int okv;
            uint32_t vertex = (uint32_t)const_int(t, in->ops[5], &okv);
            int slot = id < 64 && row < 4 ? t->gs_slot[id][row] : -1;
            if (slot < 0) { line(t, "%s = %s(0);", r, tn); return; }   // (not written by the vertex shader: undefined, zero)
            char at[96];
            if (okv) snprintf(at, sizeof at, "VXV(%u, %d)[%u]", vertex, slot, okc ? col : 0);
            else { char v[96]; ref(t, in->ops[5], v, sizeof v); snprintf(at, sizeof at, "VXV(uint(%s), %d)[%u]", v, slot, okc ? col : 0); }
            if (fp) line(t, "%s = %s(%s);", r, tn, at);
            else line(t, "%s = %s(as_type<uint>(%s));", r, tn, at);
            return;
        }
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
        int padded = t->out->stage == 0 && t->mode == 0 && md_i(t, e, 3, 0) == 16;   // (a render target: four components)
        if (cols <= 1 && !padded) line(t, "vx_out.o%u_%u = %s(%s);", id, row, base, a[3]);
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
            // (zeroinitializer: t0 in space 0, an SRV)
            int zero = bind && bind->kind == DXV_CONST_NULL;
            if (!bind || (!zero && (bind->kind != DXV_CONST_AGGREGATE || bind->u.list.n < 4))) { err(t, "createHandleFromBinding without a constant binding"); return; }
            int ok;
            uint32_t lower = zero ? 0 : (uint32_t)const_int(t, bind->u.list.ids[0], &ok);
            uint32_t space = zero ? 0 : (uint32_t)const_int(t, bind->u.list.ids[2], &ok);
            int cls = zero ? 0 : (int)const_int(t, bind->u.list.ids[3], &ok);
            slot = slot_by_binding(t, cls, space, lower);
            snprintf(index, sizeof index, "%s", a[1]);
        }
        if (slot < 0) { err(t, "a handle for a resource not in dx.resources"); return; }
        const dxil_msl_slot *s = &t->out->slots[slot];
        h->cls = s->cls; h->kind = s->kind; h->comp = s->comp; h->slot = (uint32_t)slot; h->stride = s->stride;
        h->access = s->cls == DXIL_UAV ? s->uav_access : 0;
        h->has_counter = s->has_counter;
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
            if (h->cls == DXIL_UAV)
                h->access = h->heap ? t->heap_uav_access : h->slot < t->out->nslots ? t->out->slots[h->slot].uav_access : 0;
            if (h->heap && h->cls <= DXIL_CBV)
                t->out->heap_resource_classes |= 1u << h->cls;
        }
        line(t, "%s = %s;", r, a[0]);
        return;
    }
    case 59: {                                                     // CBufferLoadLegacy(handle, row)
        const dxil_type *y = ty(t, in->type);
        // (CBufRet.f32, and CBufRet.f16 of a minimum-precision shader: four 32-bit floats a row, each made half)
        int f32 = y && y->count == 4 && is_float(t, y->members[0]);
        int h16 = y && y->count == 8;
        int w64 = y && y->count == 2 && int_bits(t, y->members[0]) == 64;
        if (h16 || w64) {
            // CBufRet.i16/f16: eight 16-bit values of the 16-byte row, low half of each word first (native 16-bit
            // types, SM 6.2); CBufRet.i64: two 64-bit integers.
            char tn[96], r[32], mt[32];
            type_name(t, in->type, tn, sizeof tn);
            type_name(t, y->members[0], mt, sizeof mt);
            ref(t, in->result, r, sizeof r);
            int half_type = is_float(t, y->members[0]);
            if (h16)
                line(t, "{ uint4 vx_w = as_type<uint4>(vx_cb(%s, uint(%s))); ushort2 vx_a = as_type<ushort2>(vx_w.x), vx_b = as_type<ushort2>(vx_w.y), "
                        "vx_c = as_type<ushort2>(vx_w.z), vx_d = as_type<ushort2>(vx_w.w); %s = %s{ %s(vx_a.x), %s(vx_a.y), %s(vx_b.x), %s(vx_b.y), "
                        "%s(vx_c.x), %s(vx_c.y), %s(vx_d.x), %s(vx_d.y) }; }", a[0], a[1], r, tn,
                     half_type ? "as_type<half>" : mt, half_type ? "as_type<half>" : mt, half_type ? "as_type<half>" : mt,
                     half_type ? "as_type<half>" : mt, half_type ? "as_type<half>" : mt, half_type ? "as_type<half>" : mt,
                     half_type ? "as_type<half>" : mt, half_type ? "as_type<half>" : mt);
            else
                line(t, "{ uint4 vx_w = as_type<uint4>(vx_cb(%s, uint(%s))); %s = %s{ %s(ulong(vx_w.x) | (ulong(vx_w.y) << 32)), "
                        "%s(ulong(vx_w.z) | (ulong(vx_w.w) << 32)) }; }", a[0], a[1], r, tn, mt, mt);
            return;
        }
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
        if (h->cls == DXIL_SRV && (op == 64 || op == 65))
            t->out->uses_component_mapping_unsupported_read = 1;
        int one_d = h->kind == 1 || h->kind == 6;
        if (one_d && op == 60 && in->n > 8 && !is_undef(t, in->ops[8])) {
            int constant_offset = 0;
            int64_t offset = const_int(t, in->ops[8], &constant_offset);
            if (!constant_offset || offset != 0) {
                err(t, "Metal 1D textures cannot preserve nonzero or dynamic texel offsets");
                return;
            }
        }
        if (one_d && (op == 61 || op == 62 || op == 63)) {
            err(t, "Metal 1D textures cannot preserve explicit bias, LOD, or gradients");
            return;
        }
        if (one_d && op == 60 && in->n > 11 && !is_undef(t, in->ops[11])) {
            err(t, "Metal 1D textures cannot preserve a sampler min-LOD clamp");
            return;
        }
        if (op == 64 || op == 65) {
            if (op == 64 && t->out->stage != 0) { err(t, "comparison sampling with derivatives outside a pixel shader"); return; }
            if (h->cls != DXIL_SRV || (h->kind != 2 && h->kind != 7 && h->kind != 5 && h->kind != 9)) {
                err(t, "comparison sampling supports 2D, 2D-array, cube, and cube-array depth textures"); return;
            }
            handle_info *s = hinfo(t, in->ops[3]);
            if (!s || s->cls != DXIL_SAMPLER) { err(t, "comparison sampling without a sampler handle"); return; }
            char tex[160], smp[160], c[256], layer[128], off[64], opt[128] = "";
            tex_of(t, in->ops[2], 1, tex, sizeof tex);
            tex_of(t, in->ops[3], 0, smp, sizeof smp);
            coords(t, in, 4, h->kind, "float", c, sizeof c, layer, sizeof layer);
            offsets(t, in, 8, h->kind, off, sizeof off);
            char lod[160], clamp[224];
            desc_min_lod_of(t, in->ops[2], lod, sizeof lod);
            if (op == 65) {
                snprintf(opt, sizeof opt, ", level(max(0.0f, %s))", lod);
            } else {
                int shader_clamp = in->n > 12 && !is_undef(t, in->ops[12]);
                if (shader_clamp) {
                    char shader_lod[96];
                    ref(t, in->ops[12], shader_lod, sizeof shader_lod);
                    snprintf(clamp, sizeof clamp, "max(%s, %s)", lod, shader_lod);
                    t->out->uses_min_lod_clamp = 1;
                } else snprintf(clamp, sizeof clamp, "%s", lod);
                snprintf(opt, sizeof opt, ", min_lod_clamp(%s)", clamp);
            }
            char e[700];
            snprintf(e, sizeof e, "float4(%s.sample_compare(%s, %s%s, %s%s%s), 0.0f, 0.0f, 0.0f)", tex, smp, c, layer, a[9], opt, off);
            if (op == 65) resret(t, in, e, 1);
            else {
                char fallback[700];
                snprintf(fallback, sizeof fallback, "float4(%s.sample_compare(%s, %s%s, %s%s), 0.0f, 0.0f, 0.0f)",
                         tex, smp, c, layer, a[9], off);
                resret_min_lod_feature(t, in, e, fallback, 1);
            }
            return;
        }
        char tex[160], smp[160], c[256], layer[128], off[64], opt[256] = "";
        tex_of(t, in->ops[2], 0, tex, sizeof tex);
        tex_of(t, in->ops[3], 0, smp, sizeof smp);
        coords(t, in, 4, h->kind, "float", c, sizeof c, layer, sizeof layer);
        offsets(t, in, 8, h->kind, off, sizeof off);
        char lod[160];
        desc_min_lod_of(t, in->ops[2], lod, sizeof lod);
        if (op == 61) snprintf(opt, sizeof opt, ", bias(%s)", a[9]);
        if (op == 62) snprintf(opt, sizeof opt, ", level(max(float(%s), %s))", a[9], lod);
        if (op == 63) {
            int k = coord_count(h->kind);
            // DXIL's fixed sampleGrad signature stores ddx0..2 and ddy0..2 at operands 11..16.
            // Keep this separate from `a`: it only retains the first 12 call arguments.
            if (in->n <= 16) { err(t, "SampleGrad is missing gradient operands"); return; }
            if (h->kind == 3 || h->kind == 8) { err(t, "SampleGrad on multisampled textures"); return; }
            char d[6][96];
            args(t, in, 11, 6, d);
            if (h->kind == 5 || h->kind == 9)
                snprintf(opt, sizeof opt, ", gradientcube(float3(%s, %s, %s), float3(%s, %s, %s))",
                         d[0], d[1], d[2], d[3], d[4], d[5]);
            else if (k == 3)
                snprintf(opt, sizeof opt, ", gradient3d(float3(%s, %s, %s), float3(%s, %s, %s))",
                         d[0], d[1], d[2], d[3], d[4], d[5]);
            else if (k == 2)
                snprintf(opt, sizeof opt, ", gradient2d(float2(%s, %s), float2(%s, %s))",
                         d[0], d[1], d[3], d[4]);
            else { err(t, "SampleGrad on unsupported texture kind %u", h->kind); return; }

            // The optional min-LOD clamp is the last operand in the canonical DXIL sampleGrad signature.
            if (in->n > 17 && !is_undef(t, in->ops[17])) {
                char shader_lod[96], clamp[224];
                ref(t, in->ops[17], shader_lod, sizeof shader_lod);
                if (!sample_min_lod_supported(h->kind)) {
                    err(t, "a min-LOD clamp on unsupported texture kind %u", h->kind);
                    return;
                }
                snprintf(clamp, sizeof clamp, "max(%s, %s)", lod, shader_lod);
                size_t used = strlen(opt);
                snprintf(opt + used, sizeof opt - used, ", min_lod_clamp(%s)", clamp);
                t->out->uses_min_lod_clamp = 1;
            }
        }
        uint32_t clamp_index = op == 60 ? 11u : 12u;
        if ((op == 60 || op == 61) && in->n > clamp_index && !is_undef(t, in->ops[clamp_index])) {
            char shader_lod[96], clamp[224];
            ref(t, in->ops[clamp_index], shader_lod, sizeof shader_lod);
            if (sample_min_lod_supported(h->kind)) {
                snprintf(clamp, sizeof clamp, "max(%s, %s)", lod, shader_lod);
                size_t used = strlen(opt);
                snprintf(opt + used, sizeof opt - used, ", min_lod_clamp(%s)", clamp);
                t->out->uses_min_lod_clamp = 1;
            } else {
                err(t, "a min-LOD clamp on unsupported texture kind %u", h->kind);
                return;
            }
        }
        char e[700], fallback[700];
        if (sample_min_lod_supported(h->kind)) {
            if (op == 60 || op == 61 || op == 63) {
                int already_has_clamp = (op == 63 && in->n > 17 && !is_undef(t, in->ops[17])) ||
                    ((op == 60 || op == 61) && in->n > clamp_index && !is_undef(t, in->ops[clamp_index]));
                if (!already_has_clamp) {
                    size_t used = strlen(opt);
                    snprintf(opt + used, sizeof opt - used, ", min_lod_clamp(%s)", lod);
                }
                snprintf(e, sizeof e, "float4(%s.sample(%s, %s%s%s%s))", tex, smp, c, layer, opt, off);
                char fallback_opt[256] = "";
                if (op == 61) snprintf(fallback_opt, sizeof fallback_opt, ", bias(%s)", a[9]);
                if (op == 63) {
                    int k = coord_count(h->kind);
                    char d[6][96];
                    args(t, in, 11, 6, d);
                    if (h->kind == 5 || h->kind == 9)
                        snprintf(fallback_opt, sizeof fallback_opt, ", gradientcube(float3(%s, %s, %s), float3(%s, %s, %s))",
                                 d[0], d[1], d[2], d[3], d[4], d[5]);
                    else if (k == 3)
                        snprintf(fallback_opt, sizeof fallback_opt, ", gradient3d(float3(%s, %s, %s), float3(%s, %s, %s))",
                                 d[0], d[1], d[2], d[3], d[4], d[5]);
                    else if (k == 2)
                        snprintf(fallback_opt, sizeof fallback_opt, ", gradient2d(float2(%s, %s), float2(%s, %s))",
                                 d[0], d[1], d[3], d[4]);
                }
                snprintf(fallback, sizeof fallback, "float4(%s.sample(%s, %s%s%s%s))", tex, smp, c, layer, fallback_opt, off);
                resret_min_lod_component_mapping(t, in, e, fallback,
                                                  h->cls == DXIL_SRV ? a[0] : NULL, 1);
            } else {
                snprintf(e, sizeof e, "float4(%s.sample(%s, %s%s%s%s))", tex, smp, c, layer, opt, off);
                resret_component_mapping(t, in, e, h->cls == DXIL_SRV ? a[0] : NULL, 1);
            }
        } else {
            snprintf(e, sizeof e, "float4(%s.sample(%s, %s%s%s%s))", tex, smp, c, layer, opt, off);
            resret_component_mapping(t, in, e, h->cls == DXIL_SRV ? a[0] : NULL, 1);
        }
        return;
    }
    case 73: case 74: {                                           // TextureGather, TextureGatherCmp
        t->out->uses_gather = 1;
        uint32_t expected_operands = op == 73 ? 11u : 12u;
        if (in->n != expected_operands) {
            err(t, "malformed texture gather signature: expected %u operands, got %u", expected_operands, in->n);
            return;
        }
        handle_info *h = hinfo(t, in->ops[2]);
        handle_info *sampler = hinfo(t, in->ops[3]);
        if (!h || h->cls != DXIL_SRV || !sampler || sampler->cls != DXIL_SAMPLER) {
            err(t, "gather requires an SRV texture and sampler handle");
            return;
        }
        if (op == 74) t->out->uses_component_mapping_unsupported_read = 1;
        if (h->kind != 2 && h->kind != 7 && h->kind != 5 && h->kind != 9) {
            err(t, "gather supports only 2D, 2D-array, cube and cube-array textures");
            return;
        }

        int ok;
        uint32_t channel = (uint32_t)const_int(t, in->ops[10], &ok);
        if (!ok || channel > 3) {
            err(t, "gather channel must be a constant in [0, 3]");
            return;
        }
        // Comparison gather addresses the depth component. DXIL still carries the channel operand, which must be 0.
        if (op == 74 && channel != 0) {
            err(t, "comparison gather must select the depth component");
            return;
        }
        if (h->kind == 2 || h->kind == 7) {
            for (uint32_t i = 0; i < 2; i++) {
                uint32_t offset_id = in->ops[8 + i];
                if (is_undef(t, offset_id)) continue;
                int offset_ok;
                int64_t offset = const_int(t, offset_id, &offset_ok);
                if (!offset_ok || offset < -8 || offset > 7) {
                    err(t, "Metal gather offsets must be constants in [-8, 7]");
                    return;
                }
            }
        }

        char tex[160], smp[160], c[256], layer[128], off[64];
        tex_of(t, in->ops[2], op == 74, tex, sizeof tex);
        tex_of(t, in->ops[3], 0, smp, sizeof smp);
        coords(t, in, 4, h->kind, "float", c, sizeof c, layer, sizeof layer);
        offsets(t, in, 8, h->kind, off, sizeof off);
        char e[700];
        if (op == 73) {
            static const char *components[] = { "component::x", "component::y", "component::z", "component::w" };
            char selector[64], gathered[64];
            snprintf(selector, sizeof selector, "vx_gather_component_%u", in->result);
            snprintf(gathered, sizeof gathered, "vx_gather_result_%u", in->result);
            line(t, "{");
            t->indent++;
            line(t, "uint %s = vx_desc_component_selector((device const VxDesc *)%s, %uu);", selector, a[0], channel);
            const char *gt = h->comp == 4 || h->comp == 2 ? "int4" : h->comp == 5 || h->comp == 3 ? "uint4" : "float4";
            int gf = gt[0] == 'f';
            const char *zero = gf ? "0.0f" : "0", *one = gf ? "1.0f" : "1";
            line(t, "%s %s = %s(%s);", gt, gathered, gt, zero);
            line(t, "switch (%s) {", selector);
            t->indent++;
            for (uint32_t component = 0; component < 4; component++) {
                if (h->kind == 2 || h->kind == 7)                   // (Metal wants the offset before the component)
                    line(t, "case %uu: %s = %s.gather(%s, %s%s%s, %s); break;", component, gathered,
                         tex, smp, c, layer, off[0] ? off : ", int2(0)", components[component]);
                else
                    line(t, "case %uu: %s = %s.gather(%s, %s%s, %s); break;", component, gathered,
                         tex, smp, c, layer, components[component]);
            }
            line(t, "case 4u: %s = %s(%s); break;", gathered, gt, zero);
            line(t, "case 5u: %s = %s(%s); break;", gathered, gt, one);
            line(t, "default: %s = %s(%s); break;", gathered, gt, zero);
            t->indent--;
            line(t, "}");
            resret(t, in, gathered, 1);
            t->indent--;
            line(t, "}");
            return;
        } else if (h->kind == 2 || h->kind == 7) {
            snprintf(e, sizeof e, "%s.gather_compare(%s, %s%s, %s%s)", tex, smp, c, layer, a[9], off);
        } else {
            snprintf(e, sizeof e, "%s.gather_compare(%s, %s%s, %s)", tex, smp, c, layer, a[9]);
        }
        resret(t, in, e, 1);
        return;
    }
    case 66: {                                                     // TextureLoad(srv, mip or sample, c0, c1, c2, o0, o1, o2)
        handle_info *h = hinfo(t, in->ops[2]);
        if (!h) { err(t, "a load from a handle that does not come from createHandle"); return; }
        if (h->cls == DXIL_SRV && !buffer_kind(h->kind) && h->kind != 3 && h->kind != 8)
            t->out->uses_texture_load = 1;
        if (buffer_kind(h->kind) == 3) {
            char tex[160], e[600];
            tex_of(t, in->ops[2], 0, tex, sizeof tex);
            const char *vt = h->comp == 4 || h->comp == 2 ? "int4" : h->comp == 5 || h->comp == 3 ? "uint4"
                         : h->comp == 8 ? "half4" : "float4";
            if (h->cls == DXIL_SRV)
                snprintf(e, sizeof e, "uint(%s) < %s->size ? vx_desc_swizzle(%s(%s.read(uint(%s->va + uint(%s)))), %s) : %s(0)",
                         a[2], a[0], vt, tex, a[0], a[2], a[0], vt);
            else
                snprintf(e, sizeof e, "uint(%s) < %s->size ? %s(%s.read(uint(%s->va + uint(%s)))) : %s(0)",
                         a[2], a[0], vt, tex, a[0], a[2], vt);
            resret(t, in, e, 1);
        }
        else emit_texture_load(t, in, a, h);
        return;
    }
    case 72: {                                                     // GetDimensions(resource, mip)
        handle_info *h = hinfo(t, in->ops[2]);
        if (!h) { err(t, "GetDimensions on a handle that does not come from createHandle"); return; }
        if (h->cls != DXIL_SRV && h->cls != DXIL_UAV) { err(t, "GetDimensions on a non-SRV/UAV resource"); return; }
        char e[640];
        int bk = buffer_kind(h->kind);
        if (bk == 1) {
            snprintf(e, sizeof e, "uint4(%s->size, 0u, 0u, 0u)", a[0]);  // ByteAddressBuffer: bytes
        } else if (bk == 2) {
            snprintf(e, sizeof e, "uint4(%s->stride ? %s->size / %s->stride : 0u, %s->stride, 0u, 0u)", a[0], a[0], a[0], a[0]);
        } else if (bk == 3) {
            snprintf(e, sizeof e, "uint4(%s->size, 0u, 0u, 0u)", a[0]);
        } else {
            char tex[160];
            tex_of(t, in->ops[2], 0, tex, sizeof tex);
            const char *lod = h->cls == DXIL_UAV ? "0u" : a[1];
            char width[192], height[192] = "0u", depth[192] = "0u", layers[192] = "0u", fourth[192] = "0u";
            if (h->kind == 1 || h->kind == 6) {
                snprintf(width, sizeof width, "%s.get_width(uint(%s))", tex, lod);
                if (h->kind == 6) snprintf(height, sizeof height, "%s.get_array_size()", tex);
                if (h->cls != DXIL_UAV) snprintf(fourth, sizeof fourth, "%s.get_num_mip_levels()", tex);
            } else if (h->kind == 2 || h->kind == 7 || h->kind == 4 || h->kind == 5 || h->kind == 9) {
                snprintf(width, sizeof width, "%s.get_width(uint(%s))", tex, lod);
                snprintf(height, sizeof height, "%s.get_height(uint(%s))", tex, lod);
                if (h->kind == 4 && h->cls == DXIL_UAV)
                    snprintf(depth, sizeof depth, "((%s->flags & %uu) != 0u ? %s->size : %s.get_depth(uint(%s)))",
                        a[0], DXIL_MSL_DESC_HAS_W_SLICE_RANGE, a[0], tex, lod);
                else if (h->kind == 4) snprintf(depth, sizeof depth, "%s.get_depth(uint(%s))", tex, lod);
                if (h->kind == 7 || h->kind == 9) snprintf(layers, sizeof layers, "%s.get_array_size()", tex);
                if (h->cls != DXIL_UAV) snprintf(fourth, sizeof fourth, "%s.get_num_mip_levels()", tex);
            } else if (h->kind == 3 || h->kind == 8) {
                snprintf(width, sizeof width, "%s.get_width()", tex);
                snprintf(height, sizeof height, "%s.get_height()", tex);
                if (h->kind == 8) snprintf(layers, sizeof layers, "%s.get_array_size()", tex);
                snprintf(fourth, sizeof fourth, "%s.get_num_samples()", tex);
            } else {
                err(t, "GetDimensions for resource kind %u", h->kind);
                return;
            }
            // DXIL returns width, height, depth/array size, then mip levels or sample count.
            const char *c2 = h->kind == 4 ? depth : layers;
            snprintf(e, sizeof e, "uint4(%s, %s, %s, %s)", width, height, c2, fourth);
        }
        resret(t, in, e, 0);
        return;
    }
    case 67: {                                                     // TextureStore(uav, c0, c1, c2, v0..v3, mask)
        handle_info *h = hinfo(t, in->ops[2]);
        if (!h) { err(t, "a store to a handle that does not come from createHandle"); return; }
        char tex[160], c[256], layer[128];
        tex_of(t, in->ops[2], 0, tex, sizeof tex);
        coords(t, in, 3, h->kind, "uint", c, sizeof c, layer, sizeof layer);
        int ints = h->comp == 4 || h->comp == 5 || h->comp == 2 || h->comp == 3;
        const char *vt = h->comp == 4 || h->comp == 2 ? "int4" : ints ? "uint4" : h->comp == 8 ? "half4" : "float4";
        // (a store outside the texture does nothing in D3D12; in Metal it is undefined - on Apple GPUs a fault that
        //  ends the command buffer as "Insufficient Memory")
        if (h->kind == 4 && h->cls == DXIL_UAV) {
            line(t, "if (uint(%s) < %s->size && uint(%s) < %s.get_width() && uint(%s) < %s.get_height()) %s.write(%s(%s, %s, %s, %s), uint3(uint(%s), uint(%s), uint(%s) + %s->stride));",
                 a[3], a[0], a[1], tex, a[2], tex, tex, vt, a[4], a[5], a[6], a[7], a[1], a[2], a[3], a[0]);
            return;
        }
        char inside[512] = "true";
        if (h->kind == 1) snprintf(inside, sizeof inside, "uint(%s) < %s.get_width()", a[1], tex);
        else if (h->kind == 6) snprintf(inside, sizeof inside, "uint(%s) < %s.get_width() && uint(%s) < %s.get_array_size()", a[1], tex, a[2], tex);
        else if (h->kind == 2) snprintf(inside, sizeof inside, "uint(%s) < %s.get_width() && uint(%s) < %s.get_height()", a[1], tex, a[2], tex);
        else if (h->kind == 7) snprintf(inside, sizeof inside, "uint(%s) < %s.get_width() && uint(%s) < %s.get_height() && uint(%s) < %s.get_array_size()",
                                        a[1], tex, a[2], tex, a[3], tex);
        else if (h->kind == 4) snprintf(inside, sizeof inside, "uint(%s) < %s.get_width() && uint(%s) < %s.get_height() && uint(%s) < %s.get_depth()",
                                        a[1], tex, a[2], tex, a[3], tex);
        line(t, "if (%s) %s.write(%s(%s, %s, %s, %s), %s%s);", inside, tex, vt, a[4], a[5], a[6], a[7], c, layer);
        return;
    }
    case 68: case 139: {                                           // BufferLoad(res, index, wot), RawBufferLoad(res, index, offset, mask, align)
        handle_info *h = hinfo(t, in->ops[2]);
        if (!h) { err(t, "a buffer load from a handle that does not come from createHandle"); return; }
        int bk = buffer_kind(h->kind);
        char e[600];
        if (bk == 3) {
            char tex[160];
            tex_of(t, in->ops[2], 0, tex, sizeof tex);
            const char *vt = h->comp == 4 || h->comp == 2 ? "int4" : h->comp == 5 || h->comp == 3 ? "uint4"
                         : h->comp == 8 ? "half4" : "float4";
            if (h->cls == DXIL_SRV) {
                snprintf(e, sizeof e, "uint(%s) < %s->size ? vx_desc_swizzle(%s(%s.read(uint(%s->va + uint(%s)))), %s) : %s(0)",
                         a[1], a[0], vt, tex, a[0], a[1], a[0], vt);
            } else {
                snprintf(e, sizeof e, "uint(%s) < %s->size ? %s(%s.read(uint(%s->va + uint(%s)))) : %s(0)",
                         a[1], a[0], vt, tex, a[0], a[1], vt);
            }
            resret(t, in, e, 1);
        } else {
            char addr[256];
            if (bk == 2 && !(op == 139 && is_undef(t, in->ops[4]))) snprintf(addr, sizeof addr, "uint(%s) * %s->stride + uint(%s)", a[1], a[0], a[2]);
            else snprintf(addr, sizeof addr, "uint(%s)", a[1]);
            const dxil_type *rt = in->result != UINT32_MAX ? ty(t, in->type) : NULL;
            // (type id 0 is a type like any other - often float, the module's first)
            int has_elem = rt && rt->count;
            uint32_t elem = has_elem ? rt->members[0] : 0;
            int f = has_elem && is_float(t, elem);
            int bytes = has_elem ? elem_bytes(t, elem) : 4;
            // (raw and structured elements of 16 or 64 bits, SM 6.2 native 16-bit types and SM 6.0 int64)
            snprintf(e, sizeof e, "%s(%s, %s)", bytes == 2 ? "vx_ld16x4" : bytes == 8 ? "vx_ld64x4" : "vx_ld4", a[0], addr);
            if (bytes == 8 && f) { err(t, "64-bit floating-point buffer loads"); return; }
            if (f) { char e2[420]; snprintf(e2, sizeof e2, "as_type<%s>(%s)", bytes == 2 ? "half4" : "float4", e); snprintf(e, sizeof e, "%s", e2); }
            resret_component_mapping(t, in, e, h->cls == DXIL_SRV ? a[0] : NULL, 1);
        }
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
            tex_of(t, in->ops[2], 0, tex, sizeof tex);
            const char *vt = h->comp == 4 || h->comp == 2 ? "int4" : h->comp == 5 || h->comp == 3 ? "uint4"
                         : h->comp == 8 ? "half4" : "float4";
            line(t, "if (uint(%s) < %s->size) %s.write(%s(%s, %s, %s, %s), uint(%s->va + uint(%s)));",
                 a[1], a[0], tex, vt, a[3], a[4], a[5], a[6], a[0], a[1]);
            return;
        }
        char addr[256];
        if (bk == 2 && !is_undef(t, in->ops[4])) snprintf(addr, sizeof addr, "uint(%s) * %s->stride + uint(%s)", a[1], a[0], a[2]);
        else snprintf(addr, sizeof addr, "uint(%s)", a[1]);
        for (int k = 0; k < 4; k++) {
            if (!(mask & (1 << k))) continue;
            uint32_t vt = val(t, in->ops[5 + k]) ? val(t, in->ops[5 + k])->type : 0;
            int f = is_float(t, vt), bytes = elem_bytes(t, vt);
            if (bytes == 2)
                line(t, "vx_st16(%s, %s + %du, as_type<ushort>(%s));", a[0], addr, 2 * k, a[3 + k]);
            else if (bytes == 8) {
                if (f) { err(t, "64-bit floating-point buffer stores"); return; }
                line(t, "vx_st64(%s, %s + %du, ulong(%s));", a[0], addr, 8 * k, a[3 + k]);
            } else
                line(t, "vx_st(%s, %s + %du, %s);", a[0], addr, 4 * k, f ? (snprintf(a[11], 96, "as_type<uint>(%s)", a[3 + k]), a[11]) : a[3 + k]);
        }
        return;
    }
    case 70: {                                                     // BufferUpdateCounter(handle, +/-1)
        handle_info *h = hinfo(t, in->ops[2]);
        const dxil_value *delta = in->n == 4 ? val(t, in->ops[3]) : NULL;
        const dxil_type *delta_type = delta ? ty(t, delta->type) : NULL;
        if (in->n != 4 || !h || h->cls != DXIL_UAV || h->kind != 12 ||
            (!h->has_counter && !h->heap) || !delta_type || delta_type->kind != DXT_INT || delta_type->bits != 8) {
            err(t, "a counter update on a handle without a structured UAV counter");
            return;
        }
        t->out->uses_counters = 1;
        // (IncrementCounter gives the count before, DecrementCounter the count after)
        if (in->result != UINT32_MAX)
            line(t, "%s = (%s->flags & %uu) != 0u ? uint(int(atomic_fetch_add_explicit((device atomic_uint *)%s->res, uint(int(as_type<char>(%s))), memory_order_relaxed)) + min(int(as_type<char>(%s)), 0)) : 0u;",
                 r, a[0], DXIL_MSL_DESC_HAS_COUNTER, a[0], a[1], a[1]);
        else
            line(t, "if ((%s->flags & %uu) != 0u) atomic_fetch_add_explicit((device atomic_uint *)%s->res, uint(int(as_type<char>(%s))), memory_order_relaxed);",
                 a[0], DXIL_MSL_DESC_HAS_COUNTER, a[0], a[1]);
        return;
    }
    case 78: case 79: {                                            // AtomicBinOp(h, op, c0, c1, c2, v), AtomicCompareExchange(h, c0, c1, c2, cmp, v)
        handle_info *h = hinfo(t, in->ops[2]);
        if (in->n != 8 || !h) { err(t, "an atomic on a handle that does not come from createHandle"); return; }
        if (h->cls != DXIL_UAV) { err(t, "an atomic on a non-UAV resource"); return; }
        int bk = buffer_kind(h->kind);
        if (h->kind == 12 && !h->stride) { err(t, "an atomic on a structured buffer without a stride"); return; }
        if (bk == 3 && (h->kind != 10 || (h->comp != 4 && h->comp != 5))) {
            err(t, "typed-buffer atomics require an R32_SINT or R32_UINT view"); return;
        }
        if (!bk && (!atomic_texture_kind(h->kind) || (h->comp != 4 && h->comp != 5))) {
            err(t, "atomics require a typed integer UAV with 32-bit components"); return;
        }
        uint32_t c0 = op == 78 ? 4 : 3;
        char c[3][96];
        for (int k = 0; k < 3; k++) ref(t, in->ops[c0 + k], c[k], 96);
        char offset[192] = { 0 }, bounds[512] = { 0 }, tex[160] = { 0 }, coord[192] = { 0 };
        if (bk == 1 || bk == 2) {
            if (bk == 2) snprintf(offset, sizeof offset, "ulong(uint(%s)) * ulong(%s->stride) + ulong(uint(%s))", c[0], a[0], c[1]);
            else snprintf(offset, sizeof offset, "ulong(uint(%s))", c[0]);
            snprintf(bounds, sizeof bounds, "%s->size >= 4u && vx_offset <= ulong(%s->size - 4u) && (vx_offset & 3ul) == 0ul", a[0], a[0]);
        } else {
            const char *coords[3] = { c[0], c[1], c[2] };
            tex_of(t, in->ops[2], 0, tex, sizeof tex);
            atomic_texture_arguments(t, h, coords, a[0], tex, coord, sizeof coord, bounds, sizeof bounds);
            if (t->failed) return;
        }
        if (op == 78) {
            int ok;
            int aop = (int)const_int(t, in->ops[3], &ok);
            static const char *fn[] = { "add", "and", "or", "xor", "min", "max", "min", "max", "exchange" };
            if (!ok || aop < 0 || aop > 8) { err(t, "atomic operation %d", aop); return; }
            int signed_operation = aop == 4 || aop == 5;
            const char *type = (bk == 1 || bk == 2) ? (signed_operation ? "int" : "uint")
                                                     : h->comp == 4 ? "int" : "uint";
            char v[96];
            char result_stmt[96], oob_stmt[96];
            ref(t, in->ops[7], v, sizeof v);
            if (r[0]) { snprintf(result_stmt, sizeof result_stmt, "%s = as_type<int>(vx_old);", r); snprintf(oob_stmt, sizeof oob_stmt, "%s = 0;", r); }
            else { snprintf(result_stmt, sizeof result_stmt, "(void)vx_old;"); snprintf(oob_stmt, sizeof oob_stmt, "(void)0;"); }
            if (bk == 1 || bk == 2) {
                const char *atomic_fn = aop == 8 ? "exchange" : (snprintf(a[11], 96, "fetch_%s", fn[aop]), a[11]);
                line(t, "{ ulong vx_offset = %s; if (%s) { auto vx_old = atomic_%s_explicit((device atomic_%s *)(%s->va + vx_offset), %s(%s), memory_order_relaxed); %s } else { %s } }",
                     offset, bounds, atomic_fn, type, a[0], type, v, result_stmt, oob_stmt);
            } else {
                const char *atomic_fn = aop == 8 ? "atomic_exchange" : (snprintf(a[11], 96, "atomic_fetch_%s", fn[aop]), a[11]);
                char operation_vector[16], storage_vector[16];
                snprintf(operation_vector, sizeof operation_vector, "%s4", type);
                snprintf(storage_vector, sizeof storage_vector, "%s4", h->comp == 4 ? "int" : "uint");
                int mismatched_minmax = (aop == 4 || aop == 5) ? h->comp != 4 :
                                        (aop == 6 || aop == 7) ? h->comp != 5 : 0;
                if (mismatched_minmax) {
                    emit_texture_atomic_minmax(t, tex, coord, bounds, storage_vector,
                                               aop == 4 || aop == 5, aop == 4 || aop == 6 ? "min" : "max", v, r);
                } else {
                    line(t, "if (%s) { auto vx_old = %s.%s(%s, %s(%s)); %s } else { %s }",
                         bounds, tex, atomic_fn, coord, operation_vector, v,
                         r[0] ? (snprintf(result_stmt, sizeof result_stmt, "%s = as_type<int>(vx_old.x);", r), result_stmt) : "(void)vx_old;",
                         oob_stmt);
                }
            }
        } else {
            char cmp[96], v[96];
            char result_stmt[96], oob_stmt[96];
            ref(t, in->ops[6], cmp, sizeof cmp);
            ref(t, in->ops[7], v, sizeof v);
            if (r[0]) { snprintf(result_stmt, sizeof result_stmt, "%s = as_type<int>(vx_e);", r); snprintf(oob_stmt, sizeof oob_stmt, "%s = 0;", r); }
            else { snprintf(result_stmt, sizeof result_stmt, "(void)vx_e;"); snprintf(oob_stmt, sizeof oob_stmt, "(void)0;"); }
            if (bk == 1 || bk == 2) {
                line(t, "{ ulong vx_offset = %s; if (%s) { uint vx_e = uint(%s); bool vx_ok = atomic_compare_exchange_weak_explicit((device atomic_uint *)(%s->va + vx_offset), &vx_e, uint(%s), memory_order_relaxed, memory_order_relaxed); while (!vx_ok && vx_e == uint(%s)) vx_ok = atomic_compare_exchange_weak_explicit((device atomic_uint *)(%s->va + vx_offset), &vx_e, uint(%s), memory_order_relaxed, memory_order_relaxed); %s } else { %s } }",
                     offset, bounds, cmp, a[0], v, cmp, a[0], v,
                     result_stmt, oob_stmt);
            } else {
                const char *type = h->comp == 4 ? "int" : "uint";
                char vector[16];
                snprintf(vector, sizeof vector, "%s4", type);
                if (r[0]) { snprintf(result_stmt, sizeof result_stmt, "%s = as_type<int>(vx_e.x);", r); snprintf(oob_stmt, sizeof oob_stmt, "%s = 0;", r); }
                line(t, "if (%s) { %s vx_e = %s(%s); bool vx_ok = %s.atomic_compare_exchange_weak(%s, &vx_e, %s(%s)); while (!vx_ok && vx_e.x == %s(%s)) vx_ok = %s.atomic_compare_exchange_weak(%s, &vx_e, %s(%s)); %s } else { %s }",
                     bounds, vector, vector, cmp, tex, coord, vector, v, type, cmp, tex, coord, vector, v,
                     result_stmt, oob_stmt);
            }
        }
        return;
    }
    case 80: {                                                     // Barrier(mode): 1 sync, 2 UAV global, 4 UAV group, 8 groupshared
        int ok;
        int mode = (int)const_int(t, in->ops[2], &ok);
        const char *flags = (mode & 2) || (mode & 4) ? ((mode & 8) ? "mem_flags::mem_device | mem_flags::mem_texture | mem_flags::mem_threadgroup"
                                                                        : "mem_flags::mem_device | mem_flags::mem_texture")
                          : (mode & 8) ? "mem_flags::mem_threadgroup" : "mem_flags::mem_none";
        if (mode & 1) line(t, "threadgroup_barrier(%s);", flags);
        else {
            t->out->uses_thread_fence = 1;
            const char *scope = mode & 2 ? "thread_scope_device" : mode & (4 | 8) ? "thread_scope_threadgroup" : "thread_scope_thread";
            line(t, "atomic_thread_fence(%s, memory_order_seq_cst, %s);", flags, scope);
        }
        return;
    }
    case 82: line(t, "if (%s) discard_fragment();", a[0]); return;
    case 83: case 84: case 85: case 86: {
        const dxil_value *source = in->n == 3 ? val(t, in->ops[2]) : NULL;
        if (in->n != 3 || in->result == UINT32_MAX || !source || source->type != in->type ||
            !is_float(t, in->type) || t->out->stage != 0) {
            err(t, "unsupported derivative type, operands, or shader stage");
            return;
        }
        if (op == 83 || op == 84) {
            uint32_t pair_lane = op == 83 ? 1u : 2u;
            line(t, "%s = quad_shuffle(%s, ushort(%uu)) - quad_shuffle(%s, ushort(0u));",
                 r, a[0], pair_lane, a[0]);
            return;
        }
        // Fine derivatives use the current pixel and its horizontal/vertical quad neighbor.
        // Metal's quad XOR permutes by the corresponding lane bit; the lane parity selects sign.
        int axis_y = op == 86;
        uint32_t axis_mask = axis_y ? 2u : 1u;
        t->need_lane = 1;
        line(t, "{");
        t->indent++;
        line(t, "uint vx_derivative_lane = vx_lane & 3u;");
        line(t, "%s vx_derivative_neighbor = quad_shuffle_xor(%s, ushort(%u));", tn, a[0], axis_mask);
        line(t, "%s = (vx_derivative_lane & %uu) != 0u ? (%s - vx_derivative_neighbor) : (vx_derivative_neighbor - %s);",
             r, axis_mask, a[0], a[0]);
        t->indent--;
        line(t, "}");
        return;
    }
    case 93: case 94: case 95: {
        int ok;
        int c = (int)const_int(t, in->ops[2], &ok);
        const char *src = op == 93 ? "vx_tid" : op == 94 ? "vx_gid" : "vx_ltid";
        if (op == 93) t->need_tid = 1; else if (op == 94) t->need_gid = 1; else t->need_ltid = 1;
        line(t, "%s = %s[%d];", r, src, c);
        return;
    }
    case 96: t->need_lidx = 1; line(t, "%s = vx_lidx;", r); return;
    case 97: case 98: case 99: {                                   // EmitStream, CutStream, EmitThenCutStream(stream)
        if (t->mode != 2) { err(t, "a stream operation outside a geometry shader"); return; }
        int ok;
        if (const_int(t, in->ops[2], &ok) != 0 || !ok) { err(t, "geometry output to a stream other than 0"); return; }
        if (op != 98) line(t, "VX_EMIT;");
        if (op != 97) line(t, "VX_CUT;");
        return;
    }
    case 100: line(t, "%s = 0u;", r); return;                     // GSInstanceID (one instance)
    case 103: {                                                    // LoadOutputControlPoint(id, row, col, index): the patch-constant phase
        int ok, okr, okc, oki;
        uint32_t id = (uint32_t)const_int(t, in->ops[2], &ok), row = (uint32_t)const_int(t, in->ops[3], &okr);
        uint32_t col = (uint32_t)const_int(t, in->ops[4], &okc), index = (uint32_t)const_int(t, in->ops[5], &oki);
        int slot = t->mode == 4 && okr ? sig_slot(t, t->cpsig, id, row) : -1;
        if (slot < 0) { err(t, "an output control point read outside a patch-constant function or by a row chosen at run time"); return; }
        char at[96];
        if (oki) snprintf(at, sizeof at, "VXO(%u, %d)[%u]", index, slot, okc ? col : 0);
        else { char v[96]; ref(t, in->ops[5], v, sizeof v); snprintf(at, sizeof at, "VXO(uint(%s), %d)[%u]", v, slot, okc ? col : 0); }
        if (fp) line(t, "%s = %s(%s);", r, tn, at); else line(t, "%s = %s(as_type<uint>(%s));", r, tn, at);
        return;
    }
    case 104: {                                                    // LoadPatchConstant(id, row, col): a domain shader
        int ok, okr, okc;
        uint32_t id = (uint32_t)const_int(t, in->ops[2], &ok), row = (uint32_t)const_int(t, in->ops[3], &okr), col = (uint32_t)const_int(t, in->ops[4], &okc);
        if (t->mode != 5 || !okr) { err(t, "a patch-constant read outside a domain shader or by a row chosen at run time"); return; }
        int slot = id < 64 && row < 4 ? t->pc_slot[id][row] : -1;
        if (slot < 0) { line(t, "%s = %s(0);", r, tn); return; }
        if (fp) line(t, "%s = %s(vx_pc[%d][%u]);", r, tn, slot, okc ? col : 0);
        else line(t, "%s = %s(as_type<uint>(vx_pc[%d][%u]));", r, tn, slot, okc ? col : 0);
        return;
    }
    case 105: {                                                    // DomainLocation(component)
        int okc;
        uint32_t col = (uint32_t)const_int(t, in->ops[2], &okc);
        if (t->mode != 5) { err(t, "DomainLocation outside a domain shader"); return; }
        line(t, "%s = vx_dl[%u];", r, okc ? col : 0);
        return;
    }
    case 106: {                                                    // StorePatchConstant(id, row, col, value): as StoreOutput
        if (t->mode != 4) { err(t, "a patch constant stored outside a patch-constant function"); return; }
        int ok, okc, okr;
        uint32_t id = (uint32_t)const_int(t, in->ops[2], &ok), row = (uint32_t)const_int(t, in->ops[3], &okr), col = (uint32_t)const_int(t, in->ops[4], &okc);
        const dxil_md *e = signature_element(t->outsig, t, id);
        if (!e || !okr) { err(t, "patch constant %u is not in the signature", id); return; }
        char et[32], base[16];
        element_type(t, e, et, sizeof et);
        snprintf(base, sizeof base, "%.*s", (int)strcspn(et, "0123456789"), et);
        if (md_i(t, e, 7, 4) <= 1) line(t, "vx_out.o%u_%u = %s(%s);", id, row, base, a[3]);
        else line(t, "vx_out.o%u_%u[%u] = %s(%s);", id, row, okc ? col : 0, base, a[3]);
        return;
    }
    case 107: line(t, "%s = vx_cpid;", r); return;                // OutputControlPointID
    case 108:                                                      // PrimitiveID
        if (t->mode >= 3) { line(t, "%s = %s(vx_pid);", r, tn); return; }
        if (t->mode == 2) { line(t, "%s = %s(vx_tid.x);", r, tn); return; }
        t->need_prim = 1;
        line(t, "%s = %s(vx_prim);", r, tn);
        return;
    case 90:                                                       // SampleIndex
        if (t->out->stage != 0) { err(t, "SampleIndex outside a pixel shader"); return; }
        t->need_sample_id = 1;
        line(t, "%s = %s(vx_sample);", r, tn);
        return;
    case 91:                                                       // Coverage
        if (t->out->stage != 0) { err(t, "Coverage outside a pixel shader"); return; }
        t->need_coverage = 1;
        line(t, "%s = %s(vx_coverage);", r, tn);
        return;
    case 92: line(t, "%s = %s(0);", r, tn); return;               // InnerCoverage (no conservative rasterization)
    case 221:                                                      // IsHelperLane (SM 6.6)
        line(t, "%s = %s;", r, t->out->stage == 0 ? "simd_is_helper_thread()" : "false");
        return;
    case 81: {                                                     // CalculateLOD(handle, sampler, c0, c1, c2, clamped)
        handle_info *h = hinfo(t, in->ops[2]);
        if (!h || t->out->stage != 0 || h->kind == 1 || h->kind == 6 || h->kind == 3 || h->kind == 8) {
            err(t, "CalculateLOD outside a pixel shader or on a 1D or multisampled texture");
            return;
        }
        int ok;
        int clamped = (int)const_int(t, in->ops[7], &ok);
        if (!ok) { err(t, "CalculateLOD with a dynamic clamp flag"); return; }
        char tex[160], smp[160], c[256], layer[128];
        tex_of(t, in->ops[2], 0, tex, sizeof tex);
        tex_of(t, in->ops[3], 0, smp, sizeof smp);
        coords(t, in, 4, h->kind, "float", c, sizeof c, layer, sizeof layer);
        line(t, "%s = %s.%s(%s, %s);", r, tex, clamped ? "calculate_clamped_lod" : "calculate_unclamped_lod", smp, c);
        return;
    }
    case 162:                                                      // Dot2AddHalf(acc, ax, ay, bx, by)
        line(t, "%s = %s + float(%s) * float(%s) + float(%s) * float(%s);", r, a[0], a[1], a[3], a[2], a[4]);
        return;
    case 163: case 164: {                                          // Dot4AddI8Packed / Dot4AddU8Packed(acc, a, b)
        const char *v = op == 163 ? "char4" : "uchar4", *w = op == 163 ? "int4" : "uint4";
        line(t, "{ %s vx_a = %s(as_type<%s>(uint(%s))), vx_b = %s(as_type<%s>(uint(%s))), vx_p = vx_a * vx_b; "
                "%s = %s(%s(%s) + vx_p.x + vx_p.y + vx_p.z + vx_p.w); }",
             w, w, v, a[1], w, v, a[2], r, tn, op == 163 ? "int" : "uint", a[0]);
        return;
    }
    case 219: {                                                    // Unpack4x8(mode 0 unsigned / 1 signed, packed)
        int ok;
        int mode = (int)const_int(t, in->ops[2], &ok);
        const dxil_type *y = ty(t, in->type);
        if (!ok || !y || y->count < 4) { err(t, "Unpack4x8 with a dynamic mode"); return; }
        char mt[32];
        type_name(t, y->members[0], mt, sizeof mt);
        line(t, "{ %s vx_u = as_type<%s>(uint(%s)); %s = %s{ %s(vx_u.x), %s(vx_u.y), %s(vx_u.z), %s(vx_u.w) }; }",
             mode ? "char4" : "uchar4", mode ? "char4" : "uchar4", a[1], r, tn, mt, mt, mt, mt);
        return;
    }
    case 220: {                                                    // Pack4x8(mode 0 trunc, 1 unsigned clamp, 2 signed clamp, x, y, z, w)
        int ok;
        int mode = (int)const_int(t, in->ops[2], &ok);
        if (!ok) { err(t, "Pack4x8 with a dynamic mode"); return; }
        int bits = int_bits(t, val(t, in->ops[3]) ? val(t, in->ops[3])->type : 0);
        const char *sn = signed_name(bits);
        if (mode == 1)
            line(t, "%s = as_type<uint>(uchar4(clamp(int4(as_type<%s>(%s), as_type<%s>(%s), as_type<%s>(%s), as_type<%s>(%s)), 0, 255)));",
                 r, sn, a[1], sn, a[2], sn, a[3], sn, a[4]);
        else if (mode == 2)
            line(t, "%s = as_type<uint>(char4(clamp(int4(as_type<%s>(%s), as_type<%s>(%s), as_type<%s>(%s), as_type<%s>(%s)), -128, 127)));",
                 r, sn, a[1], sn, a[2], sn, a[3], sn, a[4]);
        else
            line(t, "%s = as_type<uint>(uchar4(uchar(%s), uchar(%s), uchar(%s), uchar(%s)));", r, a[1], a[2], a[3], a[4]);
        return;
    }
    case 110: {                                                    // WaveIsFirstLane: helpers do not precede active pixel lanes
        const dxil_type *result_type = ty(t, in->type);
        if (in->n != 2 || in->result == UINT32_MAX || !result_type ||
            result_type->kind != DXT_INT || result_type->bits != 1) {
            err(t, "unsupported WaveIsFirstLane result or operands");
            return;
        }
        const char *active = wave_active_predicate(t);
        t->need_lane = t->need_lanes = 1;
        line(t, "{");
        t->indent++;
        line(t, "ulong vx_wf_active = ulong(simd_ballot(%s));", active);
        line(t, "uint vx_wf_first = 0u;");
        line(t, "while (vx_wf_first < vx_lanes && ((vx_wf_active >> vx_wf_first) & 1ul) == 0ul) ++vx_wf_first;");
        line(t, "%s = vx_lane == vx_wf_first;", r);
        t->indent--;
        line(t, "}");
        return;
    }
    case 111: {                                                    // WaveGetLaneIndex
        const dxil_type *result_type = ty(t, in->type);
        if (in->n != 2 || in->result == UINT32_MAX || !result_type ||
            result_type->kind != DXT_INT || result_type->bits != 32) {
            err(t, "unsupported WaveGetLaneIndex result or operands");
            return;
        }
        t->need_lane = 1;
        line(t, "%s = vx_lane;", r);
        return;
    }
    case 112: {                                                    // WaveGetLaneCount
        const dxil_type *result_type = ty(t, in->type);
        if (in->n != 2 || in->result == UINT32_MAX || !result_type ||
            result_type->kind != DXT_INT || result_type->bits != 32) {
            err(t, "unsupported WaveGetLaneCount result or operands");
            return;
        }
        t->need_lanes = 1;
        line(t, "%s = vx_lanes;", r);
        return;
    }
    case 113: case 114: {                                          // WaveActiveAnyTrue / WaveActiveAllTrue
        const dxil_value *condition = in->n == 3 ? val(t, in->ops[2]) : NULL;
        const dxil_type *condition_type = condition ? ty(t, condition->type) : NULL;
        const dxil_type *result_type = ty(t, in->type);
        if (in->n != 3 || in->result == UINT32_MAX || !condition_type ||
            condition_type->kind != DXT_INT || condition_type->bits != 1 || !result_type ||
            result_type->kind != DXT_INT || result_type->bits != 1) {
            err(t, "unsupported WaveActiveVote result or operands");
            return;
        }
        const char *active = wave_active_predicate(t);
        if (op == 113)
            line(t, "{ ulong vx_wa_true = ulong(simd_ballot((%s) && bool(%s))); %s = vx_wa_true != 0ul; }",
                 active, a[0], r);
        else
            line(t, "{ ulong vx_wt_active = ulong(simd_ballot(%s)); ulong vx_wt_true = ulong(simd_ballot((%s) && bool(%s))); %s = vx_wt_active == vx_wt_true; }",
                 active, active, a[0], r);
        return;
    }
    case 115: {                                                    // WaveActiveAllEqual
        const dxil_value *value = in->n > 2 ? val(t, in->ops[2]) : NULL;
        const dxil_type *value_type = value ? ty(t, value->type) : NULL;
        if (in->n != 3 || !value_type || !wave_value_type_supported(t, value->type)) {
            err(t, "unsupported WaveActiveAllEqual type or operands");
            return;
        }
        const char *active = wave_active_predicate(t);
        t->need_lane = t->need_lanes = 1;
        line(t, "{");
        t->indent++;
        line(t, "ulong vx_we_active = ulong(simd_ballot(%s));", active);
        line(t, "uint vx_we_first = 0u;");
        line(t, "while (vx_we_first < vx_lanes && ((vx_we_active >> vx_we_first) & 1ul) == 0ul) ++vx_we_first;");
        line(t, "if (vx_we_first == vx_lanes) vx_we_first = 0u;");
        char first_value[512];
        wave_shuffle_expression(t, in->ops[2], a[0], "vx_we_first", first_value, sizeof first_value);
        line(t, "auto vx_we_value = %s;", first_value);
        if (value_type->kind == DXT_VECTOR)
            line(t, "ulong vx_we_equal = ulong(simd_ballot((%s) && all(%s == vx_we_value)));", active, a[0]);
        else
            line(t, "ulong vx_we_equal = ulong(simd_ballot((%s) && %s == vx_we_value));", active, a[0]);
        line(t, "%s = vx_we_equal == vx_we_active;", r);
        t->indent--;
        line(t, "}");
        return;
    }
    case 117: {                                                    // WaveReadLaneAt
        if (in->n != 4 || !val(t, in->ops[2]) || in->type != val(t, in->ops[2])->type ||
            !wave_value_type_supported(t, val(t, in->ops[2])->type)) {
            err(t, "unsupported WaveReadLaneAt operands");
            return;
        }
        char expression[512];
        wave_shuffle_expression(t, in->ops[2], a[0], a[1], expression, sizeof expression);
        line(t, "%s = %s;", r, expression);
        return;
    }
    case 118: {                                                    // WaveReadLaneFirst
        if (in->n != 3 || !val(t, in->ops[2]) || in->type != val(t, in->ops[2])->type ||
            !wave_value_type_supported(t, val(t, in->ops[2])->type)) {
            err(t, "unsupported WaveReadLaneFirst operands");
            return;
        }
        const char *active = wave_active_predicate(t);
        t->need_lane = t->need_lanes = 1;
        line(t, "{");
        t->indent++;
        line(t, "ulong vx_wf_active = ulong(simd_ballot(%s));", active);
        line(t, "uint vx_wf_first = 0u;");
        line(t, "while (vx_wf_first < vx_lanes && ((vx_wf_active >> vx_wf_first) & 1ul) == 0ul) ++vx_wf_first;");
        line(t, "if (vx_wf_first == vx_lanes) vx_wf_first = 0u;");
        char expression[512];
        wave_shuffle_expression(t, in->ops[2], a[0], "vx_wf_first", expression, sizeof expression);
        line(t, "%s = %s;", r, expression);
        t->indent--;
        line(t, "}");
        return;
    }
    case 119: case 121: {                                          // WaveActiveOp / WavePrefixOp(value, op, sop)
        const dxil_value *value = in->n > 2 ? val(t, in->ops[2]) : NULL;
        const dxil_type *value_type = value ? ty(t, value->type) : NULL;
        const dxil_type *component = value_type && value_type->kind == DXT_VECTOR ?
            ty(t, value_type->elem) : value_type;
        if (in->n != 5 || !value_type || value_type->kind == DXT_VECTOR || in->type != value->type ||
            !(component && (component->kind == DXT_FLOAT || component->kind == DXT_HALF ||
              (component->kind == DXT_INT && wave_int_kind(component) && component->bits > 1)))) {
            err(t, "unsupported wave reduction type or operands");
            return;
        }
        int ok_wop, ok_sop;
        int wop = (int)const_int(t, in->ops[3], &ok_wop), sop = (int)const_int(t, in->ops[4], &ok_sop);
        static const char *fn[] = { "sum", "product", "min", "max" };
        if (!ok_wop || !ok_sop || wop < 0 || wop > 3 || sop < 0 || sop > 1) { err(t, "invalid wave operation %d/%d", wop, sop); return; }
        int fp_value = component->kind == DXT_FLOAT || component->kind == DXT_HALF;
        int value_bits = component->bits;
        if (op == 121 && (wop == 2 || wop == 3)) {
            char ot[32];
            if (fp_value) snprintf(ot, sizeof ot, "%s", tn);
            else snprintf(ot, sizeof ot, "%s", sop ? unsigned_name(value_bits) : signed_name(value_bits));
            char input[192];
            if (fp_value) snprintf(input, sizeof input, "%s", a[0]);
            else snprintf(input, sizeof input, "as_type<%s>(%s)", ot, a[0]);
            const char *identity;
            if (fp_value) {
                int half = component->kind == DXT_HALF;
                identity = half ? (wop == 2 ? "as_type<half>(ushort(0x7c00u))" : "as_type<half>(ushort(0xfc00u))")
                                : (wop == 2 ? "as_type<float>(0x7f800000u)" : "as_type<float>(0xff800000u)");
            } else if (sop) {
                if (wop == 2) identity = value_bits > 32 ? "0xfffffffffffffffful" : value_bits > 16 ? "0xffffffffu" : value_bits > 8 ? "0xffffu" : "0xffu";
                else identity = "0";
            } else if (wop == 2) {
                identity = value_bits > 32 ? "as_type<long>(0x7ffffffffffffffful)" : value_bits > 16 ? "as_type<int>(0x7fffffffu)" : value_bits > 8 ? "as_type<short>(ushort(0x7fffu))" : "as_type<char>(uchar(0x7fu))";
            } else {
                identity = value_bits > 32 ? "as_type<long>(0x8000000000000000ul)" : value_bits > 16 ? "as_type<int>(0x80000000u)" : value_bits > 8 ? "as_type<short>(ushort(0x8000u))" : "as_type<char>(uchar(0x80u))";
            }
            const char *active = wave_active_predicate(t);
            t->need_lane = t->need_lanes = 1;
            line(t, "{");
            t->indent++;
            line(t, "ulong vx_wp_active = ulong(simd_ballot(%s));", active);
            line(t, "%s vx_wp_acc = %s;", ot, identity);
            line(t, "for (uint vx_wp_i = 0; vx_wp_i < vx_lanes; ++vx_wp_i) {");
            t->indent++;
            line(t, "bool vx_wp_include = vx_wp_i < vx_lane && ((vx_wp_active >> vx_wp_i) & ulong(1)) != 0;");
            line(t, "uint vx_wp_source = vx_wp_include ? vx_wp_i : vx_lane;");
            line(t, "%s vx_wp_value = simd_shuffle(%s, ushort(vx_wp_source));", ot, input);
            if (fp) {
                line(t, "if (vx_wp_include) {");
                t->indent++;
                line(t, "if (isnan(vx_wp_acc)) vx_wp_acc = vx_wp_value;");
                line(t, "else if (!isnan(vx_wp_value)) vx_wp_acc = vx_wp_acc %s vx_wp_value ? vx_wp_acc : vx_wp_value;", wop == 2 ? "<" : ">=");
                t->indent--;
                line(t, "}");
            } else {
                line(t, "if (vx_wp_include) vx_wp_acc = %s(vx_wp_acc, vx_wp_value);", fn[wop]);
            }
            t->indent--;
            line(t, "}");
            line(t, "%s = as_type<%s>(vx_wp_acc);", r, tn);
            t->indent--;
            line(t, "}");
            return;
        }
        char ot[32], input[192];
        if (wop < 2 || fp_value || sop) snprintf(ot, sizeof ot, "%s", tn);
        else snprintf(ot, sizeof ot, "%s", signed_name(value_bits));
        if (wop >= 2 && !fp_value && !sop) snprintf(input, sizeof input, "as_type<%s>(%s)", ot, a[0]);
        else snprintf(input, sizeof input, "%s", a[0]);
        const char *identity = NULL;
        if (wop == 0) identity = fp_value ? (component->kind == DXT_HALF ? "half(0.0)" : "0.0f") : "0";
        else if (wop == 1) identity = fp_value ? (component->kind == DXT_HALF ? "half(1.0)" : "1.0f") : "1";
        else if (fp_value) {
            int half = component->kind == DXT_HALF;
            identity = half ? (wop == 2 ? "as_type<half>(ushort(0x7c00u))" : "as_type<half>(ushort(0xfc00u))")
                            : (wop == 2 ? "as_type<float>(0x7f800000u)" : "as_type<float>(0xff800000u)");
        } else if (sop) {
            if (wop == 2) identity = value_bits > 32 ? "0xfffffffffffffffful" : value_bits > 16 ? "0xffffffffu" : value_bits > 8 ? "0xffffu" : "0xffu";
            else identity = "0";
        } else if (wop == 2) {
            identity = value_bits > 32 ? "as_type<long>(0x7ffffffffffffffful)" : value_bits > 16 ? "as_type<int>(0x7fffffffu)" : value_bits > 8 ? "as_type<short>(ushort(0x7fffu))" : "as_type<char>(uchar(0x7fu))";
        } else {
            identity = value_bits > 32 ? "as_type<long>(0x8000000000000000ul)" : value_bits > 16 ? "as_type<int>(0x80000000u)" : value_bits > 8 ? "as_type<short>(ushort(0x8000u))" : "as_type<char>(uchar(0x80u))";
        }
        const char *active = wave_active_predicate(t);
        line(t, "{");
        t->indent++;
        line(t, "%s vx_wo_value = (%s) ? %s : %s;", ot, active, input, identity);
        if (op == 119) {
            if (wop >= 2 && !fp_value && !sop)
                line(t, "%s = as_type<%s>(simd_%s(vx_wo_value));", r, tn, fn[wop]);
            else
                line(t, "%s = simd_%s(vx_wo_value);", r, fn[wop]);
        } else if (wop >= 2) {
            // Metal has no prefix min/max; evaluate the source's active-lane prefix in lane order.
            line(t, "ulong vx_wp_active = ulong(simd_ballot(%s));", active);
            line(t, "%s vx_wp_acc = %s;", ot, identity);
            line(t, "for (uint vx_wp_i = 0; vx_wp_i < vx_lanes; ++vx_wp_i) {");
            t->indent++;
            line(t, "bool vx_wp_include = vx_wp_i < vx_lane && ((vx_wp_active >> vx_wp_i) & 1ul) != 0ul;");
            line(t, "uint vx_wp_source = vx_wp_include ? vx_wp_i : vx_lane;");
            line(t, "%s vx_wp_value = simd_shuffle(%s, ushort(vx_wp_source));", ot, input);
            if (fp_value) {
                line(t, "if (vx_wp_include) {");
                t->indent++;
                line(t, "if (isnan(vx_wp_acc)) vx_wp_acc = vx_wp_value;");
                line(t, "else if (!isnan(vx_wp_value)) vx_wp_acc = vx_wp_acc %s vx_wp_value ? vx_wp_acc : vx_wp_value;", wop == 2 ? "<" : ">=");
                t->indent--;
                line(t, "}");
            } else {
                line(t, "if (vx_wp_include) vx_wp_acc = %s(vx_wp_acc, vx_wp_value);", fn[wop]);
            }
            t->indent--;
            line(t, "}");
            line(t, "%s = as_type<%s>(vx_wp_acc);", r, tn);
        } else {
            line(t, "%s = simd_prefix_exclusive_%s(vx_wo_value);", r, fn[wop]);
        }
        t->indent--;
        line(t, "}");
        return;
    }
    case 120: {                                                    // WaveActiveBit(value, op): and, or, xor
        int ok;
        int bop = (int)const_int(t, in->ops[3], &ok);
        static const char *fn[] = { "simd_and", "simd_or", "simd_xor" };
        const dxil_value *value = in->n > 2 ? val(t, in->ops[2]) : NULL;
        const dxil_type *value_type = value ? ty(t, value->type) : NULL;
        if (in->n != 4 || !ok || bop < 0 || bop > 2 || !value_type || value_type->kind != DXT_INT ||
            !wave_int_kind(value_type) || value_type->bits <= 1 || value->type != in->type) {
            err(t, "unsupported WaveActiveBit operation or type");
            return;
        }
        const char *active = wave_active_predicate(t);
        line(t, "{");
        t->indent++;
        if (bop == 0) line(t, "%s vx_wb_value = (%s) ? %s : ~%s(0);", tn, active, a[0], tn);
        else line(t, "%s vx_wb_value = (%s) ? %s : %s(0);", tn, active, a[0], tn);
        line(t, "%s = %s(vx_wb_value);", r, fn[bop]);
        t->indent--;
        line(t, "}");
        return;
    }
    case 122: {                                                    // QuadReadLaneAt(value, quadLaneID)
        const dxil_value *value = in->n > 2 ? val(t, in->ops[2]) : NULL;
        const dxil_value *lane = in->n > 3 ? val(t, in->ops[3]) : NULL;
        const dxil_type *lane_type = lane ? ty(t, lane->type) : NULL;
        if (in->n != 4 || in->result == UINT32_MAX || !value || value->type != in->type ||
            !wave_value_type_supported(t, value->type) || !lane_type || lane_type->kind != DXT_INT ||
            lane_type->bits != 32) {
            err(t, "unsupported QuadReadLaneAt operands");
            return;
        }
        line(t, "%s = quad_shuffle(%s, ushort(%s));", r, a[0], a[1]);
        return;
    }
    case 123: {
        const dxil_value *value = in->n > 2 ? val(t, in->ops[2]) : NULL;
        const dxil_value *operation = in->n > 3 ? val(t, in->ops[3]) : NULL;
        const dxil_type *operation_type = operation ? ty(t, operation->type) : NULL;
        int ok = 0;
        int64_t q = in->n > 3 ? const_int(t, in->ops[3], &ok) : -1;
        if (in->n != 4 || in->result == UINT32_MAX || !value || value->type != in->type ||
            !wave_value_type_supported(t, value->type) || !operation_type ||
            operation_type->kind != DXT_INT || operation_type->bits != 8 || !ok || q < 0 || q > 2) {
            err(t, "unsupported QuadOp selector or operands");
            return;
        }
        line(t, "%s = quad_shuffle_xor(%s, ushort(%d));", r, a[0], q == 0 ? 1 : q == 1 ? 2 : 3);
        return;
    }
    case 124: case 125: case 126: case 127: line(t, "%s = as_type<%s>(%s);", r, tn, a[0]); return;
    case 130: line(t, "%s = uint(as_type<ushort>(half(%s)));", r, a[0]); return;
    case 131: line(t, "%s = float(as_type<half>(ushort(%s)));", r, a[0]); return;
    case 135: {                                                    // WaveAllBitCount
        const dxil_value *condition = in->n == 3 ? val(t, in->ops[2]) : NULL;
        const dxil_type *condition_type = condition ? ty(t, condition->type) : NULL;
        const dxil_type *result_type = ty(t, in->type);
        if (in->n != 3 || in->result == UINT32_MAX || !condition_type ||
            condition_type->kind != DXT_INT || condition_type->bits != 1 || !result_type ||
            result_type->kind != DXT_INT || result_type->bits != 32) {
            err(t, "unsupported WaveActiveCountBits result or operands");
            return;
        }
        const char *active = wave_active_predicate(t);
        line(t, "{ ulong vx_v = ulong(simd_ballot((%s) && bool(%s))); %s = popcount(uint(vx_v)) + popcount(uint(vx_v >> 32)); }",
             active, a[0], r);
        return;
    }
    case 116: {                                                    // WaveActiveBallot
        const dxil_value *condition = in->n == 3 ? val(t, in->ops[2]) : NULL;
        const dxil_type *condition_type = condition ? ty(t, condition->type) : NULL;
        if (in->n != 3 || in->result == UINT32_MAX || !condition_type ||
            condition_type->kind != DXT_INT || condition_type->bits != 1 || !wave_mask_type(t, in->type)) {
            err(t, "unsupported WaveActiveBallot result or operands");
            return;
        }
        const char *active = wave_active_predicate(t);
        line(t, "{ ulong vx_v = ulong(simd_ballot((%s) && bool(%s))); %s = %s{ uint(vx_v), uint(vx_v >> 32), 0u, 0u }; }",
             active, a[0], r, tn);
        return;
    }
    case 136: {                                                    // WavePrefixBitCount
        const dxil_value *condition = in->n == 3 ? val(t, in->ops[2]) : NULL;
        const dxil_type *condition_type = condition ? ty(t, condition->type) : NULL;
        const dxil_type *result_type = ty(t, in->type);
        if (in->n != 3 || in->result == UINT32_MAX || !condition_type ||
            condition_type->kind != DXT_INT || condition_type->bits != 1 || !result_type ||
            result_type->kind != DXT_INT || result_type->bits != 32) {
            err(t, "unsupported WavePrefixCountBits result or operands");
            return;
        }
        const char *active = wave_active_predicate(t);
        t->need_lane = t->need_lanes = 1;
        line(t, "{");
        t->indent++;
        line(t, "ulong vx_wbc_true = ulong(simd_ballot((%s) && bool(%s)));", active, a[0]);
        line(t, "uint vx_wbc_count = 0u;");
        line(t, "for (uint vx_wbc_i = 0; vx_wbc_i < vx_lane; ++vx_wbc_i) vx_wbc_count += uint((vx_wbc_true >> vx_wbc_i) & 1ul);");
        line(t, "%s = vx_wbc_count;", r);
        t->indent--;
        line(t, "}");
        return;
    }
    case 165: {                                                    // WaveMatch(value): active-lane match mask
        const dxil_value *value = in->n > 2 ? val(t, in->ops[2]) : NULL;
        const dxil_type *value_type = value ? ty(t, value->type) : NULL;
        const dxil_type *component = value_type && value_type->kind == DXT_VECTOR ?
            ty(t, value_type->elem) : value_type;
        const dxil_type *result_type = ty(t, in->type);
        if (in->n != 3 || !component || !wave_value_type_supported(t, value->type) ||
            !result_type || !wave_mask_type(t, in->type)) {
            err(t, "unsupported WaveMatch value or result type");
            return;
        }
        char shuffle_type[32] = "";
        int bool_value = component->kind == DXT_INT && component->bits == 1;
        if (bool_value && value_type->kind == DXT_VECTOR)
            snprintf(shuffle_type, sizeof shuffle_type, "uint%u", value_type->count);
        else if (bool_value)
            snprintf(shuffle_type, sizeof shuffle_type, "uint");
        const char *active_lanes = wave_active_predicate(t);
        t->need_lane = t->need_lanes = 1;
        line(t, "{");
        t->indent++;
        line(t, "ulong vx_wm_active = ulong(simd_ballot(%s));", active_lanes);
        line(t, "uint4 vx_wm_mask = uint4(0u);");
        line(t, "for (uint vx_wm_i = 0; vx_wm_i < vx_lanes; ++vx_wm_i) {");
        t->indent++;
        line(t, "if (((vx_wm_active >> vx_wm_i) & ulong(1)) != 0ul) {");
        t->indent++;
        if (bool_value)
            line(t, "%s vx_wm_value = simd_shuffle(%s(%s), ushort(vx_wm_i));", shuffle_type, shuffle_type, a[0]);
        else
            line(t, "auto vx_wm_value = simd_shuffle(%s, ushort(vx_wm_i));", a[0]);
        if (value_type->kind == DXT_VECTOR) {
            if (bool_value) line(t, "if (vx_wm_i == vx_lane || all(vx_wm_value == %s(%s)))", shuffle_type, a[0]);
            else line(t, "if (vx_wm_i == vx_lane || all(vx_wm_value == %s))", a[0]);
        } else {
            if (bool_value) line(t, "if (vx_wm_i == vx_lane || vx_wm_value == uint(%s))", a[0]);
            else line(t, "if (vx_wm_i == vx_lane || vx_wm_value == %s)", a[0]);
        }
        t->indent++;
        line(t, "vx_wm_mask[vx_wm_i >> 5] |= 1u << (vx_wm_i & 31u);");
        t->indent--;
        t->indent--;
        line(t, "}");
        t->indent--;
        line(t, "}");
        line(t, "%s = %s{ vx_wm_mask.x, vx_wm_mask.y, vx_wm_mask.z, vx_wm_mask.w };", r, tn);
        t->indent--;
        line(t, "}");
        return;
    }
    case 166: case 167: {                                          // WaveMultiPrefixOp / WaveMultiPrefixBitCount
        int bit_count = op == 167;
        uint32_t required_operands = bit_count ? 7u : 9u;
        const dxil_value *value = in->n > 2 ? val(t, in->ops[2]) : NULL;
        const dxil_type *value_type = value ? ty(t, value->type) : NULL;
        const dxil_type *component = value_type && value_type->kind == DXT_VECTOR ?
            ty(t, value_type->elem) : value_type;
        int kind = -1, signed_kind = -1;
        int ok_kind = 0, ok_signed_kind = 0;
        if (!bit_count) {
            kind = in->n > 7 ? (int)const_int(t, in->ops[7], &ok_kind) : -1;
            signed_kind = in->n > 8 ? (int)const_int(t, in->ops[8], &ok_signed_kind) : -1;
        }
        if (in->n != required_operands || !component ||
            (bit_count ? (value_type->kind != DXT_INT || component->kind != DXT_INT || component->bits != 1 ||
                          !ty(t, in->type) || ty(t, in->type)->kind != DXT_INT || ty(t, in->type)->bits != 32) :
             (!(component->kind == DXT_FLOAT || component->kind == DXT_HALF ||
                (component->kind == DXT_INT && wave_int_kind(component))) ||
              (value_type->kind == DXT_VECTOR && (value_type->count < 2 || value_type->count > 4)) ||
              in->type != value->type || !ok_kind || !ok_signed_kind || kind < 0 || kind > 4 ||
              signed_kind < 0 || signed_kind > 1 ||
              ((component->kind == DXT_FLOAT || component->kind == DXT_HALF) && kind != 0 && kind != 4) ||
              (component->kind == DXT_INT && component->bits <= 1)))) {
            err(t, "unsupported WaveMultiPrefix type or operation");
            return;
        }
        const char *active_lanes = wave_active_predicate(t);
        t->need_lane = t->need_lanes = 1;
        line(t, "{");
        t->indent++;
        line(t, "ulong vx_mp_active = ulong(simd_ballot(%s));", active_lanes);
        line(t, "uint4 vx_mp_active_words = uint4(uint(vx_mp_active), uint(vx_mp_active >> 32), 0u, 0u);");
        if (bit_count) {
            line(t, "uint4 vx_mp_target_mask = uint4(uint(%s), uint(%s), uint(%s), uint(%s)) & vx_mp_active_words;",
                 a[1], a[2], a[3], a[4]);
            line(t, "uint vx_mp_acc = 0u;");
            line(t, "for (uint vx_mp_i = 0; vx_mp_i < vx_lanes; ++vx_mp_i) {");
        } else {
            line(t, "uint4 vx_mp_target_mask = uint4(uint(%s), uint(%s), uint(%s), uint(%s)) & vx_mp_active_words;",
                 a[1], a[2], a[3], a[4]);
            if (kind == 1) line(t, "%s vx_mp_acc = ~%s(0);", tn, tn);
            else line(t, "%s vx_mp_acc = %s(%s);", tn, tn,
                kind == 4 ? (fp ? "1.0" : "1u") : (fp ? "0.0" : "0u"));
            line(t, "for (uint vx_mp_i = 0; vx_mp_i < vx_lanes; ++vx_mp_i) {");
        }
        t->indent++;
        // Keep SIMD shuffles in a uniform loop; a lane-dependent loop bound can read inactive sources.
        line(t, "bool vx_mp_include = vx_mp_i < vx_lane && ((vx_mp_active >> vx_mp_i) & ulong(1)) != 0ul;");
        line(t, "uint vx_mp_source = vx_mp_include ? vx_mp_i : vx_lane;");
        line(t, "uint4 vx_mp_lane_mask = uint4(uint(simd_shuffle(uint(%s), ushort(vx_mp_source))), uint(simd_shuffle(uint(%s), ushort(vx_mp_source))), uint(simd_shuffle(uint(%s), ushort(vx_mp_source))), uint(simd_shuffle(uint(%s), ushort(vx_mp_source)))) & vx_mp_active_words;",
             a[1], a[2], a[3], a[4]);
        if (bit_count)
            line(t, "uint vx_mp_value = simd_shuffle(uint(%s), ushort(vx_mp_source));", a[0]);
        else
            line(t, "auto vx_mp_value = simd_shuffle(%s, ushort(vx_mp_source));", a[0]);
        line(t, "if (vx_mp_include && all(vx_mp_lane_mask == vx_mp_target_mask)) {");
        t->indent++;
        if (bit_count) {
            line(t, "vx_mp_acc += vx_mp_value != 0u ? 1u : 0u;");
        } else {
            static const char *operators[] = { "+", "&", "|", "^", "*" };
            line(t, "vx_mp_acc = vx_mp_acc %s vx_mp_value;", operators[kind]);
        }
        t->indent--;
        line(t, "}");
        t->indent--;
        line(t, "}");
        line(t, "%s = vx_mp_acc;", r);
        t->indent--;
        line(t, "}");
        return;
    }
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
    case 10: *negate = 1; return "<="; case 11: *negate = 1; return "<"; case 12: *negate = 1; return ">=";
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
        case 11: {                                                                      // bitcast
            const dxil_type *dt = ty(t, in->type);
            if (dt && dt->kind == DXT_POINTER) {                                        // (a pointer as another type's)
                char et[96];
                type_name(t, dt->elem, et, sizeof et);
                uint8_t sp = space_of(t, in->ops[0]);
                line(t, "%s = (%s %s *)(%s);", r, sp == 2 ? "threadgroup" : sp == 3 ? "constant" : sp == 4 ? "device" : "thread", et, a[0]);
                return;
            }
            line(t, "%s = as_type<%s>(%s);", r, tn, a[0]);
            return;
        }
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
            if (in->sub == 9) {
                // LLVM's unordered-or-equal predicate is true for NaN operands as well as equal values.
                // Use bitwise OR so this remains component-wise when DXIL compares floating vectors.
                line(t, "%s = (%s == %s) | (isnan(%s) | isnan(%s));", r, a[0], a[1], a[0], a[1]);
                return;
            }
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
    case DXI_SHUFFLEVEC: {
        const dxil_value *lhs = in->n > 0 ? val(t, in->ops[0]) : NULL;
        const dxil_value *rhs = in->n > 1 ? val(t, in->ops[1]) : NULL;
        const dxil_value *mask = in->n > 2 ? val(t, in->ops[2]) : NULL;
        const dxil_type *input = lhs ? ty(t, lhs->type) : NULL;
        const dxil_type *output = ty(t, in->type);
        const dxil_type *rhs_type = rhs ? ty(t, rhs->type) : NULL;
        const dxil_type *mask_type = mask ? ty(t, mask->type) : NULL;
        if (in->n != 3 || !input || input->kind != DXT_VECTOR || !rhs_type || rhs_type->kind != DXT_VECTOR ||
            rhs->type != lhs->type || !output || output->kind != DXT_VECTOR || output->elem != input->elem ||
            !output->count || !input->count || !mask_type || mask_type->kind != DXT_VECTOR ||
            mask_type->elem >= t->m->ntypes || t->m->types[mask_type->elem].kind != DXT_INT ||
            t->m->types[mask_type->elem].bits != 32 || mask_type->count != output->count) {
            err(t, "malformed or unsupported shufflevector types");
            return;
        }
        sb expression = { 0 };
        put(&expression, "%s(", tn);
        for (uint32_t lane = 0; lane < output->count; lane++) {
            uint32_t index = 0;
            int undefined = 0;
            if (!shuffle_mask_lane(t, in->ops[2], lane, &index, &undefined)) {
                free(expression.s);
                err(t, "shufflevector mask is not a constant i32 vector");
                return;
            }
            put(&expression, "%s", lane ? ", " : "");
            if (undefined || (index >= input->count && index - input->count >= input->count)) put(&expression, "0");
            else if (index < input->count) put(&expression, "%s[%u]", a[0], index);
            else put(&expression, "%s[%u]", a[1], index - input->count);
        }
        put(&expression, ")");
        line(t, "%s = %s;", r, expression.s ? expression.s : "0");
        free(expression.s);
        return;
    }
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
        { const dxil_value *gv = val(t, in->ops[1]); if (gv && gv->kind == DXV_GLOBAL) snprintf(base, sizeof base, "g%u", gv->u.index); }
        int ok;
        int64_t f0 = const_int(t, in->ops[2], &ok);
        const dxil_value *bv = val(t, in->ops[1]);
        // (a global is declared as the object; an alloca as a pointer to its object, vx_a#)
        int base_is_object = bv && bv->kind == DXV_GLOBAL;
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
    case DXI_FENCE:
        t->out->uses_thread_fence = 1;
        line(t, "atomic_thread_fence(mem_flags::mem_device | mem_flags::mem_texture | mem_flags::mem_threadgroup, memory_order_seq_cst, thread_scope_device);");
        return;
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
    if (stage == 0 && t->apply_sample_mask) {
        int found = 0;
        for (uint32_t i = 0; t->outsig && i < t->outsig->n; i++) {
            const dxil_md *e = dxil_md_op(t->m, t->outsig, i);
            if (md_i(t, e, 3, 0) != 14) continue;
            int id = md_i(t, e, 0, 0), rows = md_i(t, e, 6, 1);
            for (int row = 0; row < rows; row++)
                line(t, "vx_out.o%d_%d &= 0x%08xu;", id, row, t->sample_mask);
            found = 1;
        }
        if (!found) line(t, "vx_out.vx_sample_mask = 0x%08xu;", t->sample_mask);
    }
    line(t, t->mode ? "VX_RETURN;" : stage == 5 ? "return;" : "return vx_out;");
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
    line(t, "default: %s", t->mode ? "VX_RETURN;" : t->out->stage == 5 ? "return;" : "return vx_out;");
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
        if (post && t->tail) {                                     // (a jump to a return tail is a return: not a path)
            uint32_t k2 = 0;
            for (uint32_t i = 0; i < k; i++) if (!t->tail[s[i]]) s[k2++] = s[i];
            k = k2;
        }
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

// Where every path from a and from b meets (their nearest common post-dominator), -1 if only at the exit.
static int common_pdom(tr *t, int a, int b) {
    uint8_t *on = calloc(t->nb + 1, 1);
    for (int x = a, n = 0; x >= 0 && x < (int)t->nb && n <= (int)t->nb; x = t->ipdom[x], n++) { on[x] = 1; if (t->ipdom[x] == x) break; }
    int meet = -1;
    for (int x = b, n = 0; x >= 0 && x < (int)t->nb && n <= (int)t->nb; x = t->ipdom[x], n++) { if (on[x]) { meet = x; break; } if (t->ipdom[x] == x) break; }
    free(on);
    return meet;
}

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
                if (in_loop(t, h, s[i]) || t->tail[s[i]]) continue;  // (a return tail: a return, not an exit)
                if (exit >= 0 && exit != (int)s[i]) {
                    // several exits: the loop ends where they all meet (each other exit is emitted inside, then break)
                    int meet = common_pdom(t, exit, (int)s[i]);
                    if (meet < 0 || getenv("DXIL_NO_MULTI_EXIT")) {
                        if (getenv("DXIL_STRUCT_DEBUG")) fprintf(stderr, "struct: loop %u exits %d and %u\n", h, exit, s[i]);
                        return 0;
                    }
                    exit = meet;
                    continue;
                }
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
            if (to == t->loops[i] || (int)to == t->loop_exit[t->loops[i]]) { if (getenv("DXIL_STRUCT_DEBUG")) fprintf(stderr, "struct: %u->%u leaves outer loop\n", from, to); return -1; }
    }
    if ((int)to == stop) return 0;
    if (t->flag[to] >= 0) { line(t, "vx_f%d = true;", t->flag[to]); return 0; }
    return emit_region(t, to, stop);
}

// Emits the blocks from b up to (not including) stop, in the source's structure. -1: not structurable.
static int emit_region(tr *t, uint32_t b, int stop) {
    while (!t->failed) {
        if ((int)b == stop) return 0;
        if (b >= t->nb) return -1;
        if (t->flag[b] >= 0) { line(t, "vx_f%d = true;", t->flag[b]); return 0; }   // (a join the branch emits later)
        int header = t->loop_header_of[b] == (int)b;
        int open = 0;
        for (uint32_t i = 0; i < t->nloops; i++) if (t->loops[i] == b) open = 1;
        if (header && !open) {
            if (t->emitted[b]) { if (getenv("DXIL_STRUCT_DEBUG")) fprintf(stderr, "struct: loop %u twice\n", b); return -1; }
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
        if (t->emitted[b] && !t->tail[b]) { if (getenv("DXIL_STRUCT_DEBUG")) fprintf(stderr, "struct: block %u twice (stop %d)\n", b, stop); return -1; }
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
        // Blocks between here and the merge that several arms reach (a condition of && and ||: if (a || b && c) X;):
        // the arms set a flag where they would go to one, and each is emitted once after them, under its flag.
        uint32_t joins[16], njoins = 0;
        // (no merge: the arms end in return, break or continue, and so does each join after them)
        if (!getenv("DXIL_NO_JOINS"))
            for (uint32_t x = 0; x < t->nb && njoins < 16; x++) {
                if ((int)x == merge || x == b || t->npred[x] < 2 || t->emitted[x] || t->flag[x] >= 0) continue;
                if (merge >= 0 ? t->ipdom[x] != merge : (t->nloops && t->loops[t->nloops - 1] == x)) continue;
                if (stop >= 0 && merge < 0) continue;              // (an enclosing region's end: not across it)
                if (t->loop_header_of[x] == (int)x || !dominates(t, b, x)) continue;
                if (t->nloops && !in_loop(t, t->loops[t->nloops - 1], x)) continue;
                int inner = 0;                                     // (not inside a loop the branch is outside of)
                for (uint32_t h = 0; h < t->nb && !inner; h++)
                    if (t->loop_header_of[h] == (int)h && in_loop(t, h, x) && !in_loop(t, h, b)) inner = 1;
                if (inner) continue;
                joins[njoins++] = x;
            }
        for (uint32_t i = 1; i < njoins; i++)                      // (in reverse post order: a join may lead to a later one)
            for (uint32_t j = i; j > 0 && t->rpo_index[joins[j]] < t->rpo_index[joins[j - 1]]; j--) {
                uint32_t k = joins[j]; joins[j] = joins[j - 1]; joins[j - 1] = k;
            }
        for (uint32_t i = 0; i < njoins; i++) {
            t->flag[joins[i]] = (int)t->nflags++;
            line(t, "bool vx_f%d = false;", t->flag[joins[i]]);
        }
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
        for (uint32_t i = 0; i < njoins; i++) {
            uint32_t x = joins[i];
            int f = t->flag[x];
            t->flag[x] = -1;
            line(t, "if (vx_f%d) {", f);
            t->indent++;
            if (emit_region(t, x, merge >= 0 ? merge : stop)) return -1;
            t->indent--;
            line(t, "}");
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
            if (in->op == DXI_CAST && in->n > 0) space = space_of(t, in->ops[0]);
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

static void io_structs_kernel(tr *t, sb *s);
static void io_structs(tr *t, sb *s) {
    int stage = t->out->stage;
    if (t->mode >= 2 && t->mode <= 4) { io_structs_kernel(t, s); return; }
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
        if (!any) put(s, stage == 1 ? "    float vx_unused;\n" : "    float vx_unused [[user(VX_UNUSED)]];\n");   // (a vertex shader without inputs takes no stage_in)
        put(s, "};\n");
    }
    // outputs (a domain shader's as a vertex shader's)
    if (stage == 4) stage = 1;
    if (stage == 0 || stage == 1) {
        put(s, "struct VxOut {\n");
        int any = 0;
        int has_coverage = 0, has_position = 0;
        for (uint32_t i = 0; t->outsig && i < t->outsig->n; i++) {
            const dxil_md *e = dxil_md_op(t->m, t->outsig, i);
            int id = md_i(t, e, 0, 0), kind = md_i(t, e, 3, 0), rows = md_i(t, e, 6, 1), comp = md_i(t, e, 2, 9);
            if (stage == 0 && kind == 14) has_coverage = 1;
            if (kind == 3) has_position = 1;
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
                if (stage == 1 && t->mode == 1) put(s, "    %s o%d_%d;\n", et, id, r);   // (a kernel's: no attributes)
                else if (stage == 1) {
                    if (kind == 3) put(s, "    float4 o%d_%d [[position]];\n", id, r);
                    else if (kind == 4) put(s, "    uint o%d_%d [[render_target_array_index]];\n", id, r);
                    else if (kind == 5) put(s, "    uint o%d_%d [[viewport_array_index]];\n", id, r);
                    else put(s, "    %s o%d_%d [[user(%s)]];\n", et, id, r, sem);
                } else {
                    if (kind == 16) {
                        // (four components whatever the shader writes: Metal wants as many as the format has; D3D
                        // leaves the rest undefined)
                        char base[16];
                        snprintf(base, sizeof base, "%.*s", (int)strcspn(et, "0123456789"), et);
                        int target = md_i(t, dxil_md_op(t->m, e, 4), r, r);
                        if (t->dual_source && target == 1) put(s, "    %s4 o%d_%d [[color(0), index(1)]];\n", base, id, r);
                        else put(s, "    %s4 o%d_%d [[color(%d)]];\n", base, id, r, target);
                    }
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
        // (a vertex shader feeding a hull or geometry shader may have no SV_Position; Metal's vertex function needs one)
        if (stage == 1 && !has_position) put(s, t->mode == 1 ? "    float4 vx_pos;\n" : "    float4 vx_pos [[position]];\n");
        if (stage == 0 && t->apply_sample_mask && !has_coverage)
            put(s, "    uint vx_sample_mask [[sample_mask]];\n");
        put(s, "};\n");
    }
}

// A geometry kernel's outputs: plain fields (no attributes), recorded in outputs[] in slot order.
static void io_structs_kernel(tr *t, sb *s) {
    put(s, "struct VxOut {\n");
    int any = 0;
    for (uint32_t i = 0; t->outsig && i < t->outsig->n; i++) {
        const dxil_md *e = dxil_md_op(t->m, t->outsig, i);
        int id = md_i(t, e, 0, 0), kind = md_i(t, e, 3, 0), rows = md_i(t, e, 6, 1), comp = md_i(t, e, 2, 9);
        char et[32];
        element_type(t, e, et, sizeof et);
        for (int r = 0; r < rows; r++) {
            if (t->out->noutputs < 32) {
                dxil_msl_element *x = &t->out->outputs[t->out->noutputs++];
                snprintf(x->name, sizeof x->name, "%s", md_str(t, e, 1));
                x->index = (uint32_t)md_i(t, dxil_md_op(t->m, e, 4), r, r);
                x->kind = (uint8_t)kind; x->comp = (uint8_t)comp; x->cols = (uint8_t)md_i(t, e, 7, 4);
            }
            put(s, "    %s o%d_%d;\n", et, id, r);
            any = 1;
        }
    }
    if (!any) put(s, "    float4 vx_pos;\n");
    put(s, "};\n");
}

// One output row as a float4 of bits (what the vertex and geometry kernels write), and back.
static void slot_store(tr *t, sb *s, const dxil_md *e, int id, int r) {
    int comp = md_i(t, e, 2, 9), cols = md_i(t, e, 7, 4);
    int is_float_comp = comp == 8 || comp == 9;
    const char *v4 = is_float_comp ? "float4" : "uint4";
    static const char *const pad[5] = { "", ", 0, 0, 0", ", 0, 0", ", 0", "" };
    if (cols < 1) cols = 1;
    if (cols > 4) cols = 4;
    if (is_float_comp) put(s, "float4(float%s(vx_out.o%d_%d)%s)", cols == 1 ? "" : cols == 2 ? "2" : cols == 3 ? "3" : "4", id, r, pad[cols]);
    else put(s, "as_type<float4>(%s(uint%s(vx_out.o%d_%d)%s))", v4, cols == 1 ? "" : cols == 2 ? "2" : cols == 3 ? "3" : "4", id, r, pad[cols]);
}

static const char *PRELUDE =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VxDesc { ulong va; ulong res; uint size, stride, fmt, flags; };\n"
    "static inline float vx_desc_min_lod(device const VxDesc *d) { return (d->flags & 4u) ? as_type<float>(d->size) : 0.0f; }\n"
    "template<typename T> static inline T vx_desc_swizzle_component(vec<T, 4> value, uint selector) { "
        "switch (selector) { case 0u: return value.x; case 1u: return value.y; case 2u: return value.z; "
        "case 3u: return value.w; case 4u: return T(0); case 5u: return T(1); default: return T(0); } }\n"
    "template<typename T> static inline vec<T, 4> vx_desc_swizzle(vec<T, 4> value, device const VxDesc *d) { "
        "uint mapping = (d->flags >> 8u) & 0x1fffu; if (!mapping) return value; "
        "return vec<T, 4>(vx_desc_swizzle_component(value, mapping & 7u), "
        "vx_desc_swizzle_component(value, (mapping >> 3u) & 7u), "
        "vx_desc_swizzle_component(value, (mapping >> 6u) & 7u), "
        "vx_desc_swizzle_component(value, (mapping >> 9u) & 7u)); }\n"
    "static inline uint vx_desc_component_selector(device const VxDesc *d, uint channel) { "
        "uint mapping = (d->flags >> 8u) & 0x1fffu; return mapping ? ((mapping >> (channel * 3u)) & 7u) : channel; }\n"
    "static inline float4 vx_cb(device const VxDesc *h, uint row) { return row * 16 < h->size ? ((device const float4 *)h->va)[row] : float4(0); }\n"
    "static inline uint vx_ld(device const VxDesc *h, uint off) { return off + 4 <= h->size ? *(device const uint *)(h->va + off) : 0u; }\n"
    "static inline uint4 vx_ld4(device const VxDesc *h, uint off) { return uint4(vx_ld(h, off), vx_ld(h, off + 4), vx_ld(h, off + 8), vx_ld(h, off + 12)); }\n"
    "static inline void vx_st(device const VxDesc *h, uint off, uint v) { if (off + 4 <= h->size) *(device uint *)(h->va + off) = v; }\n"
    "static inline ushort vx_ld16(device const VxDesc *h, uint off) { return off + 2 <= h->size ? *(device const ushort *)(h->va + off) : ushort(0); }\n"
    "static inline ushort4 vx_ld16x4(device const VxDesc *h, uint off) { return ushort4(vx_ld16(h, off), vx_ld16(h, off + 2), vx_ld16(h, off + 4), vx_ld16(h, off + 6)); }\n"
    "static inline void vx_st16(device const VxDesc *h, uint off, ushort v) { if (off + 2 <= h->size) *(device ushort *)(h->va + off) = v; }\n"
    "static inline ulong vx_ld64(device const VxDesc *h, uint off) { return ulong(vx_ld(h, off)) | (ulong(vx_ld(h, off + 4)) << 32); }\n"
    "static inline ulong4 vx_ld64x4(device const VxDesc *h, uint off) { return ulong4(vx_ld64(h, off), vx_ld64(h, off + 8), vx_ld64(h, off + 16), vx_ld64(h, off + 24)); }\n"
    "static inline void vx_st64(device const VxDesc *h, uint off, ulong v) { vx_st(h, off, uint(v)); vx_st(h, off + 4, uint(v >> 32)); }\n";

static int translate(const dxil_module *m, uint32_t sample_mask, int mode, const dxil_msl *vs, dxil_msl *out, int dual_source) {
    memset(out, 0, sizeof *out);
    out->stage = (int)m->shader_kind;
    tr t = { 0 };
    t.m = m;
    t.out = out;
    t.mode = mode;
    t.dual_source = dual_source;
    t.gs_from = mode == 5 ? NULL : vs;
    t.hs_from = mode == 5 ? vs : NULL;
    t.sample_mask = sample_mask;
    t.apply_sample_mask = sample_mask != UINT32_MAX;
    const dxil_function *f = NULL;
    const dxil_named_md *ep = dxil_named(m, "dx.entryPoints");
    if (ep && ep->n) {
        const dxil_md *e = &m->md[ep->nodes[0]];
        const dxil_md *fv = dxil_md_op(m, e, 0);
        if (fv && fv->kind == DXM_VALUE) for (uint32_t i = 0; i < m->nfunctions; i++) if (m->functions[i].value == fv->value) f = &m->functions[i];
    }
    if (!f) for (uint32_t i = 0; i < m->nfunctions; i++) if (!m->functions[i].is_declaration) { f = &m->functions[i]; break; }
    if (!f) { snprintf(out->error, sizeof out->error, "no function with a body"); return -1; }
    if (mode == 1 && out->stage != 1) { snprintf(out->error, sizeof out->error, "not a vertex shader"); return -1; }
    if (mode == 2 && out->stage != 2) { snprintf(out->error, sizeof out->error, "not a geometry shader"); return -1; }
    if (out->stage == 2 && mode != 2) { snprintf(out->error, sizeof out->error, "a geometry shader needs its vertex shader (geometry pipeline)"); return -1; }
    if ((mode == 3 || mode == 4) != (out->stage == 3)) { snprintf(out->error, sizeof out->error, "a hull shader needs its vertex shader (tessellation pipeline)"); return -1; }
    if ((mode == 5) != (out->stage == 4)) { snprintf(out->error, sizeof out->error, "a domain shader needs its hull shader (tessellation pipeline)"); return -1; }
    if (out->stage < 0 || out->stage > 5) { snprintf(out->error, sizeof out->error, "shader kind %d is not done yet", out->stage); return -1; }
    read_signatures(&t);                                           // (first: the shader flags decide type names)
    if (mode == 3) t.cpsig = t.outsig;
    if (mode == 4) {                                               // the patch-constant function, writing the patch constants
        if (!t.fn) { snprintf(out->error, sizeof out->error, "a hull shader without a patch-constant function"); return -1; }
        f = t.fn;
        t.cpsig = t.outsig;
        t.outsig = t.pcsig;
    }
    t.f = f;
    t.nvals = f->nlocals;
    t.handles = calloc(t.nvals + 1, sizeof *t.handles);
    t.is_handle = calloc(t.nvals + 1, 1);
    t.ptr_space = calloc(t.nvals + 1, 1);
    t.declared_types = calloc(m->ntypes + 1, 1);
    read_resources(&t);
    analyze_uav_accesses(&t);
    if (mode >= 3) {
        // inputs: the vertex kernel's (hull) or the hull's control points (domain) of the same semantic; a domain
        // shader's patch constants likewise from the hull's
        const dxil_msl_element *from = mode == 5 ? (vs ? vs->outputs : NULL) : (vs ? vs->outputs : NULL);
        uint32_t nfrom = vs ? vs->noutputs : 0;
        memset(t.gs_slot, 0xff, sizeof t.gs_slot);
        memset(t.pc_slot, 0xff, sizeof t.pc_slot);
        for (int pass = 0; pass < (mode == 5 ? 2 : 1); pass++) {
            const dxil_md *sig = pass ? t.pcsig : t.insig;
            const dxil_msl_element *src = pass ? vs->patch : from;
            uint32_t nsrc = pass ? vs->npatch : nfrom;
            for (uint32_t i = 0; sig && i < sig->n; i++) {
                const dxil_md *e = dxil_md_op(m, sig, i);
                int id = md_i(&t, e, 0, 0), rows = md_i(&t, e, 6, 1);
                for (int r = 0; r < rows && r < 4 && id < 64; r++) {
                    uint32_t index = (uint32_t)md_i(&t, dxil_md_op(m, e, 4), r, r);
                    for (uint32_t k = 0; src && k < nsrc; k++)
                        if (!strcasecmp(src[k].name, md_str(&t, e, 1)) && src[k].index == index) {
                            if (pass) t.pc_slot[id][r] = (int16_t)k; else t.gs_slot[id][r] = (int16_t)k;
                            break;
                        }
                }
            }
        }
        if (mode == 5 && vs) {
            out->tess_domain = vs->tess_domain;
            out->tess_out_cp = vs->tess_out_cp;
        }
        if (out->tess_domain != 2 && out->tess_domain != 3) { snprintf(out->error, sizeof out->error, "isoline tessellation is not done yet"); goto done; }
        if (mode != 5 && out->tess_output != 3 && out->tess_output != 4) { snprintf(out->error, sizeof out->error, "point or line tessellation output is not done yet"); goto done; }
    }
    if (mode == 2) {
        // D3D_PRIMITIVE: 1 point, 2 line, 3 triangle, 6 line with adjacency, 7 triangle with adjacency
        out->gs_in_verts = t.gs_inprim == 1 ? 1 : t.gs_inprim == 2 ? 2 : t.gs_inprim == 3 ? 3 : t.gs_inprim == 6 ? 4 : t.gs_inprim == 7 ? 6 : 0;
        out->gs_max_tris = t.gs_maxvert > 2 ? t.gs_maxvert - 2 : 0;
        const char *why = !out->gs_in_verts ? "its input primitive" : t.gs_topology != 5 ? "an output other than triangle strips" :
                          !out->gs_max_tris ? "no triangles" : t.gs_instances > 1 ? "instancing" : NULL;
        if (why) { snprintf(out->error, sizeof out->error, "geometry shader with %s is not done yet", why); goto done; }
        // its inputs: the vertex kernel's output slots of the same semantic
        memset(t.gs_slot, 0xff, sizeof t.gs_slot);
        for (uint32_t i = 0; t.insig && i < t.insig->n; i++) {
            const dxil_md *e = dxil_md_op(m, t.insig, i);
            int id = md_i(&t, e, 0, 0), rows = md_i(&t, e, 6, 1);
            for (int r = 0; r < rows && r < 4 && id < 64; r++) {
                uint32_t index = (uint32_t)md_i(&t, dxil_md_op(m, e, 4), r, r);
                for (uint32_t k = 0; vs && k < vs->noutputs; k++)
                    if (!strcasecmp(vs->outputs[k].name, md_str(&t, e, 1)) && vs->outputs[k].index == index) { t.gs_slot[id][r] = (int16_t)k; break; }
            }
        }
    }
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
    t.npred = calloc(t.nb + 1, sizeof *t.npred);
    t.flag = malloc((t.nb + 1) * sizeof *t.flag);
    for (uint32_t b = 0; b <= t.nb; b++) t.flag[b] = -1;
    for (uint32_t b = 0; b < t.nb; b++) {
        uint32_t s[64], k = nsucc(&t, b, s);
        for (uint32_t i = 0; i < k; i++) if (s[i] < t.nb) t.npred[s[i]]++;
    }
    t.tail = calloc(t.nb + 1, 1);
    if (!getenv("DXIL_NO_TAILS"))
        for (uint32_t b = 0; b < t.nb; b++) {
            uint32_t s[64];
            if (b && nsucc(&t, b, s) == 0 && t.term[b] - t.first[b] < 8) t.tail[b] = 1;
        }
    dominators(&t, 0, t.idom);
    dominators(&t, 1, t.ipdom);
    {   // (a block that reaches the exit only through return tails, as a loop whose only way out returns: as if no tails)
        int *plain = calloc(t.nb + 1, sizeof(int)), any = 0;
        for (uint32_t b = 0; b < t.nb; b++) if (t.ipdom[b] < 0) any = 1;
        if (any) {
            uint8_t *tail = t.tail;
            t.tail = NULL;
            dominators(&t, 1, plain);
            t.tail = tail;
            for (uint32_t b = 0; b < t.nb; b++) if (t.ipdom[b] < 0) t.ipdom[b] = plain[b];
        }
        free(plain);
    }
    t.indent = 1;
    int structured = find_loops(&t) && emit_region(&t, 0, -1) == 0 && !t.failed;
    if (!structured && getenv("DXIL_STRUCT_DEBUG")) fprintf(stderr, "struct: fallback (blocks %u)\n", t.nb);
    if (!structured && !t.failed) {
        free(t.body.s);
        memset(&t.body, 0, sizeof t.body);
        memset(t.emitted, 0, t.nb);
        for (uint32_t b = 0; b <= t.nb; b++) t.flag[b] = -1;
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
        if (mode == 3 || mode == 4) {
            // a hull phase: a function in a namespace of its own (the hull kernel calls both; PRELUDE and VxRoot are
            // the kernel's)
            if (glocal.s || t.need_lane || t.need_lanes) { snprintf(out->error, sizeof out->error, "a hull shader with groupshared memory or wave operations"); t.failed = 1; }
            put(&file, "namespace %s {\n", mode == 3 ? "vx_cp" : "vx_pc");
        } else {
            put(&file, "%s", PRELUDE);
            put(&file, "struct VxRoot { device const uchar *slot[%u]; device const uchar *heap; device const uchar *samplers; };\n", out->nslots ? out->nslots : 1);
        }
        put(&file, "%s", t.prelude.s ? t.prelude.s : "");
        put(&file, "%s", t.types.s ? t.types.s : "");
        put(&file, "%s", gl.s ? gl.s : "");
        put(&file, "%s", io.s ? io.s : "");
        int stage = out->stage;
        if (mode == 3 || mode == 4) {
            int n = (int)out->noutputs;
            out->io_slots = n ? (uint32_t)n : 1;
            int ncp = 0;
            for (uint32_t i = 0; t.cpsig && i < t.cpsig->n; i++) ncp += md_i(&t, dxil_md_op(m, t.cpsig, i), 6, 1);
            put(&file, "#define VXV(k, r) vx_hs_in[(k) * vx_hp[0].z + (r)]\n");
            put(&file, "#define VXO(k, r) vx_cp_outs[(k) * %d + (r)]\n", ncp ? ncp : 1);
            put(&file, "#define VX_RETURN do {");
            uint32_t slot = 0;
            int edges = -1, inside = -1, edge_rows = 0, inside_rows = 0;
            for (uint32_t i = 0; t.outsig && i < t.outsig->n; i++) {
                const dxil_md *e = dxil_md_op(m, t.outsig, i);
                int id = md_i(&t, e, 0, 0), rows = md_i(&t, e, 6, 1), kind = md_i(&t, e, 3, 0);
                if (mode == 4 && kind == 25) { edges = id; edge_rows = rows; }
                if (mode == 4 && kind == 26) { inside = id; inside_rows = rows; }
                for (int r = 0; r < rows; r++) { put(&file, " vx_dst[%u] = ", slot++); slot_store(&t, &file, e, id, r); put(&file, ";"); }
            }
            if (mode == 4) {
                // Metal's MTLTriangleTessellationFactorsHalf {edge[3], inside} / MTLQuadTessellationFactorsHalf {edge[4],
                // inside[2]}: D3D's SV_TessFactor and SV_InsideTessFactor in the same order
                int quad = out->tess_domain == 3, ne = quad ? 4 : 3, ni = quad ? 2 : 1;
                for (int k = 0; k < ne; k++)
                    if (edges >= 0 && k < edge_rows) put(&file, " vx_f[%d] = half(vx_out.o%d_%d);", k, edges, k);
                    else put(&file, " vx_f[%d] = half(0);", k);
                for (int k = 0; k < ni; k++)
                    if (inside >= 0 && k < inside_rows) put(&file, " vx_f[%d] = half(vx_out.o%d_%d);", ne + k, inside, k);
                    else put(&file, " vx_f[%d] = half(0);", ne + k);
            }
            put(&file, " return; } while (0)\n");
            if (mode == 3)
                put(&file, "static void run(constant VxRoot &vx_root, const device float4 *vx_hs_in, constant uint4 *vx_hp, uint vx_cpid, uint vx_pid, device float4 *vx_dst");
            else
                put(&file, "static void run(constant VxRoot &vx_root, const device float4 *vx_hs_in, const device float4 *vx_cp_outs, constant uint4 *vx_hp, uint vx_pid, device float4 *vx_dst, device half *vx_f");
        } else if (mode == 5) {
            put(&file, "#define VXV(k, r) vx_cps[(k) * vx_dp.y + (r)]\n#define VX_RETURN return vx_out\n");
            put(&file, "[[patch(%s)]] vertex VxOut vx_main(uint vx_pid0 [[patch_id]], %s vx_dl [[position_in_patch]], const device float4 *vx_cpb [[buffer(%d)]], "
                       "const device float4 *vx_pcb [[buffer(%d)]], constant uint4 &vx_dp [[buffer(%d)]], constant VxRoot &vx_root [[buffer(0)]]",
                out->tess_domain == 3 ? "quad" : "triangle", out->tess_domain == 3 ? "float2" : "float3", DXIL_DS_CP, DXIL_DS_PC, DXIL_DS_PARAMS);
        } else if (mode == 1) {
            int n = (int)out->noutputs;
            out->io_slots = n ? (uint32_t)n : 1;
            put(&file, "#define VX_RETURN do { device float4 *vx_o = vx_vs_out + (vx_tid.y * vx_vp[0].x + vx_tid.x) * %u;", out->io_slots);
            uint32_t slot = 0;
            for (uint32_t i = 0; t.outsig && i < t.outsig->n; i++) {
                const dxil_md *e = dxil_md_op(m, t.outsig, i);
                int id = md_i(&t, e, 0, 0), rows = md_i(&t, e, 6, 1);
                for (int r = 0; r < rows; r++) { put(&file, " vx_o[%u] = ", slot++); slot_store(&t, &file, e, id, r); put(&file, ";"); }
            }
            put(&file, " return; } while (0)\n");
            put(&file, "kernel void vx_main(%suint2 vx_tid [[thread_position_in_grid]], device float4 *vx_vs_out [[buffer(%d)]], "
                       "constant uint4 *vx_vp [[buffer(%d)]], const device uchar *vx_ib [[buffer(%d)]], constant VxRoot &vx_root [[buffer(0)]]",
                out->ninputs ? "VxIn vx_in [[stage_in]], " : "", DXIL_GS_OUT, DXIL_VS_PARAMS, DXIL_GS_INDEX);
        } else if (mode == 2) {
            int n = (int)out->noutputs;
            out->io_slots = n ? (uint32_t)n : 1;
            put(&file, "#define NOUT %u\n#define MAXTRI %u\n", out->io_slots, out->gs_max_tris);
            put(&file, "#define VXV(k, r) vx_gs_in[(vx_first + (k)) * vx_gp[0].w + (r)]\n");
            put(&file, "#define VX_EMIT do { float4 vx_c[NOUT] = {");
            uint32_t slot = 0;
            for (uint32_t i = 0; t.outsig && i < t.outsig->n; i++) {
                const dxil_md *e = dxil_md_op(m, t.outsig, i);
                int id = md_i(&t, e, 0, 0), rows = md_i(&t, e, 6, 1);
                for (int r = 0; r < rows; r++) { put(&file, "%s ", slot++ ? "," : ""); slot_store(&t, &file, e, id, r); }
            }
            if (!slot) put(&file, " float4(0)");
            put(&file, " }; \\\n    if (vx_n >= 2 && vx_tri < MAXTRI) { device float4 *vx_t = vx_dst + vx_tri * 3 * NOUT; bool vx_odd = (vx_n & 1) != 0; \\\n"
                       "        for (int vx_k = 0; vx_k < NOUT; vx_k++) { vx_t[vx_k] = vx_odd ? vx_s2[vx_k] : vx_s1[vx_k]; vx_t[NOUT + vx_k] = vx_odd ? vx_s1[vx_k] : vx_s2[vx_k]; vx_t[2 * NOUT + vx_k] = vx_c[vx_k]; } \\\n"
                       "        vx_tri++; } \\\n"
                       "    for (int vx_k = 0; vx_k < NOUT; vx_k++) { vx_s1[vx_k] = vx_s2[vx_k]; vx_s2[vx_k] = vx_c[vx_k]; } \\\n"
                       "    vx_n++; } while (0)\n");
            put(&file, "#define VX_CUT vx_n = 0\n");
            put(&file, "#define VX_RETURN do { vx_counts[vx_tid.y * vx_gp[0].x + vx_tid.x] = vx_tri; return; } while (0)\n");
            put(&file, "kernel void vx_main(uint2 vx_tid [[thread_position_in_grid]], const device float4 *vx_gs_in [[buffer(%d)]], "
                       "device float4 *vx_gs_out [[buffer(%d)]], constant uint4 *vx_gp [[buffer(%d)]], device uint *vx_counts [[buffer(%d)]], "
                       "constant VxRoot &vx_root [[buffer(0)]]", DXIL_GS_IN, DXIL_GS_OUT, DXIL_GS_PARAMS, DXIL_GS_COUNTS);
        } else if (stage == 5) {
            put(&file, "kernel void vx_main(constant VxRoot &vx_root [[buffer(0)]]");
            if (t.need_tid) put(&file, ", uint3 vx_tid [[thread_position_in_grid]]");
            if (t.need_gid) put(&file, ", uint3 vx_gid [[threadgroup_position_in_grid]]");
            if (t.need_ltid) put(&file, ", uint3 vx_ltid [[thread_position_in_threadgroup]]");
            if (t.need_lidx) put(&file, ", uint vx_lidx [[thread_index_in_threadgroup]]");
        } else if (stage == 1) {
            put(&file, out->ninputs ? "vertex VxOut vx_main(VxIn vx_in [[stage_in]], constant VxRoot &vx_root [[buffer(0)]]" : "vertex VxOut vx_main(constant VxRoot &vx_root [[buffer(0)]]");
            if (t.need_vid) put(&file, ", uint vx_vid [[vertex_id]]");
            if (t.need_iid) put(&file, ", uint vx_iid0 [[instance_id]], uint vx_base_instance [[base_instance]]");
        } else {
            put(&file, "fragment VxOut vx_main(VxIn vx_in [[stage_in]], constant VxRoot &vx_root [[buffer(0)]]");
            if (t.need_front) put(&file, ", bool vx_front [[front_facing]]");
            if (t.need_sample_id) put(&file, ", uint vx_sample [[sample_id]]");
            if (t.need_coverage) put(&file, ", uint vx_coverage [[sample_mask]]");
            if (t.need_prim) put(&file, ", uint vx_prim [[primitive_id]]");
        }
        // (a vertex function cannot take the SIMD lane attributes: its lane among the active ones, Apple's 32 wide)
        int vertex_fn = stage == 1 && mode == 0;
        if (t.need_lane && !vertex_fn) put(&file, ", uint vx_lane [[thread_index_in_simdgroup]]");
        if (t.need_lanes && !vertex_fn) put(&file, ", uint vx_lanes [[threads_per_simdgroup]]");
        put(&file, ") {\n");
        if (t.need_lane && vertex_fn) put(&file, "    uint vx_lane = simd_prefix_exclusive_sum(1u);\n");
        if (t.need_lanes && vertex_fn) put(&file, "    uint vx_lanes = 32u;\n");
        if (stage != 5) put(&file, "    VxOut vx_out = {};\n");
        if (stage == 1 && !out->ninputs) put(&file, "    VxIn vx_in = {}; (void)vx_in;\n");
        if (mode == 5) {
            put(&file, "    uint vx_pid = vx_pid0 %% max(vx_dp.z, 1u);\n");
            put(&file, "    const device float4 *vx_cps = vx_cpb + vx_pid0 * vx_dp.x * vx_dp.y;\n");
            put(&file, "    const device float4 *vx_pc = vx_pcb + vx_pid0 * vx_dp.w; (void)vx_pc; (void)vx_pid;\n");
        }
        if (mode == 1) {
            put(&file, "    if (vx_tid.x >= vx_vp[0].x) return;\n");
            put(&file, "    uint vx_vid = vx_vp[1].x == 2 ? reinterpret_cast<const device uint *>(vx_ib)[vx_vp[1].y + vx_tid.x] + vx_vp[1].z :\n"
                       "                  vx_vp[1].x == 1 ? uint(reinterpret_cast<const device ushort *>(vx_ib)[vx_vp[1].y + vx_tid.x]) + vx_vp[1].z : vx_vp[0].y + vx_tid.x;\n");
            put(&file, "    uint vx_iid = vx_tid.y; (void)vx_vid; (void)vx_iid;\n");
        } else if (mode == 2) {
            put(&file, "    if (vx_tid.x >= vx_gp[0].x) return;\n");
            put(&file, "    uint vx_first = vx_tid.x * vx_gp[0].y + vx_tid.y * vx_gp[0].z;\n");
            put(&file, "    device float4 *vx_dst = vx_gs_out + (vx_tid.y * vx_gp[0].x + vx_tid.x) * (MAXTRI * 3 * NOUT);\n");
            put(&file, "    float4 vx_s1[NOUT], vx_s2[NOUT];\n    uint vx_n = 0, vx_tri = 0;\n");
        } else if (t.need_iid) put(&file, "    uint vx_iid = vx_iid0 - vx_base_instance;   // (D3D's SV_InstanceID leaves out StartInstanceLocation)\n");
        put(&file, "%s", glocal.s ? glocal.s : "");
        put(&file, "%s", t.decls.s ? t.decls.s : "");
        put(&file, "%s", t.body.s ? t.body.s : "");
        put(&file, "}\n");
        if (mode == 3 || mode == 4) put(&file, "#undef VXV\n#undef VXO\n#undef VX_RETURN\n}\n");
        out->msl = file.s;
        free(io.s); free(gl.s); free(glocal.s);
        if (mode == 2) {
            // the pass-through vertex function: vertex v of the triangles is NOUT float4s; a triangle past the count
            // its primitive made is degenerate (zero: clipped)
            sb pt = { 0 };
            put(&pt, "#include <metal_stdlib>\nusing namespace metal;\nstruct VxOut {\n");
            sb fill = { 0 };
            uint32_t slot = 0;
            for (uint32_t i = 0; t.outsig && i < t.outsig->n && !t.failed; i++) {
                const dxil_md *e = dxil_md_op(m, t.outsig, i);
                int id = md_i(&t, e, 0, 0), kind = md_i(&t, e, 3, 0), rows = md_i(&t, e, 6, 1), comp = md_i(&t, e, 2, 9), cols = md_i(&t, e, 7, 4);
                char et[32];
                element_type(&t, e, et, sizeof et);
                static const char *const sw[5] = { ".x", ".x", ".xy", ".xyz", "" };
                for (int r = 0; r < rows; r++, slot++) {
                    char sem[64];
                    semantic_attr(&t, e, (uint32_t)r, sem, sizeof sem);
                    if (kind == 3) put(&pt, "    float4 o%d_%d [[position]];\n", id, r);
                    else if (kind == 4) put(&pt, "    uint o%d_%d [[render_target_array_index]];\n", id, r);
                    else if (kind == 5) put(&pt, "    uint o%d_%d [[viewport_array_index]];\n", id, r);
                    else if (kind == 6 || kind == 7) { snprintf(out->error, sizeof out->error, "geometry clip or cull distances are not done yet"); t.failed = 1; break; }
                    else put(&pt, "    %s o%d_%d [[user(%s)]];\n", et, id, r, sem);
                    int f = comp == 8 || comp == 9;
                    const char *x = sw[cols < 1 ? 1 : cols > 4 ? 4 : cols];
                    if (kind == 4 || kind == 5) put(&fill, "        o.o%d_%d = as_type<uint4>(p[%u]).x;\n", id, r, slot);
                    else if (f) put(&fill, "        o.o%d_%d = %s(p[%u]%s);\n", id, r, et, slot, x);
                    else put(&fill, "        o.o%d_%d = %s(as_type<uint4>(p[%u])%s);\n", id, r, et, slot, x);
                }
            }
            put(&pt, "%s};\n", slot ? "" : "    float4 vx_pos [[position]];\n");
            put(&pt, "vertex VxOut vx_main(uint vx_vid [[vertex_id]], const device float4 *vx_v [[buffer(0)]], const device uint *vx_counts [[buffer(1)]]) {\n"
                     "    VxOut o = {};\n    if ((vx_vid / 3) %% %u < vx_counts[vx_vid / %u]) {\n        const device float4 *p = vx_v + vx_vid * %u;\n%s    }\n    return o;\n}\n",
                out->gs_max_tris, 3 * out->gs_max_tris, out->io_slots, fill.s ? fill.s : "");
            free(fill.s);
            out->msl_passthrough = pt.s;
        }
    }
done:
    free(t.body.s); free(t.decls.s); free(t.prelude.s); free(t.types.s);
    free(t.handles); free(t.is_handle); free(t.ptr_space); free(t.declared_types);
    free(t.term); free(t.first); free(t.idom); free(t.ipdom); free(t.tail); free(t.npred); free(t.flag); free(t.rpo_index); free(t.loop_header_of); free(t.loop_exit); free(t.emitted); free(t.loops);
    if (t.failed || !out->msl || (mode == 2 && !out->msl_passthrough)) {
        if (!out->error[0]) snprintf(out->error, sizeof out->error, "translation failed");
        free(out->msl); out->msl = NULL;
        free(out->msl_passthrough); out->msl_passthrough = NULL;
        return -1;
    }
    return 0;
}

int dxil_to_msl_with_sample_mask(const dxil_module *m, uint32_t sample_mask, dxil_msl *out) {
    return translate(m, sample_mask, 0, NULL, out, 0);
}
int dxil_to_msl_pixel(const dxil_module *m, uint32_t sample_mask, int dual_source, dxil_msl *out) {
    return translate(m, sample_mask, 0, NULL, out, dual_source);
}
int dxil_to_msl(const dxil_module *m, dxil_msl *out) {
    return translate(m, UINT32_MAX, 0, NULL, out, 0);
}
int dxil_to_msl_vertex_kernel(const dxil_module *m, dxil_msl *out) {
    return translate(m, UINT32_MAX, 1, NULL, out, 0);
}
int dxil_to_msl_geometry(const dxil_module *m, const dxil_msl *vs, dxil_msl *out) {
    return translate(m, UINT32_MAX, 2, vs, out, 0);
}

// The hull kernel: both phases (each translated into a namespace of its own) and the kernel calling them per patch.
int dxil_to_msl_hull(const dxil_module *m, const dxil_msl *vs, dxil_msl *out) {
    dxil_msl pc;
    if (translate(m, UINT32_MAX, 3, vs, out, 0)) return -1;
    if (translate(m, UINT32_MAX, 4, vs, &pc, 0)) {
        snprintf(out->error, sizeof out->error, "patch-constant function: %s", pc.error);
        dxil_msl_free(out);
        return -1;
    }
    out->npatch = pc.noutputs;
    memcpy(out->patch, pc.outputs, sizeof out->patch);
    if (pc.nslots > out->nslots) out->nslots = pc.nslots;
    out->uses_heap |= pc.uses_heap;
    out->uses_sampler_heap |= pc.uses_sampler_heap;
    uint32_t ncp = out->io_slots, npc = pc.io_slots, in_cp = out->tess_in_cp, out_cp = out->tess_out_cp;
    uint32_t fstride = out->tess_domain == 3 ? 6 : 4;
    sb file = { 0 };
    put(&file, "%s", PRELUDE);
    put(&file, "struct VxRoot { device const uchar *slot[%u]; device const uchar *heap; device const uchar *samplers; };\n", out->nslots ? out->nslots : 1);
    put(&file, "%s%s", out->msl, pc.msl);
    put(&file, "kernel void vx_main(uint2 vx_tid [[thread_position_in_grid]], const device float4 *vx_in [[buffer(%d)]], device float4 *vx_cp [[buffer(%d)]], "
               "device float4 *vx_pcb [[buffer(%d)]], device half *vx_factors [[buffer(%d)]], constant uint4 *vx_hp [[buffer(%d)]], "
               "constant VxRoot &vx_root [[buffer(0)]]) {\n", DXIL_HS_IN, DXIL_HS_CP, DXIL_HS_PC, DXIL_HS_FACTORS, DXIL_HS_PARAMS);
    put(&file, "    if (vx_tid.x >= vx_hp[0].x) return;\n");
    put(&file, "    uint vx_patch = vx_tid.y * vx_hp[0].x + vx_tid.x;\n");
    put(&file, "    const device float4 *vx_hs_in = vx_in + (vx_tid.y * vx_hp[0].y + vx_tid.x * %uu) * vx_hp[0].z;\n", in_cp);
    put(&file, "    device float4 *vx_cps = vx_cp + vx_patch * %uu;\n", out_cp * ncp);
    put(&file, "    for (uint vx_i = 0; vx_i < %uu; vx_i++) vx_cp::run(vx_root, vx_hs_in, vx_hp, vx_i, vx_tid.x, vx_cps + vx_i * %uu);\n", out_cp, ncp);
    put(&file, "    vx_pc::run(vx_root, vx_hs_in, vx_cps, vx_hp, vx_tid.x, vx_pcb + vx_patch * %uu, vx_factors + vx_patch * %uu);\n}\n", npc, fstride);
    free(out->msl);
    out->msl = file.s;
    dxil_msl_free(&pc);
    return 0;
}

int dxil_to_msl_domain(const dxil_module *m, const dxil_msl *hs, dxil_msl *out) {
    return translate(m, UINT32_MAX, 5, hs, out, 0);
}

void dxil_msl_free(dxil_msl *out) {
    free(out->msl); out->msl = NULL;
    free(out->msl_passthrough); out->msl_passthrough = NULL;
}
