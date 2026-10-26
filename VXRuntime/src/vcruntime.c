// vcruntime140: MSVC's C++ runtime support. C++ exceptions with the compiler's "FH3" tables
// (__CxxFrameHandler3), _CxxThrowException, and the small helpers MSVC-built programs import from it.
//
// A throw raises exception 0xE06D7363 carrying the object and its ThrowInfo. The frame handler of each function
// finds its current state (from the instruction pointer, or the state it stored in its frame), looks for a try
// block around that state with a handler whose type is among the thrown object's catchable types, copies the
// object into the catch variable, and asks RtlUnwindEx to unwind the frames below with STATUS_UNWIND_CONSOLIDATE:
// at the end of that unwind, still on the original stack, the catch block (a funclet) runs and returns where the
// function continues. Destructors run as unwind funclets, frame by frame, as the states fall.
// All tables hold image-relative offsets (RVAs).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vx.h"
#include <pthread.h>

#define CXX_EXCEPTION       0xE06D7363u
#define CXX_MAGIC           0x19930520u
#define STATUS_UNWIND_CONSOLIDATE 0x80000029u
enum { EXCEPTION_NONCONTINUABLE = 1, EXCEPTION_UNWINDING = 2, EXCEPTION_EXIT_UNWIND = 4, EXCEPTION_TARGET_UNWIND = 0x20 };
enum { ExceptionContinueSearch = 1 };

typedef struct EXCEPTION_RECORD {
    DWORD code, flags;
    struct EXCEPTION_RECORD *next;
    void *address;
    DWORD nparams, pad;
    uint64_t info[15];
} EXCEPTION_RECORD;
typedef struct { DWORD begin, end, unwind; } RUNTIME_FUNCTION;
typedef struct {
    uint64_t control_pc, image_base; RUNTIME_FUNCTION *function; uint64_t establisher_frame, target_ip;
    void *context; void *language_handler, *handler_data, *history_table; DWORD scope_index, fill0;
} DISPATCHER_CONTEXT;

// ---- the compiler's tables -------------------------------------------------------------------------------------------
typedef struct { DWORD attributes; int unwind; int forward_compat; int catchable_array; } throw_info;
typedef struct { int count; int types[]; } catchable_array;
typedef struct { DWORD properties; int type; int mdisp, pdisp, vdisp; int size; int copy; } catchable_type;
typedef struct { void *vftable; void *spare; char name[]; } type_descriptor;
typedef struct {
    DWORD magic; int max_state; int unwind_map; DWORD n_try; int try_map; DWORD n_ip; int ip_map;
    int unwind_help; int es_types; int flags;
} func_info;
typedef struct { int to_state; int action; } unwind_entry;
typedef struct { int low, high, catch_high, n_catches, handlers; } try_entry;
typedef struct { DWORD adjectives; int type; int catch_obj; int handler; int frame; } handler_entry;
typedef struct { int ip; int state; } ip_state;

#define AT(base, rva) ((void *)((base) + (uint64_t)(int64_t)(rva)))
#define HT_IS_REFERENCE 0x8
#define CT_IS_SIMPLE_TYPE 0x1
#define CT_BY_REFERENCE_ONLY 0x2
#define CT_HAS_VIRTUAL_BASE 0x4

// RtlLookupFunctionEntry and RtlUnwindEx, as the program would call them (Windows calling convention).
typedef RUNTIME_FUNCTION *(WINAPI *lookup_fn)(uint64_t, uint64_t *, void *);
typedef void (WINAPI *unwind_fn)(uint64_t, uint64_t, EXCEPTION_RECORD *, uint64_t, void *, void *);
typedef void *(WINAPI *pc_to_header_fn)(uint64_t, uint64_t *);
static lookup_fn lookup_function;
static unwind_fn rtl_unwind_ex;
static pc_to_header_fn pc_to_header;

static void __attribute__((noreturn)) terminate_now(const char *why) {
    vx_log("std::terminate: %s", why);
    fflush(stdout);
    _exit(3);
}

// ---- states -----------------------------------------------------------------------------------------------------------
static int state_from_pc(const func_info *fi, uint64_t base, uint64_t pc) {
    const ip_state *map = AT(base, fi->ip_map);
    DWORD rva = (DWORD)(pc - base);
    int state = -1;
    for (DWORD i = 0; i < fi->n_ip && (DWORD)map[i].ip <= rva; i++) state = map[i].state;
    return state;
}
// A function's state lives in its frame (UnwindHelp) while the runtime manages it, else follows the IP.
static int current_state(const func_info *fi, uint64_t base, uint64_t frame, uint64_t pc) {
    if (fi->unwind_help) {
        int stored = *(int *)(frame + (int64_t)fi->unwind_help);
        if (stored != -2) return stored;
    }
    return state_from_pc(fi, base, pc);
}
static void set_state(const func_info *fi, uint64_t frame, int state) {
    if (fi->unwind_help) *(int *)(frame + (int64_t)fi->unwind_help) = state;
}

// Runs the unwind funclets (destructors) from `state` down to `target`. For the function's own frame the state
// reached is stored (`store`); a catch funclet's states come from its IP and are not stored.
static void unwind_states(const func_info *fi, uint64_t base, uint64_t frame, int state, int target, int store) {
    const unwind_entry *map = AT(base, fi->unwind_map);
    while (state > target && state < fi->max_state) {
        int next = map[state].to_state;
        if (map[state].action) {
            if (store) set_state(fi, frame, next);            // an exception from a destructor starts here
            ((void (WINAPI *)(uint64_t, uint64_t))AT(base, map[state].action))(0, frame);
        }
        state = next;
    }
    if (store) set_state(fi, frame, state);
}

