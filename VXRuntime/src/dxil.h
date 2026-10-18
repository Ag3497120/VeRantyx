// DXIL (Shader Model 6, Direct3D 12's shaders): the container and the LLVM 3.7 bitcode inside it, read into a small
// in-memory module that the DXIL -> Metal translation works from. Written for VXRuntime (no LLVM): the bitstream
// reader, the types, the constants, the functions' instructions and the metadata (dx.entryPoints, the signatures,
// the resources) as far as DXIL uses them.
#ifndef VX_DXIL_H
#define VX_DXIL_H
#include <stddef.h>
#include <stdint.h>

enum dxil_type_kind {
    DXT_VOID, DXT_HALF, DXT_FLOAT, DXT_DOUBLE, DXT_LABEL, DXT_METADATA, DXT_INT, DXT_POINTER, DXT_FUNCTION,
    DXT_STRUCT, DXT_ARRAY, DXT_VECTOR, DXT_OPAQUE, DXT_OTHER
};

typedef struct {
    uint8_t kind;
    uint8_t vararg;          // function
    uint8_t packed;          // struct
    uint32_t bits;           // int: width; pointer: address space
    uint32_t count;          // array, vector: elements; struct, function: members / parameters
    uint32_t elem;           // array, vector, pointer: element type; function: return type
    uint32_t *members;       // struct: member types; function: parameter types
    char *name;              // struct name ("dx.types.Handle", ...) or NULL
} dxil_type;

enum dxil_value_kind {
    DXV_GLOBAL, DXV_FUNCTION, DXV_ARG, DXV_INST, DXV_BLOCK,
    DXV_CONST_INT, DXV_CONST_FLOAT, DXV_CONST_NULL, DXV_CONST_UNDEF, DXV_CONST_AGGREGATE, DXV_CONST_DATA,
    DXV_CONST_STRING, DXV_CONST_EXPR, DXV_FORWARD
};

typedef struct {
    uint8_t kind;
    uint32_t type;
    union {
        int64_t i;           // CONST_INT (sign-extended)
        double f;            // CONST_FLOAT (as double; float and half widened)
        struct { uint32_t n; uint32_t *ids; } list;      // CONST_AGGREGATE (value ids), CONST_DATA / STRING (raw elements)
        struct { uint32_t op, sub, n; uint32_t *ops; } expr;   // CONST_EXPR: a DXIL_ instruction code, its sub-op, value ids
        uint32_t index;      // GLOBAL: global index; FUNCTION: function index; ARG: argument number; INST: instruction index
    } u;
    uint64_t *data;          // CONST_DATA / STRING elements
    const char *name;        // from the value symbol table (globals and functions)
} dxil_value;

// Instruction codes (LLVM's, as DXIL uses them). Operands are value ids unless noted.
enum dxil_op {
    DXI_RET = 1, DXI_BR, DXI_SWITCH, DXI_UNREACHABLE,
    DXI_BINOP, DXI_CAST, DXI_CMP, DXI_SELECT, DXI_EXTRACTELT, DXI_INSERTELT, DXI_SHUFFLEVEC,
    DXI_EXTRACTVAL, DXI_INSERTVAL, DXI_PHI, DXI_ALLOCA, DXI_LOAD, DXI_STORE, DXI_GEP, DXI_CALL,
    DXI_ATOMICRMW, DXI_CMPXCHG, DXI_FENCE
};

// BINOP sub-ops (float when the type is): add sub mul udiv sdiv urem srem shl lshr ashr and or xor
// CAST sub-ops: trunc zext sext fptoui fptosi uitofp sitofp fptrunc fpext ptrtoint inttoptr bitcast addrspacecast
// CMP sub-ops: LLVM predicates (0-15 floating point, 32-41 integer)

