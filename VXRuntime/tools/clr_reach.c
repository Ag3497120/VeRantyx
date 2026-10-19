// What a managed program calls from the framework along one path: starting at the methods whose name matches, follows
// calls into the program's own assemblies (given on the command line) and lists every framework member reached.
//   build/clr_reach <entry substring> <assembly files...>
//   build/clr_reach --find <substring> <assembly files...>      (the methods whose "Type::name" contains it)
//   build/clr_reach --fields <substring> <assembly files...>    (the fields of the types whose name contains it, with flags)
//   build/clr_reach --pinvoke <substring> <assembly files...>   (the P/Invoke methods matching it: DLL, entry point, signature)
// Over-approximate (a call to one of the program's types reaches every method of that name there), so it lists what a
// path may need, which is what the CLR host (src/dotnet_runtime.c) must provide to get through it.
#include "dotnet_metadata.h"
#include "dotnet_il.h"
#include "pe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *name; uint8_t *image; size_t size; vx_pe_info pe; vx_dotnet_metadata md; uint8_t *seen; } assembly;
static assembly asms[64];
static int nasms;

static uint8_t *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return b;
}

static int assembly_by_name(const char *name) {
    for (int i = 0; i < nasms; i++) if (!strcasecmp(asms[i].name, name)) return i;
    return -1;
}

typedef struct { int a; uint32_t token; } item;
static item *queue; static size_t qn, qcap;
static void push(int a, uint32_t token) {
    uint32_t row = token & 0xffffff;
    if (row == 0 || row > asms[a].md.row_count[6]) return;
    if (asms[a].seen[row]) return;
    asms[a].seen[row] = 1;
    if (qn == qcap) { qcap = qcap ? qcap * 2 : 1024; queue = realloc(queue, qcap * sizeof *queue); }
    queue[qn++] = (item){ a, token };
}

static void push_named(const char *ns, const char *type, const char *name, int only_asm) {
    for (int a = 0; a < nasms; a++) {
        if (only_asm >= 0 && a != only_asm) continue;
        for (uint32_t row = 1; row <= asms[a].md.row_count[6]; row++) {
            vx_dotnet_method m;
            if (!vx_dotnet_method_get(&asms[a].md, 0x06000000u | row, &m)) continue;
            if (strcmp(m.name, name)) continue;
            if (strcmp(m.declaring_type_name ? m.declaring_type_name : "", type)) continue;
            if (strcmp(m.declaring_type_namespace ? m.declaring_type_namespace : "", ns)) continue;
            push(a, m.token);
        }
    }
}

static void note(const char *asm_name, const char *ns, const char *type, const char *name);

static void method_spec_target(int a, uint32_t token, uint32_t *target) {
    uint32_t v = 0;
    *target = 0;
    if (!vx_dotnet_column(&asms[a].md, 0x2B, (token & 0xffffff) - 1, 0, &v)) return;
    *target = ((v & 1) ? 0x0A000000u : 0x06000000u) | (v >> 1);     // MethodDefOrRef
}

static void visit_token(int a, uint32_t token) {
    uint32_t table = token >> 24;
    if (table == 0x2B) { uint32_t t; method_spec_target(a, token, &t); if (t) visit_token(a, t); return; }
    if (table == 0x06) { push(a, token); return; }
    if (table != 0x0A) return;
    vx_dotnet_member_ref r;
    if (!vx_dotnet_member_ref_get(&asms[a].md, token, &r)) return;
    uint32_t ptable = r.parent_token >> 24;
    if (ptable == 0x01) {                                           // a TypeRef: the program's own assembly, or the framework
        uint32_t aref; const char *an = "?";
        if (vx_dotnet_type_assembly_ref(&asms[a].md, r.parent_token, &aref)) vx_dotnet_assembly_ref_name(&asms[a].md, aref, &an);
        int target = assembly_by_name(an);
        if (target >= 0) push_named(r.parent_namespace ? r.parent_namespace : "", r.parent_name, r.name, target);
        else note(an, r.parent_namespace ? r.parent_namespace : "", r.parent_name, r.name);
    } else if (ptable == 0x1B) {                                    // a TypeSpec: a generic instance, usually the framework's
        note("(generic)", "", "?", r.name);
    } else if (ptable == 0x02) {                                    // a TypeDef of this assembly
        const char *ns, *tn;
        if (vx_dotnet_type_name(&asms[a].md, r.parent_token, &ns, &tn)) push_named(ns ? ns : "", tn, r.name, a);
    }
}