// For a frame inside a catch funclet, the function's own frame (where the funclet's variables live) and the
// try block whose handler it is.
static uint64_t function_frame(const func_info *fi, DISPATCHER_CONTEXT *dc, uint64_t frame, const try_entry **in_catch) {
    *in_catch = NULL;
    const try_entry *tries = AT(dc->image_base, fi->try_map);
    for (DWORD t = 0; t < fi->n_try; t++) {
        const handler_entry *hs = AT(dc->image_base, tries[t].handlers);
        for (int h = 0; h < tries[t].n_catches; h++) {
            uint64_t b;
            if (lookup_function((uint64_t)AT(dc->image_base, hs[h].handler), &b, NULL) == dc->function) {
                *in_catch = &tries[t];
                return *(uint64_t *)(frame + (int64_t)hs[h].frame);
            }
        }
    }
    return frame;
}

// ---- matching -------------------------------------------------------------------------------------------------------------
static const catchable_type *match(const handler_entry *h, uint64_t hbase, const throw_info *ti, uint64_t tbase) {
    const catchable_array *types = AT(tbase, ti->catchable_array);
    const type_descriptor *want = h->type ? AT(hbase, h->type) : NULL;
    for (int i = 0; i < types->count; i++) {
        const catchable_type *ct = AT(tbase, types->types[i]);
        if (!want) return ct;                                                   // catch (...)
        const type_descriptor *have = AT(tbase, ct->type);
        if (have != want && strcmp(have->name, want->name)) continue;
        if ((ct->properties & CT_BY_REFERENCE_ONLY) && !(h->adjectives & HT_IS_REFERENCE)) continue;
        return ct;
    }
    return NULL;
}
static void *adjust(void *obj, const catchable_type *ct) {
    uint8_t *p = (uint8_t *)obj + ct->mdisp;
    if (ct->pdisp >= 0) {
        uint8_t *vbtable = *(uint8_t **)((uint8_t *)obj + ct->pdisp);
        p += *(int *)(vbtable + ct->vdisp) + ct->pdisp;
    }
    return p;
}
static void build_catch_object(const handler_entry *h, uint64_t frame, void *obj, const catchable_type *ct, uint64_t tbase) {
    if (!h->catch_obj || !h->type) return;                                      // catch (...) or no variable
    void *slot = (void *)(frame + (int64_t)h->catch_obj);
    if (h->adjectives & HT_IS_REFERENCE) { *(void **)slot = adjust(obj, ct); return; }
    if (ct->properties & CT_IS_SIMPLE_TYPE) {
        memcpy(slot, obj, (size_t)ct->size);
        if (ct->size == 8 && *(void **)slot) *(void **)slot = adjust(*(void **)slot, ct);   // pointers point at the base
        return;
    }
    if (ct->copy) {
        if (ct->properties & CT_HAS_VIRTUAL_BASE)
            ((void (WINAPI *)(void *, void *, int))AT(tbase, ct->copy))(slot, adjust(obj, ct), 1);
        else
            ((void (WINAPI *)(void *, void *))AT(tbase, ct->copy))(slot, adjust(obj, ct));
    } else {
        memcpy(slot, adjust(obj, ct), (size_t)ct->size);
    }
}

// ---- the exception being handled (for rethrow and std::current_exception) ------------------------------------------------
// Records live in the catching runtime frames, deep on the stack; a record deeper than the current stack
// pointer belongs to a catch that was left by unwinding and no longer counts.
typedef struct handled {
    void *object; const throw_info *info; uint64_t info_base;
    int rethrown;
    EXCEPTION_RECORD *record;
    struct handled *next;
} handled;
static __thread handled *t_handled;
static handled *current_handled(void) {
    uint64_t sp = (uint64_t)__builtin_frame_address(0);
    while (t_handled && (uint64_t)t_handled < sp) t_handled = t_handled->next;
    return t_handled;
}
static void destroy_object(void *obj, const throw_info *ti, uint64_t base) {
    if (obj && ti && ti->unwind) ((void (WINAPI *)(void *))AT(base, ti->unwind))(obj);
}

// The STATUS_UNWIND_CONSOLIDATE callback: the frames below are unwound; finish the function's own try block,
// run the catch funclet, return where the function continues.
enum { P_CALLBACK, P_FRAME, P_FUNCINFO, P_HANDLER, P_TRY, P_BASE, P_PC, P_OBJECT, P_INFO, P_INFO_BASE, P_IN_FUNCLET, P_COUNT };
static WINAPI void *call_catch(EXCEPTION_RECORD *rec) {
    uint64_t frame = rec->info[P_FRAME], base = rec->info[P_BASE], pc = rec->info[P_PC];
    const func_info *fi = (const func_info *)rec->info[P_FUNCINFO];
    const handler_entry *h = (const handler_entry *)rec->info[P_HANDLER];
    const try_entry *t = (const try_entry *)rec->info[P_TRY];
    int in_funclet = (int)rec->info[P_IN_FUNCLET];
    if (vx_trace) vx_log("FH3 catch: frame %#llx funclet %#x try %d..%d%s", (unsigned long long)frame, h->handler, t->low, t->high, in_funclet ? " (inside a catch)" : "");
    if (in_funclet) {
        // A try block inside a catch block: its states belong to the catch funclet; the function's stored
        // state (that of the enclosing catch) stays.
        unwind_states(fi, base, frame, state_from_pc(fi, base, pc), t->low - 1, 0);
    } else {
        unwind_states(fi, base, frame, current_state(fi, base, frame, pc), t->low - 1, 1);
    }
    handled self = { (void *)rec->info[P_OBJECT], (const throw_info *)rec->info[P_INFO], rec->info[P_INFO_BASE], 0, rec, t_handled };
    t_handled = &self;
    void *continuation = ((void *(WINAPI *)(uint64_t, uint64_t))AT(base, h->handler))(frame, frame);
    t_handled = self.next;
    if (!in_funclet) set_state(fi, frame, -2);       // the IP tells the state again
    if (!self.rethrown) destroy_object(self.object, self.info, self.info_base);
    return continuation;
}