typedef struct {
    uint16_t op;             // enum dxil_op
    uint16_t sub;            // BINOP/CAST/CMP/ATOMICRMW: the operation; GEP: inbounds
    uint32_t type;           // the result's type (void for none)
    uint32_t result;         // the value id it defines, or UINT32_MAX
    uint32_t block;          // the basic block it is in
    uint32_t flags;          // BINOP: fast-math or nsw/nuw flags; LOAD/STORE: alignment (log2 + 1)
    uint32_t n;
    uint32_t *ops;           // BR: [true block, false block, cond] or [block]; PHI: [value, block]...;
                             // SWITCH: [cond, default block, case value id, block, ...]; CALL: [callee, args...];
                             // EXTRACTVAL/INSERTVAL: values, then the literal indices (after `nvals`);
                             // ALLOCA: [size value]; GEP: [source element type, base, indices...]
    uint32_t nvals;          // EXTRACTVAL/INSERTVAL: how many of ops are values (the rest are literal indices)
} dxil_inst;

typedef struct {
    const char *name;
    uint32_t type;           // function type
    uint32_t value;          // its value id
    int is_declaration;
    uint32_t first_arg;      // value id of the first argument (bodies only)
    uint32_t nblocks;
    uint32_t ninsts;
    dxil_inst *insts;
    uint32_t nlocals;
    dxil_value *locals;      // value ids from the module's nmodule_values on: arguments, constants, results
} dxil_function;

typedef struct {
    uint32_t type;           // value type
    uint32_t init;           // initializer value id, or UINT32_MAX
    uint32_t addrspace;
    int constant;
    uint32_t value;
} dxil_global;

enum dxil_md_kind { DXM_NULL, DXM_STRING, DXM_VALUE, DXM_NODE, DXM_OTHER };
typedef struct {
    uint8_t kind;
    uint32_t n;
    uint32_t *ops;           // NODE: metadata ids + 1 (0: null)
    char *str;               // STRING
    uint32_t type, value;    // VALUE: a value of the module
} dxil_md;

typedef struct {
    char *name;
    uint32_t n;
    uint32_t *nodes;         // metadata ids
} dxil_named_md;

typedef struct {
    // The container's parts the translation needs besides the bitcode.
    uint32_t shader_kind;    // the program header's: 0 pixel, 1 vertex, 2 geometry, 3 hull, 4 domain, 5 compute, ...
    uint32_t shader_major, shader_minor;
    uint32_t dxil_version;

    uint32_t ntypes;
    dxil_type *types;
    uint32_t nmodule_values;
    dxil_value *values;      // the module's values: globals, functions, module constants
    uint32_t nglobals;
    dxil_global *globals;
    uint32_t nfunctions;
    dxil_function *functions;
    uint32_t nmd;
    dxil_md *md;
    uint32_t nnamed;
    dxil_named_md *named;
    char *triple, *datalayout;
    char error[256];
} dxil_module;

// Reads a DXIL container (or a bare bitcode blob). Returns 0, or -1 with module->error set.
int dxil_read(const void *data, size_t size, dxil_module *module);
void dxil_free(dxil_module *module);

// Helpers for readers of the module.
const dxil_named_md *dxil_named(const dxil_module *m, const char *name);
const dxil_md *dxil_md_op(const dxil_module *m, const dxil_md *node, uint32_t i);   // NULL for null or out of range
int dxil_md_int(const dxil_module *m, const dxil_md *md, int64_t *out);             // a VALUE metadata holding an integer
const char *dxil_value_name(const dxil_module *m, uint32_t value);
// A value as seen from a function (module values, then the function's own); NULL if out of range.
const dxil_value *dxil_value_at(const dxil_module *m, const dxil_function *f, uint32_t id);
// DXIL's intrinsic number of a call to dx.op.* (its first argument), or -1.
int dxil_dxop(const dxil_module *m, const dxil_function *f, const dxil_inst *inst);
// A short text form of the module (one instruction a line), for the tests and tools/dxildump.
void dxil_dump(const dxil_module *m, void (*out)(void *ctx, const char *line), void *ctx);
#endif
