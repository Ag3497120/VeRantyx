// DXIL reader: the DXBC-style container, the DXIL program header, and the LLVM 3.7 bitcode (bitstream, abbreviations,
// block info) read into dxil_module (dxil.h). Only what DXIL uses is kept: types, globals, functions, constants,
// instructions, module metadata and value names; debug information, attributes and use lists are skipped.
// Checked against Microsoft's dxcompiler (its own disassembly of the same containers): tests/dxil.c.
#include "dxil.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- bitstream ----------------------------------------------------------------------------------------------------

enum { ENC_LITERAL, ENC_FIXED, ENC_VBR, ENC_ARRAY, ENC_CHAR6, ENC_BLOB };
typedef struct { uint8_t enc; uint64_t val; } abbrev_op;
typedef struct { uint32_t n; abbrev_op *ops; } abbrev;
typedef struct { uint32_t id; uint32_t n; abbrev *list; } block_info;

typedef struct {
    const uint8_t *data;
    size_t bits, pos;
    block_info *info;
    uint32_t ninfo;
    char *error;
    int failed;
} reader;

typedef struct {
    reader *r;
    unsigned width;
    uint32_t id;
    size_t end;
    abbrev *abbrevs;
    uint32_t nabbrevs;
} cursor;

enum { E_END, E_BLOCK, E_RECORD, E_ERROR };
typedef struct {
    int kind;
    uint32_t code;
    uint64_t *ops;
    uint32_t n, cap;
    const uint8_t *blob;
    size_t bloblen;
    uint32_t block, block_width;
    size_t block_words;
} entry;

static int fail(reader *r, const char *fmt, ...) {
    if (!r->failed) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(r->error, 256, fmt, ap);
        va_end(ap);
        r->failed = 1;
    }
    return E_ERROR;
}

static uint64_t bits_read(reader *r, unsigned n) {
    uint64_t v = 0;
    if (n == 0) return 0;
    if (r->pos + n > r->bits) { fail(r, "bitcode ends early (bit %zu)", r->pos); r->pos = r->bits; return 0; }
    for (unsigned i = 0; i < n; ) {           // (bit by byte: up to 8 bits a step)
        size_t byte = r->pos >> 3;
        unsigned off = r->pos & 7, take = 8 - off;
        if (take > n - i) take = n - i;
        uint64_t piece = (r->data[byte] >> off) & ((1u << take) - 1);
        v |= piece << i;
        i += take;
        r->pos += take;
    }
    return v;
}

static uint64_t bits_vbr(reader *r, unsigned n) {
    uint64_t v = 0;
    unsigned shift = 0;
    uint64_t hi = 1ull << (n - 1);
    for (;;) {
        uint64_t piece = bits_read(r, n);
        v |= (piece & (hi - 1)) << shift;
        if (!(piece & hi) || r->failed) return v;
        shift += n - 1;
        if (shift > 63) { fail(r, "VBR value too long"); return 0; }
    }
}

static void bits_align32(reader *r) { r->pos = (r->pos + 31) & ~(size_t)31; if (r->pos > r->bits) r->pos = r->bits; }

static void push(entry *e, uint64_t v) {
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 64; e->ops = realloc(e->ops, e->cap * sizeof *e->ops); }
    e->ops[e->n++] = v;
}