// ---- __CxxFrameHandler3 ------------------------------------------------------------------------------------------------------
static WINAPI int CxxFrameHandler3(EXCEPTION_RECORD *rec, uint64_t frame, void *ctx, DISPATCHER_CONTEXT *dc) {
    (void)ctx;
    const func_info *fi = AT(dc->image_base, *(const DWORD *)dc->handler_data);
    uint64_t base = dc->image_base;
    const try_entry *in_catch;
    uint64_t fn_frame = function_frame(fi, dc, frame, &in_catch);

    if (rec->flags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        if ((rec->flags & EXCEPTION_TARGET_UNWIND) && rec->code == STATUS_UNWIND_CONSOLIDATE) return ExceptionContinueSearch;   // call_catch does it
        if (vx_trace) vx_log("FH3 unwind: pc %#llx frame %#llx flags %#x", (unsigned long long)(dc->control_pc - base), (unsigned long long)fn_frame, rec->flags);
        // Leaving this frame: destroy what it built. A catch funclet only destroys the catch block's own
        // objects (down to its try block's last state); the function's frame is unwound on its own.
        if (in_catch) unwind_states(fi, base, fn_frame, state_from_pc(fi, base, dc->control_pc), in_catch->high, 0);
        else unwind_states(fi, base, fn_frame, current_state(fi, base, fn_frame, dc->control_pc), -1, 1);
        return ExceptionContinueSearch;
    }
    if (rec->code != CXX_EXCEPTION || rec->nparams < 3 || (rec->info[0] & ~3u) != (CXX_MAGIC & ~3u)) return ExceptionContinueSearch;
    void *obj = (void *)rec->info[1];
    const throw_info *ti = (const throw_info *)rec->info[2];
    uint64_t tbase = rec->nparams >= 4 ? rec->info[3] : 0;
    if (!ti) return ExceptionContinueSearch;

    // Inside a catch funclet only try blocks nested in that catch count, by the funclet's IP; in the function
    // itself, its current state (which, while one of its catches runs, is outside that catch's try).
    int state = in_catch ? state_from_pc(fi, base, dc->control_pc) : current_state(fi, base, fn_frame, dc->control_pc);
    if (vx_trace) vx_log("FH3 search: pc %#llx frame %#llx state %d%s", (unsigned long long)(dc->control_pc - base), (unsigned long long)fn_frame, state, in_catch ? " (in a catch)" : "");
    const try_entry *tries = AT(base, fi->try_map);
    for (DWORD i = 0; i < fi->n_try; i++) {
        const try_entry *t = &tries[i];
        if (state < t->low || state > t->high) continue;
        if (in_catch && (t->low <= in_catch->high || t->high > in_catch->catch_high)) continue;
        const handler_entry *hs = AT(base, t->handlers);
        for (int k = 0; k < t->n_catches; k++) {
            const catchable_type *ct = match(&hs[k], base, ti, tbase);
            if (!ct) continue;
            build_catch_object(&hs[k], fn_frame, obj, ct, tbase);
            EXCEPTION_RECORD unwind = { STATUS_UNWIND_CONSOLIDATE, EXCEPTION_NONCONTINUABLE, rec, rec->address, P_COUNT, 0, { 0 } };
            unwind.info[P_CALLBACK] = (uint64_t)call_catch;
            unwind.info[P_FRAME] = fn_frame;
            unwind.info[P_FUNCINFO] = (uint64_t)fi;
            unwind.info[P_HANDLER] = (uint64_t)&hs[k];
            unwind.info[P_TRY] = (uint64_t)t;
            unwind.info[P_BASE] = base;
            unwind.info[P_PC] = dc->control_pc;
            unwind.info[P_OBJECT] = (uint64_t)obj;
            unwind.info[P_INFO] = (uint64_t)ti;
            unwind.info[P_INFO_BASE] = tbase;
            unwind.info[P_IN_FUNCLET] = in_catch != NULL;
            rtl_unwind_ex(frame, dc->control_pc, &unwind, 0, NULL, dc->history_table);   // does not return
        }
    }
    return ExceptionContinueSearch;
}
// __GSHandlerCheck_EH: the same tables after a stack-cookie check (cookies are not verified here).
static WINAPI int GSHandlerCheck_EH(EXCEPTION_RECORD *rec, uint64_t frame, void *ctx, DISPATCHER_CONTEXT *dc) { return CxxFrameHandler3(rec, frame, ctx, dc); }
static WINAPI int GSHandlerCheck(EXCEPTION_RECORD *rec, uint64_t frame, void *ctx, DISPATCHER_CONTEXT *dc) { (void)rec; (void)frame; (void)ctx; (void)dc; return ExceptionContinueSearch; }
// ---- __CxxFrameHandler4 -------------------------------------------------------------------------------------------------------
// The compressed tables of Visual Studio 2019 and later ("FH4"), as found in the binaries: a header byte (1 the
// function is a catch funclet, 2 its IP-to-state map is split by code segment, 4 BBT flags follow, 8 an unwind map,
// 16 a try-block map), then 32-bit RVAs of the unwind map, the try-block map and the IP-to-state map, and for a catch
// funclet the offset in its frame where the parent function's frame is kept. Counts and most numbers are compressed:
// the low bits of the first byte give the length (x0: 1 byte, 7 bits; x01: 2 bytes, 14 bits; x011: 3 bytes; x0111:
// 4 bytes, 28 bits; 1111: 5 bytes, the 32 bits after). IP-to-state entries are deltas from the function's start and
// the state plus one. An unwind entry is (offset back to the entry of the state it falls to << 2 | type) - 0 nothing,
// 1 a destructor on an object in the frame, 2 a destructor on the object a frame slot points to, 3 an unwind funclet -
// with the action's RVA and the object's frame offset where they apply. Each table is decoded once into the FH3-like
// form below, and the FH3 logic runs over it.
static uint32_t fh4_u(const uint8_t **p) {
    const uint8_t *b = *p;
    uint32_t v;
    if (!(b[0] & 1)) { v = b[0] >> 1; *p += 1; }
    else if ((b[0] & 3) == 1) { v = (uint32_t)(b[0] | b[1] << 8) >> 2; *p += 2; }
    else if ((b[0] & 7) == 3) { v = (uint32_t)(b[0] | b[1] << 8 | b[2] << 16) >> 3; *p += 3; }
    else if ((b[0] & 15) == 7) { v = ((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24) >> 4; *p += 4; }
    else { v = (uint32_t)b[1] | (uint32_t)b[2] << 8 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 24; *p += 5; }
    return v;
}
static int32_t fh4_rva(const uint8_t **p) { int32_t v; memcpy(&v, *p, 4); *p += 4; return v; }
typedef struct { int to_state; uint8_t type; int32_t action; uint32_t object; } fh4_unwind;
typedef struct { handler_entry h; int ncont; uint64_t cont[2]; } fh4_handler;   // (h.frame unused: FH4 keeps it per funclet)
typedef struct { int low, high, catch_high, n; fh4_handler *handlers; } fh4_try;
typedef struct { uint32_t ip; int state; } fh4_ip;
typedef struct fh4_info {
    const uint8_t *key; uint64_t start;  // the table and the start of the code it covers
    int is_catch; uint32_t frame_slot;
    int nuw, ntry, nip;
    fh4_unwind *uw;
    fh4_try *tries;
    fh4_ip *ips;
    struct fh4_info *next;
} fh4_info;
static fh4_info *g_fh4;
static pthread_mutex_t g_fh4_lock = PTHREAD_MUTEX_INITIALIZER;

static fh4_info *fh4_decode(uint64_t base, const uint8_t *key, uint64_t start) {
    fh4_info *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->key = key; f->start = start;
    const uint8_t *p = key;
    uint8_t header = *p++;
    int32_t uw_rva = 0, try_rva = 0, ip_rva;
    if (header & 4) (void)fh4_u(&p);                              // BBT flags
    if (header & 8) uw_rva = fh4_rva(&p);
    if (header & 16) try_rva = fh4_rva(&p);
    ip_rva = fh4_rva(&p);
    f->is_catch = header & 1;
    if (header & 1) f->frame_slot = fh4_u(&p);
    if (uw_rva) {                                                  // the unwind map: entries found by their place
        const uint8_t *q = AT(base, uw_rva);
        f->nuw = (int)fh4_u(&q);
        f->uw = calloc((size_t)f->nuw + 1, sizeof *f->uw);
        uint32_t *at = calloc((size_t)f->nuw + 1, sizeof *at), *back = calloc((size_t)f->nuw + 1, sizeof *back);
        const uint8_t *first = q;
        for (int i = 0; i < f->nuw; i++) {
            at[i] = (uint32_t)(q - first);
            uint32_t v = fh4_u(&q);
            f->uw[i].type = v & 3;
            back[i] = v >> 2;
            if (f->uw[i].type == 1 || f->uw[i].type == 2) { f->uw[i].action = fh4_rva(&q); f->uw[i].object = fh4_u(&q); }
            else if (f->uw[i].type == 3) f->uw[i].action = fh4_rva(&q);
        }
        for (int i = 0; i < f->nuw; i++) {
            f->uw[i].to_state = -1;
            if (!back[i] || back[i] > at[i]) continue;             // (before the first entry: no state)
            for (int k = 0; k < i; k++) if (at[k] == at[i] - back[i]) { f->uw[i].to_state = k; break; }
        }
        free(at); free(back);
    }
    if (try_rva) {
        const uint8_t *q = AT(base, try_rva);
        f->ntry = (int)fh4_u(&q);
        f->tries = calloc((size_t)f->ntry + 1, sizeof *f->tries);
        for (int i = 0; i < f->ntry; i++) {
            fh4_try *t = &f->tries[i];
            t->low = (int)fh4_u(&q); t->high = (int)fh4_u(&q); t->catch_high = (int)fh4_u(&q);
            const uint8_t *h = AT(base, fh4_rva(&q));
            t->n = (int)fh4_u(&h);
            t->handlers = calloc((size_t)t->n + 1, sizeof *t->handlers);
            for (int k = 0; k < t->n; k++) {
                fh4_handler *e = &t->handlers[k];
                uint8_t hh = *h++;
                if (hh & 1) e->h.adjectives = fh4_u(&h);
                if (hh & 2) e->h.type = fh4_rva(&h);
                if (hh & 4) e->h.catch_obj = (int)fh4_u(&h);
                e->h.handler = fh4_rva(&h);
                e->ncont = (hh >> 4) & 3;
                if (e->ncont > 2) e->ncont = 2;
                for (int c = 0; c < e->ncont; c++)
                    e->cont[c] = (hh & 8) ? (uint64_t)AT(base, fh4_rva(&h)) : start + fh4_u(&h);
            }
        }
    }
    const uint8_t *q = AT(base, ip_rva);
    if (header & 2) {                                              // one map per code segment: this one's
        uint32_t n = fh4_u(&q);
        const uint8_t *map = NULL;
        for (uint32_t i = 0; i < n; i++) {
            int32_t seg = fh4_rva(&q), disp = fh4_rva(&q);
            if ((uint64_t)AT(base, seg) == start) map = AT(base, disp);
        }
        q = map;
    }
    if (q) {
        f->nip = (int)fh4_u(&q);
        f->ips = calloc((size_t)f->nip + 1, sizeof *f->ips);
        uint32_t ip = 0;
        for (int i = 0; i < f->nip; i++) { ip += fh4_u(&q); f->ips[i].ip = ip; f->ips[i].state = (int)fh4_u(&q) - 1; }
    }
    return f;
}
static fh4_info *fh4_get(DISPATCHER_CONTEXT *dc) {
    const uint8_t *key = AT(dc->image_base, *(const DWORD *)dc->handler_data);
    uint64_t start = dc->image_base + dc->function->begin;
    pthread_mutex_lock(&g_fh4_lock);
    fh4_info *f = g_fh4;
    while (f && (f->key != key || f->start != start)) f = f->next;
    if (!f && (f = fh4_decode(dc->image_base, key, start))) { f->next = g_fh4; g_fh4 = f; }
    pthread_mutex_unlock(&g_fh4_lock);
    return f;
}
static int fh4_state(const fh4_info *f, uint64_t pc) {
    uint64_t at = pc - f->start;
    int state = -1;
    for (int i = 0; i < f->nip && f->ips[i].ip <= at; i++) state = f->ips[i].state;
    return state;
}
static void fh4_unwind_states(const fh4_info *f, uint64_t base, uint64_t frame, int state, int target) {
    while (state > target && state < f->nuw) {
        const fh4_unwind *u = &f->uw[state];
        if (u->type == 1) ((void (WINAPI *)(void *))AT(base, u->action))((void *)(frame + u->object));
        else if (u->type == 2) ((void (WINAPI *)(void *))AT(base, u->action))(*(void **)(frame + u->object));
        else if (u->type == 3) ((void (WINAPI *)(uint64_t, uint64_t))AT(base, u->action))(0, frame);
        state = u->to_state;
    }
}
enum { P4_HANDLER = P_HANDLER, P4_TRY = P_TRY };
static WINAPI void *call_catch4(EXCEPTION_RECORD *rec) {
    uint64_t frame = rec->info[P_FRAME], base = rec->info[P_BASE], pc = rec->info[P_PC];
    const fh4_info *f = (const fh4_info *)rec->info[P_FUNCINFO];
    const fh4_handler *h = (const fh4_handler *)rec->info[P4_HANDLER];
    const fh4_try *t = (const fh4_try *)rec->info[P4_TRY];
    if (vx_trace) vx_log("FH4 catch: frame %#llx funclet %#x try %d..%d", (unsigned long long)frame, h->h.handler, t->low, t->high);
    fh4_unwind_states(f, base, frame, fh4_state(f, pc), t->low - 1);
    handled self = { (void *)rec->info[P_OBJECT], (const throw_info *)rec->info[P_INFO], rec->info[P_INFO_BASE], 0, rec, t_handled };
    t_handled = &self;
    uint64_t r = (uint64_t)((void *(WINAPI *)(uint64_t, uint64_t))AT(base, h->h.handler))(frame, frame);
    t_handled = self.next;
    if (!self.rethrown) destroy_object(self.object, self.info, self.info_base);
    // (with continuation addresses in the table, the funclet returns which of them)
    if (h->ncont && r < (uint64_t)h->ncont) r = h->cont[r];
    return (void *)r;
}
static WINAPI int CxxFrameHandler4(EXCEPTION_RECORD *rec, uint64_t frame, void *ctx, DISPATCHER_CONTEXT *dc) {
    (void)ctx;
    fh4_info *f = fh4_get(dc);
    if (!f) return ExceptionContinueSearch;
    uint64_t base = dc->image_base, fn_frame = frame;
    const fh4_try *in_catch = NULL;
    if (f->is_catch) {                                             // a catch funclet: its function's frame, its try block
        fn_frame = *(uint64_t *)(frame + f->frame_slot);
        for (int i = 0; i < f->ntry && !in_catch; i++)
            for (int k = 0; k < f->tries[i].n; k++)
                if ((uint32_t)f->tries[i].handlers[k].h.handler == dc->function->begin) { in_catch = &f->tries[i]; break; }
    }
    int state = fh4_state(f, dc->control_pc);
    if (rec->flags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        if ((rec->flags & EXCEPTION_TARGET_UNWIND) && rec->code == STATUS_UNWIND_CONSOLIDATE) return ExceptionContinueSearch;
        if (vx_trace) vx_log("FH4 unwind: pc %#llx state %d", (unsigned long long)(dc->control_pc - base), state);
        fh4_unwind_states(f, base, fn_frame, state, in_catch ? in_catch->high : -1);
        return ExceptionContinueSearch;
    }
    if (rec->code != CXX_EXCEPTION || rec->nparams < 3 || (rec->info[0] & ~3u) != (CXX_MAGIC & ~3u)) return ExceptionContinueSearch;
    void *obj = (void *)rec->info[1];
    const throw_info *ti = (const throw_info *)rec->info[2];
    uint64_t tbase = rec->nparams >= 4 ? rec->info[3] : 0;
    if (!ti) return ExceptionContinueSearch;
    if (vx_trace) vx_log("FH4 search: pc %#llx state %d%s", (unsigned long long)(dc->control_pc - base), state, in_catch ? " (in a catch)" : "");
    for (int i = 0; i < f->ntry; i++) {
        const fh4_try *t = &f->tries[i];
        if (state < t->low || state > t->high) continue;
        if (in_catch && (t->low <= in_catch->high || t->high > in_catch->catch_high)) continue;
        for (int k = 0; k < t->n; k++) {
            const catchable_type *ct = match(&t->handlers[k].h, base, ti, tbase);
            if (!ct) continue;
            build_catch_object(&t->handlers[k].h, fn_frame, obj, ct, tbase);
            EXCEPTION_RECORD unwind = { STATUS_UNWIND_CONSOLIDATE, EXCEPTION_NONCONTINUABLE, rec, rec->address, P_COUNT, 0, { 0 } };
            unwind.info[P_CALLBACK] = (uint64_t)call_catch4;
            unwind.info[P_FRAME] = fn_frame;
            unwind.info[P_FUNCINFO] = (uint64_t)f;
            unwind.info[P4_HANDLER] = (uint64_t)&t->handlers[k];
            unwind.info[P4_TRY] = (uint64_t)t;
            unwind.info[P_BASE] = base;
            unwind.info[P_PC] = dc->control_pc;
            unwind.info[P_OBJECT] = (uint64_t)obj;
            unwind.info[P_INFO] = (uint64_t)ti;
            unwind.info[P_INFO_BASE] = tbase;
            unwind.info[P_IN_FUNCLET] = in_catch != NULL;
            rtl_unwind_ex(frame, dc->control_pc, &unwind, 0, NULL, dc->history_table);   // does not return
        }
    }
    return ExceptionContinueSearch;
}
static WINAPI int GSHandlerCheck_EH4(EXCEPTION_RECORD *rec, uint64_t frame, void *ctx, DISPATCHER_CONTEXT *dc) { return CxxFrameHandler4(rec, frame, ctx, dc); }

// ---- throwing ------------------------------------------------------------------------------------------------------------------
void vx_raise(DWORD code, DWORD flags, DWORD n, const uint64_t *args, void *ctx) __attribute__((noreturn));
// Called by _CxxThrowException (context.S) with the thrower's context. Without an object it rethrows the
// exception being handled.
void __attribute__((noreturn)) vx_cxx_throw(void *obj, const throw_info *ti, void *ctx) {
    uint64_t args[4] = { CXX_MAGIC, (uint64_t)obj, (uint64_t)ti, 0 };
    if (!obj && !ti) {
        handled *h = current_handled();
        if (!h) terminate_now("throw; with no exception being handled");
        h->rethrown = 1;
        args[1] = (uint64_t)h->object; args[2] = (uint64_t)h->info; args[3] = h->info_base;
    } else {
        pc_to_header((uint64_t)ti, &args[3]);
    }
    vx_raise(CXX_EXCEPTION, EXCEPTION_NONCONTINUABLE, 4, args, ctx);
}

// ---- helpers ---------------------------------------------------------------------------------------------------------------------
static WINAPI void std_terminate(void) { terminate_now("called"); }
// _set_purecall_handler: a call of a pure virtual function goes to the program's handler first, if it set one.
typedef void (WINAPI *purecall_handler)(void);
static purecall_handler g_purecall_handler;
static WINAPI purecall_handler set_purecall_handler(purecall_handler h) { return __atomic_exchange_n(&g_purecall_handler, h, __ATOMIC_ACQ_REL); }
static WINAPI purecall_handler get_purecall_handler(void) { return __atomic_load_n(&g_purecall_handler, __ATOMIC_ACQUIRE); }
static WINAPI int purecall(void) {
    purecall_handler h = get_purecall_handler();
    if (h) h();
    terminate_now("pure virtual function call");
}
// __std_exception_data: the message and whether this copy owns it.
typedef struct { const char *what; uint8_t do_free; } exception_data;
static WINAPI void std_exception_copy(const exception_data *from, exception_data *to) {
    if (from->do_free && from->what) { to->what = strdup(from->what); to->do_free = 1; }
    else { to->what = from->what; to->do_free = 0; }
}
static WINAPI void std_exception_destroy(exception_data *d) { if (d->do_free) free((void *)d->what); d->what = NULL; d->do_free = 0; }
// __std_type_info_data: an undecorated-name cache, then the decorated name (".?AVfoo@@").
typedef struct { const char *undecorated; char decorated[]; } type_info_data;
static WINAPI int std_type_info_compare(const type_info_data *a, const type_info_data *b) { return a == b ? 0 : strcmp(a->decorated + 1, b->decorated + 1); }
static WINAPI size_t std_type_info_hash(const type_info_data *d) {
    size_t h = 14695981039346656037ull;
    for (const char *c = d->decorated + 1; *c; c++) { h ^= (uint8_t)*c; h *= 1099511628211ull; }
    return h;
}
static WINAPI const char *std_type_info_name(type_info_data *d, void *list) { (void)list; return d->undecorated ? d->undecorated : d->decorated; }
static WINAPI void std_type_info_destroy_list(void *list) { (void)list; }
static WINAPI void type_info_destructor(void *self) { (void)self; }
static void *g_type_info_vtable[2] = { (void *)type_info_destructor, NULL };
static __thread void *t_current_record, *t_current_context;
static WINAPI void **current_exception(void) { handled *h = current_handled(); t_current_record = h ? h->record : NULL; return &t_current_record; }
static WINAPI void **current_exception_context(void) { return &t_current_context; }
static __thread int t_processing_throw;
static WINAPI int *processing_throw(void) { return &t_processing_throw; }
static WINAPI void nlg_notify(void) {}

static WINAPI void *m_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static WINAPI void *m_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static WINAPI void *m_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static WINAPI int m_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static WINAPI void *m_memchr(const void *p, int c, size_t n) { return memchr(p, c, n); }
static WINAPI char *m_strchr(const char *s, int c) { return strchr(s, c); }
static WINAPI char *m_strrchr(const char *s, int c) { return strrchr(s, c); }
static WINAPI char *m_strstr(const char *a, const char *b) { return strstr(a, b); }
static WINAPI WCHAR *m_wcschr(const WCHAR *s, WCHAR c) { for (;; s++) { if (*s == c) return (WCHAR *)s; if (!*s) return NULL; } }
static WINAPI WCHAR *m_wcsrchr(const WCHAR *s, WCHAR c) { const WCHAR *r = NULL; for (;; s++) { if (*s == c) r = s; if (!*s) return (WCHAR *)r; } }
static WINAPI WCHAR *m_wcsstr(const WCHAR *h, const WCHAR *n) {
    if (!*n) return (WCHAR *)h;
    for (; *h; h++) { size_t i = 0; while (n[i] && h[i] == n[i]) i++; if (!n[i]) return (WCHAR *)h; }
    return NULL;
}

// __AdjustPointer: a base class's address from a derived object's, by a PMD (member displacement; then, for a
// virtual base, the displacement of the virtual base table pointer and the entry in it).
typedef struct { int32_t mdisp, pdisp, vdisp; } pmd;
static WINAPI void *adjust_pointer(void *p, const pmd *d) {
    char *r = (char *)p + d->mdisp;
    if (d->pdisp >= 0) {
        const char *vbtable = *(const char *const *)((char *)p + d->pdisp);
        r += *(const int32_t *)(vbtable + d->vdisp) + d->pdisp;
    }
    return r;
}
static WINAPI int uncaught_exceptions(void) { return 0; }   // exceptions are not in flight between throw and catch here

// dynamic_cast through MSVC's RTTI: the vftable's slot -1 is the complete object locator (signature 1 on x64:
// image-relative fields: offset, constructor displacement offset, type descriptor, class hierarchy, itself). The
// hierarchy lists base class descriptors (type descriptor, contained bases, PMD, attributes, hierarchy); a type
// descriptor is a vftable pointer, a spare pointer and the decorated name. Types compare by name (each module
// has its own descriptors). A failed cast gives NULL (a failed reference cast is reported).
static const char *rtti_name(const uint8_t *td) { return (const char *)td + 16; }
static WINAPI void *rt_dynamic_cast(void *in, int32_t vfdelta, const uint8_t *src_type, const uint8_t *target_type, int is_reference) {
    (void)src_type;
    if (!in) return NULL;
    const uint8_t *col = ((const uint8_t *const *)*(void **)((uint8_t *)in + vfdelta))[-1];
    if (!col || *(const uint32_t *)col != 1) { vx_log("dynamic_cast: no x64 RTTI locator"); return NULL; }
    const uint8_t *base = col - *(const uint32_t *)(col + 20);       // pSelf is the locator's own RVA
    uint8_t *complete = (uint8_t *)in + vfdelta - *(const int32_t *)(col + 4);
    int32_t cd = *(const int32_t *)(col + 8);
    if (cd) complete -= *(const int32_t *)((uint8_t *)in + vfdelta - cd);
    const uint8_t *chd = base + *(const uint32_t *)(col + 16);
    uint32_t count = *(const uint32_t *)(chd + 8);
    const uint32_t *bases = (const uint32_t *)(base + *(const uint32_t *)(chd + 12));
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *bcd = base + bases[i];
        const uint8_t *td = base + *(const uint32_t *)bcd;
        if (strcmp(rtti_name(td), rtti_name(target_type))) continue;
        int32_t mdisp = *(const int32_t *)(bcd + 8), pdisp = *(const int32_t *)(bcd + 12), vdisp = *(const int32_t *)(bcd + 16);
        uint8_t *r = complete + mdisp;
        if (pdisp >= 0) r += *(const int32_t *)(*(const uint8_t *const *)(complete + pdisp) + vdisp) + pdisp;
        return r;
    }
    if (is_reference) vx_log("dynamic_cast to %s failed (a bad_cast would be thrown)", rtti_name(target_type));
    return NULL;
}
// __RTCastToVoid: the complete object.
static WINAPI void *rt_cast_to_void(void *in) {
    if (!in) return NULL;
    const uint8_t *col = ((const uint8_t *const *)*(void **)in)[-1];
    uint8_t *complete = (uint8_t *)in - *(const int32_t *)(col + 4);
    int32_t cd = *(const int32_t *)(col + 8);
    if (cd) complete -= *(const int32_t *)((uint8_t *)in - cd);
    return complete;
}
// __RTtypeid: the type descriptor of the complete object.
static WINAPI const void *rt_typeid(void *in) {
    if (!in) { vx_log("typeid of a null pointer (a bad_typeid would be thrown)"); return NULL; }
    const uint8_t *col = ((const uint8_t *const *)*(void **)in)[-1];
    const uint8_t *base = col - *(const uint32_t *)(col + 20);
    return base + *(const uint32_t *)(col + 12);
}