typedef struct { char key[300]; } found;
static found *found_list; static size_t fn, fcap;
static void note(const char *asm_name, const char *ns, const char *type, const char *name) {
    char key[300];
    snprintf(key, sizeof key, "%s\t%s%s%s::%s", asm_name, ns, *ns ? "." : "", type, name);
    for (size_t i = 0; i < fn; i++) if (!strcmp(found_list[i].key, key)) return;
    if (fn == fcap) { fcap = fcap ? fcap * 2 : 512; found_list = realloc(found_list, fcap * sizeof *found_list); }
    snprintf(found_list[fn++].key, sizeof found_list[0].key, "%s", key);
}

static void describe(int a, uint32_t token, char *out, size_t cap) {
    uint32_t table = token >> 24;
    snprintf(out, cap, "%08x", token);
    if (table == 0x2B) { uint32_t t; method_spec_target(a, token, &t); if (t) describe(a, t, out, cap); return; }
    if (table == 0x06) {
        vx_dotnet_method m;
        if (vx_dotnet_method_get(&asms[a].md, token, &m))
            snprintf(out, cap, "%s.%s::%s", m.declaring_type_namespace ? m.declaring_type_namespace : "", m.declaring_type_name, m.name);
    } else if (table == 0x01 || table == 0x02) {                        // a type: isinst, castclass, newarr, box
        const char *ns, *tn;
        if (vx_dotnet_type_name(&asms[a].md, token, &ns, &tn)) snprintf(out, cap, "%s%s%s", ns ? ns : "", ns && *ns ? "." : "", tn);
    } else if (table == 0x0A) {
        vx_dotnet_member_ref r;
        if (vx_dotnet_member_ref_get(&asms[a].md, token, &r)) {
            const char *pn = r.parent_name ? r.parent_name : "<generic>";
            snprintf(out, cap, "%s%s%s::%s", r.parent_namespace ? r.parent_namespace : "", r.parent_namespace && *r.parent_namespace ? "." : "", pn, r.name);
        }
    }
}