static uint64_t read_op(reader *r, const abbrev_op *op) {
    switch (op->enc) {
    case ENC_LITERAL: return op->val;
    case ENC_FIXED: return bits_read(r, (unsigned)op->val);
    case ENC_VBR: return bits_vbr(r, (unsigned)op->val);
    case ENC_CHAR6: {
        static const char c6[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";
        return (uint8_t)c6[bits_read(r, 6)];
    }
    }
    fail(r, "bad abbreviation operand");
    return 0;
}

static block_info *info_for(reader *r, uint32_t id, int create) {
    for (uint32_t i = 0; i < r->ninfo; i++) if (r->info[i].id == id) return &r->info[i];
    if (!create) return NULL;
    r->info = realloc(r->info, (r->ninfo + 1) * sizeof *r->info);
    r->info[r->ninfo] = (block_info){ id, 0, NULL };
    return &r->info[r->ninfo++];
}

static void add_abbrev(abbrev **list, uint32_t *n, abbrev a) {
    *list = realloc(*list, (*n + 1) * sizeof **list);
    (*list)[(*n)++] = a;
}

static int define_abbrev(reader *r, abbrev *out) {
    uint32_t n = (uint32_t)bits_vbr(r, 5);
    out->n = 0;
    out->ops = calloc(n ? n : 1, sizeof *out->ops);
    for (uint32_t i = 0; i < n && !r->failed; i++) {
        abbrev_op op = { 0, 0 };
        if (bits_read(r, 1)) { op.enc = ENC_LITERAL; op.val = bits_vbr(r, 8); }
        else {
            op.enc = (uint8_t)bits_read(r, 3);
            if (op.enc == ENC_FIXED || op.enc == ENC_VBR) {
                op.val = bits_vbr(r, 5);
                if (op.val == 0) { op.enc = ENC_LITERAL; op.val = 0; }   // (LLVM reads Fixed(0)/VBR(0) as literal 0)
            } else if (op.enc != ENC_ARRAY && op.enc != ENC_CHAR6 && op.enc != ENC_BLOB) {
                return fail(r, "bad abbreviation encoding %u", op.enc);
            }
        }
        out->ops[out->n++] = op;
    }
    return r->failed ? E_ERROR : 0;
}

// The next entry of a block: its end, a sub-block (enter it with cursor_enter or skip it with cursor_skip), or a record.
// Abbreviation definitions are taken in on the way (into the block info while in the BLOCKINFO block).
static int cursor_next(cursor *c, entry *e, uint32_t *blockinfo_bid) {
    reader *r = c->r;
    for (;;) {
        if (r->failed) return E_ERROR;
        if (r->pos >= c->end) return fail(r, "block %u has no end", c->id);
        uint32_t id = (uint32_t)bits_read(r, c->width);
        if (id == 0) { bits_align32(r); return e->kind = E_END; }
        if (id == 1) {
            e->block = (uint32_t)bits_vbr(r, 8);
            e->block_width = (uint32_t)bits_vbr(r, 4);
            bits_align32(r);
            e->block_words = (size_t)bits_read(r, 32);
            return e->kind = E_BLOCK;
        }
        if (id == 2) {
            abbrev a;
            if (define_abbrev(r, &a)) return E_ERROR;
            if (c->id == 0) {
                block_info *bi = info_for(r, *blockinfo_bid, 1);
                add_abbrev(&bi->list, &bi->n, a);
            } else {
                add_abbrev(&c->abbrevs, &c->nabbrevs, a);
            }
            continue;
        }
        e->n = 0;
        e->blob = NULL;
        e->bloblen = 0;
        if (id == 3) {
            e->code = (uint32_t)bits_vbr(r, 6);
            uint32_t n = (uint32_t)bits_vbr(r, 6);
            for (uint32_t i = 0; i < n && !r->failed; i++) push(e, bits_vbr(r, 6));
            return r->failed ? E_ERROR : (e->kind = E_RECORD);
        }
        if (id - 4 >= c->nabbrevs) return fail(r, "unknown abbreviation %u in block %u", id, c->id);
        abbrev *a = &c->abbrevs[id - 4];
        for (uint32_t i = 0; i < a->n && !r->failed; i++) {
            const abbrev_op *op = &a->ops[i];
            if (op->enc == ENC_ARRAY) {
                if (i + 1 >= a->n) return fail(r, "array without an element encoding");
                uint32_t len = (uint32_t)bits_vbr(r, 6);
                for (uint32_t k = 0; k < len && !r->failed; k++) push(e, read_op(r, &a->ops[i + 1]));
                i++;
            } else if (op->enc == ENC_BLOB) {
                uint32_t len = (uint32_t)bits_vbr(r, 6);
                bits_align32(r);
                if (r->pos + (size_t)len * 8 > r->bits) return fail(r, "blob past the end");
                e->blob = r->data + r->pos / 8;
                e->bloblen = len;
                r->pos += (size_t)len * 8;
                bits_align32(r);
            } else {
                push(e, read_op(r, op));
            }
        }
        if (r->failed) return E_ERROR;
        if (e->n == 0 && !e->blob) return fail(r, "record without a code");
        e->code = (uint32_t)e->ops[0];
        memmove(e->ops, e->ops + 1, (e->n - 1) * sizeof *e->ops);
        e->n--;
        return e->kind = E_RECORD;
    }
}

static void cursor_enter(cursor *parent, entry *e, cursor *child) {
    reader *r = parent->r;
    child->r = r;
    child->width = e->block_width;
    child->id = e->block;
    child->end = r->pos + e->block_words * 32;
    if (child->end > r->bits) child->end = r->bits;
    child->abbrevs = NULL;
    child->nabbrevs = 0;
    block_info *bi = info_for(r, e->block, 0);
    if (bi) for (uint32_t i = 0; i < bi->n; i++) {
        abbrev copy = { bi->list[i].n, malloc((bi->list[i].n ? bi->list[i].n : 1) * sizeof(abbrev_op)) };
        memcpy(copy.ops, bi->list[i].ops, bi->list[i].n * sizeof(abbrev_op));
        add_abbrev(&child->abbrevs, &child->nabbrevs, copy);
    }
}

static void cursor_leave(cursor *c) {
    for (uint32_t i = 0; i < c->nabbrevs; i++) free(c->abbrevs[i].ops);
    free(c->abbrevs);
    c->abbrevs = NULL;
    c->nabbrevs = 0;
}

static void cursor_skip(cursor *c, entry *e) {
    c->r->pos += e->block_words * 32;
    if (c->r->pos > c->r->bits) c->r->pos = c->r->bits;
}

// A whole block skipped from its ENTER (nothing in it is needed).
static int skip_block(cursor *c, entry *e) { cursor_skip(c, e); return 0; }

// BLOCKINFO: SETBID (1) chooses the block the abbreviations that follow are for.
static int read_blockinfo(cursor *c) {
    entry e = { 0 };
    uint32_t bid = 0;
    int k;
    while ((k = cursor_next(c, &e, &bid)) != E_END) {
        if (k == E_ERROR) { free(e.ops); return -1; }
        if (k == E_BLOCK) { cursor_skip(c, &e); continue; }
        if (e.code == 1 && e.n >= 1) bid = (uint32_t)e.ops[0];
    }
    free(e.ops);
    return 0;
}

// ---- the module ---------------------------------------------------------------------------------------------------

typedef struct {
    dxil_module *m;
    reader *r;
    uint32_t *bodies;         // functions with a body, in order (their blocks come in that order)
    uint32_t nbodies, next_body;
    // while a function body is read
    dxil_function *f;
    uint32_t cap_locals, cap_insts;
} ctx;

static char *record_string(const entry *e, uint32_t from) {
    char *s = malloc(e->n - from + 1);
    for (uint32_t i = from; i < e->n; i++) s[i - from] = (char)e->ops[i];
    s[e->n > from ? e->n - from : 0] = 0;
    return s;
}

static uint32_t add_type(dxil_module *m) {
    m->types = realloc(m->types, (m->ntypes + 1) * sizeof *m->types);
    memset(&m->types[m->ntypes], 0, sizeof *m->types);
    return m->ntypes++;
}

static int read_types(ctx *x, cursor *c) {
    dxil_module *m = x->m;
    entry e = { 0 };
    char *next_name = NULL;
    uint32_t bid = 0;
    int k, rc = 0;
    while ((k = cursor_next(c, &e, &bid)) != E_END) {
        if (k == E_ERROR) { rc = -1; break; }
        if (k == E_BLOCK) { cursor_skip(c, &e); continue; }
        if (e.code == 1) continue;                                      // NUMENTRY
        if (e.code == 19) { free(next_name); next_name = record_string(&e, 0); continue; }   // STRUCT_NAME
        uint32_t t = add_type(m);
        dxil_type *ty = &m->types[t];
        switch (e.code) {
        case 2: ty->kind = DXT_VOID; break;
        case 3: ty->kind = DXT_FLOAT; break;
        case 4: ty->kind = DXT_DOUBLE; break;
        case 5: ty->kind = DXT_LABEL; break;
        case 6: ty->kind = DXT_OPAQUE; ty->name = next_name; next_name = NULL; break;
        case 7: ty->kind = DXT_INT; ty->bits = e.n ? (uint32_t)e.ops[0] : 0; break;
        case 8: ty->kind = DXT_POINTER; ty->elem = e.n ? (uint32_t)e.ops[0] : 0; ty->bits = e.n > 1 ? (uint32_t)e.ops[1] : 0; break;
        case 10: ty->kind = DXT_HALF; break;
        case 11: case 12:
            ty->kind = e.code == 11 ? DXT_ARRAY : DXT_VECTOR;
            ty->count = e.n ? (uint32_t)e.ops[0] : 0;
            ty->elem = e.n > 1 ? (uint32_t)e.ops[1] : 0;
            break;
        case 16: ty->kind = DXT_METADATA; break;
        case 18: case 20:                                               // STRUCT_ANON, STRUCT_NAMED
            ty->kind = DXT_STRUCT;
            ty->packed = e.n ? (uint8_t)e.ops[0] : 0;
            ty->count = e.n ? e.n - 1 : 0;
            ty->members = malloc((ty->count ? ty->count : 1) * sizeof *ty->members);
            for (uint32_t i = 0; i < ty->count; i++) ty->members[i] = (uint32_t)e.ops[i + 1];
            if (e.code == 20) { ty->name = next_name; next_name = NULL; }
            break;
        case 21:                                                        // FUNCTION [vararg, retty, paramty...]
            ty->kind = DXT_FUNCTION;
            ty->vararg = e.n ? (uint8_t)e.ops[0] : 0;
            ty->elem = e.n > 1 ? (uint32_t)e.ops[1] : 0;
            ty->count = e.n > 2 ? e.n - 2 : 0;
            ty->members = malloc((ty->count ? ty->count : 1) * sizeof *ty->members);
            for (uint32_t i = 0; i < ty->count; i++) ty->members[i] = (uint32_t)e.ops[i + 2];
            break;
        default: ty->kind = DXT_OTHER; break;
        }
    }
    free(next_name);
    free(e.ops);
    return rc;
}

// Values: the module's (globals, functions, module constants) in m->values; a function's own in f->locals.
static dxil_value *new_value(ctx *x, uint32_t *id) {
    dxil_module *m = x->m;
    if (x->f) {
        dxil_function *f = x->f;
        if (f->nlocals == x->cap_locals) { x->cap_locals = x->cap_locals ? x->cap_locals * 2 : 64; f->locals = realloc(f->locals, x->cap_locals * sizeof *f->locals); }
        *id = m->nmodule_values + f->nlocals;
        memset(&f->locals[f->nlocals], 0, sizeof *f->locals);
        return &f->locals[f->nlocals++];
    }
    m->values = realloc(m->values, (m->nmodule_values + 1) * sizeof *m->values);
    *id = m->nmodule_values;
    memset(&m->values[m->nmodule_values], 0, sizeof *m->values);
    return &m->values[m->nmodule_values++];
}

static uint32_t next_value_id(ctx *x) { return x->m->nmodule_values + (x->f ? x->f->nlocals : 0); }

static int64_t sign_rotated(uint64_t v) { return (v & 1) ? -(int64_t)(v >> 1) : (int64_t)(v >> 1); }

static double float_of(const dxil_module *m, uint32_t type, uint64_t bits) {
    uint32_t kind = type < m->ntypes ? m->types[type].kind : DXT_DOUBLE;
    if (kind == DXT_FLOAT) { uint32_t b = (uint32_t)bits; float f; memcpy(&f, &b, 4); return f; }
    if (kind == DXT_HALF) {
        uint32_t h = (uint32_t)bits & 0xFFFF, s = h >> 15, ex = (h >> 10) & 31, fr = h & 1023;
        double v = ex == 0 ? fr / 1024.0 / 16384.0 : ex == 31 ? (fr ? 0.0 / 0.0 : 1.0 / 0.0) : (1 + fr / 1024.0) * (double)(1u << ex) / 32768.0;
        return s ? -v : v;
    }
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static int read_constants(ctx *x, cursor *c) {
    dxil_module *m = x->m;
    entry e = { 0 };
    uint32_t type = 0, bid = 0, id;
    int k, rc = 0;
    while ((k = cursor_next(c, &e, &bid)) != E_END) {
        if (k == E_ERROR) { rc = -1; break; }
        if (k == E_BLOCK) { cursor_skip(c, &e); continue; }
        if (e.code == 1) { type = e.n ? (uint32_t)e.ops[0] : 0; continue; }   // SETTYPE
        dxil_value *v = new_value(x, &id);
        v->type = type;
        switch (e.code) {
        case 2: v->kind = DXV_CONST_NULL; break;
        case 3: v->kind = DXV_CONST_UNDEF; break;
        case 4: v->kind = DXV_CONST_INT; v->u.i = e.n ? sign_rotated(e.ops[0]) : 0; break;
        case 5: v->kind = DXV_CONST_INT; v->u.i = e.n ? sign_rotated(e.ops[0]) : 0; break;   // (wide: the low word)
        case 6: v->kind = DXV_CONST_FLOAT; v->u.f = e.n ? float_of(m, type, e.ops[0]) : 0; break;
        case 7:                                                         // AGGREGATE [value ids]
            v->kind = DXV_CONST_AGGREGATE;
            v->u.list.n = e.n;
            v->u.list.ids = malloc((e.n ? e.n : 1) * sizeof(uint32_t));
            for (uint32_t i = 0; i < e.n; i++) v->u.list.ids[i] = (uint32_t)e.ops[i];
            break;
        case 8: case 9: case 22:                                        // STRING, CSTRING, DATA
            v->kind = e.code == 22 ? DXV_CONST_DATA : DXV_CONST_STRING;
            v->u.list.n = e.n;
            v->data = malloc((e.n ? e.n : 1) * sizeof(uint64_t));
            memcpy(v->data, e.ops, e.n * sizeof(uint64_t));
            break;
        case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 20: {   // expressions
            v->kind = DXV_CONST_EXPR;
            uint32_t n = e.n;
            v->u.expr.ops = malloc((n ? n : 1) * sizeof(uint32_t));
            v->u.expr.n = 0;
            if (e.code == 10) {                                         // BINOP [opcode, lhs, rhs]
                v->u.expr.op = DXI_BINOP; v->u.expr.sub = e.n ? (uint32_t)e.ops[0] : 0;
                for (uint32_t i = 1; i < 3 && i < e.n; i++) v->u.expr.ops[v->u.expr.n++] = (uint32_t)e.ops[i];
            } else if (e.code == 11) {                                  // CAST [opcode, opty, op]
                v->u.expr.op = DXI_CAST; v->u.expr.sub = e.n ? (uint32_t)e.ops[0] : 0;
                if (e.n > 2) v->u.expr.ops[v->u.expr.n++] = (uint32_t)e.ops[2];
            } else if (e.code == 12 || e.code == 20) {                  // GEP [pointee type if odd, (type, value)...]
                v->u.expr.op = DXI_GEP; v->u.expr.sub = e.code == 20;
                uint32_t i = 0;
                v->u.expr.ops[v->u.expr.n++] = (e.n % 2) ? (uint32_t)e.ops[i++] : UINT32_MAX;
                for (; i + 1 < e.n; i += 2) v->u.expr.ops[v->u.expr.n++] = (uint32_t)e.ops[i + 1];
            } else {                                                    // select, element ops, cmp: values as they are
                v->u.expr.op = e.code == 13 ? DXI_SELECT : e.code == 14 ? DXI_EXTRACTELT : e.code == 15 ? DXI_INSERTELT : e.code == 16 ? DXI_SHUFFLEVEC : DXI_CMP;
                uint32_t from = e.code == 17 || e.code == 14 ? 1 : 0;   // (CMP and EXTRACTELT lead with a type)
                for (uint32_t i = from; i < e.n; i++) v->u.expr.ops[v->u.expr.n++] = (uint32_t)e.ops[i];
                if (e.code == 17 && v->u.expr.n == 3) { v->u.expr.sub = v->u.expr.ops[2]; v->u.expr.n = 2; }
            }
            break;
        }
        default: v->kind = DXV_CONST_UNDEF; break;
        }
    }
    free(e.ops);
    return rc;
}

static int read_metadata(ctx *x, cursor *c) {
    dxil_module *m = x->m;
    entry e = { 0 };
    char *name = NULL;
    uint32_t bid = 0;
    int k, rc = 0;
    while ((k = cursor_next(c, &e, &bid)) != E_END) {
        if (k == E_ERROR) { rc = -1; break; }
        if (k == E_BLOCK) { cursor_skip(c, &e); continue; }
        if (e.code == 4) { free(name); name = record_string(&e, 0); continue; }          // NAME
        if (e.code == 6 || e.code == 11) continue;                                       // KIND, ATTACHMENT
        if (e.code == 10) {                                                              // NAMED_NODE [md ids]
            m->named = realloc(m->named, (m->nnamed + 1) * sizeof *m->named);
            dxil_named_md *nm = &m->named[m->nnamed++];
            nm->name = name ? name : strdup("");
            name = NULL;
            nm->n = e.n;
            nm->nodes = malloc((e.n ? e.n : 1) * sizeof(uint32_t));
            for (uint32_t i = 0; i < e.n; i++) nm->nodes[i] = (uint32_t)e.ops[i];
            continue;
        }
        m->md = realloc(m->md, (m->nmd + 1) * sizeof *m->md);
        dxil_md *md = &m->md[m->nmd++];
        memset(md, 0, sizeof *md);
        switch (e.code) {
        case 1: md->kind = DXM_STRING; md->str = record_string(&e, 0); break;           // STRING
        case 2: md->kind = DXM_VALUE; md->type = e.n ? (uint32_t)e.ops[0] : 0; md->value = e.n > 1 ? (uint32_t)e.ops[1] : 0; break;
        case 3: case 5: case 8:                                                          // NODE, DISTINCT_NODE, OLD_NODE
            md->kind = DXM_NODE;
            md->n = e.n;
            md->ops = malloc((e.n ? e.n : 1) * sizeof(uint32_t));
            for (uint32_t i = 0; i < e.n; i++) md->ops[i] = (uint32_t)e.ops[i];
            break;
        default: md->kind = DXM_OTHER; break;                                            // (debug information: kept as a slot)
        }
    }
    free(name);
    free(e.ops);
    return rc;
}

static int read_symtab(ctx *x, cursor *c) {
    dxil_module *m = x->m;
    entry e = { 0 };
    uint32_t bid = 0;
    int k;
    while ((k = cursor_next(c, &e, &bid)) != E_END) {
        if (k == E_ERROR) { free(e.ops); return -1; }
        if (k == E_BLOCK) { cursor_skip(c, &e); continue; }
        if ((e.code == 1 || e.code == 3) && e.n >= 1) {                 // ENTRY [valueid, name...], FNENTRY [valueid, offset, name...]
            uint32_t id = (uint32_t)e.ops[0];
            char *s = record_string(&e, e.code == 3 ? 2 : 1);
            if (!x->f && id < m->nmodule_values) {
                m->values[id].name = s;
                if (m->values[id].kind == DXV_FUNCTION) m->functions[m->values[id].u.index].name = s;
            } else free(s);                                             // (a function's own names: not kept)
        }
    }
    free(e.ops);
    return 0;
}

// ---- function bodies --------------------------------------------------------------------------------------------

static const dxil_value *value(ctx *x, uint32_t id) { return dxil_value_at(x->m, x->f, id); }

static uint32_t type_of(ctx *x, uint32_t id) {
    const dxil_value *v = value(x, id);
    return v ? v->type : UINT32_MAX;
}

static uint32_t find_type(dxil_module *m, uint8_t kind, uint32_t elem, uint32_t count_or_bits) {
    for (uint32_t i = 0; i < m->ntypes; i++) {
        const dxil_type *t = &m->types[i];
        if (t->kind != kind) continue;
        if (kind == DXT_INT && t->bits == count_or_bits) return i;
        if ((kind == DXT_VECTOR || kind == DXT_ARRAY) && t->elem == elem && t->count == count_or_bits) return i;
        if (kind == DXT_POINTER && t->elem == elem && t->bits == count_or_bits) return i;
    }
    // (a type the module's table lacks, needed for a result: added, as LLVM would have it)
    uint32_t t = add_type(m);
    m->types[t].kind = kind;
    m->types[t].elem = elem;
    if (kind == DXT_INT || kind == DXT_POINTER) m->types[t].bits = count_or_bits; else m->types[t].count = count_or_bits;
    return t;
}

typedef struct { const entry *e; uint32_t i; ctx *x; int bad; } ops_cursor;

// A value and its type: relative to the next value id; a forward reference brings its type along.
static uint32_t pair(ops_cursor *o, uint32_t *type) {
    if (o->i >= o->e->n) { o->bad = 1; return 0; }
    uint32_t next = next_value_id(o->x);
    uint32_t id = next - (uint32_t)o->e->ops[o->i++];
    if (id < next) { if (type) *type = type_of(o->x, id); }
    else {
        if (o->i >= o->e->n) { o->bad = 1; return 0; }
        uint32_t t = (uint32_t)o->e->ops[o->i++];
        if (type) *type = t;
    }
    return id;
}

// A value whose type the instruction implies: relative, no type.
static uint32_t val(ops_cursor *o) {
    if (o->i >= o->e->n) { o->bad = 1; return 0; }
    return next_value_id(o->x) - (uint32_t)o->e->ops[o->i++];
}

static uint64_t lit(ops_cursor *o) {
    if (o->i >= o->e->n) { o->bad = 1; return 0; }
    return o->e->ops[o->i++];
}

static dxil_inst *new_inst(ctx *x, uint16_t op) {
    dxil_function *f = x->f;
    if (f->ninsts == x->cap_insts) { x->cap_insts = x->cap_insts ? x->cap_insts * 2 : 64; f->insts = realloc(f->insts, x->cap_insts * sizeof *f->insts); }
    dxil_inst *in = &f->insts[f->ninsts++];
    memset(in, 0, sizeof *in);
    in->op = op;
    in->result = UINT32_MAX;
    return in;
}

static void inst_ops(dxil_inst *in, uint32_t n) { in->ops = malloc((n ? n : 1) * sizeof(uint32_t)); in->n = 0; }
static void add_op(dxil_inst *in, uint32_t v) { in->ops[in->n++] = v; }

static int read_function(ctx *x, cursor *c, dxil_function *f) {
    dxil_module *m = x->m;
    x->f = f;
    x->cap_locals = x->cap_insts = 0;
    f->first_arg = m->nmodule_values;
    const dxil_type *ft = f->type < m->ntypes ? &m->types[f->type] : NULL;
    if (ft && ft->kind == DXT_POINTER && ft->elem < m->ntypes) ft = &m->types[ft->elem];
    for (uint32_t i = 0; ft && ft->kind == DXT_FUNCTION && i < ft->count; i++) {
        uint32_t id;
        dxil_value *v = new_value(x, &id);
        v->kind = DXV_ARG;
        v->type = ft->members[i];
        v->u.index = i;
    }
    entry e = { 0 };
    uint32_t bid = 0, block = 0;
    int k, rc = 0;
    uint32_t void_t = UINT32_MAX;
    for (uint32_t i = 0; i < m->ntypes; i++) if (m->types[i].kind == DXT_VOID) { void_t = i; break; }
    while ((k = cursor_next(c, &e, &bid)) != E_END) {
        if (k == E_ERROR) { rc = -1; break; }
        if (k == E_BLOCK) {
            if (e.block == 11) {
                cursor child;
                cursor_enter(c, &e, &child);
                rc = read_constants(x, &child);
                cursor_leave(&child);
                if (rc) break;
            } else cursor_skip(c, &e);                                  // (metadata attachments, use lists, names)
            continue;
        }
        ops_cursor o = { &e, 0, x, 0 };
        dxil_inst *in = NULL;
        uint32_t t1 = 0, t2 = 0;
        switch (e.code) {
        case 1: f->nblocks = e.n ? (uint32_t)e.ops[0] : 0; continue;    // DECLAREBLOCKS
        case 2: {                                                        // BINOP [lhs pair, rhs, opcode, flags?]
            in = new_inst(x, DXI_BINOP);
            inst_ops(in, 2);
            add_op(in, pair(&o, &t1));
            add_op(in, val(&o));
            in->sub = (uint16_t)lit(&o);
            if (o.i < e.n) in->flags = (uint32_t)lit(&o);
            in->type = t1;
            break;
        }
        case 3: {                                                        // CAST [op pair, destty, opcode]
            in = new_inst(x, DXI_CAST);
            inst_ops(in, 1);
            add_op(in, pair(&o, &t1));
            in->type = (uint32_t)lit(&o);
            in->sub = (uint16_t)lit(&o);
            break;
        }
        case 5: case 29: {                                               // SELECT (old: cond i1), VSELECT [t pair, f, cond pair]
            in = new_inst(x, DXI_SELECT);
            inst_ops(in, 3);
            uint32_t tv = pair(&o, &t1), fv = val(&o), cv = e.code == 29 ? pair(&o, &t2) : val(&o);
            add_op(in, cv); add_op(in, tv); add_op(in, fv);
            in->type = t1;
            break;
        }
        case 6: {                                                        // EXTRACTELT [vec pair, idx pair]
            in = new_inst(x, DXI_EXTRACTELT);
            inst_ops(in, 2);
            add_op(in, pair(&o, &t1));
            add_op(in, pair(&o, &t2));
            in->type = t1 < m->ntypes ? m->types[t1].elem : 0;
            break;
        }
        case 7: {                                                        // INSERTELT [vec pair, elt, idx pair]
            in = new_inst(x, DXI_INSERTELT);
            inst_ops(in, 3);
            add_op(in, pair(&o, &t1));
            add_op(in, val(&o));
            add_op(in, pair(&o, &t2));
            in->type = t1;
            break;
        }
        case 8: {                                                        // SHUFFLEVEC [v1 pair, v2, mask pair]
            in = new_inst(x, DXI_SHUFFLEVEC);
            inst_ops(in, 3);
            add_op(in, pair(&o, &t1));
            add_op(in, val(&o));
            add_op(in, pair(&o, &t2));
            uint32_t n = t2 < m->ntypes ? m->types[t2].count : 0;
            in->type = find_type(m, DXT_VECTOR, t1 < m->ntypes ? m->types[t1].elem : 0, n);
            break;
        }
        case 9: case 28: {                                               // CMP, CMP2 [lhs pair, rhs, predicate, flags?]
            in = new_inst(x, DXI_CMP);
            inst_ops(in, 2);
            add_op(in, pair(&o, &t1));
            add_op(in, val(&o));
            in->sub = (uint16_t)lit(&o);
            if (o.i < e.n) in->flags = (uint32_t)lit(&o);
            uint32_t i1 = find_type(m, DXT_INT, 0, 1);
            in->type = t1 < m->ntypes && m->types[t1].kind == DXT_VECTOR ? find_type(m, DXT_VECTOR, i1, m->types[t1].count) : i1;
            break;
        }
        case 10:                                                         // RET [] or [value pair]
            in = new_inst(x, DXI_RET);
            inst_ops(in, 1);
            if (e.n) add_op(in, pair(&o, NULL));
            in->type = void_t;
            break;
        case 11:                                                         // BR [block] or [true, false, cond]
            in = new_inst(x, DXI_BR);
            inst_ops(in, 3);
            add_op(in, (uint32_t)lit(&o));
            if (e.n >= 3) { add_op(in, (uint32_t)lit(&o)); add_op(in, val(&o)); }
            in->type = void_t;
            break;
        case 12: {                                                       // SWITCH [opty, cond, default, (case value id, block)...]
            if (e.n && (e.ops[0] >> 16) == 0x4B5) { fail(x->r, "switch with case ranges is not supported"); rc = -1; break; }
            in = new_inst(x, DXI_SWITCH);
            inst_ops(in, e.n);
            lit(&o);
            add_op(in, val(&o));
            add_op(in, (uint32_t)lit(&o));
            while (o.i + 1 < e.n) { add_op(in, (uint32_t)lit(&o)); add_op(in, (uint32_t)lit(&o)); }
            in->type = void_t;
            break;
        }
        case 15: in = new_inst(x, DXI_UNREACHABLE); inst_ops(in, 0); in->type = void_t; break;
        case 16: {                                                       // PHI [type, (signed relative value, block)...]
            in = new_inst(x, DXI_PHI);
            inst_ops(in, e.n);
            in->type = (uint32_t)lit(&o);
            uint32_t next = next_value_id(x);
            while (o.i + 1 < e.n) {
                int64_t rel = sign_rotated(lit(&o));
                add_op(in, (uint32_t)((int64_t)next - rel));
                add_op(in, (uint32_t)lit(&o));
            }
            break;
        }
        case 19: {                                                       // ALLOCA [instty, opty, size (absolute), align]
            in = new_inst(x, DXI_ALLOCA);
            inst_ops(in, 1);
            uint32_t ty = (uint32_t)lit(&o);
            lit(&o);
            add_op(in, (uint32_t)lit(&o));
            uint64_t align = lit(&o);
            in->flags = (uint32_t)(align & 31);
            in->type = (align >> 6) & 1 ? find_type(m, DXT_POINTER, ty, 0) : ty;   // (explicit type: the allocated one)
            break;
        }
        case 20: {                                                       // LOAD [ptr pair, ty?, align, vol]
            in = new_inst(x, DXI_LOAD);
            inst_ops(in, 1);
            add_op(in, pair(&o, &t1));
            if (o.i + 3 == e.n) in->type = (uint32_t)lit(&o);
            else in->type = t1 < m->ntypes ? m->types[t1].elem : 0;
            in->flags = (uint32_t)lit(&o);
            break;
        }
        case 26: case 27: {                                              // EXTRACTVAL [agg pair, idx...], INSERTVAL [agg pair, val pair, idx...]
            in = new_inst(x, e.code == 26 ? DXI_EXTRACTVAL : DXI_INSERTVAL);
            inst_ops(in, e.n);
            add_op(in, pair(&o, &t1));
            if (e.code == 27) add_op(in, pair(&o, &t2));
            in->nvals = in->n;
            uint32_t t = t1;
            while (o.i < e.n) {
                uint32_t idx = (uint32_t)lit(&o);
                add_op(in, idx);
                if (t < m->ntypes) t = m->types[t].kind == DXT_STRUCT ? (idx < m->types[t].count ? m->types[t].members[idx] : 0) : m->types[t].elem;
            }
            in->type = e.code == 26 ? t : t1;
            break;
        }
        case 34: {                                                       // CALL [attrs, cc, (fmf), (fnty), callee pair, args...]
            in = new_inst(x, DXI_CALL);
            inst_ops(in, e.n + 1);
            lit(&o);
            uint64_t cc = lit(&o);
            if ((cc >> 17) & 1) lit(&o);
            uint32_t fty = (cc >> 15) & 1 ? (uint32_t)lit(&o) : UINT32_MAX;
            uint32_t callee_t = 0;
            add_op(in, pair(&o, &callee_t));
            if (fty == UINT32_MAX) {
                fty = callee_t;
                if (fty < m->ntypes && m->types[fty].kind == DXT_POINTER) fty = m->types[fty].elem;
            }
            const dxil_type *ft2 = fty < m->ntypes ? &m->types[fty] : NULL;
            if (!ft2 || ft2->kind != DXT_FUNCTION) { fail(x->r, "call through a value that is not a function"); rc = -1; break; }
            for (uint32_t i = 0; i < ft2->count && !o.bad; i++) {
                uint32_t pt = ft2->members[i];
                add_op(in, pt < m->ntypes && m->types[pt].kind == DXT_LABEL ? (uint32_t)lit(&o) : val(&o));
            }
            while (ft2->vararg && o.i < e.n && !o.bad) add_op(in, pair(&o, NULL));
            in->type = ft2->elem;
            break;
        }
        case 36:                                                         // FENCE [ordering, scope]
            in = new_inst(x, DXI_FENCE);
            inst_ops(in, 0);
            in->type = void_t;
            break;
        case 38: {                                                       // ATOMICRMW [ptr pair, val, op, vol, ordering, scope]
            in = new_inst(x, DXI_ATOMICRMW);
            inst_ops(in, 2);
            add_op(in, pair(&o, &t1));
            add_op(in, val(&o));
            in->sub = (uint16_t)lit(&o);
            in->type = t1 < m->ntypes ? m->types[t1].elem : 0;
            break;
        }
        case 43: {                                                       // GEP [inbounds, source type, base pair, index pairs...]
            in = new_inst(x, DXI_GEP);
            inst_ops(in, e.n);
            in->sub = (uint16_t)lit(&o);
            uint32_t src = (uint32_t)lit(&o);
            add_op(in, src);
            uint32_t base_t = 0;
            add_op(in, pair(&o, &base_t));
            uint32_t t = src, first = 1;
            while (o.i < e.n && !o.bad) {
                uint32_t it = 0;
                uint32_t idx = pair(&o, &it);
                add_op(in, idx);
                if (first) { first = 0; continue; }                     // (the first index steps over the pointer)
                if (t < m->ntypes && m->types[t].kind == DXT_STRUCT) {
                    const dxil_value *iv = value(x, idx);
                    uint32_t k2 = iv && iv->kind == DXV_CONST_INT ? (uint32_t)iv->u.i : 0;
                    t = k2 < m->types[t].count ? m->types[t].members[k2] : 0;
                } else if (t < m->ntypes) t = m->types[t].elem;
            }
            uint32_t as = base_t < m->ntypes && m->types[base_t].kind == DXT_POINTER ? m->types[base_t].bits : 0;
            in->type = find_type(m, DXT_POINTER, t, as);
            break;
        }
        case 44: {                                                       // STORE [ptr pair, val pair, align, vol]
            in = new_inst(x, DXI_STORE);
            inst_ops(in, 2);
            add_op(in, pair(&o, &t1));
            add_op(in, pair(&o, &t2));
            in->flags = (uint32_t)lit(&o);
            in->type = void_t;
            break;
        }
        case 46: {                                                       // CMPXCHG [ptr pair, cmp pair, new, vol, ordering, scope, failure, weak]
            in = new_inst(x, DXI_CMPXCHG);
            inst_ops(in, 3);
            add_op(in, pair(&o, &t1));
            add_op(in, pair(&o, &t2));
            add_op(in, val(&o));
            uint32_t i1 = find_type(m, DXT_INT, 0, 1);
            in->type = UINT32_MAX;
            for (uint32_t i = 0; i < m->ntypes; i++) {
                const dxil_type *t = &m->types[i];
                if (t->kind == DXT_STRUCT && t->count == 2 && t->members[0] == t2 && t->members[1] == i1) { in->type = i; break; }
            }
            if (in->type == UINT32_MAX) { fail(x->r, "cmpxchg's result type is not in the module"); rc = -1; }
            break;
        }
        case 33: case 35: continue;                                      // debug locations
        default:
            fail(x->r, "instruction record %u is not supported", e.code);
            rc = -1;
            break;
        }
        if (rc) break;
        if (o.bad) { fail(x->r, "instruction record %u is short", e.code); rc = -1; break; }
        in->block = block;
        if (in->type != void_t && in->type != UINT32_MAX && !(in->type < m->ntypes && m->types[in->type].kind == DXT_VOID)) {
            uint32_t id;
            dxil_value *v = new_value(x, &id);
            v->kind = DXV_INST;
            v->type = in->type;
            v->u.index = (uint32_t)(in - f->insts);
            in = &f->insts[v->u.index];                                 // (new_value does not move insts; kept for clarity)
            in->result = id;
        }
        if (in->op == DXI_RET || in->op == DXI_BR || in->op == DXI_SWITCH || in->op == DXI_UNREACHABLE) block++;
    }
    free(e.ops);
    x->f = NULL;
    return rc;
}

static int read_module(ctx *x, cursor *c) {
    dxil_module *m = x->m;
    entry e = { 0 };
    uint32_t bid = 0;
    int k, rc = 0;
    while ((k = cursor_next(c, &e, &bid)) != E_END) {
        if (k == E_ERROR) { rc = -1; break; }
        if (k == E_BLOCK) {
            cursor child;
            cursor_enter(c, &e, &child);
            switch (e.block) {
            case 0: rc = read_blockinfo(&child); break;
            case 17: rc = read_types(x, &child); break;
            case 11: rc = read_constants(x, &child); break;
            case 15: rc = read_metadata(x, &child); break;
            case 14: rc = read_symtab(x, &child); break;
            case 12:
                if (x->next_body >= x->nbodies) { fail(x->r, "more function bodies than definitions"); rc = -1; break; }
                rc = read_function(x, &child, &m->functions[x->bodies[x->next_body++]]);
                break;
            default: cursor_leave(&child); skip_block(c, &e); continue;
            }
            cursor_leave(&child);
            if (rc) break;
            continue;
        }
        uint32_t id;
        switch (e.code) {
        case 2: free(m->triple); m->triple = record_string(&e, 0); break;
        case 3: free(m->datalayout); m->datalayout = record_string(&e, 0); break;
        case 7: {                                                        // GLOBALVAR [type, isconst|explicit<<1, init+1, linkage, ...]
            m->globals = realloc(m->globals, (m->nglobals + 1) * sizeof *m->globals);
            dxil_global *g = &m->globals[m->nglobals];
            memset(g, 0, sizeof *g);
            uint32_t flags = e.n > 1 ? (uint32_t)e.ops[1] : 0;
            g->type = e.n ? (uint32_t)e.ops[0] : 0;
            g->constant = flags & 1;
            g->addrspace = (flags & 2) ? flags >> 2 : 0;
            g->init = e.n > 2 && e.ops[2] ? (uint32_t)e.ops[2] - 1 : UINT32_MAX;
            dxil_value *v = new_value(x, &id);
            v->kind = DXV_GLOBAL;
            // (the value is a pointer to the variable's type in its address space)
            v->type = (flags & 2) ? find_type(m, DXT_POINTER, g->type, g->addrspace) : g->type;
            v->u.index = m->nglobals;
            g->value = id;
            m->nglobals++;
            break;
        }
        case 8: {                                                        // FUNCTION [type, cc, isproto, linkage, ...]
            m->functions = realloc(m->functions, (m->nfunctions + 1) * sizeof *m->functions);
            dxil_function *f = &m->functions[m->nfunctions];
            memset(f, 0, sizeof *f);
            f->type = e.n ? (uint32_t)e.ops[0] : 0;
            f->is_declaration = e.n > 2 ? (int)e.ops[2] : 1;
            dxil_value *v = new_value(x, &id);
            v->kind = DXV_FUNCTION;
            v->type = f->type;
            v->u.index = m->nfunctions;
            f->value = id;
            if (!f->is_declaration) {
                x->bodies = realloc(x->bodies, (x->nbodies + 1) * sizeof *x->bodies);
                x->bodies[x->nbodies++] = m->nfunctions;
            }
            m->nfunctions++;
            break;
        }
        default: break;
        }
    }
    free(e.ops);
    return rc;
}

// ---- the container ------------------------------------------------------------------------------------------------

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int dxil_read(const void *data, size_t size, dxil_module *m) {
    memset(m, 0, sizeof *m);
    const uint8_t *p = data, *bc = NULL;
    size_t bcsize = 0;
    if (size >= 32 && !memcmp(p, "DXBC", 4)) {
        uint32_t count = le32(p + 28);
        for (uint32_t i = 0; i < count && 32 + 4 * (size_t)i + 4 <= size; i++) {
            uint32_t off = le32(p + 32 + 4 * i);
            if ((size_t)off + 8 > size) continue;
            uint32_t len = le32(p + off + 4);
            if (memcmp(p + off, "DXIL", 4) || (size_t)off + 8 + len > size || len < 24) continue;
            const uint8_t *chunk = p + off + 8;
            uint32_t version = le32(chunk);
            m->shader_kind = version >> 16;
            m->shader_major = (version >> 4) & 15;
            m->shader_minor = version & 15;
            if (memcmp(chunk + 8, "DXIL", 4)) { snprintf(m->error, sizeof m->error, "DXIL chunk without its header"); return -1; }
            m->dxil_version = le32(chunk + 12);
            uint32_t bcoff = le32(chunk + 16), bclen = le32(chunk + 20);
            if (8 + (size_t)bcoff + bclen > len) { snprintf(m->error, sizeof m->error, "DXIL bitcode past its chunk"); return -1; }
            bc = chunk + 8 + bcoff;
            bcsize = bclen;
            break;
        }
        if (!bc) { snprintf(m->error, sizeof m->error, "no DXIL chunk in the container"); return -1; }
    } else {
        bc = p;
        bcsize = size;
    }
    if (bcsize < 4 || bc[0] != 'B' || bc[1] != 'C' || bc[2] != 0xC0 || bc[3] != 0xDE) {
        snprintf(m->error, sizeof m->error, "not LLVM bitcode");
        return -1;
    }
    reader r = { bc, bcsize * 8, 32, NULL, 0, m->error, 0 };
    cursor top = { &r, 2, UINT32_MAX, bcsize * 8, NULL, 0 };
    ctx x = { m, &r, NULL, 0, 0, NULL, 0, 0 };
    entry e = { 0 };
    uint32_t bid = 0;
    int rc = 0;
    while (r.pos + 32 <= r.bits && !r.failed) {
        int k = cursor_next(&top, &e, &bid);
        if (k == E_ERROR) { rc = -1; break; }
        if (k != E_BLOCK) continue;
        cursor child;
        cursor_enter(&top, &e, &child);
        if (e.block == 8) rc = read_module(&x, &child);
        else if (e.block == 0) rc = read_blockinfo(&child);
        else cursor_skip(&top, &e);
        cursor_leave(&child);
        if (rc || e.block == 8) break;
    }
    free(e.ops);
    for (uint32_t i = 0; i < r.ninfo; i++) {
        for (uint32_t k = 0; k < r.info[i].n; k++) free(r.info[i].list[k].ops);
        free(r.info[i].list);
    }
    free(r.info);
    free(x.bodies);
    if (r.failed || rc) { if (!m->error[0]) snprintf(m->error, sizeof m->error, "bitcode could not be read"); return -1; }
    return 0;
}

static void free_value(dxil_value *v) {
    if (v->kind == DXV_CONST_AGGREGATE) free(v->u.list.ids);
    if (v->kind == DXV_CONST_EXPR) free(v->u.expr.ops);
    free(v->data);
}

void dxil_free(dxil_module *m) {
    for (uint32_t i = 0; i < m->ntypes; i++) { free(m->types[i].members); free(m->types[i].name); }
    free(m->types);
    for (uint32_t i = 0; i < m->nmodule_values; i++) { free_value(&m->values[i]); free((char *)m->values[i].name); }
    free(m->values);
    free(m->globals);
    for (uint32_t i = 0; i < m->nfunctions; i++) {
        dxil_function *f = &m->functions[i];
        for (uint32_t k = 0; k < f->ninsts; k++) free(f->insts[k].ops);
        free(f->insts);
        for (uint32_t k = 0; k < f->nlocals; k++) free_value(&f->locals[k]);
        free(f->locals);
    }
    free(m->functions);
    for (uint32_t i = 0; i < m->nmd; i++) { free(m->md[i].ops); free(m->md[i].str); }
    free(m->md);
    for (uint32_t i = 0; i < m->nnamed; i++) { free(m->named[i].name); free(m->named[i].nodes); }
    free(m->named);
    free(m->triple);
    free(m->datalayout);
    memset(m, 0, sizeof *m);
}

// ---- helpers --------------------------------------------------------------------------------------------------------

const dxil_value *dxil_value_at(const dxil_module *m, const dxil_function *f, uint32_t id) {
    if (id < m->nmodule_values) return &m->values[id];
    if (f && id - m->nmodule_values < f->nlocals) return &f->locals[id - m->nmodule_values];
    return NULL;
}

const dxil_named_md *dxil_named(const dxil_module *m, const char *name) {
    for (uint32_t i = 0; i < m->nnamed; i++) if (!strcmp(m->named[i].name, name)) return &m->named[i];
    return NULL;
}

const dxil_md *dxil_md_op(const dxil_module *m, const dxil_md *node, uint32_t i) {
    if (!node || node->kind != DXM_NODE || i >= node->n || node->ops[i] == 0 || node->ops[i] - 1 >= m->nmd) return NULL;
    return &m->md[node->ops[i] - 1];
}

int dxil_md_int(const dxil_module *m, const dxil_md *md, int64_t *out) {
    if (!md || md->kind != DXM_VALUE || md->value >= m->nmodule_values || m->values[md->value].kind != DXV_CONST_INT) return 0;
    *out = m->values[md->value].u.i;
    return 1;
}

const char *dxil_value_name(const dxil_module *m, uint32_t id) {
    return id < m->nmodule_values ? m->values[id].name : NULL;
}

int dxil_dxop(const dxil_module *m, const dxil_function *f, const dxil_inst *in) {
    if (in->op != DXI_CALL || in->n < 2) return -1;
    const char *name = dxil_value_name(m, in->ops[0]);
    if (!name || strncmp(name, "dx.op.", 6)) return -1;
    const dxil_value *v = dxil_value_at(m, f, in->ops[1]);
    return v && v->kind == DXV_CONST_INT ? (int)v->u.i : -1;
}

// ---- dump ----------------------------------------------------------------------------------------------------------

static const char *binop_name(const dxil_module *m, const dxil_inst *in) {
    static const char *ints[] = { "add", "sub", "mul", "udiv", "sdiv", "urem", "srem", "shl", "lshr", "ashr", "and", "or", "xor" };
    static const char *fps[] = { "fadd", "fsub", "fmul", "udiv", "fdiv", "urem", "frem", "shl", "lshr", "ashr", "and", "or", "xor" };
    uint32_t t = in->type;
    if (t < m->ntypes && m->types[t].kind == DXT_VECTOR) t = m->types[t].elem;
    int fp = t < m->ntypes && (m->types[t].kind == DXT_FLOAT || m->types[t].kind == DXT_HALF || m->types[t].kind == DXT_DOUBLE);
    return in->sub < 13 ? (fp ? fps : ints)[in->sub] : "binop?";
}

static void type_text(const dxil_module *m, uint32_t t, char *out, size_t cap) {
    if (t >= m->ntypes) { snprintf(out, cap, "?"); return; }
    const dxil_type *ty = &m->types[t];
    char inner[128];
    switch (ty->kind) {
    case DXT_VOID: snprintf(out, cap, "void"); break;
    case DXT_HALF: snprintf(out, cap, "half"); break;
    case DXT_FLOAT: snprintf(out, cap, "float"); break;
    case DXT_DOUBLE: snprintf(out, cap, "double"); break;
    case DXT_INT: snprintf(out, cap, "i%u", ty->bits); break;
    case DXT_POINTER: type_text(m, ty->elem, inner, sizeof inner); snprintf(out, cap, ty->bits ? "%s addrspace(%u)*" : "%s*", inner, ty->bits); break;
    case DXT_VECTOR: type_text(m, ty->elem, inner, sizeof inner); snprintf(out, cap, "<%u x %s>", ty->count, inner); break;
    case DXT_ARRAY: type_text(m, ty->elem, inner, sizeof inner); snprintf(out, cap, "[%u x %s]", ty->count, inner); break;
    case DXT_STRUCT: snprintf(out, cap, ty->name ? "%%%s" : "{...}", ty->name); break;
    default: snprintf(out, cap, "type%u", t); break;
    }
}

static void value_text(const dxil_module *m, const dxil_function *f, uint32_t id, char *out, size_t cap) {
    const dxil_value *v = dxil_value_at(m, f, id);
    if (!v) { snprintf(out, cap, "%%v%u", id >= m->nmodule_values ? id - m->nmodule_values : id); return; }   // (forward)
    switch (v->kind) {
    case DXV_CONST_INT: snprintf(out, cap, "%lld", (long long)v->u.i); break;
    case DXV_CONST_FLOAT: snprintf(out, cap, "%g", v->u.f); break;
    case DXV_CONST_UNDEF: snprintf(out, cap, "undef"); break;
    case DXV_CONST_NULL: snprintf(out, cap, "null"); break;
    case DXV_GLOBAL: case DXV_FUNCTION: snprintf(out, cap, "@%s", v->name ? v->name : "?"); break;
    default: snprintf(out, cap, "%%v%u", id - m->nmodule_values); break;
    }
}

void dxil_dump(const dxil_module *m, void (*out)(void *, const char *), void *cx) {
    static const char *casts[] = { "trunc", "zext", "sext", "fptoui", "fptosi", "uitofp", "sitofp", "fptrunc", "fpext", "ptrtoint", "inttoptr", "bitcast", "addrspacecast" };
    static const char *names[] = { "", "ret", "br", "switch", "unreachable", "binop", "cast", "cmp", "select", "extractelement",
                                   "insertelement", "shufflevector", "extractvalue", "insertvalue", "phi", "alloca", "load", "store",
                                   "getelementptr", "call", "atomicrmw", "cmpxchg", "fence" };
    char line[1024], a[128];
    snprintf(line, sizeof line, "; shader kind %u, model %u.%u, %u types, %u module values, %u metadata, %u named",
             m->shader_kind, m->shader_major, m->shader_minor, m->ntypes, m->nmodule_values, m->nmd, m->nnamed);
    out(cx, line);
    for (uint32_t fi = 0; fi < m->nfunctions; fi++) {
        const dxil_function *f = &m->functions[fi];
        if (f->is_declaration) continue;
        snprintf(line, sizeof line, "define @%s (%u blocks)", f->name ? f->name : "?", f->nblocks);
        out(cx, line);
        uint32_t block = UINT32_MAX;
        for (uint32_t i = 0; i < f->ninsts; i++) {
            const dxil_inst *in = &f->insts[i];
            if (in->block != block) { block = in->block; snprintf(line, sizeof line, "b%u:", block); out(cx, line); }
            size_t n = 0;
            if (in->result != UINT32_MAX) n += snprintf(line + n, sizeof line - n, "  %%v%u = ", in->result - m->nmodule_values);
            else n += snprintf(line + n, sizeof line - n, "  ");
            const char *name = in->op == DXI_BINOP ? binop_name(m, in) : in->op == DXI_CAST ? (in->sub < 13 ? casts[in->sub] : "cast?")
                             : in->op == DXI_CMP ? (in->sub < 32 ? "fcmp" : "icmp") : names[in->op];
            n += snprintf(line + n, sizeof line - n, "%s", name);
            if (in->op == DXI_CMP) n += snprintf(line + n, sizeof line - n, " p%u", in->sub);
            if (in->op == DXI_CALL) {
                value_text(m, f, in->ops[0], a, sizeof a);
                n += snprintf(line + n, sizeof line - n, " %s(", a);
                for (uint32_t k = 1; k < in->n && n < sizeof line - 140; k++) {
                    value_text(m, f, in->ops[k], a, sizeof a);
                    n += snprintf(line + n, sizeof line - n, "%s%s", k > 1 ? ", " : "", a);
                }
                n += snprintf(line + n, sizeof line - n, ")");
            } else if (in->op == DXI_BR || in->op == DXI_SWITCH || in->op == DXI_PHI) {
                for (uint32_t k = 0; k < in->n && n < sizeof line - 140; k++) {
                    int is_block = (in->op == DXI_BR && k < 2 && in->n == 3) || (in->op == DXI_BR && in->n == 1) ||
                                   (in->op == DXI_SWITCH && (k == 1 || (k >= 2 && k % 2 == 1))) || (in->op == DXI_PHI && k % 2 == 1);
                    if (is_block) snprintf(a, sizeof a, "b%u", in->ops[k]); else value_text(m, f, in->ops[k], a, sizeof a);
                    n += snprintf(line + n, sizeof line - n, "%s%s", k ? ", " : " ", a);
                }
            } else {
                uint32_t from = in->op == DXI_GEP ? 1 : 0;
                for (uint32_t k = from; k < in->n && n < sizeof line - 140; k++) {
                    if ((in->op == DXI_EXTRACTVAL || in->op == DXI_INSERTVAL) && k >= in->nvals) snprintf(a, sizeof a, "%u", in->ops[k]);
                    else value_text(m, f, in->ops[k], a, sizeof a);
                    n += snprintf(line + n, sizeof line - n, "%s%s", k > from ? ", " : " ", a);
                }
            }
            if (in->result != UINT32_MAX) { type_text(m, in->type, a, sizeof a); n += snprintf(line + n, sizeof line - n, "  ; %s", a); }
            out(cx, line);
        }
    }
}