void vx_CxxThrowException(void *, const void *);
// vcruntime140's own critical-section helper (VC++ 2015-2019 builds of the CRT startup call it).
static WINAPI BOOL __vcrt_InitializeCriticalSectionEx(void *cs, DWORD spin, DWORD flags) {
    BOOL (WINAPI *init)(void *, DWORD, DWORD) = (BOOL (WINAPI *)(void *, DWORD, DWORD))vx_find_export("kernel32.dll", "InitializeCriticalSectionEx");
    return init(cs, spin, flags);
}
static vx_export g_vcruntime[] = {
    VX_EXPORT(__vcrt_InitializeCriticalSectionEx),
    { "__AdjustPointer", (void *)adjust_pointer }, { "__uncaught_exception", (void *)uncaught_exceptions },
    { "__uncaught_exceptions", (void *)uncaught_exceptions }, { "__RTDynamicCast", (void *)rt_dynamic_cast },
    { "__RTCastToVoid", (void *)rt_cast_to_void }, { "__RTtypeid", (void *)rt_typeid },
    { "_CxxThrowException", (void *)vx_CxxThrowException }, { "__CxxFrameHandler3", (void *)CxxFrameHandler3 },
    { "__CxxFrameHandler", (void *)CxxFrameHandler3 },              // msvcrt.dll's name for the same tables
    { "??1type_info@@UEAA@XZ", (void *)type_info_destructor }, { "?terminate@@YAXXZ", (void *)std_terminate },
    { "__CxxFrameHandler4", (void *)CxxFrameHandler4 }, { "__GSHandlerCheck_EH", (void *)GSHandlerCheck_EH }, { "__GSHandlerCheck_EH4", (void *)GSHandlerCheck_EH4 },
    { "__GSHandlerCheck", (void *)GSHandlerCheck },
    { "__std_terminate", (void *)std_terminate }, { "_purecall", (void *)purecall },
    { "_set_purecall_handler", (void *)set_purecall_handler }, { "_get_purecall_handler", (void *)get_purecall_handler },
    { "__std_exception_copy", (void *)std_exception_copy }, { "__std_exception_destroy", (void *)std_exception_destroy },
    { "__std_type_info_compare", (void *)std_type_info_compare }, { "__std_type_info_hash", (void *)std_type_info_hash },
    { "__std_type_info_name", (void *)std_type_info_name }, { "__std_type_info_destroy_list", (void *)std_type_info_destroy_list },
    { "??_7type_info@@6B@", (void *)g_type_info_vtable },
    { "__current_exception", (void *)current_exception }, { "__current_exception_context", (void *)current_exception_context },
    { "__processing_throw", (void *)processing_throw }, { "__NLG_Notify", (void *)nlg_notify },
    { "__NLG_Dispatch2", (void *)nlg_notify }, { "__NLG_Return2", (void *)nlg_notify },
    { "memcpy", (void *)m_memcpy }, { "memmove", (void *)m_memmove }, { "memset", (void *)m_memset }, { "memcmp", (void *)m_memcmp },
    { "memchr", (void *)m_memchr }, { "strchr", (void *)m_strchr }, { "strrchr", (void *)m_strrchr }, { "strstr", (void *)m_strstr },
    { "wcschr", (void *)m_wcschr }, { "wcsrchr", (void *)m_wcsrchr }, { "wcsstr", (void *)m_wcsstr },
    // Filled in at start-up from the SEH module (the same functions as in ucrtbase and kernel32).
    { "__C_specific_handler", NULL }, { "_setjmp", NULL }, { "__intrinsic_setjmp", NULL }, { "longjmp", NULL },
};