int main(int argc, char **argv) {
    int find = argc > 1 && !strcmp(argv[1], "--find");
    int calls = argc > 1 && !strcmp(argv[1], "--calls");
    int il = argc > 1 && !strcmp(argv[1], "--il");                  // every instruction: offset, opcode, operand
    int fields = argc > 1 && !strcmp(argv[1], "--fields");
    int pinvoke = argc > 1 && !strcmp(argv[1], "--pinvoke");
    int specs = argc > 1 && !strcmp(argv[1], "--methodspec");               // the MethodSpec rows: generic method instantiations
    const char *needle = argv[(find || calls || il || fields || pinvoke || specs) ? 2 : 1];
    int first = (find || calls || il || fields || pinvoke || specs) ? 3 : 2;
    if (argc < first + 1) { fprintf(stderr, "usage: clr_reach [--find] <substring> <assemblies...>\n"); return 2; }
    for (int i = first; i < argc && nasms < 64; i++) {
        assembly *as = &asms[nasms];
        const char *base = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        as->name = strdup(base);
        char *dot = strrchr(as->name, '.'); if (dot) *dot = 0;
        as->image = slurp(argv[i], &as->size);
        if (!as->image || !vx_pe_inspect(as->image, as->size, &as->pe) || !vx_dotnet_metadata_open(as->image, as->size, &as->pe, &as->md)) continue;
        as->seen = calloc(as->md.row_count[6] + 2, 1);
        nasms++;
    }
    if (specs) {
        for (int a = 0; a < nasms; a++) {
            const vx_dotnet_metadata *md = &asms[a].md;
            for (uint32_t row = 0; row < md->row_count[43]; row++) {
                uint32_t coded, inst_index; char what[300];
                if (!vx_dotnet_column(md, 43, row, 0, &coded) || !vx_dotnet_column(md, 43, row, 1, &inst_index)) continue;
                uint32_t target = ((coded & 1) ? 0x0a000000u : 0x06000000u) | (coded >> 1);
                describe(a, target, what, sizeof what);
                if (*needle && !strstr(what, needle)) continue;
                printf("%s  2b%06x -> %08x %s\n", asms[a].name, row + 1, target, what);
            }
        }
        return 0;
    }
    if (pinvoke) {
        for (int a = 0; a < nasms; a++) {
            const vx_dotnet_metadata *md = &asms[a].md;
            for (uint32_t row = 0; row < md->row_count[28]; row++) {          // ImplMap: flags, MemberForwarded, ImportName, ImportScope
                uint32_t flags, forwarded, name_index, scope;
                if (!vx_dotnet_column(md, 28, row, 0, &flags) || !vx_dotnet_column(md, 28, row, 1, &forwarded) ||
                    !vx_dotnet_column(md, 28, row, 2, &name_index) || !vx_dotnet_column(md, 28, row, 3, &scope)) continue;
                if ((forwarded & 1) != 1) continue;                               // a MethodDef
                vx_dotnet_method m;
                if (!vx_dotnet_method_get(md, 0x06000000u | (forwarded >> 1), &m)) continue;
                char full[400];
                snprintf(full, sizeof full, "%s.%s::%s", m.declaring_type_namespace ? m.declaring_type_namespace : "", m.declaring_type_name, m.name);
                if (!strstr(full, needle)) continue;
                const char *entry = "?", *dll = "?";
                uint32_t dll_index;
                vx_dotnet_string(md, name_index, &entry);
                if (scope && vx_dotnet_column(md, 26, scope - 1, 0, &dll_index)) vx_dotnet_string(md, dll_index, &dll);
                uint32_t sig_index, sig_size; const uint8_t *sig;
                printf("%s  %s  -> %s!%s  flags %04x  method flags %04x  sig", asms[a].name, full, dll, entry, flags, m.flags);
                if (vx_dotnet_column(md, 6, (forwarded >> 1) - 1, 4, &sig_index) && vx_dotnet_blob(md, sig_index, &sig, &sig_size))
                    for (uint32_t i = 0; i < sig_size; i++) printf(" %02x", sig[i]);
                printf("\n");
            }
        }
        return 0;
    }
    if (fields) {
        for (int a = 0; a < nasms; a++) {
            const vx_dotnet_metadata *md = &asms[a].md;
            for (uint32_t t = 1; t <= md->row_count[2]; t++) {
                const char *ns, *tn;
                uint32_t first_field, end_field, flags;
                if (!vx_dotnet_type_name(md, 0x02000000u | t, &ns, &tn) || !strstr(tn, needle)) continue;
                if (!vx_dotnet_column(md, 2, t - 1, 4, &first_field)) continue;
                end_field = md->row_count[4] + 1;
                if (t < md->row_count[2] && !vx_dotnet_column(md, 2, t, 4, &end_field)) continue;
                printf("== %s  %s.%s  (TypeDef %08x)\n", asms[a].name, ns ? ns : "", tn, 0x02000000u | t);
                for (uint32_t f = first_field; f < end_field; f++) {
                    vx_dotnet_field field;
                    if (!vx_dotnet_field_get(md, 0x04000000u | f, &field)) continue;
                    (void)flags;
                    printf("  %08x  flags %04x  %s\n", field.token, field.flags, field.name);
                }
            }
        }
        return 0;
    }
    for (int a = 0; a < nasms; a++)
        for (uint32_t row = 1; row <= asms[a].md.row_count[6]; row++) {
            vx_dotnet_method m;
            if (!vx_dotnet_method_get(&asms[a].md, 0x06000000u | row, &m)) continue;
            char full[400];
            snprintf(full, sizeof full, "%s.%s::%s", m.declaring_type_namespace ? m.declaring_type_namespace : "", m.declaring_type_name, m.name);
            if (!strstr(full, needle)) continue;
            if (find) printf("%s  %s\n", asms[a].name, full);
            else if (il) {
                vx_dotnet_method_body body;
                if (!m.rva || !vx_dotnet_method_body_get(&asms[a].md, &m, &body)) continue;
                printf("== %s  %s  (%u bytes of IL, %u locals token %08x)\n", asms[a].name, full, body.code_size, 0u, body.local_signature_token);
                uint32_t cursor = 0; vx_dotnet_instruction ins;
                while (vx_dotnet_il_next(body.code, body.code_size, &cursor, &ins) == 1) {
                    printf("  %04x  op %04x", ins.offset, ins.opcode);
                    if (ins.operand_kind == VX_IL_OPERAND_TOKEN && ins.operand_size >= 4) {
                        uint32_t token; memcpy(&token, ins.operand, 4);
                        char what[300]; describe(a, token, what, sizeof what);
                        if ((token >> 24) == 0x70) { const uint8_t *u; uint32_t units; if (vx_dotnet_user_string(&asms[a].md, token & 0xffffff, &u, &units)) { printf("  \""); for (uint32_t i = 0; i < units && i < 60; i++) putchar(u[i * 2] < 127 ? u[i * 2] : '?'); printf("\""); } }
                        else printf("  %s", what);
                    } else if (ins.operand_size == 1) printf("  %d", (int8_t)ins.operand[0]);
                    else if (ins.operand_size == 4) { int32_t v; memcpy(&v, ins.operand, 4); printf("  %d", v); }
                    printf("\n");
                }
            }
            else if (calls) {
                vx_dotnet_method_body body;
                if (!m.rva || !vx_dotnet_method_body_get(&asms[a].md, &m, &body)) continue;
                printf("== %s  %s  (%u bytes of IL)\n", asms[a].name, full, body.code_size);
                uint32_t cursor = 0; vx_dotnet_instruction ins;
                while (vx_dotnet_il_next(body.code, body.code_size, &cursor, &ins) == 1) {
                    if (ins.operand_kind != VX_IL_OPERAND_TOKEN || ins.operand_size < 4) continue;
                    uint16_t op = ins.opcode;
                    uint32_t token; memcpy(&token, ins.operand, 4);
                    char what[300];
                    if (op == 0x28 || op == 0x6F || op == 0x73) { describe(a, token, what, sizeof what); printf("   %s %s\n", op == 0x73 ? "newobj  " : op == 0x6F ? "callvirt" : "call    ", what); }
                    else if (op == 0x7B || op == 0x7D || op == 0x7E || op == 0x80) {                // ldfld stfld ldsfld stsfld
                        vx_dotnet_field f;
                        if (vx_dotnet_field_get(&asms[a].md, token, &f)) printf("   %s %s\n", op == 0x7B ? "ldfld   " : op == 0x7D ? "stfld   " : op == 0x7E ? "ldsfld  " : "stsfld  ", f.name);
                    } else if (op == 0x72) {                                                          // ldstr
                        const uint8_t *u; uint32_t units;
                        if (vx_dotnet_user_string(&asms[a].md, token & 0xffffff, &u, &units)) { printf("   ldstr    \""); for (uint32_t i = 0; i < units && i < 60; i++) putchar(u[i * 2] < 127 ? u[i * 2] : '?'); printf("\"\n"); }
                    }
                }
            } else push(a, m.token);
        }
    if (find) return 0;
    size_t methods = 0;
    for (size_t head = 0; head < qn; head++) {
        item it = queue[head];
        vx_dotnet_method m; vx_dotnet_method_body body;
        if (!vx_dotnet_method_get(&asms[it.a].md, it.token, &m) || !m.rva) continue;
        if (!vx_dotnet_method_body_get(&asms[it.a].md, &m, &body)) continue;
        methods++;
        uint32_t cursor = 0; vx_dotnet_instruction ins;
        while (vx_dotnet_il_next(body.code, body.code_size, &cursor, &ins) == 1) {
            if (ins.operand_kind != VX_IL_OPERAND_TOKEN || ins.operand_size < 4) continue;
            uint16_t op = ins.opcode;
            if (op != 0x28 && op != 0x6F && op != 0x73 && op != 0xFE06 && op != 0xFE07) continue;   // call callvirt newobj ldftn ldvirtftn
            uint32_t token; memcpy(&token, ins.operand, 4);
            visit_token(it.a, token);
        }
    }
    fprintf(stderr, "%zu of the program's methods reached, %zu framework members\n", methods, fn);
    for (size_t i = 0; i < fn; i++) printf("%s\n", found_list[i].key);
    return 0;
}