void vx_vcruntime_init(void) {
    lookup_function = (lookup_fn)vx_find_export("kernel32.dll", "RtlLookupFunctionEntry");
    rtl_unwind_ex = (unwind_fn)vx_find_export("kernel32.dll", "RtlUnwindEx");
    pc_to_header = (pc_to_header_fn)vx_find_export("kernel32.dll", "RtlPcToFileHeader");
    size_t n = sizeof g_vcruntime / sizeof g_vcruntime[0];
    for (size_t i = 0; i < n; i++)
        if (!g_vcruntime[i].fn) g_vcruntime[i].fn = vx_find_export("ucrtbase.dll", g_vcruntime[i].name);
    vx_register_module("vcruntime140.dll", g_vcruntime, n);
    vx_register_module("vcruntime140_1.dll", g_vcruntime, n);
    vx_register_module("msvcrt.dll", g_vcruntime, n);                // msvcrt.dll carried the C++ support itself
}

#ifdef __aarch64__
#include "arm64.h"
__attribute__((constructor)) static void vcrt_guest_add(void) {   // (vCPU: answered in the guest)
    vx_guest_add(m_memcpy, xv_g_memcpy, NULL, 0); vx_guest_add(m_memmove, xv_g_memmove, NULL, 0);
    vx_guest_add(m_memset, xv_g_memset, NULL, 0); vx_guest_add(m_memcmp, xv_g_memcmp, NULL, 0);
}
#endif

#ifdef __aarch64__
#include "thunks/vcruntime.c.inc"
#endif
