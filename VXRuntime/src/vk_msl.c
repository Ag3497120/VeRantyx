// SPIR-V → MSL for the Vulkan front end (vk_msl.h). One pass over the module collects types, constants,
// decorations and the entry point's blocks; the entry function is then written as structured MSL: SPIR-V's merge
// and continue declarations become if/switch/while, phis become variables assigned on each edge, buffer blocks are
// read and written at byte offsets computed from their Offset/ArrayStride/MatrixStride decorations (so any
// std140/std430/scalar layout works without matching MSL struct layouts), and images/samplers come from the
// descriptor sets' argument buffers.
#include "vk_msl.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---- text ------------------------------------------------------------------------------------------------------
typedef struct { char *p; size_t n, cap; int oom; } sb;
static void sb_putn(sb *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) { b->oom = 1; return; }
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_puts(sb *b, const char *s) { sb_putn(b, s, strlen(s)); }
static void sb_printf(sb *b, const char *f, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(tmp, sizeof tmp, f, ap);
    va_end(ap);
    if (n < 0) { b->oom = 1; return; }
    if ((size_t)n < sizeof tmp) { sb_putn(b, tmp, (size_t)n); return; }
    char *big = malloc((size_t)n + 1);
    if (!big) { b->oom = 1; return; }
    va_start(ap, f);
    vsnprintf(big, (size_t)n + 1, f, ap);
    va_end(ap);
    sb_putn(b, big, (size_t)n);
    free(big);
}
static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }
static char *xprintf(const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(NULL, 0, f, ap);
    va_end(ap);
    if (n < 0) return NULL;
    char *p = malloc((size_t)n + 1);
    if (!p) return NULL;
    va_start(ap, f);
    vsnprintf(p, (size_t)n + 1, f, ap);
    va_end(ap);
    return p;
}

// ---- SPIR-V enumerants used here --------------------------------------------------------------------------------
enum {
    OpUndef = 1, OpName = 5, OpMemberName = 6, OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12,
    OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17, OpTypeVoid = 19,
    OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24, OpTypeImage = 25,
    OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30,
    OpTypePointer = 32, OpTypeFunction = 33, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43,
    OpConstantComposite = 44, OpConstantNull = 46, OpSpecConstantTrue = 48, OpSpecConstantFalse = 49,
    OpSpecConstant = 50, OpSpecConstantComposite = 51, OpSpecConstantOp = 52, OpFunction = 54,
    OpFunctionParameter = 55, OpFunctionEnd = 56, OpFunctionCall = 57, OpVariable = 59, OpImageTexelPointer = 60,
    OpLoad = 61, OpStore = 62, OpCopyMemory = 63, OpAccessChain = 65, OpInBoundsAccessChain = 66,
    OpArrayLength = 68, OpDecorate = 71, OpMemberDecorate = 72, OpDecorationGroup = 73, OpGroupDecorate = 74,
    OpGroupMemberDecorate = 75, OpVectorExtractDynamic = 77, OpVectorInsertDynamic = 78, OpVectorShuffle = 79,
    OpCompositeConstruct = 80, OpCompositeExtract = 81, OpCompositeInsert = 82, OpCopyObject = 83, OpCopyLogical = 400,
    OpTranspose = 84, OpSampledImage = 86, OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88,
    OpImageSampleDrefImplicitLod = 89, OpImageSampleDrefExplicitLod = 90, OpImageSampleProjImplicitLod = 91,
    OpImageSampleProjExplicitLod = 92, OpImageSampleProjDrefImplicitLod = 93, OpImageSampleProjDrefExplicitLod = 94,
    OpImageFetch = 95, OpImageGather = 96, OpImageDrefGather = 97, OpImageRead = 98, OpImageWrite = 99,
    OpImage = 100, OpImageQuerySizeLod = 103, OpImageQuerySize = 104, OpImageQueryLod = 105,
    OpImageQueryLevels = 106, OpImageQuerySamples = 107, OpConvertFToU = 109, OpConvertFToS = 110,
    OpConvertSToF = 111, OpConvertUToF = 112, OpUConvert = 113, OpSConvert = 114, OpFConvert = 115,
    OpQuantizeToF16 = 116, OpBitcast = 124, OpSNegate = 126, OpFNegate = 127, OpIAdd = 128, OpFAdd = 129,
    OpISub = 130, OpFSub = 131, OpIMul = 132, OpFMul = 133, OpUDiv = 134, OpSDiv = 135, OpFDiv = 136,
    OpUMod = 137, OpSRem = 138, OpSMod = 139, OpFRem = 140, OpFMod = 141, OpVectorTimesScalar = 142,
    OpMatrixTimesScalar = 143, OpVectorTimesMatrix = 144, OpMatrixTimesVector = 145, OpMatrixTimesMatrix = 146,
    OpOuterProduct = 147, OpDot = 148, OpIAddCarry = 149, OpISubBorrow = 150, OpUMulExtended = 151,
    OpSMulExtended = 152, OpAny = 154, OpAll = 155, OpIsNan = 156, OpIsInf = 157, OpIsFinite = 158,
    OpIsNormal = 159, OpSignBitSet = 160, OpLessOrGreater = 161, OpOrdered = 162, OpUnordered = 163,
    OpLogicalEqual = 164, OpLogicalNotEqual = 165, OpLogicalOr = 166, OpLogicalAnd = 167, OpLogicalNot = 168,
    OpSelect = 169, OpIEqual = 170, OpINotEqual = 171, OpUGreaterThan = 172, OpSGreaterThan = 173,
    OpUGreaterThanEqual = 174, OpSGreaterThanEqual = 175, OpULessThan = 176, OpSLessThan = 177,
    OpULessThanEqual = 178, OpSLessThanEqual = 179, OpFOrdEqual = 180, OpFUnordEqual = 181, OpFOrdNotEqual = 182,
    OpFUnordNotEqual = 183, OpFOrdLessThan = 184, OpFUnordLessThan = 185, OpFOrdGreaterThan = 186,
    OpFUnordGreaterThan = 187, OpFOrdLessThanEqual = 188, OpFUnordLessThanEqual = 189,
    OpFOrdGreaterThanEqual = 190, OpFUnordGreaterThanEqual = 191, OpShiftRightLogical = 194,
    OpShiftRightArithmetic = 195, OpShiftLeftLogical = 196, OpBitwiseOr = 197, OpBitwiseXor = 198,
    OpBitwiseAnd = 199, OpNot = 200, OpBitFieldInsert = 201, OpBitFieldSExtract = 202, OpBitFieldUExtract = 203,
    OpBitReverse = 204, OpBitCount = 205, OpDPdx = 207, OpDPdy = 208, OpFwidth = 209, OpDPdxFine = 210,
    OpDPdyFine = 211, OpFwidthFine = 212, OpDPdxCoarse = 213, OpDPdyCoarse = 214, OpFwidthCoarse = 215,
    OpEmitVertex = 218, OpEndPrimitive = 219, OpControlBarrier = 224, OpMemoryBarrier = 225, OpAtomicLoad = 227,
    OpAtomicStore = 228, OpAtomicExchange = 229, OpAtomicCompareExchange = 230, OpAtomicCompareExchangeWeak = 231,
    OpAtomicIIncrement = 232, OpAtomicIDecrement = 233, OpAtomicIAdd = 234, OpAtomicISub = 235,
    OpAtomicSMin = 236, OpAtomicUMin = 237, OpAtomicSMax = 238, OpAtomicUMax = 239, OpAtomicAnd = 240,
    OpAtomicOr = 241, OpAtomicXor = 242, OpPhi = 245, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248,
    OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251, OpKill = 252, OpReturn = 253,
    OpReturnValue = 254, OpUnreachable = 255, OpLine = 8, OpNoLine = 317, OpModuleProcessed = 330,
    OpExecutionModeId = 331, OpDecorateId = 332, OpGroupNonUniformElect = 333, OpGroupNonUniformAll = 334,
    OpGroupNonUniformAny = 335, OpGroupNonUniformAllEqual = 336, OpGroupNonUniformBroadcast = 337,
    OpGroupNonUniformBroadcastFirst = 338, OpGroupNonUniformBallot = 339, OpGroupNonUniformInverseBallot = 340,
    OpGroupNonUniformBallotBitExtract = 341, OpGroupNonUniformBallotBitCount = 342,
    OpGroupNonUniformBallotFindLSB = 343, OpGroupNonUniformBallotFindMSB = 344, OpGroupNonUniformShuffle = 345,
    OpGroupNonUniformShuffleXor = 346, OpGroupNonUniformShuffleUp = 347, OpGroupNonUniformShuffleDown = 348,
    OpGroupNonUniformIAdd = 349, OpGroupNonUniformFAdd = 350, OpGroupNonUniformIMul = 351,
    OpGroupNonUniformFMul = 352, OpGroupNonUniformSMin = 353, OpGroupNonUniformUMin = 354,
    OpGroupNonUniformFMin = 355, OpGroupNonUniformSMax = 356, OpGroupNonUniformUMax = 357,
    OpGroupNonUniformFMax = 358, OpGroupNonUniformBitwiseAnd = 359, OpGroupNonUniformBitwiseOr = 360,
    OpGroupNonUniformBitwiseXor = 361, OpGroupNonUniformLogicalAnd = 362, OpGroupNonUniformLogicalOr = 363,
    OpGroupNonUniformLogicalXor = 364, OpGroupNonUniformQuadBroadcast = 365, OpGroupNonUniformQuadSwap = 366,
    OpTerminateInvocation = 4416, OpSubgroupBallotKHR = 4421, OpSubgroupFirstInvocationKHR = 4422,
    OpDemoteToHelperInvocation = 5380, OpIsHelperInvocationEXT = 5381
};
enum { SC_UniformConstant = 0, SC_Input = 1, SC_Uniform = 2, SC_Output = 3, SC_Workgroup = 4,
       SC_CrossWorkgroup = 5, SC_Private = 6, SC_Function = 7, SC_PushConstant = 9, SC_Image = 11,
       SC_StorageBuffer = 12 };
enum { DIM_1D = 0, DIM_2D = 1, DIM_3D = 2, DIM_Cube = 3, DIM_Rect = 4, DIM_Buffer = 5, DIM_SubpassData = 6 };
enum { BI_Position = 0, BI_PointSize = 1, BI_ClipDistance = 3, BI_CullDistance = 4, BI_PrimitiveId = 7,
       BI_InvocationId = 8, BI_Layer = 9, BI_ViewportIndex = 10, BI_FragCoord = 15, BI_PointCoord = 16,
       BI_FrontFacing = 17, BI_SampleId = 18, BI_SamplePosition = 19, BI_SampleMask = 20, BI_FragDepth = 22,
       BI_HelperInvocation = 23, BI_NumWorkgroups = 24, BI_WorkgroupSize = 25, BI_WorkgroupId = 26,
       BI_LocalInvocationId = 27, BI_GlobalInvocationId = 28, BI_LocalInvocationIndex = 29, BI_SubgroupSize = 36,
       BI_NumSubgroups = 38, BI_SubgroupId = 40, BI_SubgroupLocalInvocationId = 41, BI_VertexIndex = 42,
       BI_InstanceIndex = 43, BI_SubgroupEqMask = 4416, BI_SubgroupGeMask = 4417, BI_SubgroupGtMask = 4418,
       BI_SubgroupLeMask = 4419, BI_SubgroupLtMask = 4420, BI_BaseVertex = 4424, BI_BaseInstance = 4425,
       BI_DrawIndex = 4426, BI_ViewIndex = 4440 };
enum { VK_DT_SAMPLER = 0, VK_DT_COMBINED = 1, VK_DT_SAMPLED_IMAGE = 2, VK_DT_STORAGE_IMAGE = 3,
       VK_DT_UNIFORM_TEXEL = 4, VK_DT_STORAGE_TEXEL = 5, VK_DT_UNIFORM_BUFFER = 6, VK_DT_STORAGE_BUFFER = 7,
       VK_DT_UNIFORM_BUFFER_DYNAMIC = 8, VK_DT_STORAGE_BUFFER_DYNAMIC = 9, VK_DT_INPUT_ATTACHMENT = 10 };

uint32_t vkmsl_descriptor_words(uint32_t type) {
    switch (type) {
    case VK_DT_UNIFORM_BUFFER: case VK_DT_STORAGE_BUFFER:
    case VK_DT_UNIFORM_BUFFER_DYNAMIC: case VK_DT_STORAGE_BUFFER_DYNAMIC: return 2;
    case VK_DT_COMBINED: return 2;
    default: return 1;
    }
}

// ---- module -----------------------------------------------------------------------------------------------------
enum { K_NONE, K_TYPE, K_CONST, K_SPEC, K_VAR, K_FUNC, K_VALUE, K_LABEL, K_EXTSET, K_DECOGROUP };
enum {
    D_BLOCK = 1u << 0, D_BUFFERBLOCK = 1u << 1, D_ROWMAJOR = 1u << 2, D_FLAT = 1u << 3, D_NOPERSP = 1u << 4,
    D_CENTROID = 1u << 5, D_SAMPLE = 1u << 6, D_NONWRITABLE = 1u << 7, D_NONREADABLE = 1u << 8,
    D_LOCATION = 1u << 9, D_BUILTIN = 1u << 10, D_OFFSET = 1u << 11, D_SET = 1u << 12, D_BINDING = 1u << 13,
    D_SPECID = 1u << 14, D_INDEX = 1u << 15, D_PATCH = 1u << 16, D_INVARIANT = 1u << 17, D_COMPONENT = 1u << 18,
    D_NONUNIFORM = 1u << 19, D_ARRAYSTRIDE = 1u << 20, D_MATRIXSTRIDE = 1u << 21,
    D_DEPTHCMP = 1u << 22     // (not a decoration: the image is sampled with depth comparisons)
};
typedef struct {
    uint32_t deco, builtin, location, component, set, binding, spec_id, index, offset, array_stride, matrix_stride;
} decos;

// Where a pointer value points: a local/interface variable (an MSL lvalue), a byte offset into a buffer, or an
// element of a descriptor array.
enum { P_LVALUE = 1, P_BUFFER, P_HANDLE, P_IMAGE_TEXEL, P_OPAQUE };   // (P_OPAQUE: a local variable holding an image or sampler)
typedef struct {
    int kind;
    uint32_t pointee;          // type id
    char *lvalue;              // P_LVALUE: expression; P_HANDLE: the descriptor expression
    char *base;                // P_BUFFER: byte pointer expression
    char *offset;              // P_BUFFER: uint expression (a variable name or a literal)
    uint32_t align;            // P_BUFFER: guaranteed byte alignment of base + offset
    uint32_t matrix_stride;    // P_BUFFER pointing at a matrix (or arrays of them): its layout
    uint8_t row_major, device; // device: writable (SSBO); else constant
    uint8_t depth;             // P_HANDLE: declared as a depth texture
    uint8_t column_of_row_major; // P_BUFFER: a column vector of a row-major matrix (components matrix_stride apart)
    uint32_t storage;          // storage class
    uint32_t var;              // root variable
    char *coord, *sample;      // P_IMAGE_TEXEL
} pinfo;

typedef struct {
    uint8_t kind;
    uint16_t op, n;            // defining opcode and operand word count (after the opcode word)
    const uint32_t *w;         // operands
    uint32_t type;             // result type (values, constants, variables)
    decos d;
    uint32_t mbase, mcount;    // struct types: member decorations in t->members
    char *expr;                // a forwarded expression for this id (constants, handles), else NULL
    char *aux;                 // sampled images: the sampler expression
    char *tname;               // types: the MSL value type name
    pinfo *ptr;                // pointer values
    int block;                 // labels: block index
    uint8_t used, written;     // variables
    uint8_t is_phi;
    uint8_t depthtex;          // image values: their texture is declared as a depth texture
    uint8_t odecl;             // access chains: their offset variable is declared (an inlined function runs it again)
} ent;

typedef struct {
    uint32_t label;
    size_t first, term;        // word offsets: first instruction after OpLabel, the terminator
    uint32_t merge_op;         // 0, OpSelectionMerge or OpLoopMerge
    uint32_t merge, cont;      // merge block, continue target
    size_t merge_pos;
} block;

typedef struct { int kind; uint32_t merge, cont, header; int brk_flag; } construct;
typedef struct { uint32_t set, word, words; char *decl; } dmember;
enum { C_LOOP = 1, C_SWITCH, C_SELECTION, C_FUNC };

typedef struct {
    const uint32_t *w;
    size_t count;
    uint32_t bound;
    ent *e;
    decos *members;
    uint32_t member_count, member_cap;
    const vkmsl_options *opt;
    vkmsl_result *res;
    int rc;
    uint32_t model, entry, glsl;
    const uint32_t *iface;
    uint32_t iface_count;
    uint32_t local_size[3], local_size_id[3], has_local_size_id;
    uint8_t origin_lower_left, depth_mode, early_tests, point_mode;
    block *blocks;
    uint32_t block_count, block_cap;
    construct stack[64];
    int depth;
    int flag_count, tmp_count;
    sb pre;                    // helper functions and type declarations
    sb glob;                   // program-scope constants
    sb vars;                   // entry-function-scope declarations
    sb body;
    sb ret;                    // statements that write the stage outputs before a return
    int indent;
    uint8_t helper[64];        // which prelude helpers are needed
    uint8_t set_used[VKMSL_MAX_SETS];
    uint8_t binding_used[256];
    uint8_t uses_subgroup, uses_fragcoord;
    dmember *dm;
    uint32_t dm_count, dm_cap;
    uint32_t cur_op;
    uint32_t *fparams;         // function parameters, in order (a function's ent: mbase first, mcount of them)
    uint32_t fparam_count, fparam_cap;
    uint32_t entry_block;      // the entry function's first block
    uint8_t vkernel, gs;       // a vertex shader as a kernel; a geometry shader as a kernel
    uint32_t gs_in_n, gs_maxt, gs_os, gs_loc, gs_clip, gs_out_prim;   // geometry: vertices in, triangles out per run, slots
    uint32_t gs_invocations;
    sb gs_pack;                // geometry: the statements that pack the outputs into spvCur
} T;

static void fail(T *t, int rc, const char *f, ...) {
    if (t->rc) return;
    t->rc = rc;
    va_list ap;
    va_start(ap, f);
    vsnprintf(t->res->error, sizeof t->res->error, f, ap);
    va_end(ap);
}

static ent *E(T *t, uint32_t id) {
    static _Thread_local ent none;
    if (id >= t->bound) { fail(t, VKMSL_INVALID, "id %u out of range", id); memset(&none, 0, sizeof none); return &none; }
    return &t->e[id];
}

static void ind(T *t, sb *b) { for (int i = 0; i < t->indent; i++) sb_puts(b, "    "); }
#define EMIT(...) do { ind(t, &t->body); sb_printf(&t->body, __VA_ARGS__); } while (0)

// ---- types ------------------------------------------------------------------------------------------------------
static int type_op(T *t, uint32_t id) { return E(t, id)->kind == K_TYPE ? E(t, id)->op : 0; }
static const uint32_t *tw(T *t, uint32_t id) { return E(t, id)->w; }
static int is_float(T *t, uint32_t id) { return type_op(t, id) == OpTypeFloat; }
static int is_int(T *t, uint32_t id) { return type_op(t, id) == OpTypeInt; }
static int is_bool(T *t, uint32_t id) { return type_op(t, id) == OpTypeBool; }
static uint32_t scalar_of(T *t, uint32_t id) {
    int op = type_op(t, id);
    if (op == OpTypeVector) return tw(t, id)[1];
    if (op == OpTypeMatrix) return scalar_of(t, tw(t, id)[1]);
    return id;
}
static uint32_t vec_size(T *t, uint32_t id) { return type_op(t, id) == OpTypeVector ? tw(t, id)[2] : 1; }
static int is_signed(T *t, uint32_t id) { uint32_t s = scalar_of(t, id); return is_int(t, s) && tw(t, s)[2]; }
static uint32_t scalar_width(T *t, uint32_t id) {
    uint32_t s = scalar_of(t, id);
    return is_bool(t, s) ? 32 : (is_int(t, s) || is_float(t, s)) ? tw(t, s)[1] : 0;
}
static uint32_t const_u32(T *t, uint32_t id, int *ok);

static const char *scalar_name(T *t, uint32_t id) {
    int op = type_op(t, id);
    if (op == OpTypeBool) return "bool";
    if (op == OpTypeFloat) return tw(t, id)[1] == 16 ? "half" : tw(t, id)[1] == 64 ? "double" : "float";
    if (op == OpTypeInt) {
        uint32_t width = tw(t, id)[1], s = tw(t, id)[2];
        if (width == 8) return s ? "char" : "uchar";
        if (width == 16) return s ? "short" : "ushort";
        if (width == 64) return s ? "long" : "ulong";
        return s ? "int" : "uint";
    }
    return NULL;
}
// The same scalar type with the given signedness (integers), as an MSL name.
static const char *int_name(uint32_t width, int s) {
    if (width == 8) return s ? "char" : "uchar";
    if (width == 16) return s ? "short" : "ushort";
    if (width == 64) return s ? "long" : "ulong";
    return s ? "int" : "uint";
}

static const char *tname(T *t, uint32_t id);
static void declare_struct(T *t, uint32_t id) {
    ent *s = E(t, id);
    // (member types first: a nested struct is declared before the one that holds it)
    for (uint32_t i = 1; i < s->n; i++) tname(t, s->w[i]);
    sb_printf(&t->pre, "struct S%u {", id);
    for (uint32_t i = 1; i < s->n; i++) sb_printf(&t->pre, " %s m%u;", tname(t, s->w[i]), i - 1);
    if (s->n == 1) sb_puts(&t->pre, " uint _empty;");
    sb_puts(&t->pre, " };\n");
}
static const char *tname(T *t, uint32_t id) {
    ent *e = E(t, id);
    if (e->tname) return e->tname;
    char buf[256];
    switch (type_op(t, id)) {
    case OpTypeVoid: snprintf(buf, sizeof buf, "void"); break;
    case OpTypeBool: case OpTypeInt: case OpTypeFloat: snprintf(buf, sizeof buf, "%s", scalar_name(t, id)); break;
    case OpTypeVector: snprintf(buf, sizeof buf, "%s%u", scalar_name(t, e->w[1]), e->w[2]); break;
    case OpTypeMatrix: {
        uint32_t col = e->w[1];
        snprintf(buf, sizeof buf, "%s%ux%u", scalar_name(t, scalar_of(t, col)), e->w[2], vec_size(t, col));
        break;
    }
    case OpTypeArray: {
        int ok = 1;
        uint32_t n = const_u32(t, e->w[2], &ok);
        snprintf(buf, sizeof buf, "spvArr<%s, %u>", tname(t, e->w[1]), n ? n : 1);
        break;
    }
    case OpTypeRuntimeArray: snprintf(buf, sizeof buf, "spvArr<%s, 1>", tname(t, e->w[1])); break;
    case OpTypeStruct: declare_struct(t, id); snprintf(buf, sizeof buf, "S%u", id); break;
    case OpTypePointer: return tname(t, e->w[2]);
    default: fail(t, VKMSL_UNSUPPORTED, "type %u (opcode %u) has no MSL value form", id, e->op); snprintf(buf, sizeof buf, "int");
    }
    e->tname = xstrdup(buf);
    return e->tname;
}

// A 4-component vector of the type's scalar (user varyings travel as 4-wide vectors so both stages agree).
static const char *vec4_of(T *t, uint32_t id) {
    static _Thread_local char buf[32];
    snprintf(buf, sizeof buf, "%s4", scalar_name(t, scalar_of(t, id)));
    return buf;
}

// Byte size of a type as laid out in a buffer (arrays by ArrayStride, structs by their last member).
static uint32_t buffer_size(T *t, uint32_t id, uint32_t matrix_stride, int row_major);

// ---- constants --------------------------------------------------------------------------------------------------
static const vkmsl_spec *spec_override(T *t, uint32_t id) {
    ent *e = E(t, id);
    if (!(e->d.deco & D_SPECID)) return NULL;
    for (uint32_t i = 0; i < t->opt->spec_count; i++)
        if (t->opt->specs[i].id == e->d.spec_id) return &t->opt->specs[i];
    return NULL;
}
static uint64_t const_bits(T *t, uint32_t id, int *ok) {
    ent *e = E(t, id);
    if (e->kind != K_CONST && e->kind != K_SPEC) { *ok = 0; return 0; }
    const vkmsl_spec *sp = spec_override(t, id);
    switch (e->op) {
    case OpConstantTrue: return 1;
    case OpConstantFalse: return 0;
    case OpSpecConstantTrue: case OpSpecConstantFalse: return sp ? sp->value != 0 : e->op == OpSpecConstantTrue;
    case OpConstantNull: return 0;
    case OpConstant: case OpSpecConstant: {
        if (sp) return sp->value;
        uint64_t v = e->w[2];
        if (e->n > 3) v |= (uint64_t)e->w[3] << 32;
        return v;
    }
    default: *ok = 0; return 0;
    }
}
static uint32_t const_u32(T *t, uint32_t id, int *ok) {
    ent *e = E(t, id);
    if (e->kind == K_SPEC && e->op == OpSpecConstantOp) {
        // (integer arithmetic on other constants: array sizes and workgroup sizes)
        uint32_t op = e->w[2];
        int ok2 = 1;
        uint32_t a = e->n > 3 ? const_u32(t, e->w[3], &ok2) : 0, b = e->n > 4 ? const_u32(t, e->w[4], &ok2) : 0;
        if (!ok2) { *ok = 0; return 0; }
        switch (op) {
        case OpIAdd: return a + b; case OpISub: return a - b; case OpIMul: return a * b;
        case OpUDiv: case OpSDiv: return b ? a / b : 0; case OpUMod: case OpSRem: case OpSMod: return b ? a % b : 0;
        case OpShiftLeftLogical: return a << (b & 31); case OpShiftRightLogical: return a >> (b & 31);
        case OpBitwiseAnd: return a & b; case OpBitwiseOr: return a | b; case OpBitwiseXor: return a ^ b;
        case OpUConvert: case OpSConvert: return a;
        default: *ok = 0; return 0;
        }
    }
    return (uint32_t)const_bits(t, id, ok);
}

static char *float_literal(double v, int half) {
    if (isnan(v)) return xstrdup(half ? "half(NAN)" : "NAN");
    if (isinf(v)) return xstrdup(v > 0 ? (half ? "half(INFINITY)" : "INFINITY") : (half ? "half(-INFINITY)" : "(-INFINITY)"));
    char buf[64];
    snprintf(buf, sizeof buf, "%.9g", v);
    if (!strpbrk(buf, ".eEn")) strcat(buf, ".0");
    return half ? xprintf("half(%s)", buf) : xprintf("%sf", buf);
}
static float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
static char *scalar_literal(T *t, uint32_t type, uint64_t bits) {
    int op = type_op(t, type);
    if (op == OpTypeBool) return xstrdup(bits ? "true" : "false");
    if (op == OpTypeFloat) {
        uint32_t width = tw(t, type)[1];
        if (width == 16) return float_literal(half_to_float((uint16_t)bits), 1);
        if (width == 64) { double d; memcpy(&d, &bits, 8); return float_literal(d, 0); }
        float f; uint32_t b32 = (uint32_t)bits; memcpy(&f, &b32, 4);
        return float_literal(f, 0);
    }
    if (op == OpTypeInt) {
        uint32_t width = tw(t, type)[1];
        int s = tw(t, type)[2];
        if (width == 64) return s ? xprintf("long(%lld)", (long long)bits) : xprintf("%lluul", (unsigned long long)bits);
        if (width < 32) {
            uint64_t mask = (1ull << width) - 1;
            int64_t sv = (int64_t)(bits & mask);
            if (s && (sv >> (width - 1))) sv -= (int64_t)1 << width;
            return s ? xprintf("%s(%lld)", int_name(width, 1), (long long)sv) :
                       xprintf("%s(%llu)", int_name(width, 0), (unsigned long long)(bits & mask));
        }
        if (s) {
            int32_t v = (int32_t)(uint32_t)bits;
            return v == INT32_MIN ? xstrdup("int(0x80000000)") : xprintf("%d", v);
        }
        return xprintf("%uu", (uint32_t)bits);
    }
    fail(t, VKMSL_UNSUPPORTED, "constant of type %u", type);
    return xstrdup("0");
}

static const char *value(T *t, uint32_t id);
// A zero value of any type.
static char *zero_of(T *t, uint32_t type) {
    switch (type_op(t, type)) {
    case OpTypeBool: return xstrdup("false");
    case OpTypeInt: case OpTypeFloat: return xprintf("%s(0)", tname(t, type));
    default: return xprintf("%s{}", tname(t, type));
    }
}
static void define_constant(T *t, uint32_t id) {
    ent *e = E(t, id);
    if (e->expr) return;
    uint32_t type = e->w[0];
    int ok = 1;
    switch (e->op) {
    case OpConstantTrue: case OpConstantFalse: case OpConstant: case OpSpecConstantTrue: case OpSpecConstantFalse:
    case OpSpecConstant:
        e->expr = scalar_literal(t, type, const_bits(t, id, &ok));
        if (!ok) fail(t, VKMSL_INVALID, "constant %u", id);
        return;
    case OpConstantNull:
        if (type_op(t, type) == OpTypeBool || type_op(t, type) == OpTypeInt || type_op(t, type) == OpTypeFloat)
            e->expr = scalar_literal(t, type, 0);
        else e->expr = zero_of(t, type);
        return;
    case OpConstantComposite: case OpSpecConstantComposite: {
        sb v = { 0 };
        int top = type_op(t, type);
        if (top == OpTypeVector || top == OpTypeMatrix) sb_printf(&v, "%s(", tname(t, type));
        else if (top == OpTypeArray) sb_printf(&v, "%s{{", tname(t, type));
        else sb_printf(&v, "%s{", tname(t, type));
        for (uint32_t i = 2; i < e->n; i++) sb_printf(&v, "%s%s", i > 2 ? ", " : "", value(t, e->w[i]));
        sb_puts(&v, top == OpTypeArray ? "}}" : (top == OpTypeVector || top == OpTypeMatrix) ? ")" : "}");
        // (aggregates are program-scope constants: their text can be long and they are used by name)
        if (top == OpTypeVector) { e->expr = v.p; return; }
        sb_printf(&t->glob, "constant %s _c%u = %s;\n", tname(t, type), id, v.p ? v.p : "{}");
        free(v.p);
        e->expr = xprintf("_c%u", id);
        return;
    }
    case OpSpecConstantOp: {
        uint32_t op = e->w[2];
        const char *a = e->n > 3 ? value(t, e->w[3]) : "", *b = e->n > 4 ? value(t, e->w[4]) : "";
        const char *sym = NULL;
        switch (op) {
        case OpIAdd: sym = "+"; break; case OpISub: sym = "-"; break; case OpIMul: sym = "*"; break;
        case OpUDiv: case OpSDiv: sym = "/"; break; case OpUMod: case OpSRem: sym = "%"; break;
        case OpShiftLeftLogical: sym = "<<"; break; case OpShiftRightLogical: case OpShiftRightArithmetic: sym = ">>"; break;
        case OpBitwiseAnd: sym = "&"; break; case OpBitwiseOr: sym = "|"; break; case OpBitwiseXor: sym = "^"; break;
        case OpLogicalAnd: sym = "&&"; break; case OpLogicalOr: sym = "||"; break;
        case OpIEqual: case OpLogicalEqual: sym = "=="; break; case OpINotEqual: case OpLogicalNotEqual: sym = "!="; break;
        case OpULessThan: case OpSLessThan: sym = "<"; break; case OpUGreaterThan: case OpSGreaterThan: sym = ">"; break;
        case OpULessThanEqual: case OpSLessThanEqual: sym = "<="; break;
        case OpUGreaterThanEqual: case OpSGreaterThanEqual: sym = ">="; break;
        }
        char *x;
        if (sym) x = xprintf("%s(%s %s %s)", tname(t, type), a, sym, b);
        else if (op == OpLogicalNot) x = xprintf("(!%s)", a);
        else if (op == OpNot) x = xprintf("%s(~%s)", tname(t, type), a);
        else if (op == OpSNegate) x = xprintf("%s(-%s)", tname(t, type), a);
        else if (op == OpSelect) x = xprintf("(%s ? %s : %s)", a, b, value(t, e->w[5]));
        else if (op == OpUConvert || op == OpSConvert || op == OpFConvert) x = xprintf("%s(%s)", tname(t, type), a);
        else if (op == OpCompositeExtract) {
            sb s = { 0 };
            sb_puts(&s, a);
            uint32_t ct = E(t, e->w[3])->w[0];
            for (uint32_t i = 4; i < e->n; i++) {
                int cop = type_op(t, ct);
                if (cop == OpTypeStruct) { sb_printf(&s, ".m%u", e->w[i]); ct = tw(t, ct)[e->w[i] + 1]; }
                else { sb_printf(&s, "[%u]", e->w[i]); ct = tw(t, ct)[1]; }
            }
            x = s.p;
        } else if (op == OpVectorShuffle) {
            sb s = { 0 };
            sb_printf(&s, "%s(", tname(t, type));
            uint32_t na = vec_size(t, E(t, e->w[3])->w[0]);
            for (uint32_t i = 5; i < e->n; i++) {
                uint32_t c = e->w[i];
                sb_printf(&s, "%s%s[%u]", i > 5 ? ", " : "", c < na ? a : b, c < na ? c : c - na);
            }
            sb_puts(&s, ")");
            x = s.p;
        } else { fail(t, VKMSL_UNSUPPORTED, "OpSpecConstantOp %u", op); x = xstrdup("0"); }
        sb_printf(&t->glob, "constant %s _c%u = %s;\n", tname(t, type), id, x);
        free(x);
        e->expr = xprintf("_c%u", id);
        return;
    }
    default:
        fail(t, VKMSL_UNSUPPORTED, "constant opcode %u", e->op);
        e->expr = xstrdup("0");
    }
}

static const char *value(T *t, uint32_t id) {
    ent *e = E(t, id);
    if (e->kind == K_CONST || e->kind == K_SPEC) { define_constant(t, id); return e->expr ? e->expr : "0"; }
    if (e->expr) return e->expr;
    if (e->kind == K_VALUE) {
        if (!e->expr) e->expr = xprintf("_%u", id);
        return e->expr;
    }
    if (e->kind == K_VAR) { fail(t, VKMSL_UNSUPPORTED, "pointer %u used as a value", id); return "0"; }
    fail(t, VKMSL_INVALID, "id %u is not a value (in opcode %u)", id, t->cur_op);
    return "0";
}

// ---- parsing ----------------------------------------------------------------------------------------------------
static void apply_decoration(decos *d, uint32_t deco, const uint32_t *arg, uint32_t argc) {
    uint32_t a = argc ? arg[0] : 0;
    switch (deco) {
    case 2: d->deco |= D_BLOCK; break;
    case 3: d->deco |= D_BUFFERBLOCK; break;
    case 4: d->deco |= D_ROWMAJOR; break;
    case 5: d->deco &= ~D_ROWMAJOR; break;
    case 6: d->deco |= D_ARRAYSTRIDE; d->array_stride = a; break;
    case 7: d->deco |= D_MATRIXSTRIDE; d->matrix_stride = a; break;
    case 11: d->deco |= D_BUILTIN; d->builtin = a; break;
    case 13: d->deco |= D_NOPERSP; break;
    case 14: d->deco |= D_FLAT; break;
    case 15: d->deco |= D_PATCH; break;
    case 16: d->deco |= D_CENTROID; break;
    case 17: d->deco |= D_SAMPLE; break;
    case 18: d->deco |= D_INVARIANT; break;
    case 24: d->deco |= D_NONWRITABLE; break;
    case 25: d->deco |= D_NONREADABLE; break;
    case 1: d->deco |= D_SPECID; d->spec_id = a; break;
    case 30: d->deco |= D_LOCATION; d->location = a; break;
    case 31: d->deco |= D_COMPONENT; d->component = a; break;
    case 32: d->deco |= D_INDEX; d->index = a; break;
    case 33: d->deco |= D_BINDING; d->binding = a; break;
    case 34: d->deco |= D_SET; d->set = a; break;
    case 35: d->deco |= D_OFFSET; d->offset = a; break;
    case 5300: d->deco |= D_NONUNIFORM; break;
    default: break;
    }
}

static decos *member_decos(T *t, uint32_t struct_id, uint32_t member) {
    ent *s = E(t, struct_id);
    if (s->kind != K_TYPE || s->op != OpTypeStruct || member + 1 >= s->n) return NULL;
    if (!s->mcount) {
        uint32_t n = s->n - 1;
        if (t->member_count + n > t->member_cap) {
            uint32_t cap = t->member_cap ? t->member_cap * 2 : 1024;
            while (cap < t->member_count + n) cap *= 2;
            decos *m = realloc(t->members, cap * sizeof *m);
            if (!m) { fail(t, VKMSL_OUT_OF_MEMORY, "out of memory"); return NULL; }
            t->members = m;
            t->member_cap = cap;
        }
        memset(t->members + t->member_count, 0, n * sizeof *t->members);
        s->mbase = t->member_count;
        s->mcount = n;
        t->member_count += n;
    }
    return &t->members[s->mbase + member];
}
static const decos *mdeco(T *t, uint32_t struct_id, uint32_t member) {
    static const decos none;
    ent *s = E(t, struct_id);
    if (s->kind != K_TYPE || s->op != OpTypeStruct || !s->mcount || member >= s->mcount) return &none;
    return &t->members[s->mbase + member];
}

static int add_block(T *t, uint32_t label, size_t first) {
    if (t->block_count == t->block_cap) {
        uint32_t cap = t->block_cap ? t->block_cap * 2 : 64;
        block *b = realloc(t->blocks, cap * sizeof *b);
        if (!b) { fail(t, VKMSL_OUT_OF_MEMORY, "out of memory"); return -1; }
        t->blocks = b;
        t->block_cap = cap;
    }
    block *b = &t->blocks[t->block_count];
    memset(b, 0, sizeof *b);
    b->label = label;
    b->first = first;
    E(t, label)->block = (int)t->block_count;
    E(t, label)->kind = K_LABEL;
    return (int)t->block_count++;
}

static uint32_t stage_model(uint32_t stage) { return stage; }

static int parse(T *t) {
    if (t->count < 5 || t->w[0] != 0x07230203u) { fail(t, VKMSL_INVALID, "not SPIR-V"); return 0; }
    t->bound = t->w[3];
    if (!t->bound || t->bound > (1u << 22)) { fail(t, VKMSL_INVALID, "id bound %u", t->bound); return 0; }
    t->e = calloc(t->bound, sizeof *t->e);
    if (!t->e) { fail(t, VKMSL_OUT_OF_MEMORY, "out of memory"); return 0; }
    for (uint32_t i = 0; i < t->bound; i++) t->e[i].block = -1;
    // First pass: entry point, definitions.
    uint32_t want = stage_model(t->opt->stage);
    uint32_t cur_func = 0;
    int in_entry = 0;
    int cur_block = -1;
    for (size_t pos = 5; pos < t->count;) {
        uint32_t op = t->w[pos] & 0xffff, n = t->w[pos] >> 16;
        if (!n || pos + n > t->count) { fail(t, VKMSL_INVALID, "truncated instruction at word %zu", pos); return 0; }
        const uint32_t *w = t->w + pos + 1;
        uint32_t argc = n - 1;
        switch (op) {
        case OpEntryPoint: {
            if (argc < 3 || w[0] != want || t->entry) break;
            const char *name = (const char *)(w + 2);
            size_t len = strnlen(name, argc * 4 - 8);
            if (t->opt->entry && strcmp(t->opt->entry, name)) break;
            t->entry = w[1];
            uint32_t name_words = (uint32_t)(len / 4 + 1);
            t->iface = w + 2 + name_words;
            t->iface_count = argc > 2 + name_words ? argc - 2 - name_words : 0;
            break;
        }
        case OpExtInstImport:
            if (argc >= 2 && !strcmp((const char *)(w + 1), "GLSL.std.450")) t->glsl = w[0];
            if (w[0] < t->bound) t->e[w[0]].kind = K_EXTSET;
            break;
        case OpDecorationGroup:
            if (w[0] < t->bound) t->e[w[0]].kind = K_DECOGROUP;
            break;
        case OpTypeVoid: case OpTypeBool: case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeMatrix:
        case OpTypeImage: case OpTypeSampler: case OpTypeSampledImage: case OpTypeArray: case OpTypeRuntimeArray:
        case OpTypeStruct: case OpTypePointer: case OpTypeFunction:
            if (!argc || w[0] >= t->bound) { fail(t, VKMSL_INVALID, "type"); return 0; }
            t->e[w[0]].kind = K_TYPE; t->e[w[0]].op = (uint16_t)op; t->e[w[0]].w = w; t->e[w[0]].n = (uint16_t)argc;
            break;
        case OpConstantTrue: case OpConstantFalse: case OpConstant: case OpConstantComposite: case OpConstantNull:
        case OpSpecConstantTrue: case OpSpecConstantFalse: case OpSpecConstant: case OpSpecConstantComposite:
        case OpSpecConstantOp:
            if (argc < 2 || w[1] >= t->bound) { fail(t, VKMSL_INVALID, "constant"); return 0; }
            t->e[w[1]].kind = op >= OpSpecConstantTrue ? K_SPEC : K_CONST;
            t->e[w[1]].op = (uint16_t)op; t->e[w[1]].w = w; t->e[w[1]].n = (uint16_t)argc; t->e[w[1]].type = w[0];
            break;
        case OpUndef:
            if (!cur_func && argc >= 2 && w[1] < t->bound) {
                // (a module-scope undefined value: zero)
                t->e[w[1]].kind = K_CONST; t->e[w[1]].op = OpConstantNull; t->e[w[1]].w = w; t->e[w[1]].n = 2;
                t->e[w[1]].type = w[0];
            }
            break;
        case OpVariable:
            if (argc < 3 || w[1] >= t->bound) { fail(t, VKMSL_INVALID, "variable"); return 0; }
            t->e[w[1]].kind = K_VAR; t->e[w[1]].op = (uint16_t)op; t->e[w[1]].w = w; t->e[w[1]].n = (uint16_t)argc;
            t->e[w[1]].type = w[0];
            break;
        case OpFunction:
            if (argc < 4 || w[1] >= t->bound) { fail(t, VKMSL_INVALID, "function"); return 0; }
            t->e[w[1]].kind = K_FUNC; t->e[w[1]].w = w; t->e[w[1]].n = (uint16_t)argc;
            t->e[w[1]].block = -1;
            t->e[w[1]].mbase = t->fparam_count;
            t->e[w[1]].mcount = 0;
            cur_func = w[1];
            in_entry = 1;   // (the blocks of every function: calls are inlined)
            break;
        case OpFunctionParameter:
            if (cur_func && argc >= 2) {
                if (t->fparam_count == t->fparam_cap) {
                    t->fparam_cap = t->fparam_cap ? t->fparam_cap * 2 : 64;
                    t->fparams = realloc(t->fparams, t->fparam_cap * sizeof *t->fparams);
                }
                t->fparams[t->fparam_count++] = w[1];
                t->e[cur_func].mcount++;
            }
            break;
        case OpFunctionEnd: cur_func = 0; in_entry = 0; cur_block = -1; break;
        case OpLabel:
            if (!argc || w[0] >= t->bound) { fail(t, VKMSL_INVALID, "label"); return 0; }
            t->e[w[0]].kind = K_LABEL;
            if (in_entry) {
                cur_block = add_block(t, w[0], pos + n);
                if (cur_func && t->e[cur_func].block < 0) t->e[cur_func].block = cur_block;
            }
            break;
        case OpSelectionMerge: case OpLoopMerge:
            if (in_entry && cur_block >= 0) {
                block *b = &t->blocks[cur_block];
                b->merge_op = op; b->merge = w[0]; b->cont = op == OpLoopMerge ? w[1] : 0; b->merge_pos = pos;
            }
            break;
        case OpBranch: case OpBranchConditional: case OpSwitch: case OpKill: case OpReturn: case OpReturnValue:
        case OpUnreachable: case OpTerminateInvocation:
            if (in_entry && cur_block >= 0) { t->blocks[cur_block].term = pos; cur_block = -1; }
            break;
        default:
            break;
        }
        // Results of instructions inside functions are values.
        if (cur_func && op != OpFunction && op != OpLabel && op != OpVariable) {
            // (instructions with a result type and id: operand 0 = type, operand 1 = id)
            int has_result = 0;
            switch (op) {
            case OpStore: case OpCopyMemory: case OpImageWrite: case OpControlBarrier: case OpMemoryBarrier:
            case OpAtomicStore: case OpLoopMerge: case OpSelectionMerge: case OpBranch: case OpBranchConditional:
            case OpSwitch: case OpKill: case OpReturn: case OpReturnValue: case OpUnreachable: case OpEmitVertex:
            case OpEndPrimitive: case OpLine: case OpNoLine: case OpFunctionEnd: case OpTerminateInvocation:
            case OpDemoteToHelperInvocation:
                break;
            default:
                has_result = argc >= 2;
            }
            if (op >= 4000 && op != OpSubgroupBallotKHR && op != OpSubgroupFirstInvocationKHR &&
                op != OpIsHelperInvocationEXT) has_result = 0;
            if (has_result && w[1] < t->bound && t->e[w[1]].kind == K_NONE) {
                t->e[w[1]].kind = K_VALUE; t->e[w[1]].op = (uint16_t)op; t->e[w[1]].w = w;
                t->e[w[1]].n = (uint16_t)argc; t->e[w[1]].type = w[0];
                if (op == OpPhi) t->e[w[1]].is_phi = 1;
            }
            if (op == OpFunctionParameter && w[1] < t->bound) {
                t->e[w[1]].kind = K_VALUE; t->e[w[1]].op = (uint16_t)op; t->e[w[1]].w = w; t->e[w[1]].type = w[0];
            }
        }
        pos += n;
    }
    if (!t->entry) { fail(t, VKMSL_INVALID, "no entry point for stage %u", t->opt->stage); return 0; }
    if (t->e[t->entry].block < 0) { fail(t, VKMSL_INVALID, "entry point without blocks"); return 0; }
    t->entry_block = (uint32_t)t->e[t->entry].block;
    t->model = want;
    // Second pass: decorations and execution modes (they may name ids defined later).
    for (size_t pos = 5; pos < t->count;) {
        uint32_t op = t->w[pos] & 0xffff, n = t->w[pos] >> 16;
        const uint32_t *w = t->w + pos + 1;
        uint32_t argc = n - 1;
        if (op == OpDecorate && argc >= 2 && w[0] < t->bound) apply_decoration(&t->e[w[0]].d, w[1], w + 2, argc - 2);
        else if (op == OpMemberDecorate && argc >= 3) {
            decos *m = member_decos(t, w[0], w[1]);
            if (m) apply_decoration(m, w[2], w + 3, argc - 3);
        } else if (op == OpGroupDecorate && argc >= 1) {
            // (copies the group's decorations, already applied to the group id)
            for (uint32_t i = 1; i < argc; i++)
                if (w[i] < t->bound && w[0] < t->bound) {
                    decos *d = &t->e[w[i]].d, *g = &t->e[w[0]].d;
                    uint32_t keep = d->deco;
                    *d = *g;
                    d->deco |= keep;
                }
        } else if ((op == OpExecutionMode || op == OpExecutionModeId) && argc >= 2 && w[0] == t->entry) {
            switch (w[1]) {
            case 17: if (argc >= 5) { t->local_size[0] = w[2]; t->local_size[1] = w[3]; t->local_size[2] = w[4]; } break;
            case 38: if (argc >= 5) { t->local_size_id[0] = w[2]; t->local_size_id[1] = w[3]; t->local_size_id[2] = w[4];
                                      t->has_local_size_id = 1; } break;
            case 0: t->gs_invocations = w[2]; break;
            case 19: t->gs_in_n = 1; break;
            case 20: t->gs_in_n = 2; break;
            case 21: t->gs_in_n = 4; break;
            case 22: t->gs_in_n = 3; break;
            case 23: t->gs_in_n = 6; break;
            case 26: t->gs_maxt = w[2]; break;   // (OutputVertices: made triangles below)
            case 27: t->gs_out_prim = 1; break;
            case 28: t->gs_out_prim = 2; break;
            case 29: t->gs_out_prim = 3; break;
            case 8: t->origin_lower_left = 1; break;
            case 9: t->early_tests = 1; break;
            case 12: if (!t->depth_mode) t->depth_mode = 1; break;
            case 14: t->depth_mode = 2; break;
            case 15: t->depth_mode = 3; break;
            default: break;
            }
        }
        pos += n;
    }
    return !t->rc;
}

// ---- buffer memory ----------------------------------------------------------------------------------------------
static uint32_t array_length(T *t, uint32_t type) {
    int ok = 1;
    uint32_t n = type_op(t, type) == OpTypeArray ? const_u32(t, tw(t, type)[2], &ok) : 0;
    return ok ? n : 0;
}
static uint32_t buffer_size(T *t, uint32_t id, uint32_t matrix_stride, int row_major) {
    switch (type_op(t, id)) {
    case OpTypeBool: return 4;
    case OpTypeInt: case OpTypeFloat: return tw(t, id)[1] / 8;
    case OpTypeVector: return vec_size(t, id) * buffer_size(t, tw(t, id)[1], 0, 0);
    case OpTypeMatrix: {
        uint32_t cols = tw(t, id)[2], rows = vec_size(t, tw(t, id)[1]);
        return (row_major ? rows : cols) * matrix_stride;
    }
    case OpTypeArray: return array_length(t, id) * E(t, id)->d.array_stride;
    case OpTypeRuntimeArray: return 0;
    case OpTypeStruct: {
        uint32_t n = E(t, id)->n - 1, end = 0;
        for (uint32_t i = 0; i < n; i++) {
            const decos *m = mdeco(t, id, i);
            uint32_t e = m->offset + buffer_size(t, tw(t, id)[i + 1], m->matrix_stride, (m->deco & D_ROWMAJOR) != 0);
            if (e > end) end = e;
        }
        return end;
    }
    default: return 0;
    }
}

static uint32_t gcd32(uint32_t a, uint32_t b) { while (b) { uint32_t r = a % b; a = b; b = r; } return a; }
static const char *packed_prefix(void) { return "packed_"; }

// A scalar or vector load from a buffer at base + offset. The vector types of MSL are aligned to their size (a
// float3 to 16), so a vector whose address is not known to be that aligned is read through its packed type.
static char *buffer_scalar_load(T *t, const pinfo *p, uint32_t type, const char *off, uint32_t align) {
    const char *as = p->device ? "device" : "constant";
    uint32_t s = scalar_of(t, type), n = vec_size(t, type);
    int is_b = is_bool(t, s);
    const char *sname = is_b ? "uint" : scalar_name(t, s);
    uint32_t sw = is_b ? 4 : scalar_width(t, s) / 8;
    char *x;
    if (n == 1) x = xprintf("(*(%s %s*)(%s + %s))", as, sname, p->base, off);
    else {
        uint32_t natural = (n == 3 ? 4 : n) * sw;
        if (n != 3 && align >= natural && sw >= 2) x = xprintf("(*(%s %s%u*)(%s + %s))", as, sname, n, p->base, off);
        else if (sw >= 2) x = xprintf("%s%u(*(%s %s%s%u*)(%s + %s))", sname, n, as, packed_prefix(), sname, n, p->base, off);
        else {
            // (8-bit vectors: component by component)
            sb v = { 0 };
            sb_printf(&v, "%s%u(", sname, n);
            for (uint32_t i = 0; i < n; i++)
                sb_printf(&v, "%s*(%s %s*)(%s + %s + %u)", i ? ", " : "", as, sname, p->base, off, i * sw);
            sb_puts(&v, ")");
            x = v.p;
        }
    }
    if (is_b) { char *y = n == 1 ? xprintf("(%s != 0u)", x) : xprintf("(%s != uint%u(0))", x, n); free(x); x = y; }
    return x;
}

// Writes into dst (an lvalue) the value of type `type` stored in a buffer at p->base + off.
static void buffer_load_into(T *t, const pinfo *p, uint32_t type, const char *dst, const char *off, uint32_t align,
                             uint32_t mstride, int row_major, int col_of_rm) {
    int op = type_op(t, type);
    if (col_of_rm && op == OpTypeVector) {
        uint32_t n = vec_size(t, type), s = scalar_of(t, type);
        EMIT("%s = %s%u(", dst, scalar_name(t, s), n);
        for (uint32_t i = 0; i < n; i++) {
            char *o = xprintf("(%s + %uu)", off, i * mstride);
            char *x = buffer_scalar_load(t, p, s, o, 4);
            sb_printf(&t->body, "%s%s", i ? ", " : "", x);
            free(x); free(o);
        }
        sb_puts(&t->body, ");\n");
        return;
    }
    switch (op) {
    case OpTypeBool: case OpTypeInt: case OpTypeFloat: case OpTypeVector: {
        char *x = buffer_scalar_load(t, p, type, off, align);
        EMIT("%s = %s;\n", dst, x);
        free(x);
        return;
    }
    case OpTypeMatrix: {
        uint32_t cols = tw(t, type)[2], col = tw(t, type)[1], rows = vec_size(t, col), s = scalar_of(t, col);
        uint32_t sw = scalar_width(t, s) / 8;
        for (uint32_t c = 0; c < cols; c++) {
            char *d = xprintf("%s[%u]", dst, c);
            if (!row_major) {
                char *o = xprintf("(%s + %uu)", off, c * mstride);
                buffer_load_into(t, p, col, d, o, gcd32(align, mstride ? mstride : 16), 0, 0, 0);
                free(o);
            } else {
                EMIT("%s = %s%u(", d, scalar_name(t, s), rows);
                for (uint32_t r = 0; r < rows; r++) {
                    char *o = xprintf("(%s + %uu)", off, r * mstride + c * sw);
                    char *x = buffer_scalar_load(t, p, s, o, 4);
                    sb_printf(&t->body, "%s%s", r ? ", " : "", x);
                    free(x); free(o);
                }
                sb_puts(&t->body, ");\n");
            }
            free(d);
        }
        return;
    }
    case OpTypeArray: {
        uint32_t n = array_length(t, type), stride = E(t, type)->d.array_stride, elem = tw(t, type)[1];
        if (!stride) { fail(t, VKMSL_UNSUPPORTED, "buffer array %u without ArrayStride", type); return; }
        if (n <= 4) {
            for (uint32_t i = 0; i < n; i++) {
                char *d = xprintf("%s[%u]", dst, i), *o = xprintf("(%s + %uu)", off, i * stride);
                buffer_load_into(t, p, elem, d, o, gcd32(align, stride), mstride, row_major, 0);
                free(d); free(o);
            }
        } else {
            int k = t->tmp_count++;
            EMIT("for (uint _i%d = 0; _i%d < %uu; _i%d++) {\n", k, k, n, k);
            t->indent++;
            char *d = xprintf("%s[_i%d]", dst, k), *o = xprintf("(%s + _i%d * %uu)", off, k, stride);
            buffer_load_into(t, p, elem, d, o, gcd32(align, stride), mstride, row_major, 0);
            free(d); free(o);
            t->indent--;
            EMIT("}\n");
        }
        return;
    }
    case OpTypeStruct: {
        uint32_t n = E(t, type)->n - 1;
        for (uint32_t i = 0; i < n; i++) {
            const decos *m = mdeco(t, type, i);
            uint32_t mt = tw(t, type)[i + 1];
            if (type_op(t, mt) == OpTypeRuntimeArray) continue;
            char *d = xprintf("%s.m%u", dst, i), *o = xprintf("(%s + %uu)", off, m->offset);
            buffer_load_into(t, p, mt, d, o, gcd32(align, m->offset ? m->offset : 64), m->matrix_stride,
                             (m->deco & D_ROWMAJOR) != 0, 0);
            free(d); free(o);
        }
        return;
    }
    default:
        fail(t, VKMSL_UNSUPPORTED, "load of type %u from a buffer", type);
    }
}

static void buffer_scalar_store(T *t, const pinfo *p, uint32_t type, const char *off, uint32_t align, const char *v) {
    uint32_t s = scalar_of(t, type), n = vec_size(t, type);
    int is_b = is_bool(t, s);
    const char *sname = is_b ? "uint" : scalar_name(t, s);
    uint32_t sw = is_b ? 4 : scalar_width(t, s) / 8;
    char *val = is_b ? (n == 1 ? xprintf("uint(%s)", v) : xprintf("uint%u(%s)", n, v)) : xstrdup(v);
    if (n == 1) EMIT("*(device %s*)(%s + %s) = %s;\n", sname, p->base, off, val);
    else if (n != 3 && align >= n * sw && sw >= 2) EMIT("*(device %s%u*)(%s + %s) = %s;\n", sname, n, p->base, off, val);
    else if (sw >= 2) EMIT("*(device packed_%s%u*)(%s + %s) = packed_%s%u(%s);\n", sname, n, p->base, off, sname, n, val);
    else for (uint32_t i = 0; i < n; i++) EMIT("*(device %s*)(%s + %s + %u) = (%s)[%u];\n", sname, p->base, off, i * sw, val, i);
    free(val);
}
static void buffer_store_from(T *t, const pinfo *p, uint32_t type, const char *src, const char *off, uint32_t align,
                              uint32_t mstride, int row_major, int col_of_rm) {
    if (!p->device) { fail(t, VKMSL_INVALID, "store to a read-only buffer"); return; }
    int op = type_op(t, type);
    if (col_of_rm && op == OpTypeVector) {
        uint32_t n = vec_size(t, type), s = scalar_of(t, type);
        for (uint32_t i = 0; i < n; i++) {
            char *o = xprintf("(%s + %uu)", off, i * mstride), *v = xprintf("(%s)[%u]", src, i);
            buffer_scalar_store(t, p, s, o, 4, v);
            free(o); free(v);
        }
        return;
    }
    switch (op) {
    case OpTypeBool: case OpTypeInt: case OpTypeFloat: case OpTypeVector:
        buffer_scalar_store(t, p, type, off, align, src);
        return;
    case OpTypeMatrix: {
        uint32_t cols = tw(t, type)[2], col = tw(t, type)[1], rows = vec_size(t, col), s = scalar_of(t, col);
        uint32_t sw = scalar_width(t, s) / 8;
        for (uint32_t c = 0; c < cols; c++) {
            if (!row_major) {
                char *o = xprintf("(%s + %uu)", off, c * mstride), *v = xprintf("(%s)[%u]", src, c);
                buffer_store_from(t, p, col, v, o, gcd32(align, mstride ? mstride : 16), 0, 0, 0);
                free(o); free(v);
            } else for (uint32_t r = 0; r < rows; r++) {
                char *o = xprintf("(%s + %uu)", off, r * mstride + c * sw), *v = xprintf("(%s)[%u][%u]", src, c, r);
                buffer_scalar_store(t, p, s, o, 4, v);
                free(o); free(v);
            }
        }
        return;
    }
    case OpTypeArray: {
        uint32_t n = array_length(t, type), stride = E(t, type)->d.array_stride, elem = tw(t, type)[1];
        int k = t->tmp_count++;
        EMIT("for (uint _i%d = 0; _i%d < %uu; _i%d++) {\n", k, k, n, k);
        t->indent++;
        char *v = xprintf("(%s)[_i%d]", src, k), *o = xprintf("(%s + _i%d * %uu)", off, k, stride);
        buffer_store_from(t, p, elem, v, o, gcd32(align, stride), mstride, row_major, 0);
        free(v); free(o);
        t->indent--;
        EMIT("}\n");
        return;
    }
    case OpTypeStruct: {
        uint32_t n = E(t, type)->n - 1;
        for (uint32_t i = 0; i < n; i++) {
            const decos *m = mdeco(t, type, i);
            char *v = xprintf("(%s).m%u", src, i), *o = xprintf("(%s + %uu)", off, m->offset);
            buffer_store_from(t, p, tw(t, type)[i + 1], v, o, gcd32(align, m->offset ? m->offset : 64),
                              m->matrix_stride, (m->deco & D_ROWMAJOR) != 0, 0);
            free(v); free(o);
        }
        return;
    }
    default:
        fail(t, VKMSL_UNSUPPORTED, "store of type %u to a buffer", type);
    }
}

// ---- descriptors ------------------------------------------------------------------------------------------------

static const vkmsl_binding *find_binding(T *t, uint32_t set, uint32_t binding) {
    for (uint32_t i = 0; i < t->opt->binding_count; i++)
        if (t->opt->bindings[i].set == set && t->opt->bindings[i].binding == binding) return &t->opt->bindings[i];
    return NULL;
}

// The MSL texture type for a SPIR-V image type. access: 0 sampled, else 1 read, 2 write, 3 read_write.
static char *texture_type(T *t, uint32_t image, int access, int force_depth) {
    const uint32_t *w = tw(t, image);
    if (type_op(t, image) != OpTypeImage) { fail(t, VKMSL_INVALID, "%u is not an image type", image); return xstrdup("texture2d<float>"); }
    uint32_t sampled = w[1], dim = w[2], depth = w[3], arrayed = w[4], ms = w[5];
    const char *st = scalar_name(t, sampled);
    if (!st || !strcmp(st, "void")) st = "float";
    if (!strcmp(st, "half")) st = "half";
    const char *acc = access == 1 ? ", access::read" : access == 2 ? ", access::write" : access == 3 ? ", access::read_write" : "";
    if ((depth == 1 || force_depth) && !access && (dim == DIM_2D || dim == DIM_Cube) && !strcmp(st, "float")) {
        if (dim == DIM_Cube) return xstrdup(arrayed ? "depthcube_array<float>" : "depthcube<float>");
        if (ms) return xstrdup(arrayed ? "depth2d_ms_array<float>" : "depth2d_ms<float>");
        return xstrdup(arrayed ? "depth2d_array<float>" : "depth2d<float>");
    }
    switch (dim) {
    case DIM_1D: return xprintf(arrayed ? "texture1d_array<%s%s>" : "texture1d<%s%s>", st, acc);
    case DIM_2D: case DIM_Rect: case DIM_SubpassData:
        if (ms) return xprintf(arrayed ? "texture2d_ms_array<%s%s>" : "texture2d_ms<%s%s>", st, acc);
        return xprintf(arrayed ? "texture2d_array<%s%s>" : "texture2d<%s%s>", st, acc);
    case DIM_3D: return xprintf("texture3d<%s%s>", st, acc);
    case DIM_Cube: return xprintf(arrayed ? "texturecube_array<%s%s>" : "texturecube<%s%s>", st, acc);
    case DIM_Buffer: return xprintf("texture_buffer<%s%s>", st, access ? (access == 2 ? ", access::write" : access == 3 ? ", access::read_write" : ", access::read") : "");
    default: fail(t, VKMSL_UNSUPPORTED, "image dimension %u", dim); return xstrdup("texture2d<float>");
    }
}

static void add_dmember(T *t, uint32_t set, uint32_t word, uint32_t words, char *decl) {
    for (uint32_t i = 0; i < t->dm_count; i++)
        if (t->dm[i].set == set && t->dm[i].word == word) { free(decl); return; }
    if (t->dm_count == t->dm_cap) {
        uint32_t cap = t->dm_cap ? t->dm_cap * 2 : 32;
        dmember *m = realloc(t->dm, cap * sizeof *m);
        if (!m) { fail(t, VKMSL_OUT_OF_MEMORY, "out of memory"); free(decl); return; }
        t->dm = m; t->dm_cap = cap;
    }
    t->dm[t->dm_count++] = (dmember){ set, word, words, decl };
    if (set < VKMSL_MAX_SETS) t->set_used[set] = 1;
}

static pinfo *new_ptr(T *t, uint32_t id, int kind) {
    pinfo *p = calloc(1, sizeof *p);
    if (!p) { fail(t, VKMSL_OUT_OF_MEMORY, "out of memory"); return NULL; }
    p->kind = kind;
    E(t, id)->ptr = p;
    return p;
}

// Element type and count of a variable's pointee when it is an array of descriptors.
static uint32_t descriptor_element(T *t, uint32_t type, uint32_t *count, int *arrayed) {
    *arrayed = 0;
    *count = 1;
    int op = type_op(t, type);
    if (op == OpTypeArray) { *arrayed = 1; *count = array_length(t, type); return tw(t, type)[1]; }
    if (op == OpTypeRuntimeArray) { *arrayed = 2; *count = 0; return tw(t, type)[1]; }
    return type;
}

static void setup_resource_variable(T *t, uint32_t var) {
    ent *v = E(t, var);
    uint32_t sc = v->w[2], pointee = tw(t, v->type)[2];
    if (sc == SC_PushConstant) {
        pinfo *p = new_ptr(t, var, P_BUFFER);
        if (!p) return;
        p->pointee = pointee; p->base = xstrdup("spvPC"); p->offset = xstrdup("0u"); p->align = 16;
        p->storage = sc; p->var = var;
        t->res->uses_push_constants = 1;
        return;
    }
    if (!(v->d.deco & D_BINDING)) { fail(t, VKMSL_INVALID, "resource variable %u has no binding", var); return; }
    uint32_t set = v->d.set, binding = v->d.binding;
    const vkmsl_binding *b = find_binding(t, set, binding);
    if (!b) { fail(t, VKMSL_INVALID, "set %u binding %u is not in the pipeline layout", set, binding); return; }
    if (set >= VKMSL_MAX_SETS) { fail(t, VKMSL_UNSUPPORTED, "descriptor set %u", set); return; }
    uint32_t count = 1;
    int arrayed = 0;
    uint32_t elem = descriptor_element(t, pointee, &count, &arrayed);
    if (arrayed == 2 || !count) count = b->count ? b->count : 1;
    char arr[32] = "";
    int is_array = arrayed != 0;
    int eop = type_op(t, elem);
    if (sc == SC_Uniform || sc == SC_StorageBuffer) {
        int device = sc == SC_StorageBuffer || (E(t, elem)->d.deco & D_BUFFERBLOCK);
        const char *bt = device ? "spvBufD" : "spvBufC";
        char *decl = is_array ? xprintf("array<%s, %u> b%u", bt, count, binding) : xprintf("%s b%u", bt, binding);
        add_dmember(t, set, b->word, 2 * (is_array ? count : 1), decl);
        pinfo *p = new_ptr(t, var, is_array ? P_HANDLE : P_BUFFER);
        if (!p) return;
        p->pointee = pointee; p->storage = sc; p->var = var; p->device = (uint8_t)device; p->align = 16;
        char *dyn = NULL;
        if (b->dynamic != VKMSL_NO_DYNAMIC) { t->res->uses_aux = 1; dyn = xprintf("spvAux[%uu", b->dynamic); }
        if (is_array) {
            // (P_HANDLE of buffers: the first access-chain index picks the descriptor)
            p->lvalue = xprintf("ds%u.b%u", set, binding);
            p->coord = dyn ? xstrdup(dyn) : NULL;   // dynamic offsets: spvAux[first + index]
            p->sample = xstrdup("buffer");
        } else {
            p->base = dyn ? xprintf("(ds%u.b%u.p + %s])", set, binding, dyn) : xprintf("ds%u.b%u.p", set, binding);
            p->offset = xstrdup("0u");
        }
        free(dyn);
        return;
    }
    // UniformConstant: images, samplers, combined image samplers, texel buffers.
    snprintf(arr, sizeof arr, "%u", count);
    pinfo *p = new_ptr(t, var, P_HANDLE);
    if (!p) return;
    p->pointee = pointee; p->storage = sc; p->var = var;
    if (eop == OpTypeSampler) {
        add_dmember(t, set, b->type == VK_DT_COMBINED ? b->word + count : b->word, count,
                    is_array ? xprintf("array<sampler, %u> s%u", count, binding) : xprintf("sampler s%u", binding));
        p->lvalue = xprintf("ds%u.s%u", set, binding);
        return;
    }
    uint32_t image = eop == OpTypeSampledImage ? tw(t, elem)[1] : elem;
    if (type_op(t, image) != OpTypeImage) { fail(t, VKMSL_UNSUPPORTED, "descriptor of type %u", elem); return; }
    int access = 0;
    if (tw(t, image)[6] == 2 && tw(t, image)[2] != DIM_SubpassData) {   // (storage image or storage texel buffer)
        int nr = (v->d.deco & D_NONREADABLE) != 0, nw = (v->d.deco & D_NONWRITABLE) != 0;
        access = nw ? 1 : nr ? 2 : 3;
    }
    // (HLSL's SPIR-V declares comparison-sampled textures without Depth: their use decides)
    const uint32_t *iw = tw(t, image);
    int depth = !access && (iw[2] == DIM_2D || iw[2] == DIM_Cube) && is_float(t, iw[1]) &&
                (iw[3] == 1 || (v->d.deco & D_DEPTHCMP));
    p->depth = (uint8_t)depth;
    char *tt = texture_type(t, image, access, depth);
    add_dmember(t, set, b->word, count, is_array ? xprintf("array<%s, %u> t%u", tt, count, binding) :
                                                   xprintf("%s t%u", tt, binding));
    free(tt);
    p->lvalue = xprintf("ds%u.t%u", set, binding);
    if (eop == OpTypeSampledImage) {
        if (b->type == VK_DT_COMBINED)
            add_dmember(t, set, b->word + count, count,
                        is_array ? xprintf("array<sampler, %u> s%u", count, binding) : xprintf("sampler s%u", binding));
        p->base = xprintf("ds%u.s%u", set, binding);
    }
}

// ---- stage interface --------------------------------------------------------------------------------------------
static int is_interface(T *t, uint32_t var) {
    for (uint32_t i = 0; i < t->iface_count; i++) if (t->iface[i] == var) return 1;
    return 0;
}

static const char *interp_attr(uint32_t deco) {
    if (deco & D_FLAT) return ", flat";
    if (deco & D_NOPERSP) return deco & D_CENTROID ? ", centroid_no_perspective" :
                                 deco & D_SAMPLE ? ", sample_no_perspective" : ", center_no_perspective";
    if (deco & D_CENTROID) return ", centroid_perspective";
    if (deco & D_SAMPLE) return ", sample_perspective";
    return "";
}

static const char *swizzle_n(uint32_t n) { return n == 1 ? ".x" : n == 2 ? ".xy" : n == 3 ? ".xyz" : ""; }
static char *widen4(T *t, uint32_t type, const char *v) {
    uint32_t n = vec_size(t, type);
    const char *z = is_bool(t, scalar_of(t, type)) ? "false" : "0";
    if (n == 4) return xprintf("%s(%s)", vec4_of(t, type), v);
    if (n == 3) return xprintf("%s(%s, %s)", vec4_of(t, type), v, z);
    if (n == 2) return xprintf("%s(%s, %s, %s)", vec4_of(t, type), v, z, z);
    return xprintf("%s(%s, %s, %s, %s)", vec4_of(t, type), v, z, z, z);
}

// A value as a float4 slot (integers keep their bits), and back.
static char *to_slot(T *t, uint32_t type, const char *v) {
    uint32_t s = scalar_of(t, type);
    char *w = widen4(t, type, v);
    if (is_float(t, s)) { char *x = xprintf("float4(%s)", w); free(w); return x; }
    char *x = is_bool(t, s) ? xprintf("float4(%s)", w) : xprintf("as_type<float4>(%s)", w);
    free(w);
    return x;
}
static char *from_slot(T *t, uint32_t type, const char *slot) {
    uint32_t s = scalar_of(t, type), n = vec_size(t, type);
    if (is_float(t, s)) return xprintf("%s(%s%s)", tname(t, type), slot, swizzle_n(n));
    if (is_bool(t, s)) return xprintf("%s((%s)%s != 0.0f)", tname(t, type), slot, swizzle_n(n));
    return xprintf("%s(as_type<%s>(%s)%s)", tname(t, type), vec4_of(t, type), slot, swizzle_n(n));
}
// Packs (dir 0) or unpacks (dir 1) a user varying's leaves at their locations: slot base + location.
static void slot_leaves(T *t, uint32_t type, const char *path, uint32_t *loc, sb *out, const char *base, int dir, uint32_t *max_loc) {
    int op = type_op(t, type);
    if (op == OpTypeMatrix || op == OpTypeArray) {
        uint32_t n = op == OpTypeMatrix ? tw(t, type)[2] : array_length(t, type), elem = tw(t, type)[1];
        for (uint32_t i = 0; i < n; i++) { char *p = xprintf("%s[%u]", path, i); slot_leaves(t, elem, p, loc, out, base, dir, max_loc); free(p); }
        return;
    }
    if (op == OpTypeStruct) {
        for (uint32_t i = 0; i + 1 < E(t, type)->n; i++) {
            const decos *m = mdeco(t, type, i);
            if (m->deco & D_LOCATION) *loc = m->location;
            char *p = xprintf("%s.m%u", path, i);
            slot_leaves(t, tw(t, type)[i + 1], p, loc, out, base, dir, max_loc);
            free(p);
        }
        return;
    }
    uint32_t L = (*loc)++;
    if (L + 1 > *max_loc) *max_loc = L + 1;
    char *slot = xprintf("%s[%u]", base, 1 + L);
    char *x = dir == 0 ? to_slot(t, type, path) : from_slot(t, type, slot);
    if (dir == 0) sb_printf(out, "    %s = %s;\n", slot, x);
    else sb_printf(out, "    %s = %s;\n", path, x);
    free(x); free(slot);
}

// Walks a user varying's type, giving each scalar/vector leaf its location: matrices take a location per column,
// arrays per element, structs per member (a member's own Location wins).
typedef struct { sb *decl; sb *init; int input; uint32_t deco, index; } leafctx;
static void varying_leaves(T *t, leafctx *c, uint32_t type, const char *path, uint32_t *loc) {
    int op = type_op(t, type);
    if (op == OpTypeMatrix || op == OpTypeArray) {
        uint32_t n = op == OpTypeMatrix ? tw(t, type)[2] : array_length(t, type), elem = tw(t, type)[1];
        for (uint32_t i = 0; i < n; i++) {
            char *p = xprintf("%s[%u]", path, i);
            varying_leaves(t, c, elem, p, loc);
            free(p);
        }
        return;
    }
    if (op == OpTypeStruct) {
        for (uint32_t i = 0; i + 1 < E(t, type)->n; i++) {
            const decos *m = mdeco(t, type, i);
            if (m->deco & D_LOCATION) *loc = m->location;
            char *p = xprintf("%s.m%u", path, i);
            uint32_t save = c->deco;
            c->deco |= m->deco & (D_FLAT | D_NOPERSP | D_CENTROID | D_SAMPLE);
            varying_leaves(t, c, tw(t, type)[i + 1], p, loc);
            c->deco = save;
            free(p);
        }
        return;
    }
    if (op != OpTypeVector && op != OpTypeFloat && op != OpTypeInt && op != OpTypeBool) {
        fail(t, VKMSL_UNSUPPORTED, "varying of type %u", type);
        return;
    }
    uint32_t L = (*loc)++;
    uint32_t n = vec_size(t, type);
    if (t->model == VKMSL_STAGE_VERTEX && c->input) {
        sb_printf(c->decl, "    %s a%u [[attribute(%u)]];\n", tname(t, type), L, L);
        sb_printf(c->init, "    %s = spvIn.a%u;\n", path, L);
        t->res->vertex_inputs |= 1u << (L & 31);
    } else if (t->model == VKMSL_STAGE_FRAGMENT && c->input) {
        int integer = !is_float(t, scalar_of(t, type));
        sb_printf(c->decl, "    %s v%u [[user(locn%u)%s]];\n", vec4_of(t, type), L, L,
                  integer ? ", flat" : interp_attr(c->deco));
        sb_printf(c->init, "    %s = %s(spvIn.v%u%s);\n", path, tname(t, type), L, swizzle_n(n));
    } else if (t->model == VKMSL_STAGE_VERTEX) {
        sb_printf(c->decl, "    %s v%u [[user(locn%u)]];\n", vec4_of(t, type), L, L);
        char *wv = widen4(t, type, path);
        sb_printf(c->init, "    spvOut.v%u = %s;\n", L, wv);
        free(wv);
    } else if (t->model == VKMSL_STAGE_FRAGMENT) {
        // Always four components: Metal refuses a pipeline whose color output has fewer than the attachment's format
        // (Vulkan leaves the missing ones undefined); the extra ones go nowhere.
        uint32_t index = c->index;
        if (index) sb_printf(c->decl, "    %s c%u_1 [[color(%u), index(1)]];\n", vec4_of(t, type), L, L);
        else sb_printf(c->decl, "    %s c%u [[color(%u)]];\n", vec4_of(t, type), L, L);
        char *wv = widen4(t, type, path);
        sb_printf(c->init, "    spvOut.c%u%s = %s;\n", L, index ? "_1" : "", wv);
        free(wv);
        t->res->fragment_outputs |= 1u << (L & 31);
    }
}

// ---- values -----------------------------------------------------------------------------------------------------
static uint32_t vtype(T *t, uint32_t id) {
    ent *e = E(t, id);
    return e->kind == K_CONST || e->kind == K_SPEC ? e->w[0] : e->type;
}
static int opaque_type(T *t, uint32_t type) {
    int op = type_op(t, type);
    return op == OpTypeImage || op == OpTypeSampler || op == OpTypeSampledImage || op == OpTypePointer || op == OpTypeVoid;
}
static void declare(T *t, uint32_t id) {
    ent *e = E(t, id);
    if (e->used) return;
    e->used = 1;
    if (opaque_type(t, e->type)) return;
    sb_printf(&t->vars, "    %s _%u;\n", tname(t, e->type), id);
}
static const char *dst(T *t, uint32_t id) { declare(t, id); return value(t, id); }

// OpCopyLogical: a value of one struct or array type into another of the same shape (a buffer's laid-out struct
// passed as a function's plain one), member by member.
static void copy_logical(T *t, const char *d, uint32_t dt, const char *src, uint32_t st, int depth) {
    if (dt == st || (type_op(t, dt) != OpTypeStruct && type_op(t, dt) != OpTypeArray) || type_op(t, dt) != type_op(t, st)) {
        EMIT("%s = %s;\n", d, src);
        return;
    }
    ent *de = E(t, dt), *se = E(t, st);
    if (type_op(t, dt) == OpTypeStruct) {
        for (uint32_t i = 1; i < de->n && i < se->n; i++) {
            char *dm = xprintf("%s.m%u", d, i - 1), *sm = xprintf("(%s).m%u", src, i - 1);
            copy_logical(t, dm, de->w[i], sm, se->w[i], depth);
            free(dm); free(sm);
        }
        return;
    }
    int ok = 1;
    uint32_t n = const_u32(t, de->w[2], &ok);
    EMIT("for (uint _cl%d = 0; _cl%d < %uu; _cl%d++) {\n", depth, depth, n, depth);
    char *dm = xprintf("%s[_cl%d]", d, depth), *sm = xprintf("(%s)[_cl%d]", src, depth);
    copy_logical(t, dm, de->w[1], sm, se->w[1], depth + 1);
    free(dm); free(sm);
    EMIT("}\n");
}

// The same-shape type with the given integer signedness, as an MSL name.
static char *int_type(T *t, uint32_t type, int s) {
    uint32_t n = vec_size(t, type), w = scalar_width(t, type);
    return n > 1 ? xprintf("%s%u", int_name(w, s), n) : xstrdup(int_name(w, s));
}
// `v` as the signedness s (a no-op when it already is).
static char *as_sign(T *t, uint32_t id, int s) {
    uint32_t type = vtype(t, id);
    if (!is_int(t, scalar_of(t, type)) || is_signed(t, type) == s) return xstrdup(value(t, id));
    char *n = int_type(t, type, s);
    char *x = xprintf("%s(%s)", n, value(t, id));
    free(n);
    return x;
}
// `v` converted to the type `type` when their MSL names differ.
static char *as_type_of(T *t, uint32_t id, uint32_t type) {
    if (!strcmp(tname(t, vtype(t, id)), tname(t, type))) return xstrdup(value(t, id));
    return xprintf("%s(%s)", tname(t, type), value(t, id));
}

static char *index_expr(T *t, uint32_t id) {
    int ok = 1;
    ent *e = E(t, id);
    if (e->kind == K_CONST) { uint32_t v = const_u32(t, id, &ok); if (ok) return xprintf("%u", v); }
    return is_signed(t, vtype(t, id)) || scalar_width(t, vtype(t, id)) != 32 ?
        xprintf("uint(%s)", value(t, id)) : xstrdup(value(t, id));
}

static void need(T *t, int helper) { t->helper[helper] = 1; }
enum { H_FMOD = 1, H_SMOD, H_INVERSE, H_FINDLSB, H_FINDSMSB, H_FINDUMSB, H_SSIGN, H_OUTER, H_BALLOT, H_REFLECT1,
       H_REFRACT1, H_FACEFWD1, H_MODFSTRUCT, H_FREXPSTRUCT, H_UMULEXT, H_SMULEXT, H_ADDCARRY, H_SUBBORROW };

// ---- access chains ----------------------------------------------------------------------------------------------
static pinfo *copy_ptr(const pinfo *s) {
    pinfo *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    *p = *s;
    p->lvalue = s->lvalue ? xstrdup(s->lvalue) : NULL;
    p->base = s->base ? xstrdup(s->base) : NULL;
    p->offset = s->offset ? xstrdup(s->offset) : NULL;
    p->coord = s->coord ? xstrdup(s->coord) : NULL;
    p->sample = s->sample ? xstrdup(s->sample) : NULL;
    return p;
}

static void mark_member_use(T *t, uint32_t var, uint32_t member) {
    if (member < 8) E(t, var)->written |= (uint8_t)(1u << member);
}

static void access_chain(T *t, const uint32_t *w, uint32_t n) {
    uint32_t id = w[1], base = w[2];
    pinfo *b = E(t, base)->ptr;
    if (!b) { fail(t, VKMSL_UNSUPPORTED, "access chain %u on %u (not a known pointer)", id, base); return; }
    pinfo *p = copy_ptr(b);
    if (!p) { fail(t, VKMSL_OUT_OF_MEMORY, "out of memory"); return; }
    E(t, id)->ptr = p;
    uint32_t ct = p->pointee;
    uint32_t first = 3;
    if (p->kind == P_HANDLE) {
        if (n <= 3) return;
        char *i0 = index_expr(t, w[3]);
        uint32_t elem = tw(t, ct)[1];
        if (p->sample && !strcmp(p->sample, "buffer")) {
            char *pb = p->coord ? xprintf("(%s[%s].p + %s + %s])", p->lvalue, i0, p->coord, i0) : xprintf("%s[%s].p", p->lvalue, i0);
            free(p->base); p->base = pb;
            free(p->offset); p->offset = xstrdup("0u");
            free(p->sample); p->sample = NULL;
            free(p->coord); p->coord = NULL;
            p->kind = P_BUFFER;
            p->pointee = elem;
            ct = elem;
            first = 4;
        } else {
            char *l = xprintf("%s[%s]", p->lvalue, i0);
            free(p->lvalue); p->lvalue = l;
            if (p->base) { char *s = xprintf("%s[%s]", p->base, i0); free(p->base); p->base = s; }
            p->pointee = elem;
            free(i0);
            if (n > 4) fail(t, VKMSL_UNSUPPORTED, "access chain past a descriptor");
            return;
        }
        free(i0);
    }
    if (p->kind == P_LVALUE) {
        sb l = { 0 };
        sb_puts(&l, p->lvalue);
        for (uint32_t i = first; i < n; i++) {
            int op = type_op(t, ct);
            if (op == OpTypeStruct) {
                int ok = 1;
                uint32_t m = const_u32(t, w[i], &ok);
                if (i == first) mark_member_use(t, p->var, m);
                sb_printf(&l, ".m%u", m);
                ct = tw(t, ct)[m + 1];
            } else {
                char *ix = index_expr(t, w[i]);
                sb_printf(&l, "[%s]", ix);
                free(ix);
                ct = op == OpTypeVector ? tw(t, ct)[1] : tw(t, ct)[1];
            }
        }
        free(p->lvalue);
        p->lvalue = l.p ? l.p : xstrdup("");
        p->pointee = ct;
        return;
    }
    if (p->kind != P_BUFFER) { fail(t, VKMSL_UNSUPPORTED, "access chain kind %d", p->kind); return; }
    // Buffer: byte offsets.
    uint32_t konst = 0, align = p->align;
    sb dyn = { 0 };
    for (uint32_t i = first; i < n; i++) {
        int op = type_op(t, ct);
        int ok = 1;
        uint32_t ci = E(t, w[i])->kind == K_CONST ? const_u32(t, w[i], &ok) : 0;
        int is_const = E(t, w[i])->kind == K_CONST && ok;
        if (op == OpTypeStruct) {
            if (!is_const) { fail(t, VKMSL_INVALID, "dynamic struct index"); break; }
            const decos *m = mdeco(t, ct, ci);
            konst += m->offset;
            p->matrix_stride = m->matrix_stride;
            p->row_major = (m->deco & D_ROWMAJOR) != 0;
            ct = tw(t, ct)[ci + 1];
            continue;
        }
        uint32_t stride;
        uint32_t next;
        if (op == OpTypeArray || op == OpTypeRuntimeArray) { stride = E(t, ct)->d.array_stride; next = tw(t, ct)[1]; }
        else if (op == OpTypeMatrix) {
            next = tw(t, ct)[1];
            if (p->row_major) { stride = scalar_width(t, next) / 8; p->column_of_row_major = 1; }
            else stride = p->matrix_stride;
        } else if (op == OpTypeVector) {
            next = tw(t, ct)[1];
            stride = p->column_of_row_major ? p->matrix_stride : scalar_width(t, next) / 8;
            p->column_of_row_major = 0;
        } else { fail(t, VKMSL_INVALID, "access chain into a scalar"); break; }
        if (!stride) { fail(t, VKMSL_UNSUPPORTED, "buffer type %u without a stride", ct); break; }
        if (is_const) konst += ci * stride;
        else {
            char *ix = index_expr(t, w[i]);
            sb_printf(&dyn, " + %s * %uu", ix, stride);
            free(ix);
            align = gcd32(align, stride);
        }
        ct = next;
    }
    if (konst) align = gcd32(align, konst);
    p->align = align;
    p->pointee = ct;
    int base_zero = !strcmp(p->offset, "0u");
    if (!dyn.p && base_zero) { free(p->offset); p->offset = xprintf("%uu", konst); }
    else if (!dyn.p && konst == 0) { /* same offset */ }
    else {
        char *name = xprintf("_%uo", id);
        if (!E(t, id)->odecl) { E(t, id)->odecl = 1; sb_printf(&t->vars, "    uint %s;\n", name); }
        EMIT("%s = %s + %uu%s;\n", name, p->offset, konst, dyn.p ? dyn.p : "");
        free(p->offset);
        p->offset = name;
    }
    free(dyn.p);
}

static void emit_load(T *t, const uint32_t *w) {
    uint32_t type = w[0], id = w[1];
    pinfo *p = E(t, w[2])->ptr;
    if (!p) { fail(t, VKMSL_UNSUPPORTED, "load from %u (not a known pointer)", w[2]); return; }
    ent *e = E(t, id);
    switch (p->kind) {
    case P_HANDLE:
        e->expr = xstrdup(p->lvalue);
        e->depthtex = p->depth;
        if (p->base) e->aux = xstrdup(p->base);
        if (p->sample && !strcmp(p->sample, "buffer")) fail(t, VKMSL_UNSUPPORTED, "load of a whole buffer array");
        return;
    case P_LVALUE:
        EMIT("%s = %s;\n", dst(t, id), p->lvalue);
        return;
    case P_OPAQUE:
        e->expr = xstrdup(p->lvalue ? p->lvalue : "");
        e->aux = p->base ? xstrdup(p->base) : NULL;
        e->depthtex = p->depth;
        if (!p->lvalue) fail(t, VKMSL_UNSUPPORTED, "load of local image/sampler %u before any store", p->var);
        return;
    case P_BUFFER: {
        const char *d = dst(t, id);
        buffer_load_into(t, p, type, d, p->offset, p->align, p->matrix_stride, p->row_major, p->column_of_row_major);
        return;
    }
    default:
        fail(t, VKMSL_UNSUPPORTED, "load through pointer kind %d", p->kind);
    }
}

static void emit_store(T *t, const uint32_t *w) {
    pinfo *p = E(t, w[0])->ptr;
    if (!p) { fail(t, VKMSL_UNSUPPORTED, "store to %u (not a known pointer)", w[0]); return; }
    if (p->var) E(t, p->var)->written |= 0x80;
    if (p->kind == P_OPAQUE) {
        ent *v = E(t, w[1]);
        free(p->lvalue); free(p->base);
        p->lvalue = xstrdup(v->expr ? v->expr : "");
        p->base = v->aux ? xstrdup(v->aux) : NULL;
        p->depth = v->depthtex;
        return;
    }
    if (p->kind == P_LVALUE) {
        char *whole = xprintf("_%u", p->var);
        if (p->var && E(t, p->var)->kind == K_VAR && !strcmp(p->lvalue, whole)) E(t, p->var)->written = 0xff;
        free(whole);
        EMIT("%s = %s;\n", p->lvalue, value(t, w[1]));
    } else if (p->kind == P_BUFFER)
        buffer_store_from(t, p, p->pointee, value(t, w[1]), p->offset, p->align, p->matrix_stride, p->row_major,
                          p->column_of_row_major);
    else fail(t, VKMSL_UNSUPPORTED, "store through pointer kind %d", p->kind);
}

// ---- images -----------------------------------------------------------------------------------------------------
typedef struct {
    uint32_t dim, depth, arrayed, ms, storage, sampled_type;
    const char *tex, *smp;
    int is_depth;              // declared as a depth texture (depth2d...)
} imginfo;
static int image_info(T *t, uint32_t value_id, imginfo *im) {
    uint32_t type = vtype(t, value_id);
    if (type_op(t, type) == OpTypeSampledImage) type = tw(t, type)[1];
    if (type_op(t, type) != OpTypeImage) { fail(t, VKMSL_INVALID, "%u is not an image", value_id); return 0; }
    const uint32_t *w = tw(t, type);
    im->sampled_type = w[1]; im->dim = w[2]; im->depth = w[3]; im->arrayed = w[4]; im->ms = w[5]; im->storage = w[6] == 2;
    im->tex = E(t, value_id)->expr;
    im->smp = E(t, value_id)->aux;
    im->is_depth = E(t, value_id)->depthtex;
    if (!im->tex) { fail(t, VKMSL_UNSUPPORTED, "image %u has no descriptor", value_id); return 0; }
    return 1;
}
static int depth_texture(const imginfo *im) { return im->is_depth; }

typedef struct { char *bias, *lod, *dx, *dy, *offset, *sample, *minlod, *component; } imops;
static void imops_free(imops *o) { free(o->bias); free(o->lod); free(o->dx); free(o->dy); free(o->offset); free(o->sample); free(o->minlod); free(o->component); }
static void parse_imops(T *t, const uint32_t *w, uint32_t n, uint32_t at, imops *o) {
    memset(o, 0, sizeof *o);
    if (at >= n) return;
    uint32_t mask = w[at++];
    if (mask & 0x1) o->bias = xstrdup(value(t, w[at++]));
    if (mask & 0x2) o->lod = xstrdup(value(t, w[at++]));
    if (mask & 0x4) { o->dx = xstrdup(value(t, w[at])); o->dy = xstrdup(value(t, w[at + 1])); at += 2; }
    if (mask & 0x8) o->offset = xstrdup(value(t, w[at++]));
    if (mask & 0x10) { o->offset = xstrdup(value(t, w[at++])); if (E(t, w[at - 1])->kind != K_CONST) fail(t, VKMSL_UNSUPPORTED, "non-constant texel offset"); }
    if (mask & 0x20) { fail(t, VKMSL_UNSUPPORTED, "ConstOffsets"); at++; }
    if (mask & 0x40) o->sample = xstrdup(value(t, w[at++]));
    if (mask & 0x80) o->minlod = xstrdup(value(t, w[at++]));
}

// The coordinate arguments of a sample/gather: "coord[, array]" for float coordinates.
static char *sample_coords(const imginfo *im, const char *c, int proj, uint32_t cn) {
    const char *q = cn == 2 ? ".y" : cn == 3 ? ".z" : ".w";
    if (proj) {
        if (im->dim == DIM_1D) return xprintf("(%s.x / %s%s)", c, c, q);
        if (im->dim == DIM_3D) return xprintf("(%s.xyz / %s%s)", c, c, q);
        return xprintf("(%s.xy / %s%s)", c, c, q);
    }
    switch (im->dim) {
    case DIM_1D: return im->arrayed ? xprintf("%s.x, uint(rint(%s.y))", c, c) : cn > 1 ? xprintf("%s.x", c) : xstrdup(c);
    case DIM_3D: return xprintf("%s.xyz", c);
    case DIM_Cube: return im->arrayed ? xprintf("%s.xyz, uint(rint(%s.w))", c, c) : xprintf("%s.xyz", c);
    default: return im->arrayed ? xprintf("%s.xy, uint(rint(%s.z))", c, c) : xprintf("%s.xy", c);
    }
}
// Integer coordinates for read/write: "coord[, array]".
static char *texel_coords(T *t, const imginfo *im, const char *c, uint32_t cn) {
    (void)t;
    switch (im->dim) {
    case DIM_1D: case DIM_Buffer:
        return im->arrayed ? xprintf("uint(%s.x), uint(%s.y)", c, c) : cn > 1 ? xprintf("uint(%s.x)", c) : xprintf("uint(%s)", c);
    case DIM_3D: return xprintf("uint3(%s.xyz)", c);
    case DIM_Cube: return im->arrayed ? xprintf("uint2(%s.xy), uint(%s.z) %% 6u, uint(%s.z) / 6u", c, c, c) :
                                        xprintf("uint2(%s.xy), uint(%s.z)", c, c);
    default: return im->arrayed ? xprintf("uint2(%s.xy), uint(%s.z)", c, c) : xprintf("uint2(%s.xy)", c);
    }
}
static const char *grad_name(const imginfo *im) {
    return im->dim == DIM_3D ? "gradient3d" : im->dim == DIM_Cube ? "gradientcube" : "gradient2d";
}
// ", level(x)" / ", bias(x)" / ", gradient2d(a, b)" (+ min_lod_clamp), and the offset; 1D textures take none.
static void sample_options(const imginfo *im, const imops *o, sb *b, int allow_offset) {
    if (im->dim == DIM_1D) return;
    if (o->lod) sb_printf(b, ", level(%s)", o->lod);
    else if (o->bias) sb_printf(b, ", bias(%s)", o->bias);
    else if (o->dx) sb_printf(b, ", %s(%s, %s)", grad_name(im), o->dx, o->dy);
    if (o->minlod && !o->lod) sb_printf(b, ", min_lod_clamp(%s)", o->minlod);
    if (o->offset && allow_offset && im->dim != DIM_Cube) sb_printf(b, ", %s", o->offset);
}

static char *result_cast(T *t, uint32_t type, const char *x, const char *have) {
    if (have && !strcmp(have, tname(t, type))) return xstrdup(x);
    return xprintf("%s(%s)", tname(t, type), x);
}

static void emit_image(T *t, uint32_t op, const uint32_t *w, uint32_t n) {
    uint32_t type = w[0], id = w[1];
    imginfo im;
    switch (op) {
    case OpSampledImage: {
        ent *e = E(t, id);
        e->depthtex = E(t, w[2])->depthtex;
        e->expr = xstrdup(E(t, w[2])->expr ? E(t, w[2])->expr : "");
        e->aux = xstrdup(E(t, w[3])->expr ? E(t, w[3])->expr : "");
        return;
    }
    case OpImage:
        E(t, id)->depthtex = E(t, w[2])->depthtex;
        E(t, id)->expr = xstrdup(E(t, w[2])->expr ? E(t, w[2])->expr : "");
        return;
    case OpImageWrite: {
        // (w: image, coordinate, texel, [operands])
        if (!image_info(t, w[0], &im)) return;
        const char *st = scalar_name(t, im.sampled_type);
        char have[32];
        snprintf(have, sizeof have, "%s4", st ? st : "float");
        char *cc = texel_coords(t, &im, value(t, w[1]), vec_size(t, vtype(t, w[1])));
        uint32_t texel_type = vtype(t, w[2]);
        char *v;
        if (vec_size(t, texel_type) == 4) v = !strcmp(tname(t, texel_type), have) ? xstrdup(value(t, w[2])) :
                                                xprintf("%s(%s)", have, value(t, w[2]));
        else { char *wv = widen4(t, texel_type, value(t, w[2])); v = xprintf("%s(%s)", have, wv); free(wv); }
        EMIT("%s.write(%s, %s);\n", im.tex, v, cc);
        free(v); free(cc);
        return;
    }
    case OpImageTexelPointer: {
        // (the image operand is the variable's pointer)
        pinfo *ip = E(t, w[2])->ptr;
        if (!ip || ip->kind != P_HANDLE) { fail(t, VKMSL_UNSUPPORTED, "texel pointer into %u", w[2]); return; }
        uint32_t it = ip->pointee;
        if (type_op(t, it) != OpTypeImage) { fail(t, VKMSL_UNSUPPORTED, "texel pointer into a non-image"); return; }
        const uint32_t *iw = tw(t, it);
        im.sampled_type = iw[1]; im.dim = iw[2]; im.depth = iw[3]; im.arrayed = iw[4]; im.ms = iw[5]; im.storage = 1;
        pinfo *p = new_ptr(t, id, P_IMAGE_TEXEL);
        if (p) { p->lvalue = xstrdup(ip->lvalue); p->coord = texel_coords(t, &im, value(t, w[3]), vec_size(t, vtype(t, w[3]))); p->pointee = tw(t, type)[2]; }
        return;
    }
    default: break;
    }
    if (!image_info(t, w[2], &im)) return;
    char *c = n > 3 ? xstrdup(value(t, w[3])) : NULL;
    uint32_t cn = n > 3 ? vec_size(t, vtype(t, w[3])) : 0;
    imops o;
    sb x = { 0 };
    const char *st = scalar_name(t, im.sampled_type);
    char have[32];
    snprintf(have, sizeof have, "%s4", st ? st : "float");
    switch (op) {
    case OpImageSampleImplicitLod: case OpImageSampleExplicitLod:
    case OpImageSampleProjImplicitLod: case OpImageSampleProjExplicitLod: {
        int proj = op == OpImageSampleProjImplicitLod || op == OpImageSampleProjExplicitLod;
        parse_imops(t, w, n, 4, &o);
        if (!im.smp) { fail(t, VKMSL_UNSUPPORTED, "sampling without a sampler"); break; }
        char *cc = sample_coords(&im, c, proj, cn);
        sb_printf(&x, "%s.sample(%s, %s", im.tex, im.smp, cc);
        sample_options(&im, &o, &x, 1);
        sb_puts(&x, ")");
        free(cc);
        char *r;
        if (depth_texture(&im)) r = xprintf("%s(%s, 0, 0, 1)", tname(t, type), x.p);
        else r = result_cast(t, type, x.p, have);
        EMIT("%s = %s;\n", dst(t, id), r);
        free(r);
        imops_free(&o);
        break;
    }
    case OpImageSampleDrefImplicitLod: case OpImageSampleDrefExplicitLod:
    case OpImageSampleProjDrefImplicitLod: case OpImageSampleProjDrefExplicitLod: {
        int proj = op == OpImageSampleProjDrefImplicitLod || op == OpImageSampleProjDrefExplicitLod;
        parse_imops(t, w, n, 5, &o);
        if (!depth_texture(&im)) { fail(t, VKMSL_UNSUPPORTED, "depth comparison on a non-depth image"); break; }
        char *cc = sample_coords(&im, c, proj, cn);
        const char *q = cn == 2 ? ".y" : cn == 3 ? ".z" : ".w";
        char *dref = proj ? xprintf("(%s / %s%s)", value(t, w[4]), c, q) : xstrdup(value(t, w[4]));
        sb_printf(&x, "%s.sample_compare(%s, %s, %s", im.tex, im.smp, cc, dref);
        if (o.lod) sb_printf(&x, ", level(%s)", o.lod);
        else if (o.dx) sb_printf(&x, ", %s(%s, %s)", grad_name(&im), o.dx, o.dy);
        if (o.offset && im.dim != DIM_Cube) sb_printf(&x, ", %s", o.offset);
        sb_puts(&x, ")");
        free(cc); free(dref);
        char *r = result_cast(t, type, x.p, "float");
        EMIT("%s = %s;\n", dst(t, id), r);
        free(r);
        imops_free(&o);
        break;
    }
    case OpImageFetch: case OpImageRead: {
        parse_imops(t, w, n, 4, &o);
        char *cc;
        if (im.dim == DIM_SubpassData) {
            t->uses_fragcoord = 1;
            cc = im.arrayed ? xprintf("uint2(spvFragCoord.xy) + uint2(%s.xy), 0u", c) : xprintf("uint2(spvFragCoord.xy) + uint2(%s)", c);
        } else if (o.offset) {
            char *cs = xprintf("(%s + %s)", c, o.offset);
            cc = texel_coords(t, &im, cs, cn);
            free(cs);
        } else cc = texel_coords(t, &im, c, cn);
        sb_printf(&x, "%s.read(%s", im.tex, cc);
        if (im.ms) sb_printf(&x, ", uint(%s)", o.sample ? o.sample : "0");
        else if (o.lod && im.dim != DIM_Buffer) sb_printf(&x, ", uint(%s)", o.lod);
        sb_puts(&x, ")");
        free(cc);
        char *r = depth_texture(&im) ? xprintf("%s(%s, 0, 0, 1)", tname(t, type), x.p) : result_cast(t, type, x.p, have);
        EMIT("%s = %s;\n", dst(t, id), r);
        free(r);
        imops_free(&o);
        break;
    }
    case OpImageGather: case OpImageDrefGather: {
        parse_imops(t, w, n, 5, &o);
        char *cc = sample_coords(&im, c, 0, cn);
        if (op == OpImageDrefGather) {
            sb_printf(&x, "%s.gather_compare(%s, %s, %s", im.tex, im.smp, cc, value(t, w[4]));
            if (o.offset && im.dim != DIM_Cube) sb_printf(&x, ", %s", o.offset);
        } else {
            int ok = 1;
            uint32_t comp = const_u32(t, w[4], &ok);
            if (!ok) fail(t, VKMSL_UNSUPPORTED, "gather component not constant");
            sb_printf(&x, "%s.gather(%s, %s", im.tex, im.smp, cc);
            if (im.dim != DIM_Cube) sb_printf(&x, ", %s", o.offset ? o.offset : "int2(0)");
            if (!depth_texture(&im)) sb_printf(&x, ", component::%c", "xyzw"[comp & 3]);
        }
        sb_puts(&x, ")");
        free(cc);
        char *r = result_cast(t, type, x.p, depth_texture(&im) ? "float4" : have);
        EMIT("%s = %s;\n", dst(t, id), r);
        free(r);
        imops_free(&o);
        break;
    }
    case OpImageQuerySizeLod: case OpImageQuerySize: {
        const char *lod = op == OpImageQuerySizeLod ? value(t, w[3]) : NULL;
        char l[160] = "";
        if (lod && im.dim != DIM_Buffer && !im.ms) snprintf(l, sizeof l, "uint(%s)", lod);
        uint32_t rn = vec_size(t, type);
        if (im.dim == DIM_Buffer || im.dim == DIM_1D) {
            if (im.arrayed) sb_printf(&x, "uint2(%s.get_width(%s), %s.get_array_size())", im.tex, l, im.tex);
            else sb_printf(&x, "%s.get_width(%s)", im.tex, l);
        } else if (im.dim == DIM_3D) sb_printf(&x, "uint3(%s.get_width(%s), %s.get_height(%s), %s.get_depth(%s))", im.tex, l, im.tex, l, im.tex, l);
        else if (im.arrayed) sb_printf(&x, "uint3(%s.get_width(%s), %s.get_height(%s), %s.get_array_size())", im.tex, l, im.tex, l, im.tex);
        else sb_printf(&x, "uint2(%s.get_width(%s), %s.get_height(%s))", im.tex, l, im.tex, l);
        (void)rn;
        char *r = xprintf("%s(%s)", tname(t, type), x.p);
        EMIT("%s = %s;\n", dst(t, id), r);
        free(r);
        break;
    }
    case OpImageQueryLevels:
        EMIT("%s = %s(%s.get_num_mip_levels());\n", dst(t, id), tname(t, type), im.tex);
        break;
    case OpImageQuerySamples:
        EMIT("%s = %s(%s.get_num_samples());\n", dst(t, id), tname(t, type), im.tex);
        break;
    case OpImageQueryLod: {
        char *cc = sample_coords(&im, c, 0, cn);
        EMIT("%s = %s(float2(%s.calculate_clamped_lod(%s, %s), %s.calculate_unclamped_lod(%s, %s)));\n", dst(t, id),
             tname(t, type), im.tex, im.smp, cc, im.tex, im.smp, cc);
        free(cc);
        break;
    }
    default:
        fail(t, VKMSL_UNSUPPORTED, "image opcode %u", op);
    }
    free(x.p);
    free(c);
}

// ---- GLSL.std.450 ------------------------------------------------------------------------------------------------
static void emit_glsl(T *t, const uint32_t *w, uint32_t n) {
    uint32_t type = w[0], id = w[1], inst = w[3];
    const char *a = n > 4 ? value(t, w[4]) : "", *b = n > 5 ? value(t, w[5]) : "", *c = n > 6 ? value(t, w[6]) : "";
    const char *R = tname(t, type);
    const char *f1 = NULL, *f2 = NULL, *f3 = NULL;
    int integer_signed = -1;   // integer functions: the signedness their operands are read with
    switch (inst) {
    case 1: f1 = "round"; break; case 2: f1 = "rint"; break; case 3: f1 = "trunc"; break; case 4: f1 = "abs"; break;
    case 5: f1 = "abs"; integer_signed = 1; break; case 6: f1 = "sign"; break;
    case 8: f1 = "floor"; break; case 9: f1 = "ceil"; break; case 10: f1 = "fract"; break;
    case 13: f1 = "sin"; break; case 14: f1 = "cos"; break; case 15: f1 = "tan"; break; case 16: f1 = "asin"; break;
    case 17: f1 = "acos"; break; case 18: f1 = "atan"; break; case 19: f1 = "sinh"; break; case 20: f1 = "cosh"; break;
    case 21: f1 = "tanh"; break; case 22: f1 = "asinh"; break; case 23: f1 = "acosh"; break; case 24: f1 = "atanh"; break;
    case 25: f2 = "atan2"; break; case 26: f2 = "pow"; break; case 27: f1 = "exp"; break; case 28: f1 = "log"; break;
    case 29: f1 = "exp2"; break; case 30: f1 = "log2"; break; case 31: f1 = "sqrt"; break; case 32: f1 = "rsqrt"; break;
    case 33: f1 = "determinant"; break;
    case 37: case 79: f2 = "min"; break; case 40: case 80: f2 = "max"; break;
    case 38: f2 = "min"; integer_signed = 0; break; case 39: f2 = "min"; integer_signed = 1; break;
    case 41: f2 = "max"; integer_signed = 0; break; case 42: f2 = "max"; integer_signed = 1; break;
    case 43: case 81: f3 = "clamp"; break;
    case 44: f3 = "clamp"; integer_signed = 0; break; case 45: f3 = "clamp"; integer_signed = 1; break;
    case 46: f3 = "mix"; break; case 48: f2 = "step"; break; case 49: f3 = "smoothstep"; break; case 50: f3 = "fma"; break;
    case 53: f2 = "ldexp"; break;
    case 68: f2 = "cross"; break;
    case 72: f3 = "refract"; break;
    case 70: f3 = "faceforward"; break;
    default: break;
    }
    char *x = NULL;
    if (integer_signed >= 0) {
        char *ra = n > 4 ? as_sign(t, w[4], integer_signed) : NULL, *rb = n > 5 ? as_sign(t, w[5], integer_signed) : NULL,
             *rc = n > 6 ? as_sign(t, w[6], integer_signed) : NULL;
        const char *f = f1 ? f1 : f2 ? f2 : f3;
        char *inner = f1 ? xprintf("%s(%s)", f, ra) : f2 ? xprintf("%s(%s, %s)", f, ra, rb) : xprintf("%s(%s, %s, %s)", f, ra, rb, rc);
        x = xprintf("%s(%s)", R, inner);
        free(inner); free(ra); free(rb); free(rc);
    } else if (f1) x = xprintf("%s(%s)", f1, a);
    else if (f2) x = xprintf("%s(%s, %s)", f2, a, b);
    else if (f3) x = xprintf("%s(%s, %s, %s)", f3, a, b, c);
    else switch (inst) {
    case 7: need(t, H_SSIGN); { char *ra = as_sign(t, w[4], 1); x = xprintf("%s(spvSSign(%s))", R, ra); free(ra); } break;
    case 11: x = xprintf("(%s * 0.01745329251994329577f)", a); break;
    case 12: x = xprintf("(%s * 57.2957795130823208768f)", a); break;
    case 34: need(t, H_INVERSE); x = xprintf("spvInverse(%s)", a); break;
    case 35: {
        pinfo *p = E(t, w[5])->ptr;
        if (!p || p->kind != P_LVALUE) { fail(t, VKMSL_UNSUPPORTED, "modf into a buffer"); return; }
        x = xprintf("modf(%s, %s)", a, p->lvalue);
        break;
    }
    case 36: x = xprintf("%s{ %s - trunc(%s), trunc(%s) }", R, a, a, a); break;
    case 47: x = xprintf("select(%s, %s, %s)", a, b, c); break;
    case 51: {
        pinfo *p = E(t, w[5])->ptr;
        if (!p || p->kind != P_LVALUE) { fail(t, VKMSL_UNSUPPORTED, "frexp into a buffer"); return; }
        x = xprintf("frexp(%s, %s)", a, p->lvalue);
        break;
    }
    case 52: fail(t, VKMSL_UNSUPPORTED, "FrexpStruct"); return;
    case 54: x = xprintf("pack_float_to_snorm4x8(%s)", a); break;
    case 55: x = xprintf("pack_float_to_unorm4x8(%s)", a); break;
    case 56: x = xprintf("pack_float_to_snorm2x16(%s)", a); break;
    case 57: x = xprintf("pack_float_to_unorm2x16(%s)", a); break;
    case 58: x = xprintf("as_type<uint>(half2(%s))", a); break;
    case 60: x = xprintf("unpack_snorm2x16_to_float(%s)", a); break;
    case 61: x = xprintf("unpack_unorm2x16_to_float(%s)", a); break;
    case 62: x = xprintf("float2(as_type<half2>(%s))", a); break;
    case 63: x = xprintf("unpack_snorm4x8_to_float(%s)", a); break;
    case 64: x = xprintf("unpack_unorm4x8_to_float(%s)", a); break;
    case 66: x = vec_size(t, vtype(t, w[4])) > 1 ? xprintf("length(%s)", a) : xprintf("abs(%s)", a); break;
    case 67: x = vec_size(t, vtype(t, w[4])) > 1 ? xprintf("distance(%s, %s)", a, b) : xprintf("abs(%s - %s)", a, b); break;
    case 69: x = vec_size(t, vtype(t, w[4])) > 1 ? xprintf("normalize(%s)", a) : xprintf("sign(%s)", a); break;
    case 71: x = vec_size(t, vtype(t, w[4])) > 1 ? xprintf("reflect(%s, %s)", a, b) :
                 xprintf("(%s - 2.0f * %s * %s * %s)", a, b, a, b); break;
    case 73: need(t, H_FINDLSB); x = xprintf("%s(spvFindLSB(%s))", R, a); break;
    case 74: need(t, H_FINDSMSB); { char *ra = as_sign(t, w[4], 1); x = xprintf("%s(spvFindSMSB(%s))", R, ra); free(ra); } break;
    case 75: need(t, H_FINDUMSB); { char *ra = as_sign(t, w[4], 0); x = xprintf("%s(spvFindUMSB(%s))", R, ra); free(ra); } break;
    case 76: x = xprintf("%s", a); break;                      // (centroid interpolation: the value as interpolated)
    case 77: case 78: x = xprintf("%s", a); break;             // (sample/offset interpolation: likewise)
    default: fail(t, VKMSL_UNSUPPORTED, "GLSL.std.450 instruction %u", inst); return;
    }
    if (inst == 72 && vec_size(t, vtype(t, w[4])) == 1) { free(x); x = xprintf("refract(float2(%s, 0), float2(%s, 0), %s).x", a, b, c); }
    if (inst == 70 && vec_size(t, vtype(t, w[4])) == 1) { free(x); x = xprintf("(dot(%s, %s) < 0 ? %s : -%s)", c, b, a, a); }
    if (x) EMIT("%s = %s;\n", dst(t, id), x);
    free(x);
}

// ---- atomics ------------------------------------------------------------------------------------------------------
static char *atomic_target(T *t, uint32_t ptr_id, uint32_t value_type, int *texture) {
    pinfo *p = E(t, ptr_id)->ptr;
    *texture = 0;
    if (!p) { fail(t, VKMSL_UNSUPPORTED, "atomic on %u (not a known pointer)", ptr_id); return xstrdup("0"); }
    const char *at = is_signed(t, value_type) ? "atomic_int" : "atomic_uint";
    if (scalar_width(t, value_type) == 64) at = is_signed(t, value_type) ? "atomic_long" : "atomic_ulong";
    if (p->kind == P_BUFFER) return xprintf("((device %s*)(%s + %s))", at, p->base, p->offset);
    if (p->kind == P_LVALUE) {
        uint32_t sc = E(t, p->var)->kind == K_VAR ? E(t, p->var)->w[2] : SC_Function;
        if (sc == SC_Workgroup) return xprintf("((threadgroup %s*)&%s)", at, p->lvalue);
        return xprintf("((thread %s*)&%s)", at, p->lvalue);
    }
    if (p->kind == P_IMAGE_TEXEL) { *texture = 1; return xprintf("%s|%s", p->lvalue, p->coord); }
    fail(t, VKMSL_UNSUPPORTED, "atomic through pointer kind %d", p->kind);
    return xstrdup("0");
}
static void emit_atomic(T *t, uint32_t op, const uint32_t *w, uint32_t n) {
    int tex = 0;
    if (op == OpAtomicStore) {
        uint32_t vt = vtype(t, w[3]);
        char *p = atomic_target(t, w[0], vt, &tex);
        if (tex) fail(t, VKMSL_UNSUPPORTED, "atomic store to an image");
        else EMIT("atomic_store_explicit(%s, %s, memory_order_relaxed);\n", p, value(t, w[3]));
        free(p);
        return;
    }
    uint32_t type = w[0], id = w[1];
    char *p = atomic_target(t, w[2], type, &tex);
    const char *R = tname(t, type);
    const char *f = NULL;
    char *v = n > 5 ? as_type_of(t, w[5], type) : NULL;
    switch (op) {
    case OpAtomicExchange: f = "exchange"; break;
    case OpAtomicIAdd: f = "fetch_add"; break; case OpAtomicISub: f = "fetch_sub"; break;
    case OpAtomicSMin: case OpAtomicUMin: f = "fetch_min"; break; case OpAtomicSMax: case OpAtomicUMax: f = "fetch_max"; break;
    case OpAtomicAnd: f = "fetch_and"; break; case OpAtomicOr: f = "fetch_or"; break; case OpAtomicXor: f = "fetch_xor"; break;
    case OpAtomicIIncrement: f = "fetch_add"; free(v); v = xprintf("%s(1)", R); break;
    case OpAtomicIDecrement: f = "fetch_sub"; free(v); v = xprintf("%s(1)", R); break;
    default: break;
    }
    if (tex) {
        char *bar = strchr(p, '|');
        *bar = 0;
        if (op == OpAtomicLoad) EMIT("%s = %s.read(%s).x;\n", dst(t, id), p, bar + 1);
        else if (op == OpAtomicCompareExchange || op == OpAtomicCompareExchangeWeak) {
            char *cmp = as_type_of(t, w[7], type), *val = as_type_of(t, w[6], type);
            const char *d = dst(t, id);
            EMIT("{ %s4 _e = %s4(%s); %s.atomic_compare_exchange_weak(%s, &_e, %s4(%s)); %s = _e.x; }\n", R, R, cmp, p, bar + 1, R, val, d);
            free(cmp); free(val);
        } else if (f) EMIT("%s = %s.atomic_%s(%s, %s4(%s)).x;\n", dst(t, id), p, f, bar + 1, R, v);
        else fail(t, VKMSL_UNSUPPORTED, "image atomic %u", op);
        free(p); free(v);
        return;
    }
    if (op == OpAtomicLoad) EMIT("%s = atomic_load_explicit(%s, memory_order_relaxed);\n", dst(t, id), p);
    else if (op == OpAtomicCompareExchange || op == OpAtomicCompareExchangeWeak) {
        char *cmp = as_type_of(t, w[7], type), *val = as_type_of(t, w[6], type);
        const char *d = dst(t, id);
        EMIT("{ %s _e; bool _ok; do { _e = %s; _ok = atomic_compare_exchange_weak_explicit(%s, &_e, %s, memory_order_relaxed, memory_order_relaxed); } while (!_ok && _e == %s); %s = _e; }\n",
             R, cmp, p, val, cmp, d);
        free(cmp); free(val);
    } else if (f) EMIT("%s = atomic_%s_explicit(%s, %s, memory_order_relaxed);\n", dst(t, id), f, p, v);
    else fail(t, VKMSL_UNSUPPORTED, "atomic %u", op);
    free(p); free(v);
}

// ---- subgroups ----------------------------------------------------------------------------------------------------
static void emit_subgroup(T *t, uint32_t op, const uint32_t *w, uint32_t n) {
    uint32_t type = w[0], id = w[1];
    const char *R = tname(t, type);
    t->uses_subgroup = 1;
    // (operand 2 is the scope; Metal's SIMD groups are the subgroups)
    // (operands 3 and 4, except for the operations whose operand 3 is a group operation literal)
    int literal_group_op = (op >= OpGroupNonUniformIAdd && op <= OpGroupNonUniformLogicalXor) ||
                           op == OpGroupNonUniformBallotBitCount || op == OpSubgroupBallotKHR ||
                           op == OpSubgroupFirstInvocationKHR;
    const char *a = !literal_group_op && n > 3 ? value(t, w[3]) : "", *b = !literal_group_op && n > 4 ? value(t, w[4]) : "";
    char *x = NULL;
    switch (op) {
    case OpGroupNonUniformElect: x = xstrdup("simd_is_first()"); break;
    case OpGroupNonUniformAll: x = xprintf("simd_all(%s)", a); break;
    case OpGroupNonUniformAny: x = xprintf("simd_any(%s)", a); break;
    case OpGroupNonUniformAllEqual:
        x = vec_size(t, vtype(t, w[3])) > 1 ? xprintf("simd_all(all(%s == simd_broadcast_first(%s)))", a, a) :
                                               xprintf("simd_all(%s == simd_broadcast_first(%s))", a, a);
        break;
    case OpGroupNonUniformBroadcast: x = is_bool(t, scalar_of(t, type)) ? xprintf("(simd_broadcast(uint(%s), ushort(%s)) != 0u)", a, b) :
                                                                         xprintf("simd_broadcast(%s, ushort(%s))", a, b); break;
    case OpGroupNonUniformBroadcastFirst: case OpSubgroupFirstInvocationKHR: {
        const char *v = op == OpSubgroupFirstInvocationKHR ? value(t, w[2]) : a;
        x = is_bool(t, scalar_of(t, type)) ? xprintf("(simd_broadcast_first(uint(%s)) != 0u)", v) : xprintf("simd_broadcast_first(%s)", v);
        break;
    }
    case OpGroupNonUniformBallot: case OpSubgroupBallotKHR:
        need(t, H_BALLOT);
        x = xprintf("spvBallot(%s)", op == OpSubgroupBallotKHR ? value(t, w[2]) : a);
        break;
    case OpGroupNonUniformInverseBallot:
        x = xprintf("(((%s)[spvSubgroupLane / 32u] >> (spvSubgroupLane %% 32u)) & 1u) != 0u", a);
        break;
    case OpGroupNonUniformBallotBitExtract:
        x = xprintf("((((%s)[uint(%s) / 32u] >> (uint(%s) %% 32u)) & 1u) != 0u)", a, b, b);
        break;
    case OpGroupNonUniformBallotBitCount: {
        uint32_t group_op = w[3];
        const char *v = value(t, w[4]);
        if (group_op == 0) x = xprintf("%s(popcount((%s).x) + popcount((%s).y))", R, v, v);
        else {
            // (scans count the lanes below, inclusive or not)
            const char *mask = group_op == 1 ? "spvLeMask" : "spvLtMask";
            x = xprintf("%s(popcount((%s).x & %s.x) + popcount((%s).y & %s.y))", R, v, mask, v, mask);
        }
        break;
    }
    case OpGroupNonUniformBallotFindLSB:
        x = xprintf("%s((%s).x != 0u ? ctz((%s).x) : 32u + ctz((%s).y))", R, a, a, a); break;
    case OpGroupNonUniformBallotFindMSB:
        x = xprintf("%s((%s).y != 0u ? 63u - clz((%s).y) : 31u - clz((%s).x))", R, a, a, a); break;
    case OpGroupNonUniformShuffle: x = xprintf("simd_shuffle(%s, ushort(%s))", a, b); break;
    case OpGroupNonUniformShuffleXor: x = xprintf("simd_shuffle_xor(%s, ushort(%s))", a, b); break;
    case OpGroupNonUniformShuffleUp: x = xprintf("simd_shuffle_up(%s, ushort(%s))", a, b); break;
    case OpGroupNonUniformShuffleDown: x = xprintf("simd_shuffle_down(%s, ushort(%s))", a, b); break;
    case OpGroupNonUniformQuadBroadcast: x = xprintf("quad_broadcast(%s, ushort(%s))", a, b); break;
    case OpGroupNonUniformQuadSwap: {
        int ok = 1;
        uint32_t dir = const_u32(t, w[4], &ok);
        x = xprintf("quad_shuffle_xor(%s, ushort(%u))", a, dir == 0 ? 1 : dir == 1 ? 2 : 3);
        break;
    }
    default: {
        // Arithmetic: operand 3 is the group operation (0 reduce, 1 inclusive scan, 2 exclusive scan).
        uint32_t group_op = w[3];
        const char *v = value(t, w[4]);
        const char *f = NULL;
        int scannable = 0, boolean = 0;
        switch (op) {
        case OpGroupNonUniformIAdd: case OpGroupNonUniformFAdd: f = "sum"; scannable = 1; break;
        case OpGroupNonUniformIMul: case OpGroupNonUniformFMul: f = "product"; scannable = 1; break;
        case OpGroupNonUniformSMin: case OpGroupNonUniformUMin: case OpGroupNonUniformFMin: f = "min"; break;
        case OpGroupNonUniformSMax: case OpGroupNonUniformUMax: case OpGroupNonUniformFMax: f = "max"; break;
        case OpGroupNonUniformBitwiseAnd: f = "and"; break;
        case OpGroupNonUniformBitwiseOr: f = "or"; break;
        case OpGroupNonUniformBitwiseXor: f = "xor"; break;
        case OpGroupNonUniformLogicalAnd: f = "and"; boolean = 1; break;
        case OpGroupNonUniformLogicalOr: f = "or"; boolean = 1; break;
        case OpGroupNonUniformLogicalXor: f = "xor"; boolean = 1; break;
        }
        if (!f) { fail(t, VKMSL_UNSUPPORTED, "subgroup opcode %u", op); return; }
        if (group_op > 2 || (group_op && !scannable)) { fail(t, VKMSL_UNSUPPORTED, "subgroup scan of %s", f); return; }
        if (boolean) x = xprintf("(simd_%s(uint(%s)) != 0u)", f, v);
        else if (group_op == 0) x = xprintf("simd_%s(%s)", f, v);
        else x = xprintf("simd_prefix_%s_%s(%s)", group_op == 1 ? "inclusive" : "exclusive", f, v);
        break;
    }
    }
    if (x) { char *r = result_cast(t, type, x, NULL); EMIT("%s = %s;\n", dst(t, id), x); free(r); }
    (void)R;
    free(x);
}

// ---- instructions -----------------------------------------------------------------------------------------------
static void binop(T *t, const uint32_t *w, const char *sym, int sign) {
    // sign: -1 operands as the result type, 0 unsigned, 1 signed (the result converted back)
    uint32_t type = w[0], id = w[1];
    char *a, *b, *x;
    if (sign < 0) { a = as_type_of(t, w[2], type); b = as_type_of(t, w[3], type); x = xprintf("%s %s %s", a, sym, b); }
    else {
        a = as_sign(t, w[2], sign); b = as_sign(t, w[3], sign);
        x = xprintf("%s(%s %s %s)", tname(t, type), a, sym, b);
    }
    EMIT("%s = %s;\n", dst(t, id), x);
    free(a); free(b); free(x);
}
static void compare(T *t, const uint32_t *w, const char *sym, int sign) {
    // sign: -1 as the first operand's type, 0 unsigned, 1 signed
    char *a, *b;
    if (sign < 0) { a = xstrdup(value(t, w[2])); b = as_type_of(t, w[3], vtype(t, w[2])); }
    else { a = as_sign(t, w[2], sign); b = as_sign(t, w[3], sign); }
    EMIT("%s = %s %s %s;\n", dst(t, w[1]), a, sym, b);
    free(a); free(b);
}
static void unop(T *t, const uint32_t *w, const char *f) {
    EMIT("%s = %s(%s);\n", dst(t, w[1]), f, value(t, w[2]));
}

// Path of a composite extract/insert: ".m1[2].x" style.
static char *composite_path(T *t, uint32_t type, const uint32_t *idx, uint32_t count) {
    sb p = { 0 };
    sb_puts(&p, "");
    for (uint32_t i = 0; i < count; i++) {
        int op = type_op(t, type);
        if (op == OpTypeStruct) { sb_printf(&p, ".m%u", idx[i]); type = tw(t, type)[idx[i] + 1]; }
        else { sb_printf(&p, "[%u]", idx[i]); type = tw(t, type)[1]; }
    }
    return p.p ? p.p : xstrdup("");
}

static void emit_from(T *t, uint32_t label, uint32_t stop);
static void after_construct(T *t);
// A call, inlined: arguments into the callee's parameters (pointers alias, values are copied), its body in a
// do-while that its returns leave, its result in the call's result.
static void emit_call(T *t, const uint32_t *w, uint32_t n) {
    ent *f = E(t, w[2]);
    if (f->kind != K_FUNC || f->block < 0) { fail(t, VKMSL_INVALID, "call of %u", w[2]); return; }
    for (int i = 0; i < t->depth; i++)
        if (t->stack[i].kind == C_FUNC && t->stack[i].header == w[2]) { fail(t, VKMSL_INVALID, "recursive call"); return; }
    if (n - 3 != f->mcount) { fail(t, VKMSL_INVALID, "call with %u arguments for %u parameters", n - 3, f->mcount); return; }
    for (uint32_t i = 0; i < f->mcount; i++) {
        uint32_t param = t->fparams[f->mbase + i], arg = w[3 + i];
        ent *pe = E(t, param), *ae = E(t, arg);
        if (type_op(t, pe->type) == OpTypePointer) {
            if (!ae->ptr) { fail(t, VKMSL_UNSUPPORTED, "pointer argument %u", arg); return; }
            if (pe->ptr) { pinfo *o = pe->ptr; free(o->lvalue); free(o->base); free(o->offset); free(o->coord); free(o->sample); free(o); }
            pe->ptr = copy_ptr(ae->ptr);
        } else if (opaque_type(t, pe->type)) {
            free(pe->expr); free(pe->aux);
            pe->expr = xstrdup(ae->expr ? ae->expr : "");
            pe->aux = ae->aux ? xstrdup(ae->aux) : NULL;
            pe->depthtex = ae->depthtex;
        } else EMIT("%s = %s;\n", dst(t, param), value(t, arg));
    }
    // (the callee's variables with initializers start from them on every call)
    for (uint32_t b = (uint32_t)f->block; b < t->block_count; b++) {
        block *bk = &t->blocks[b];
        if (b > (uint32_t)f->block && E(t, bk->label)->block != (int)b) break;
        for (size_t pos = bk->first; pos < bk->term;) {
            uint32_t op = t->w[pos] & 0xffff, nn = t->w[pos] >> 16;
            if (op == OpVariable && nn > 4) EMIT("_%u = %s;\n", t->w[pos + 2], value(t, t->w[pos + 4]));
            else if (op != OpVariable && op != OpLine && op != OpNoLine) break;
            pos += nn;
        }
        break;   // (variables are declared in the first block)
    }
    if (t->depth >= 63) { fail(t, VKMSL_UNSUPPORTED, "nesting too deep"); return; }
    int flag = ++t->flag_count;
    sb_printf(&t->vars, "    bool _rf%d;\n", flag);
    EMIT("_rf%d = false;\n", flag);
    t->stack[t->depth++] = (construct){ C_FUNC, w[1], 0, w[2], flag };
    EMIT("do {\n");
    t->indent++;
    emit_from(t, t->blocks[f->block].label, 0);
    t->indent--;
    EMIT("} while (false);\n");
    t->depth--;
    after_construct(t);
}

static void emit_insn(T *t, uint32_t op, const uint32_t *w, uint32_t n) {
    t->cur_op = op;
    switch (op) {
    case OpNoLine: case OpLine: case OpSelectionMerge: case OpLoopMerge: case OpPhi: case OpFunctionParameter:
    case OpName: case OpMemberName: case 4: /* OpSourceExtension */ case 3: /* OpSource */ case 7: /* OpString */
        return;
    case OpVariable: return;
    case OpUndef: declare(t, w[1]); return;
    case OpAccessChain: case OpInBoundsAccessChain: access_chain(t, w, n); return;
    case OpLoad: emit_load(t, w); return;
    case OpStore: emit_store(t, w); return;
    case OpCopyMemory: {
        pinfo *d = E(t, w[0])->ptr, *s = E(t, w[1])->ptr;
        if (d && s && d->kind == P_LVALUE && s->kind == P_LVALUE) EMIT("%s = %s;\n", d->lvalue, s->lvalue);
        else fail(t, VKMSL_UNSUPPORTED, "OpCopyMemory between buffers");
        return;
    }
    case OpArrayLength: {
        pinfo *p = E(t, w[2])->ptr;
        uint32_t st = p ? p->pointee : 0, m = w[3];
        if (!p || p->kind != P_BUFFER || type_op(t, st) != OpTypeStruct) { fail(t, VKMSL_UNSUPPORTED, "OpArrayLength"); return; }
        uint32_t arr = tw(t, st)[m + 1], stride = E(t, arr)->d.array_stride;
        // (the descriptor's size word: the buffer's base pointer expression names its spvBuf)
        char *buf = xstrdup(p->base);
        char *dotp = strstr(buf, ".p");
        if (!dotp || !stride) { fail(t, VKMSL_UNSUPPORTED, "OpArrayLength on %s", buf); free(buf); return; }
        *dotp = 0;
        const char *open = buf[0] == '(' ? buf + 1 : buf;
        EMIT("%s = uint((%s.n - %uu) / %uu);\n", dst(t, w[1]), open, mdeco(t, st, m)->offset, stride);
        free(buf);
        return;
    }
    case OpCompositeExtract: {
        char *path = composite_path(t, vtype(t, w[2]), w + 3, n - 3);
        EMIT("%s = %s%s;\n", dst(t, w[1]), value(t, w[2]), path);
        free(path);
        return;
    }
    case OpCompositeInsert: {
        char *path = composite_path(t, w[0], w + 4, n - 4);
        const char *d = dst(t, w[1]);
        EMIT("%s = %s;\n", d, value(t, w[3]));
        EMIT("%s%s = %s;\n", d, path, value(t, w[2]));
        free(path);
        return;
    }
    case OpCompositeConstruct: {
        uint32_t type = w[0];
        int top = type_op(t, type);
        sb x = { 0 };
        if (top == OpTypeVector || top == OpTypeMatrix) sb_printf(&x, "%s(", tname(t, type));
        else sb_printf(&x, top == OpTypeArray ? "%s{{" : "%s{", tname(t, type));
        for (uint32_t i = 2; i < n; i++) {
            const char *v = value(t, w[i]);
            if (top == OpTypeVector && is_int(t, scalar_of(t, type))) {
                // (components of another signedness)
                uint32_t vt = vtype(t, w[i]);
                if (is_signed(t, vt) != is_signed(t, type) && is_int(t, scalar_of(t, vt))) {
                    char *c = int_type(t, vt, is_signed(t, type));
                    sb_printf(&x, "%s%s(%s)", i > 2 ? ", " : "", c, v);
                    free(c);
                    continue;
                }
            }
            sb_printf(&x, "%s%s", i > 2 ? ", " : "", v);
        }
        sb_puts(&x, top == OpTypeVector || top == OpTypeMatrix ? ")" : top == OpTypeArray ? "}}" : "}");
        EMIT("%s = %s;\n", dst(t, w[1]), x.p);
        free(x.p);
        return;
    }
    case OpVectorShuffle: {
        uint32_t type = w[0], na = vec_size(t, vtype(t, w[2]));
        const char *a = value(t, w[2]), *b = value(t, w[3]);
        sb x = { 0 };
        sb_printf(&x, "%s(", tname(t, type));
        for (uint32_t i = 4; i < n; i++) {
            uint32_t c = w[i];
            if (c == 0xffffffffu) sb_printf(&x, "%s0", i > 4 ? ", " : "");
            else if (c < na) sb_printf(&x, "%s%s%s", i > 4 ? ", " : "", a, na > 1 ? (const char *[]){ ".x", ".y", ".z", ".w" }[c & 3] : "");
            else { uint32_t nb = vec_size(t, vtype(t, w[3])); sb_printf(&x, "%s%s%s", i > 4 ? ", " : "", b, nb > 1 ? (const char *[]){ ".x", ".y", ".z", ".w" }[(c - na) & 3] : ""); }
        }
        sb_puts(&x, ")");
        EMIT("%s = %s;\n", dst(t, w[1]), x.p);
        free(x.p);
        return;
    }
    case OpVectorExtractDynamic: { char *i = index_expr(t, w[3]); EMIT("%s = %s[%s];\n", dst(t, w[1]), value(t, w[2]), i); free(i); return; }
    case OpVectorInsertDynamic: {
        char *i = index_expr(t, w[4]);
        const char *d = dst(t, w[1]);
        EMIT("%s = %s;\n", d, value(t, w[2]));
        EMIT("%s[%s] = %s;\n", d, i, value(t, w[3]));
        free(i);
        return;
    }
    case OpCopyLogical: copy_logical(t, dst(t, w[1]), w[0], value(t, w[2]), vtype(t, w[2]), 0); return;
    case OpCopyObject: {
        ent *s = E(t, w[2]);
        if (s->ptr) { E(t, w[1])->ptr = copy_ptr(s->ptr); return; }
        if (opaque_type(t, w[0])) {
            E(t, w[1])->expr = xstrdup(s->expr ? s->expr : "");
            E(t, w[1])->depthtex = s->depthtex;
            if (s->aux) E(t, w[1])->aux = xstrdup(s->aux);
            return;
        }
        EMIT("%s = %s;\n", dst(t, w[1]), value(t, w[2]));
        return;
    }
    case OpTranspose: unop(t, w, "transpose"); return;
    case OpConvertFToU: case OpConvertFToS: case OpFConvert:
        EMIT("%s = %s(%s);\n", dst(t, w[1]), tname(t, w[0]), value(t, w[2])); return;
    case OpConvertSToF: case OpConvertUToF: case OpSConvert: case OpUConvert: {
        char *a = as_sign(t, w[2], op == OpConvertSToF || op == OpSConvert);
        if (op == OpUConvert || op == OpSConvert) {
            // (the width changes with the source's signedness, then the result's)
            uint32_t rt = w[0];
            char *mid = int_type(t, rt, op == OpSConvert);
            EMIT("%s = %s(%s(%s));\n", dst(t, w[1]), tname(t, rt), mid, a);
            free(mid);
        } else EMIT("%s = %s(%s);\n", dst(t, w[1]), tname(t, w[0]), a);
        free(a);
        return;
    }
    case OpQuantizeToF16: EMIT("%s = %s(half(%s));\n", dst(t, w[1]), tname(t, w[0]), value(t, w[2])); return;
    case OpBitcast:
        if (!strcmp(tname(t, w[0]), tname(t, vtype(t, w[2])))) EMIT("%s = %s;\n", dst(t, w[1]), value(t, w[2]));
        else EMIT("%s = as_type<%s>(%s);\n", dst(t, w[1]), tname(t, w[0]), value(t, w[2]));
        return;
    case OpSNegate: { char *a = as_sign(t, w[2], 1); EMIT("%s = %s(-%s);\n", dst(t, w[1]), tname(t, w[0]), a); free(a); return; }
    case OpFNegate: EMIT("%s = -%s;\n", dst(t, w[1]), value(t, w[2])); return;
    case OpNot: { char *a = as_type_of(t, w[2], w[0]); EMIT("%s = ~%s;\n", dst(t, w[1]), a); free(a); return; }
    case OpIAdd: binop(t, w, "+", -1); return;
    case OpISub: binop(t, w, "-", -1); return;
    case OpIMul: binop(t, w, "*", -1); return;
    case OpUDiv: binop(t, w, "/", 0); return;
    case OpSDiv: binop(t, w, "/", 1); return;
    case OpUMod: binop(t, w, "%", 0); return;
    case OpSRem: binop(t, w, "%", 1); return;
    case OpSMod: {
        need(t, H_SMOD);
        char *a = as_sign(t, w[2], 1), *b = as_sign(t, w[3], 1);
        EMIT("%s = %s(spvSMod(%s, %s));\n", dst(t, w[1]), tname(t, w[0]), a, b);
        free(a); free(b);
        return;
    }
    case OpFAdd: binop(t, w, "+", -1); return;
    case OpFSub: binop(t, w, "-", -1); return;
    case OpFMul: binop(t, w, "*", -1); return;
    case OpFDiv: binop(t, w, "/", -1); return;
    case OpFRem: EMIT("%s = fmod(%s, %s);\n", dst(t, w[1]), value(t, w[2]), value(t, w[3])); return;
    case OpFMod: need(t, H_FMOD); EMIT("%s = spvFMod(%s, %s);\n", dst(t, w[1]), value(t, w[2]), value(t, w[3])); return;
    case OpVectorTimesScalar: case OpMatrixTimesScalar: case OpVectorTimesMatrix: case OpMatrixTimesVector:
    case OpMatrixTimesMatrix:
        EMIT("%s = %s * %s;\n", dst(t, w[1]), value(t, w[2]), value(t, w[3])); return;
    case OpOuterProduct: {
        uint32_t cols = tw(t, w[0])[2];
        sb x = { 0 };
        sb_printf(&x, "%s(", tname(t, w[0]));
        for (uint32_t c = 0; c < cols; c++) sb_printf(&x, "%s%s * %s[%u]", c ? ", " : "", value(t, w[2]), value(t, w[3]), c);
        sb_puts(&x, ")");
        EMIT("%s = %s;\n", dst(t, w[1]), x.p);
        free(x.p);
        return;
    }
    case OpDot: EMIT("%s = dot(%s, %s);\n", dst(t, w[1]), value(t, w[2]), value(t, w[3])); return;
    case OpIAddCarry: case OpISubBorrow: case OpUMulExtended: case OpSMulExtended: {
        const char *a = value(t, w[2]), *b = value(t, w[3]);
        const char *d = dst(t, w[1]);
        if (op == OpIAddCarry) EMIT("%s.m0 = %s + %s; %s.m1 = select(%s(0), %s(1), %s.m0 < %s);\n", d, a, b, d, tname(t, scalar_of(t, vtype(t, w[2]))), tname(t, scalar_of(t, vtype(t, w[2]))), d, a);
        else if (op == OpISubBorrow) EMIT("%s.m0 = %s - %s; %s.m1 = select(%s(0), %s(1), %s < %s);\n", d, a, b, d, tname(t, scalar_of(t, vtype(t, w[2]))), tname(t, scalar_of(t, vtype(t, w[2]))), a, b);
        else { char *sa = as_sign(t, w[2], op == OpSMulExtended), *sb2 = as_sign(t, w[3], op == OpSMulExtended);
               EMIT("%s.m0 = %s(%s * %s); %s.m1 = %s(mulhi(%s, %s));\n", d, tname(t, tw(t, w[0])[1]), sa, sb2, d, tname(t, tw(t, w[0])[2]), sa, sb2);
               free(sa); free(sb2); }
        return;
    }
    case OpAny: unop(t, w, "any"); return;
    case OpAll: unop(t, w, "all"); return;
    case OpIsNan: unop(t, w, "isnan"); return;
    case OpIsInf: unop(t, w, "isinf"); return;
    case OpIsFinite: unop(t, w, "isfinite"); return;
    case OpIsNormal: unop(t, w, "isnormal"); return;
    case OpSignBitSet: unop(t, w, "signbit"); return;
    case OpLessOrGreater: case OpFOrdNotEqual:
        EMIT("%s = (%s < %s) || (%s > %s);\n", dst(t, w[1]), value(t, w[2]), value(t, w[3]), value(t, w[2]), value(t, w[3])); return;
    case OpOrdered: EMIT("%s = !isnan(%s) && !isnan(%s);\n", dst(t, w[1]), value(t, w[2]), value(t, w[3])); return;
    case OpUnordered: EMIT("%s = isnan(%s) || isnan(%s);\n", dst(t, w[1]), value(t, w[2]), value(t, w[3])); return;
    case OpLogicalEqual: compare(t, w, "==", -1); return;
    case OpLogicalNotEqual: compare(t, w, "!=", -1); return;
    case OpLogicalOr: compare(t, w, "||", -1); return;
    case OpLogicalAnd: compare(t, w, "&&", -1); return;
    case OpLogicalNot: EMIT("%s = !%s;\n", dst(t, w[1]), value(t, w[2])); return;
    case OpSelect: {
        uint32_t ct = vtype(t, w[2]);
        if (type_op(t, ct) == OpTypeVector) EMIT("%s = select(%s, %s, %s);\n", dst(t, w[1]), value(t, w[4]), value(t, w[3]), value(t, w[2]));
        else EMIT("%s = %s ? %s : %s;\n", dst(t, w[1]), value(t, w[2]), value(t, w[3]), value(t, w[4]));
        return;
    }
    case OpIEqual: compare(t, w, "==", -1); return;
    case OpINotEqual: compare(t, w, "!=", -1); return;
    case OpUGreaterThan: compare(t, w, ">", 0); return;
    case OpSGreaterThan: compare(t, w, ">", 1); return;
    case OpUGreaterThanEqual: compare(t, w, ">=", 0); return;
    case OpSGreaterThanEqual: compare(t, w, ">=", 1); return;
    case OpULessThan: compare(t, w, "<", 0); return;
    case OpSLessThan: compare(t, w, "<", 1); return;
    case OpULessThanEqual: compare(t, w, "<=", 0); return;
    case OpSLessThanEqual: compare(t, w, "<=", 1); return;
    case OpFOrdEqual: compare(t, w, "==", -1); return;
    case OpFUnordNotEqual: compare(t, w, "!=", -1); return;
    case OpFOrdLessThan: compare(t, w, "<", -1); return;
    case OpFOrdGreaterThan: compare(t, w, ">", -1); return;
    case OpFOrdLessThanEqual: compare(t, w, "<=", -1); return;
    case OpFOrdGreaterThanEqual: compare(t, w, ">=", -1); return;
    case OpFUnordEqual: EMIT("%s = !((%s < %s) || (%s > %s));\n", dst(t, w[1]), value(t, w[2]), value(t, w[3]), value(t, w[2]), value(t, w[3])); return;
    case OpFUnordLessThan: EMIT("%s = !(%s >= %s);\n", dst(t, w[1]), value(t, w[2]), value(t, w[3])); return;
    case OpFUnordGreaterThan: EMIT("%s = !(%s <= %s);\n", dst(t, w[1]), value(t, w[2]), value(t, w[3])); return;
    case OpFUnordLessThanEqual: EMIT("%s = !(%s > %s);\n", dst(t, w[1]), value(t, w[2]), value(t, w[3])); return;
    case OpFUnordGreaterThanEqual: EMIT("%s = !(%s < %s);\n", dst(t, w[1]), value(t, w[2]), value(t, w[3])); return;
    case OpShiftRightLogical: binop(t, w, ">>", 0); return;
    case OpShiftRightArithmetic: binop(t, w, ">>", 1); return;
    case OpShiftLeftLogical: binop(t, w, "<<", -1); return;
    case OpBitwiseOr: binop(t, w, "|", -1); return;
    case OpBitwiseXor: binop(t, w, "^", -1); return;
    case OpBitwiseAnd: binop(t, w, "&", -1); return;
    case OpBitFieldInsert: {
        char *b = as_type_of(t, w[2], w[0]), *i = as_type_of(t, w[3], w[0]);
        EMIT("%s = insert_bits(%s, %s, uint(%s), uint(%s));\n", dst(t, w[1]), b, i, value(t, w[4]), value(t, w[5]));
        free(b); free(i);
        return;
    }
    case OpBitFieldSExtract: case OpBitFieldUExtract: {
        char *b = as_sign(t, w[2], op == OpBitFieldSExtract);
        EMIT("%s = %s(extract_bits(%s, uint(%s), uint(%s)));\n", dst(t, w[1]), tname(t, w[0]), b, value(t, w[3]), value(t, w[4]));
        free(b);
        return;
    }
    case OpBitReverse: unop(t, w, "reverse_bits"); return;
    case OpBitCount: EMIT("%s = %s(popcount(%s));\n", dst(t, w[1]), tname(t, w[0]), value(t, w[2])); return;
    case OpDPdx: case OpDPdxFine: case OpDPdxCoarse: unop(t, w, "dfdx"); return;
    case OpDPdy: case OpDPdyFine: case OpDPdyCoarse: unop(t, w, "dfdy"); return;
    case OpFwidth: case OpFwidthFine: case OpFwidthCoarse: unop(t, w, "fwidth"); return;
    case OpControlBarrier: case OpMemoryBarrier: {
        uint32_t sem_id = op == OpControlBarrier ? w[2] : w[1];
        int ok = 1;
        uint32_t sem = const_u32(t, sem_id, &ok);
        if (op == OpMemoryBarrier || t->model != VKMSL_STAGE_COMPUTE) return;   // (no pure fences: see below)
        const char *f = (sem & 0x800) ? "mem_flags::mem_texture | " : "";
        const char *g = (sem & 0x40) ? "mem_flags::mem_device | " : "";
        EMIT("threadgroup_barrier(%s%smem_flags::mem_threadgroup);\n", f, g);
        return;
    }
    case OpExtInst:
        if (w[2] != t->glsl) { fail(t, VKMSL_UNSUPPORTED, "extended instruction set %u", w[2]); return; }
        emit_glsl(t, w, n);
        return;
    case OpAtomicLoad: case OpAtomicStore: case OpAtomicExchange: case OpAtomicCompareExchange:
    case OpAtomicCompareExchangeWeak: case OpAtomicIIncrement: case OpAtomicIDecrement: case OpAtomicIAdd:
    case OpAtomicISub: case OpAtomicSMin: case OpAtomicUMin: case OpAtomicSMax: case OpAtomicUMax: case OpAtomicAnd:
    case OpAtomicOr: case OpAtomicXor:
        emit_atomic(t, op, w, n);
        return;
    case OpSampledImage: case OpImage: case OpImageSampleImplicitLod: case OpImageSampleExplicitLod:
    case OpImageSampleDrefImplicitLod: case OpImageSampleDrefExplicitLod: case OpImageSampleProjImplicitLod:
    case OpImageSampleProjExplicitLod: case OpImageSampleProjDrefImplicitLod: case OpImageSampleProjDrefExplicitLod:
    case OpImageFetch: case OpImageGather: case OpImageDrefGather: case OpImageRead: case OpImageWrite:
    case OpImageQuerySizeLod: case OpImageQuerySize: case OpImageQueryLod: case OpImageQueryLevels:
    case OpImageQuerySamples: case OpImageTexelPointer:
        emit_image(t, op, w, n);
        return;
    case OpIsHelperInvocationEXT: EMIT("%s = simd_is_helper_thread();\n", dst(t, w[1])); return;
    case OpDemoteToHelperInvocation: EMIT("discard_fragment();\n"); t->res->uses_discard = 1; return;
    case OpFunctionCall: emit_call(t, w, n); return;
    case OpEmitVertex: EMIT("@EMIT@\n"); return;   // (the packing, known once the interface is)
    case OpEndPrimitive: EMIT("spvStrip = 0u;\n"); return;
    default:
        if ((op >= OpGroupNonUniformElect && op <= OpGroupNonUniformQuadSwap) || op == OpSubgroupBallotKHR ||
            op == OpSubgroupFirstInvocationKHR) { emit_subgroup(t, op, w, n); return; }
        fail(t, VKMSL_UNSUPPORTED, "opcode %u", op);
    }
}

// ---- control flow -----------------------------------------------------------------------------------------------
static block *blk(T *t, uint32_t label) {
    int i = E(t, label)->block;
    if (i < 0) { fail(t, VKMSL_INVALID, "branch to %u, not a block of the entry point", label); return NULL; }
    return &t->blocks[i];
}

static void emit_phis(T *t, uint32_t from, uint32_t to) {
    block *b = blk(t, to);
    if (!b) return;
    uint32_t dsts[64], srcs[64], count = 0;
    int self = 0;
    for (size_t pos = b->first; pos < b->term;) {
        uint32_t op = t->w[pos] & 0xffff, n = t->w[pos] >> 16;
        const uint32_t *w = t->w + pos + 1;
        if (op == OpPhi) {
            for (uint32_t i = 2; i + 1 < n; i += 2)
                if (w[i + 1] == from && count < 64) {
                    dsts[count] = w[1];
                    srcs[count] = w[i];
                    if (E(t, w[i])->is_phi) self = 1;
                    count++;
                }
        } else if (op != OpLine && op != OpNoLine) break;
        pos += n;
    }
    if (count == 0) return;
    if (!self) {
        for (uint32_t i = 0; i < count; i++) EMIT("%s = %s;\n", dst(t, dsts[i]), value(t, srcs[i]));
        return;
    }
    // (a phi reading another phi of the same block reads its old value: copy through temporaries)
    EMIT("{\n");
    for (uint32_t i = 0; i < count; i++) EMIT("    %s _p%u = %s;\n", tname(t, E(t, dsts[i])->type), i, value(t, srcs[i]));
    for (uint32_t i = 0; i < count; i++) EMIT("    %s = _p%u;\n", dst(t, dsts[i]), i);
    EMIT("}\n");
}

static void emit_from(T *t, uint32_t label, uint32_t stop);
static int innermost(T *t, int kind) {
    for (int i = t->depth - 1; i >= 0; i--) if (t->stack[i].kind == kind) return i;
    return -1;
}
// A return: from an inlined function, out of its do-while (through its loops and switches with its return flag);
// from the entry point, the stage's outputs and return.
static void emit_return(T *t) {
    int F = innermost(t, C_FUNC);
    if (F >= 0) {
        int nested = 0;
        for (int i = F + 1; i < t->depth; i++) nested |= t->stack[i].kind == C_LOOP || t->stack[i].kind == C_SWITCH;
        if (nested) EMIT("{ _rf%d = true; break; }\n", t->stack[F].brk_flag);
        else EMIT("break;\n");
        return;
    }
    if (t->model == VKMSL_STAGE_COMPUTE) { EMIT("return;\n"); return; }
    EMIT("@RET@\n");   // (the output stores, known once the whole body is written)
}
// After a loop or switch inside an inlined function: a return from inside it goes on out.
static void after_construct(T *t) {
    int F = innermost(t, C_FUNC);
    if (F >= 0) EMIT("if (_rf%d) break;\n", t->stack[F].brk_flag);
}
// A break out of the innermost loop, through any switches inside it (each sets its flag and breaks; the code after
// that switch breaks on).
static void break_loop(T *t) {
    int top = -1;
    for (int i = t->depth - 1; i >= 0; i--) if (t->stack[i].kind == C_LOOP || t->stack[i].kind == C_SWITCH) { top = i; break; }
    if (top < 0) { fail(t, VKMSL_INVALID, "break outside a loop"); return; }
    if (t->stack[top].kind == C_LOOP) { EMIT("break;\n"); return; }
    if (!t->stack[top].brk_flag) {
        t->stack[top].brk_flag = ++t->flag_count;
        sb_printf(&t->vars, "    bool _bf%d = false;\n", t->stack[top].brk_flag);
    }
    EMIT("{ _bf%d = true; break; }\n", t->stack[top].brk_flag);
}
static void emit_continue_construct(T *t, int loop);

// The jump from `from` to `to`; returns 1 when `to` is an ordinary block the caller goes on with.
static int jump(T *t, uint32_t from, uint32_t to, uint32_t stop) {
    emit_phis(t, from, to);
    if (to == stop) return 0;
    int L = innermost(t, C_LOOP), S = innermost(t, C_SWITCH);
    if (S > L && to == t->stack[S].merge) { EMIT("break;\n"); return 0; }
    if (L >= 0) {
        if (to == t->stack[L].header) { EMIT("continue;\n"); return 0; }
        if (to == t->stack[L].cont) { emit_continue_construct(t, L); return 0; }
        if (to == t->stack[L].merge) { break_loop(t); return 0; }
    }
    for (int i = t->depth - 1; i >= 0; i--)
        if (t->stack[i].kind == C_SELECTION && t->stack[i].merge == to) return 0;   // (an exit to an enclosing merge)
    return 1;
}

static void emit_continue_construct(T *t, int loop) {
    // (the continue construct runs inline wherever the loop body continues; it ends by branching to the header)
    uint32_t cont = t->stack[loop].cont;
    construct saved = t->stack[loop];
    t->stack[loop].cont = 0;                 // (inside it, a branch to the header is the back edge)
    emit_from(t, cont, 0);
    t->stack[loop] = saved;
}

static void emit_edge(T *t, uint32_t from, uint32_t to, uint32_t stop) {
    if (jump(t, from, to, stop)) emit_from(t, to, stop);
}

static void emit_block_insns(T *t, block *b) {
    for (size_t pos = b->first; pos < b->term && !t->rc;) {
        uint32_t op = t->w[pos] & 0xffff, n = t->w[pos] >> 16;
        if (pos == b->merge_pos) { pos += n; continue; }
        emit_insn(t, op, t->w + pos + 1, n - 1);
        pos += n;
    }
}

// Emits the terminator; returns the block control goes on with (0: none).
static uint32_t emit_terminator(T *t, block *b, uint32_t stop) {
    size_t pos = b->term;
    if (!pos) { fail(t, VKMSL_INVALID, "block %u has no terminator", b->label); return 0; }
    uint32_t op = t->w[pos] & 0xffff, n = t->w[pos] >> 16;
    const uint32_t *w = t->w + pos + 1;
    switch (op) {
    case OpBranch:
        return jump(t, b->label, w[0], stop) ? w[0] : 0;
    case OpBranchConditional: {
        const char *c = value(t, w[0]);
        if (b->merge_op == OpSelectionMerge) {
            uint32_t m = b->merge;
            if (t->depth >= 63) { fail(t, VKMSL_UNSUPPORTED, "nesting too deep"); return 0; }
            t->stack[t->depth++] = (construct){ C_SELECTION, m, 0, 0, 0 };
            if (w[1] == w[2]) emit_edge(t, b->label, w[1], m);
            else {
                EMIT("if (%s) {\n", c);
                t->indent++; emit_edge(t, b->label, w[1], m); t->indent--;
                EMIT("} else {\n");
                t->indent++; emit_edge(t, b->label, w[2], m); t->indent--;
                EMIT("}\n");
            }
            t->depth--;
            // (the merge block follows, unless it is a loop's continue, merge or header)
            return m;
        }
        EMIT("if (%s) {\n", c);
        t->indent++; emit_edge(t, b->label, w[1], 0); t->indent--;
        EMIT("} else {\n");
        t->indent++; emit_edge(t, b->label, w[2], 0); t->indent--;
        EMIT("}\n");
        return 0;
    }
    case OpSwitch: {
        if (b->merge_op != OpSelectionMerge) { fail(t, VKMSL_INVALID, "switch without a merge"); return 0; }
        uint32_t m = b->merge, sel = w[0], def = w[1];
        uint32_t sel_type = vtype(t, sel), lw = scalar_width(t, sel_type) == 64 ? 2 : 1;
        int sgn = is_signed(t, sel_type);
        if (t->depth >= 63) { fail(t, VKMSL_UNSUPPORTED, "nesting too deep"); return 0; }
        t->stack[t->depth++] = (construct){ C_SWITCH, m, 0, 0, 0 };
        EMIT("switch (%s) {\n", value(t, sel));
        // (cases grouped by target, in order; the default's group gets the default label)
        uint32_t targets[256], tc = 0;
        for (uint32_t i = 2; i + lw < n; i += 1 + lw) {
            uint32_t tg = w[i + lw], seen = 0;
            for (uint32_t k = 0; k < tc; k++) seen |= targets[k] == tg;
            if (!seen && tc < 256) targets[tc++] = tg;
        }
        int def_seen = 0;
        for (uint32_t k = 0; k < tc; k++) def_seen |= targets[k] == def;
        if (!def_seen && tc < 256) targets[tc++] = def;
        for (uint32_t k = 0; k < tc; k++) {
            for (uint32_t i = 2; i + lw < n; i += 1 + lw) {
                if (w[i + lw] != targets[k]) continue;
                if (lw == 2) EMIT("case %s%llu:\n", sgn ? "(long)" : "", (unsigned long long)w[i] | ((unsigned long long)w[i + 1] << 32));
                else if (sgn) EMIT("case %d:\n", (int32_t)w[i]);
                else EMIT("case %uu:\n", w[i]);
            }
            if (targets[k] == def) EMIT("default:\n");
            EMIT("{\n");
            t->indent++;
            emit_edge(t, b->label, targets[k], m);
            EMIT("break;\n");
            t->indent--;
            EMIT("}\n");
        }
        EMIT("}\n");
        int flag = t->stack[t->depth - 1].brk_flag;
        t->depth--;
        after_construct(t);
        if (flag) {
            // (cleared as it is acted on, so the switch can run again in an outer loop)
            EMIT("if (_bf%d) {\n", flag);
            t->indent++; EMIT("_bf%d = false;\n", flag); break_loop(t); t->indent--;
            EMIT("}\n");
        }
        return m;
    }
    case OpReturn: emit_return(t); return 0;
    case OpReturnValue: {
        int F = innermost(t, C_FUNC);
        if (F < 0) { fail(t, VKMSL_UNSUPPORTED, "return value from the entry point"); return 0; }
        EMIT("%s = %s;\n", dst(t, t->stack[F].merge), value(t, w[0]));
        emit_return(t);
        return 0;
    }
    case OpKill: case OpTerminateInvocation:
        t->res->uses_discard = 1;
        EMIT("discard_fragment();\n");
        emit_return(t);
        return 0;
    case OpUnreachable: return 0;
    default: fail(t, VKMSL_INVALID, "terminator %u", op); return 0;
    }
}

static void emit_from(T *t, uint32_t label, uint32_t stop) {
    int guard = 0;
    while (label && label != stop && !t->rc) {
        if (++guard > 100000) { fail(t, VKMSL_UNSUPPORTED, "control flow too large"); return; }
        block *b = blk(t, label);
        if (!b) return;
        if (b->merge_op == OpLoopMerge) {
            int L = innermost(t, C_LOOP);
            if (!(L >= 0 && t->stack[L].header == label && t->stack[L].cont == 0)) {
                if (t->depth >= 63) { fail(t, VKMSL_UNSUPPORTED, "nesting too deep"); return; }
                t->stack[t->depth++] = (construct){ C_LOOP, b->merge, b->cont, label, 0 };
                EMIT("while (true) {\n");
                t->indent++;
                emit_block_insns(t, b);
                uint32_t next = emit_terminator(t, b, 0);
                if (next) emit_from(t, next, 0);
                t->indent--;
                EMIT("}\n");
                t->depth--;
                after_construct(t);
                label = b->merge;
                continue;
            }
        }
        emit_block_insns(t, b);
        uint32_t next = emit_terminator(t, b, stop);
        if (next && b->merge_op == OpSelectionMerge) {
            // (after an if: the merge, unless it is a special target of the enclosing loop)
            if (!jump(t, 0, next, stop)) return;
        }
        label = next;
    }
}

// ---- variables --------------------------------------------------------------------------------------------------
static void setup_variable(T *t, uint32_t var) {
    ent *v = E(t, var);
    if (v->ptr || v->kind != K_VAR) return;
    uint32_t sc = v->w[2], pointee = tw(t, v->type)[2];
    switch (sc) {
    case SC_UniformConstant: case SC_Uniform: case SC_StorageBuffer: case SC_PushConstant:
        setup_resource_variable(t, var);
        return;
    case SC_Input: case SC_Output: case SC_Private: case SC_Function: case SC_Workgroup: {
        if (opaque_type(t, pointee)) {
            // (a local image/sampler variable: its stores give the expression its loads forward)
            pinfo *p = new_ptr(t, var, P_OPAQUE);
            if (p) { p->pointee = pointee; p->storage = sc; p->var = var; }
            return;
        }
        pinfo *p = new_ptr(t, var, P_LVALUE);
        if (!p) return;
        p->lvalue = xprintf("_%u", var);
        p->pointee = pointee;
        p->storage = sc;
        p->var = var;
        if (sc == SC_Workgroup) sb_printf(&t->vars, "    threadgroup %s _%u;\n", tname(t, pointee), var);
        else if (v->n > 3) sb_printf(&t->vars, "    %s _%u = %s;\n", tname(t, pointee), var, value(t, v->w[3]));
        else sb_printf(&t->vars, "    %s _%u;\n", tname(t, pointee), var);
        return;
    }
    default:
        fail(t, VKMSL_UNSUPPORTED, "storage class %u", sc);
    }
}

// ---- assembly ---------------------------------------------------------------------------------------------------
static const char *k_prelude =
    "#include <metal_stdlib>\n#include <simd/simd.h>\nusing namespace metal;\n"
    "template<typename T, uint N> struct spvArr {\n"
    "    T e[N];\n"
    "    thread T& operator[](uint i) thread { return e[i]; }\n"
    "    const thread T& operator[](uint i) const thread { return e[i]; }\n"
    "    threadgroup T& operator[](uint i) threadgroup { return e[i]; }\n"
    "    const threadgroup T& operator[](uint i) const threadgroup { return e[i]; }\n"
    "    device T& operator[](uint i) device { return e[i]; }\n"
    "    const device T& operator[](uint i) const device { return e[i]; }\n"
    "    const constant T& operator[](uint i) const constant { return e[i]; }\n"
    "};\n"
    "struct spvBufC { constant uchar *p; ulong n; };\n"
    "struct spvBufD { device uchar *p; ulong n; };\n";

static void emit_helpers(T *t, sb *out) {
    if (t->helper[H_FMOD])
        sb_puts(out, "template<typename T> T spvFMod(T x, T y) { return x - y * floor(x / y); }\n");
    if (t->helper[H_SMOD])
        sb_puts(out, "template<typename T> T spvSMod(T x, T y) { T r = x % y; return select(r, r + y, (r != 0) && ((r < 0) != (y < 0))); }\n");
    if (t->helper[H_SSIGN])
        sb_puts(out, "template<typename T> T spvSSign(T x) { return select(select(x, T(1), x > 0), T(-1), x < 0); }\n");
    if (t->helper[H_FINDLSB])
        sb_puts(out, "template<typename T> T spvFindLSB(T x) { return select(T(ctz(x)), T(-1), x == T(0)); }\n");
    if (t->helper[H_FINDUMSB])
        sb_puts(out, "template<typename T> T spvFindUMSB(T x) { return select(T(31) - T(clz(x)), T(-1), x == T(0)); }\n");
    if (t->helper[H_FINDSMSB])
        sb_puts(out, "template<typename T> T spvFindSMSB(T x) { T v = select(x, ~x, x < T(0)); return select(T(31) - T(clz(v)), T(-1), v == T(0)); }\n");
    if (t->helper[H_BALLOT])
        sb_puts(out, "inline uint4 spvBallot(bool p) { ulong b = ulong(simd_ballot(p)); return uint4(uint(b), uint(b >> 32), 0u, 0u); }\n");
    if (t->helper[H_INVERSE])
        sb_puts(out,
            "inline float2x2 spvInverse(float2x2 m) { float d = determinant(m); return float2x2(float2(m[1][1], -m[0][1]), float2(-m[1][0], m[0][0])) / d; }\n"
            "inline float3x3 spvInverse(float3x3 m) {\n"
            "    float3 a = m[0], b = m[1], c = m[2]; float3 r0 = cross(b, c), r1 = cross(c, a), r2 = cross(a, b);\n"
            "    float d = dot(a, r0); return transpose(float3x3(r0, r1, r2)) / d; }\n"
            "inline float4x4 spvInverse(float4x4 m) {\n"
            "    float a00 = m[0][0], a01 = m[0][1], a02 = m[0][2], a03 = m[0][3], a10 = m[1][0], a11 = m[1][1], a12 = m[1][2], a13 = m[1][3];\n"
            "    float a20 = m[2][0], a21 = m[2][1], a22 = m[2][2], a23 = m[2][3], a30 = m[3][0], a31 = m[3][1], a32 = m[3][2], a33 = m[3][3];\n"
            "    float b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10, b03 = a01 * a12 - a02 * a11;\n"
            "    float b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12, b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30;\n"
            "    float b08 = a20 * a33 - a23 * a30, b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;\n"
            "    float d = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;\n"
            "    return float4x4(float4(a11 * b11 - a12 * b10 + a13 * b09, a02 * b10 - a01 * b11 - a03 * b09, a31 * b05 - a32 * b04 + a33 * b03, a22 * b04 - a21 * b05 - a23 * b03),\n"
            "                    float4(a12 * b08 - a10 * b11 - a13 * b07, a00 * b11 - a02 * b08 + a03 * b07, a32 * b02 - a30 * b05 - a33 * b01, a20 * b05 - a22 * b02 + a23 * b01),\n"
            "                    float4(a10 * b10 - a11 * b08 + a13 * b06, a01 * b08 - a00 * b10 - a03 * b06, a30 * b04 - a31 * b02 + a33 * b00, a21 * b02 - a20 * b04 - a23 * b00),\n"
            "                    float4(a11 * b07 - a10 * b09 - a12 * b06, a00 * b09 - a01 * b07 + a02 * b06, a31 * b01 - a30 * b03 - a32 * b00, a20 * b03 - a21 * b01 + a22 * b00)) / d; }\n");
}

typedef struct { uint32_t var, builtin; const char *path; } bvar;

// The parameter (and its MSL type) a built-in input comes from, for the entry point's stage.
static const char *builtin_param(T *t, uint32_t bi, const char **decl) {
    *decl = NULL;
    if (t->vkernel) {
        switch (bi) {
        case BI_VertexIndex:
            return "(spvGP.indexed ? (spvGP.index16 ? uint(((const device ushort *)spvIdx)[spvTid.x]) : "
                   "((const device uint *)spvIdx)[spvTid.x]) + uint(spvGP.base_vertex) : spvGP.first + spvTid.x)";
        case BI_InstanceIndex: return "(spvGP.first_instance + spvTid.y)";
        case BI_BaseVertex: return "uint(spvGP.indexed ? spvGP.base_vertex : int(spvGP.first))";
        case BI_BaseInstance: return "spvGP.first_instance";
        case BI_ViewIndex: return "0u";
        default: break;
        }
    }
    if (t->gs) {
        switch (bi) {
        case BI_InvocationId: return "spvInvocation";
        case BI_PrimitiveId: return "spvTid.x";
        case BI_ViewIndex: return "0u";
        default: break;
        }
    }
    int vs = t->model == VKMSL_STAGE_VERTEX, fs = t->model == VKMSL_STAGE_FRAGMENT, cs = t->model == VKMSL_STAGE_COMPUTE;
    switch (bi) {
    case BI_VertexIndex: if (vs) { *decl = "uint spvVertexId [[vertex_id]]"; return "spvVertexId"; } break;
    case BI_InstanceIndex: if (vs) { *decl = "uint spvInstanceId [[instance_id]]"; return "spvInstanceId"; } break;
    case BI_BaseVertex: if (vs) { *decl = "uint spvBaseVertex [[base_vertex]]"; return "spvBaseVertex"; } break;
    case BI_BaseInstance: if (vs) { *decl = "uint spvBaseInstance [[base_instance]]"; return "spvBaseInstance"; } break;
    case BI_FragCoord: if (fs) { t->uses_fragcoord = 1; return "spvFragCoord"; } break;
    case BI_FrontFacing: if (fs) { *decl = "bool spvFrontFacing [[front_facing]]"; return "spvFrontFacing"; } break;
    case BI_SampleId: if (fs) { *decl = "uint spvSampleId [[sample_id]]"; t->res->uses_sample_rate = 1; return "spvSampleId"; } break;
    case BI_SamplePosition:
        if (fs) { *decl = "uint spvSampleId [[sample_id]]"; t->res->uses_sample_rate = 1; return "get_sample_position(spvSampleId)"; }
        break;
    case BI_SampleMask: if (fs) { *decl = "uint spvSampleMaskIn [[sample_mask]]"; return "spvSampleMaskIn"; } break;
    case BI_PointCoord: if (fs) { *decl = "float2 spvPointCoord [[point_coord]]"; return "spvPointCoord"; } break;
    case BI_Layer: if (fs) { *decl = "uint spvLayerIn [[render_target_array_index]]"; return "spvLayerIn"; } break;
    case BI_ViewportIndex: if (fs) { *decl = "uint spvViewportIn [[viewport_array_index]]"; return "spvViewportIn"; } break;
    case BI_PrimitiveId: if (fs) { *decl = "uint spvPrimitiveId [[primitive_id]]"; return "spvPrimitiveId"; } break;
    case BI_HelperInvocation: if (fs) return "simd_is_helper_thread()"; break;
    case BI_GlobalInvocationId: if (cs) { *decl = "uint3 spvGlobalId [[thread_position_in_grid]]"; return "spvGlobalId"; } break;
    case BI_LocalInvocationId: if (cs) { *decl = "uint3 spvLocalId [[thread_position_in_threadgroup]]"; return "spvLocalId"; } break;
    case BI_WorkgroupId: if (cs) { *decl = "uint3 spvGroupId [[threadgroup_position_in_grid]]"; return "spvGroupId"; } break;
    case BI_NumWorkgroups: if (cs) { *decl = "uint3 spvNumGroups [[threadgroups_per_grid]]"; return "spvNumGroups"; } break;
    case BI_LocalInvocationIndex: if (cs) { *decl = "uint spvLocalIndex [[thread_index_in_threadgroup]]"; return "spvLocalIndex"; } break;
    case BI_SubgroupId: if (cs) { *decl = "uint spvSubgroupId [[simdgroup_index_in_threadgroup]]"; return "spvSubgroupId"; } break;
    case BI_NumSubgroups: if (cs) { *decl = "uint spvNumSubgroups [[simdgroups_per_threadgroup]]"; return "spvNumSubgroups"; } break;
    case BI_ViewIndex: return "0u";   // (multiview: the one view of a pass without a view mask)
    case BI_SubgroupLocalInvocationId: t->uses_subgroup = 1; return "spvSubgroupLane";
    case BI_SubgroupSize: t->uses_subgroup = 1; return "spvSubgroupSize";
    case BI_SubgroupEqMask: t->uses_subgroup = 1; return "spvEqMask";
    case BI_SubgroupGeMask: t->uses_subgroup = 1; return "spvGeMask";
    case BI_SubgroupGtMask: t->uses_subgroup = 1; return "spvGtMask";
    case BI_SubgroupLeMask: t->uses_subgroup = 1; return "spvLeMask";
    case BI_SubgroupLtMask: t->uses_subgroup = 1; return "spvLtMask";
    default: break;
    }
    fail(t, VKMSL_UNSUPPORTED, "built-in input %u in stage %u", bi, t->model);
    return "0";
}

static int cmp_dm(const void *a, const void *b) {
    const dmember *x = a, *y = b;
    if (x->set != y->set) return x->set < y->set ? -1 : 1;
    return x->word < y->word ? -1 : x->word > y->word;
}

// The interface of a vertex shader run as a kernel (its outputs into slots of spvVOut) and of a geometry shader run as
// a kernel (its inputs from those slots, its outputs packed by EmitVertex into triangles of spvGOut).
static void builtin_slots(T *t, uint32_t bi, uint32_t type, const char *path, sb *out, const char *base, uint32_t L, int dir) {
    // (slot 0 position, L+1 and L+2 clip distances, L+3 point size / layer / viewport index)
    const char *a = dir == 0 ? "" : path;
    (void)a;
    switch (bi) {
    case BI_Position:
        if (dir == 0) sb_printf(out, "    %s[0] = %s;\n", base, path); else sb_printf(out, "    %s = %s[0];\n", path, base);
        break;
    case BI_PointSize:
        if (dir == 0) sb_printf(out, "    %s[%u].x = %s;\n", base, L + 3, path); else sb_printf(out, "    %s = %s[%u].x;\n", path, base, L + 3);
        break;
    case BI_ClipDistance: {
        uint32_t n = array_length(t, type);
        for (uint32_t k = 0; k < n && k < 8; k++) {
            if (dir == 0) sb_printf(out, "    %s[%u][%u] = %s[%u];\n", base, L + 1 + k / 4, k % 4, path, k);
            else sb_printf(out, "    %s[%u] = %s[%u][%u];\n", path, k, base, L + 1 + k / 4, k % 4);
        }
        if (n > t->gs_clip) t->gs_clip = n;
        break;
    }
    case BI_Layer:
        if (dir == 0) sb_printf(out, "    %s[%u].y = as_type<float>(uint(%s));\n", base, L + 3, path);
        else sb_printf(out, "    %s = %s(as_type<uint>(%s[%u].y));\n", path, tname(t, type), base, L + 3);
        t->res->uses_layer = 1;
        break;
    case BI_ViewportIndex:
        if (dir == 0) sb_printf(out, "    %s[%u].z = as_type<float>(uint(%s));\n", base, L + 3, path);
        else sb_printf(out, "    %s = %s(as_type<uint>(%s[%u].z));\n", path, tname(t, type), base, L + 3);
        t->res->uses_viewport_index = 1;
        break;
    default: break;   // (cull distances and others: not carried)
    }
}

static void kernel_interface(T *t, sb *in_decl, sb *in_init, sb *params, sb *prologue) {
    // The user locations each side carries.
    uint32_t out_loc = 0, in_loc = 0;
    for (uint32_t i = 0; i < t->iface_count; i++) {
        ent *v = E(t, t->iface[i]);
        if (v->kind != K_VAR || (v->d.deco & D_BUILTIN)) continue;
        uint32_t type = tw(t, v->type)[2];
        if (type_op(t, type) == OpTypeStruct && mdeco(t, type, 0)->deco & D_BUILTIN) continue;
        if (t->gs && v->w[2] == SC_Input && type_op(t, type) == OpTypeArray) type = tw(t, type)[1];
        if (type_op(t, type) == OpTypeArray && type_op(t, tw(t, type)[1]) == OpTypeStruct &&
            mdeco(t, tw(t, type)[1], 0)->deco & D_BUILTIN) continue;
        uint32_t loc = v->d.location, mx = 0;
        sb dummy = { 0 };
        char *path = xprintf("_%u", t->iface[i]);
        slot_leaves(t, type, path, &loc, &dummy, "x", 0, &mx);
        free(path); free(dummy.p);
        if (v->w[2] == SC_Output && mx > out_loc) out_loc = mx;
        if (v->w[2] == SC_Input && mx > in_loc) in_loc = mx;
    }
    if (t->vkernel) {
        uint32_t L = out_loc, S = L + 4;
        t->res->vary_locations = L;
        sb_printf(params, ",\n    uint2 spvTid [[thread_position_in_grid]]");
        sb_printf(params, ",\n    constant spvGsParams& spvGP [[buffer(%u)]]", VKMSL_GS_PARAMS);
        sb_printf(params, ",\n    device float4 *spvVOut [[buffer(%u)]]", VKMSL_GS_IN);
        sb_printf(params, ",\n    const device uchar *spvIdx [[buffer(%u)]]", VKMSL_GS_INDEX);
        sb_puts(prologue, "    if (spvTid.x >= spvGP.count || spvTid.y >= spvGP.instances) return;\n");
        sb_printf(&t->ret, "    device float4 *spvV = spvVOut + (spvTid.y * spvGP.count + spvTid.x) * %uu;\n", S);
        sb_printf(&t->ret, "    for (uint i = 0; i < %uu; i++) spvV[i] = float4(0.0f);\n", S);
        for (uint32_t i = 0; i < t->iface_count; i++) {
            uint32_t var = t->iface[i];
            ent *v = E(t, var);
            if (v->kind != K_VAR) continue;
            uint32_t sc = v->w[2], type = tw(t, v->type)[2];
            int builtin_block = type_op(t, type) == OpTypeStruct && mdeco(t, type, 0)->deco & D_BUILTIN;
            char *path = xprintf("_%u", var);
            if (sc == SC_Input) {
                if (v->d.deco & D_BUILTIN) {
                    const char *decl;
                    const char *src = builtin_param(t, v->d.builtin, &decl);
                    sb_printf(prologue, "    _%u = %s(%s);\n", var, tname(t, type), src);
                } else {
                    uint32_t loc = v->d.location;
                    leafctx c = { in_decl, in_init, 1, v->d.deco, 0 };
                    varying_leaves(t, &c, type, path, &loc);
                }
            } else if (sc == SC_Output) {
                if (builtin_block) {
                    for (uint32_t m = 0; m + 1 < E(t, type)->n; m++) {
                        if (!(v->written & (1u << m))) continue;
                        char *mp = xprintf("_%u.m%u", var, m);
                        builtin_slots(t, mdeco(t, type, m)->builtin, tw(t, type)[m + 1], mp, &t->ret, "spvV", L, 0);
                        free(mp);
                    }
                } else if (v->d.deco & D_BUILTIN) builtin_slots(t, v->d.builtin, type, path, &t->ret, "spvV", L, 0);
                else { uint32_t loc = v->d.location, mx = 0; slot_leaves(t, type, path, &loc, &t->ret, "spvV", 0, &mx); }
            }
            free(path);
        }
        return;
    }
    // Geometry: N vertices in from slots of IS, outputs of OS slots per vertex, up to MAXT triangles per run.
    uint32_t N = t->gs_in_n, IS = t->opt->vary_slots + 4;
    t->gs_loc = out_loc;
    t->gs_os = out_loc + 4;
    t->res->gs_in_vertices = N;
    t->res->gs_max_triangles = t->gs_maxt;
    t->res->gs_out_slots = t->gs_os;
    t->res->gs_invocations = t->gs_invocations;
    sb_printf(params, ",\n    uint2 spvTid [[thread_position_in_grid]]");
    sb_printf(params, ",\n    constant spvGsParams& spvGP [[buffer(%u)]]", VKMSL_GS_PARAMS);
    sb_printf(params, ",\n    const device float4 *spvGIn [[buffer(%u)]]", VKMSL_GS_IN);
    sb_printf(params, ",\n    device float4 *spvGOut [[buffer(%u)]]", VKMSL_GS_OUT);
    sb_printf(params, ",\n    device uint *spvGCount [[buffer(%u)]]", VKMSL_GS_COUNT);
    sb_puts(prologue,
        "    if (spvTid.x >= spvGP.prims) return;\n"
        "    uint spvInstance = spvTid.y / spvGP.invocations, spvInvocation = spvTid.y % spvGP.invocations;\n"
        "    uint spvRun = spvTid.y * spvGP.prims + spvTid.x;\n"
        "    uint spvVert[6];\n"
        "    uint spvP = spvTid.x;\n");
    // (the primitive's vertices: lists, strips (odd triangles turned back), fans, with adjacency)
    sb_printf(prologue,
        "    switch (spvGP.topology) {\n"
        "    case 0u: spvVert[0] = spvP; break;\n"
        "    case 1u: spvVert[0] = 2u * spvP; spvVert[1] = 2u * spvP + 1u; break;\n"
        "    case 2u: spvVert[0] = spvP; spvVert[1] = spvP + 1u; break;\n"
        "    case 4u: if (spvP & 1u) { spvVert[0] = spvP + 1u; spvVert[1] = spvP; } else { spvVert[0] = spvP; spvVert[1] = spvP + 1u; } spvVert[2] = spvP + 2u; break;\n"
        "    case 5u: spvVert[0] = spvP + 1u; spvVert[1] = spvP + 2u; spvVert[2] = 0u; break;\n"
        "    case 6u: for (uint j = 0; j < 4u; j++) spvVert[j] = 4u * spvP + j; break;\n"
        "    case 7u: for (uint j = 0; j < 4u; j++) spvVert[j] = spvP + j; break;\n"
        "    case 8u: for (uint j = 0; j < 6u; j++) spvVert[j] = 6u * spvP + j; break;\n"
        "    default: for (uint j = 0; j < 3u; j++) spvVert[j] = 3u * spvP + j; break;\n"
        "    }\n"
        "    for (uint j = 0; j < %uu; j++) spvVert[j] = (spvInstance * spvGP.count + spvVert[j]) * %uu;\n", N, IS);
    sb_printf(prologue, "    spvArr<float4, %u> spvA, spvB, spvCur;\n    uint spvStrip = 0, spvTris = 0;\n", t->gs_os);
    sb_printf(&t->ret, "    spvGCount[spvRun] = spvTris;\n");
    for (uint32_t i = 0; i < t->iface_count; i++) {
        uint32_t var = t->iface[i];
        ent *v = E(t, var);
        if (v->kind != K_VAR) continue;
        uint32_t sc = v->w[2], type = tw(t, v->type)[2];
        char *path = xprintf("_%u", var);
        if (sc == SC_Input) {
            if (type_op(t, type) != OpTypeArray) {
                // (per-primitive built-ins: InvocationId, PrimitiveId)
                const char *decl;
                const char *src = builtin_param(t, v->d.builtin, &decl);
                sb_printf(in_init, "    _%u = %s(%s);\n", var, tname(t, type), src);
            } else {
                uint32_t elem = tw(t, type)[1];
                int bblock = type_op(t, elem) == OpTypeStruct && mdeco(t, elem, 0)->deco & D_BUILTIN;
                for (uint32_t j = 0; j < N; j++) {
                    char *ep = xprintf("_%u[%u]", var, j), *base = xprintf("(spvGIn + spvVert[%u])", j);
                    if (bblock) {
                        for (uint32_t m = 0; m + 1 < E(t, elem)->n; m++) {
                            char *mp = xprintf("%s.m%u", ep, m);
                            builtin_slots(t, mdeco(t, elem, m)->builtin, tw(t, elem)[m + 1], mp, in_init, base, t->opt->vary_slots, 1);
                            free(mp);
                        }
                    } else if (v->d.deco & D_BUILTIN) builtin_slots(t, v->d.builtin, elem, ep, in_init, base, t->opt->vary_slots, 1);
                    else { uint32_t loc = v->d.location, mx = 0; slot_leaves(t, elem, ep, &loc, in_init, base, 1, &mx); }
                    free(ep); free(base);
                }
            }
        } else if (sc == SC_Output) {
            int builtin_block = type_op(t, type) == OpTypeStruct && mdeco(t, type, 0)->deco & D_BUILTIN;
            if (builtin_block) {
                for (uint32_t m = 0; m + 1 < E(t, type)->n; m++) {
                    char *mp = xprintf("_%u.m%u", var, m);
                    builtin_slots(t, mdeco(t, type, m)->builtin, tw(t, type)[m + 1], mp, &t->gs_pack, "spvCur", t->gs_loc, 0);
                    free(mp);
                }
            } else if (v->d.deco & D_BUILTIN) builtin_slots(t, v->d.builtin, type, path, &t->gs_pack, "spvCur", t->gs_loc, 0);
            else { uint32_t loc = v->d.location, mx = 0; slot_leaves(t, type, path, &loc, &t->gs_pack, "spvCur", 0, &mx); }
        }
        free(path);
    }
}

// "vkpass": draws a geometry kernel's triangles (vertex id = triangle * 3 + corner, MAXT triangles per run, the
// unused ones placed off the screen), with the fragment shader's inputs at the geometry outputs' locations.
static void gs_passthrough(T *t, sb *src) {
    uint32_t L = t->gs_loc, OS = t->gs_os, MAXT = t->gs_maxt;
    sb decl = { 0 }, body = { 0 };
    sb_puts(&decl, "    float4 spvPosition [[position]];\n");
    for (uint32_t i = 0; i < t->iface_count; i++) {
        ent *v = E(t, t->iface[i]);
        if (v->kind != K_VAR || v->w[2] != SC_Output || (v->d.deco & D_BUILTIN)) continue;
        uint32_t type = tw(t, v->type)[2];
        if (type_op(t, type) == OpTypeStruct && mdeco(t, type, 0)->deco & D_BUILTIN) continue;
        // (each location's scalar class, as the fragment shader declares it: float4, int4 or uint4)
        uint32_t loc = v->d.location, base = loc, mx = 0;
        sb dummy = { 0 };
        char *path = xprintf("_%u", t->iface[i]);
        slot_leaves(t, type, path, &loc, &dummy, "x", 0, &mx);
        free(path); free(dummy.p);
        uint32_t s = scalar_of(t, type_op(t, type) == OpTypeArray || type_op(t, type) == OpTypeMatrix ? tw(t, type)[1] : type);
        const char *v4 = is_float(t, s) || is_bool(t, s) ? "float4" : is_signed(t, s) ? "int4" : "uint4";
        for (uint32_t l = base; l < mx; l++) {
            sb_printf(&decl, "    %s v%u [[user(locn%u)]];\n", v4, l, l);
            if (!strcmp(v4, "float4")) sb_printf(&body, "    o.v%u = v[%u];\n", l, 1 + l);
            else sb_printf(&body, "    o.v%u = as_type<%s>(v[%u]);\n", l, v4, 1 + l);
        }
    }
    if (t->gs_clip) {
        sb_printf(&decl, "    float spvClip [[clip_distance]] [%u];\n", t->gs_clip);
        for (uint32_t k = 0; k < t->gs_clip; k++) sb_printf(&body, "    o.spvClip[%u] = v[%u][%u];\n", k, L + 1 + k / 4, k % 4);
    }
    if (t->res->uses_layer) { sb_puts(&decl, "    uint spvLayer [[render_target_array_index]];\n"); sb_printf(&body, "    o.spvLayer = as_type<uint>(v[%u].y);\n", L + 3); }
    if (t->res->uses_viewport_index) { sb_puts(&decl, "    uint spvViewport [[viewport_array_index]];\n"); sb_printf(&body, "    o.spvViewport = as_type<uint>(v[%u].z);\n", L + 3); }
    sb_printf(src, "struct spvPassOut {\n%s};\n", decl.p);
    sb_printf(src, "vertex spvPassOut vkpass(uint vid [[vertex_id]], const device float4 *spvGOut [[buffer(0)]], const device uint *spvGCount [[buffer(1)]]) {\n"
                   "    uint tri = vid / 3u, run = tri / %uu;\n"
                   "    const device float4 *v = spvGOut + vid * %uu;\n"
                   "    spvPassOut o;\n"
                   "    o.spvPosition = tri - run * %uu < spvGCount[run] ? v[0] : float4(0.0f, 0.0f, -2.0f, 1.0f);\n"
                   "    o.spvPosition.y = -o.spvPosition.y;\n%s"
                   "    return o;\n}\n", MAXT, OS, MAXT, body.p ? body.p : "");
    free(decl.p);
    free(body.p);
}

// Finds the resource variables a depth-comparison sample reads, back through loads, copies, sampled-image
// combinations, local variables (their stores), phis and function parameters (their call sites).
static void mark_depth_compared(T *t) {
    uint32_t *work = NULL, nwork = 0, cap = 0;
    uint8_t *seen = calloc(t->bound, 1);
#define PUSH(x) do { uint32_t x_ = (x); if (x_ < t->bound && !seen[x_]) { seen[x_] = 1; \
        if (nwork == cap) { cap = cap ? cap * 2 : 64; work = realloc(work, cap * sizeof *work); } work[nwork++] = x_; } } while (0)
    for (uint32_t i = 0; i < t->block_count; i++)
        for (size_t pos = t->blocks[i].first; pos < t->blocks[i].term;) {
            uint32_t op = t->w[pos] & 0xffff, n = t->w[pos] >> 16;
            if ((op == OpImageSampleDrefImplicitLod || op == OpImageSampleDrefExplicitLod || op == OpImageSampleProjDrefImplicitLod ||
                 op == OpImageSampleProjDrefExplicitLod || op == OpImageDrefGather) && n > 3) PUSH(t->w[pos + 3]);
            pos += n;
        }
    while (nwork && seen) {
        uint32_t id = work[--nwork];
        ent *e = &t->e[id];
        if (e->kind == K_VAR) {
            uint32_t sc = e->w[2];
            if (sc == SC_UniformConstant) { e->d.deco |= D_DEPTHCMP; continue; }
            // (a local variable: what is stored into it)
            for (uint32_t i = 0; i < t->block_count; i++)
                for (size_t pos = t->blocks[i].first; pos < t->blocks[i].term;) {
                    uint32_t op = t->w[pos] & 0xffff, n = t->w[pos] >> 16;
                    if (op == OpStore && n > 2 && t->w[pos + 1] == id) PUSH(t->w[pos + 2]);
                    pos += n;
                }
            continue;
        }
        if (e->kind != K_VALUE) continue;
        switch (e->op) {
        case OpLoad: case OpSampledImage: case OpImage: case OpCopyObject: case OpAccessChain: case OpInBoundsAccessChain:
            if (e->n > 2) PUSH(e->w[2]);
            break;
        case OpPhi:
            for (uint32_t k = 2; k + 1 < e->n; k += 2) PUSH(e->w[k]);
            break;
        case OpSelect:
            if (e->n > 4) { PUSH(e->w[3]); PUSH(e->w[4]); }
            break;
        case OpFunctionParameter: {
            // (the arguments of every call of its function)
            for (uint32_t f = 1; f < t->bound; f++) {
                ent *fe = &t->e[f];
                if (fe->kind != K_FUNC) continue;
                for (uint32_t k = 0; k < fe->mcount; k++) {
                    if (t->fparams[fe->mbase + k] != id) continue;
                    for (uint32_t i = 0; i < t->block_count; i++)
                        for (size_t pos = t->blocks[i].first; pos < t->blocks[i].term;) {
                            uint32_t op = t->w[pos] & 0xffff, n = t->w[pos] >> 16;
                            if (op == OpFunctionCall && n > 4 + k && t->w[pos + 3] == f) PUSH(t->w[pos + 4 + k]);
                            pos += n;
                        }
                }
            }
            break;
        }
        default: break;
        }
    }
#undef PUSH
    free(work);
    free(seen);
}

int vkmsl_translate(const uint32_t *words, size_t word_count, const vkmsl_options *options, vkmsl_result *result) {
    if (!result) return VKMSL_INVALID;
    memset(result, 0, sizeof *result);
    if (!words || !options) { snprintf(result->error, sizeof result->error, "no module"); return VKMSL_INVALID; }
    T tt;
    T *t = &tt;
    memset(t, 0, sizeof *t);
    t->w = words; t->count = word_count; t->opt = options; t->res = result;
    t->indent = 1;
    if (!parse(t)) goto out;
    if (!t->block_count) { fail(t, VKMSL_INVALID, "entry point has no blocks"); goto out; }
    t->vkernel = t->model == VKMSL_STAGE_VERTEX && (options->flags & VKMSL_FLAG_VERTEX_KERNEL);
    t->gs = t->model == VKMSL_STAGE_GEOMETRY;
    if (t->gs) {
        if (t->gs_out_prim != 3) { fail(t, VKMSL_UNSUPPORTED, "geometry shader output other than triangle strips"); goto out; }
        if (!t->gs_in_n) t->gs_in_n = 3;
        if (!t->gs_invocations) t->gs_invocations = 1;
        t->gs_maxt = t->gs_maxt > 2 ? t->gs_maxt - 2 : 1;
    }

    // Images sampled with depth comparisons are depth textures whatever their type says (HLSL compilers leave Depth 0).
    mark_depth_compared(t);
    // Every variable gets its pointer form: resources and interface variables are set up on first use below.
    for (uint32_t id = 1; id < t->bound; id++) {
        ent *e = &t->e[id];
        if (e->kind != K_VAR) continue;
        uint32_t sc = e->w[2];
        if (sc == SC_Input || sc == SC_Output) { if (is_interface(t, id)) setup_variable(t, id); }
        else if (sc == SC_Private || sc == SC_Workgroup) setup_variable(t, id);
    }
    // Function-scope variables of the entry point and its phis are declared up front.
    for (uint32_t i = 0; i < t->block_count; i++) {
        block *b = &t->blocks[i];
        for (size_t pos = b->first; pos < b->term;) {
            uint32_t op = t->w[pos] & 0xffff, n = t->w[pos] >> 16;
            const uint32_t *w = t->w + pos + 1;
            if (op == OpVariable) setup_variable(t, w[1]);
            else if (op == OpPhi) declare(t, w[1]);
            pos += n;
        }
    }
    for (uint32_t id = 1; id < t->bound; id++) {
        ent *e = &t->e[id];
        if (e->kind == K_VAR && !e->ptr) {
            uint32_t sc = e->w[2];
            if (sc == SC_UniformConstant || sc == SC_Uniform || sc == SC_StorageBuffer || sc == SC_PushConstant) {
                // (only the resources the entry point's code reaches: set up when an instruction names them)
                (void)sc;
            }
        }
    }
    // Resource variables used by the entry point.
    for (uint32_t i = 0; i < t->block_count && !t->rc; i++) {
        block *b = &t->blocks[i];
        for (size_t pos = b->first; pos <= b->term;) {
            uint32_t n = t->w[pos] >> 16;
            if (!n) break;
            for (uint32_t k = 1; k < n; k++) {
                uint32_t id = t->w[pos + k];
                if (id < t->bound && t->e[id].kind == K_VAR && !t->e[id].ptr) setup_variable(t, id);
            }
            pos += n;
        }
    }
    if (t->rc) goto out;

    emit_from(t, t->blocks[t->entry_block].label, 0);
    if (t->rc) goto out;

    // ---- the stage interface
    sb in_decl = { 0 }, in_init = { 0 }, out_decl = { 0 }, params = { 0 }, prologue = { 0 };
    const char *ret_type = "void";
    uint32_t clip_count = 0;
    if (t->vkernel || t->gs) kernel_interface(t, &in_decl, &in_init, &params, &prologue);
    for (uint32_t i = 0; i < t->iface_count && !t->vkernel && !t->gs; i++) {
        uint32_t var = t->iface[i];
        ent *v = E(t, var);
        if (v->kind != K_VAR) continue;
        uint32_t sc = v->w[2], type = tw(t, v->type)[2];
        int builtin_block = type_op(t, type) == OpTypeStruct && mdeco(t, type, 0)->deco & D_BUILTIN;
        if (sc == SC_Input) {
            if (v->d.deco & D_BUILTIN) {
                const char *decl;
                const char *src = builtin_param(t, v->d.builtin, &decl);
                if (decl) sb_printf(&params, ",\n    %s", decl);
                int top = type_op(t, type);
                if (top == OpTypeArray) sb_printf(&prologue, "    _%u[0] = %s(%s);\n", var, tname(t, tw(t, type)[1]), src);
                else sb_printf(&prologue, "    _%u = %s(%s);\n", var, tname(t, type), src);
            } else if (builtin_block) {
                fail(t, VKMSL_UNSUPPORTED, "built-in input block");
            } else if (t->model == VKMSL_STAGE_VERTEX || t->model == VKMSL_STAGE_FRAGMENT) {
                uint32_t loc = v->d.location;
                leafctx c = { &in_decl, &in_init, 1, v->d.deco, v->d.index };
                char *path = xprintf("_%u", var);
                varying_leaves(t, &c, type, path, &loc);
                free(path);
            }
        } else if (sc == SC_Output) {
            uint8_t used = v->written;
            if (v->d.deco & D_BUILTIN || builtin_block) {
                uint32_t members = builtin_block ? E(t, type)->n - 1 : 1;
                for (uint32_t m = 0; m < members; m++) {
                    uint32_t bi = builtin_block ? mdeco(t, type, m)->builtin : v->d.builtin;
                    uint32_t mt = builtin_block ? tw(t, type)[m + 1] : type;
                    if (builtin_block && !(used & (1u << m))) continue;
                    if (!used) continue;
                    char *src = builtin_block ? xprintf("_%u.m%u", var, m) : xprintf("_%u", var);
                    if (t->model == VKMSL_STAGE_VERTEX) {
                        switch (bi) {
                        case BI_Position:
                            sb_puts(&out_decl, "    float4 spvPosition [[position]];\n");
                            sb_printf(&t->ret, "    spvOut.spvPosition = %s;\n", src);
                            if (!(t->opt->flags & VKMSL_FLAG_NO_FLIP_Y)) sb_puts(&t->ret, "    spvOut.spvPosition.y = -spvOut.spvPosition.y;\n");
                            break;
                        case BI_PointSize:
                            sb_puts(&out_decl, "    float spvPointSize [[point_size]];\n");
                            sb_printf(&t->ret, "    spvOut.spvPointSize = %s;\n", src);
                            break;
                        case BI_ClipDistance:
                            clip_count = array_length(t, mt);
                            if (clip_count) {
                                sb_printf(&out_decl, "    float spvClip [[clip_distance]] [%u];\n", clip_count);
                                for (uint32_t k = 0; k < clip_count; k++) sb_printf(&t->ret, "    spvOut.spvClip[%u] = %s[%u];\n", k, src, k);
                            }
                            break;
                        case BI_CullDistance: break;   // (Metal has no cull distances: primitives are kept)
                        case BI_Layer:
                            sb_puts(&out_decl, "    uint spvLayer [[render_target_array_index]];\n");
                            sb_printf(&t->ret, "    spvOut.spvLayer = uint(%s);\n", src);
                            t->res->uses_layer = 1;
                            break;
                        case BI_ViewportIndex:
                            sb_puts(&out_decl, "    uint spvViewport [[viewport_array_index]];\n");
                            sb_printf(&t->ret, "    spvOut.spvViewport = uint(%s);\n", src);
                            t->res->uses_viewport_index = 1;
                            break;
                        default: fail(t, VKMSL_UNSUPPORTED, "vertex built-in output %u", bi);
                        }
                    } else if (t->model == VKMSL_STAGE_FRAGMENT) {
                        if (bi == BI_FragDepth && (t->opt->flags & VKMSL_FLAG_NO_DEPTH_ATTACHMENT)) {
                            // no depth attachment: Vulkan drops the value; Metal refuses a depth output without one
                        } else if (bi == BI_FragDepth) {
                            const char *mode = t->depth_mode == 2 ? "greater" : t->depth_mode == 3 ? "less" : "any";
                            sb_printf(&out_decl, "    float spvDepth [[depth(%s)]];\n", mode);
                            sb_printf(&t->ret, "    spvOut.spvDepth = %s;\n", src);
                            t->res->writes_depth = 1;
                        } else if (bi == BI_SampleMask) {
                            sb_puts(&out_decl, "    uint spvSampleMask [[sample_mask]];\n");
                            sb_printf(&t->ret, "    spvOut.spvSampleMask = uint(%s[0]);\n", src);
                            t->res->writes_sample_mask = 1;
                        } else fail(t, VKMSL_UNSUPPORTED, "fragment built-in output %u", bi);
                    }
                    free(src);
                }
            } else if (t->model == VKMSL_STAGE_VERTEX || t->model == VKMSL_STAGE_FRAGMENT) {
                uint32_t loc = v->d.location;
                leafctx c = { &out_decl, &t->ret, 0, v->d.deco, (v->d.deco & D_INDEX) ? v->d.index : 0 };
                char *path = xprintf("_%u", var);
                varying_leaves(t, &c, type, path, &loc);
                free(path);
            }
        }
    }
    result->clip_distances = clip_count;
    if (t->model == VKMSL_STAGE_VERTEX && !t->vkernel && !strstr(out_decl.p ? out_decl.p : "", "[[position]]")) {
        // (a vertex function must write a position: one that writes none places everything at the origin)
        sb_puts(&out_decl, "    float4 spvPosition [[position]];\n");
        sb_puts(&t->ret, "    spvOut.spvPosition = float4(0.0f);\n");
    }
    if (t->uses_fragcoord) sb_puts(&params, ",\n    float4 spvFragCoord [[position]]");
    if (t->uses_subgroup && t->model != VKMSL_STAGE_VERTEX) {
        sb_puts(&params, ",\n    uint spvSubgroupLane [[thread_index_in_simdgroup]],\n    uint spvSubgroupSize [[threads_per_simdgroup]]");
        sb_puts(&prologue,
            "    uint4 spvEqMask = uint4(1u << spvSubgroupLane, 0u, 0u, 0u);\n"
            "    uint4 spvLtMask = uint4((1u << spvSubgroupLane) - 1u, 0u, 0u, 0u);\n"
            "    uint4 spvLeMask = uint4((2u << spvSubgroupLane) - 1u, 0u, 0u, 0u);\n"
            "    uint4 spvGtMask = uint4(~((2u << spvSubgroupLane) - 1u), 0u, 0u, 0u);\n"
            "    uint4 spvGeMask = uint4(~((1u << spvSubgroupLane) - 1u), 0u, 0u, 0u);\n"
            "    (void)spvEqMask; (void)spvLtMask; (void)spvLeMask; (void)spvGtMask; (void)spvGeMask; (void)spvSubgroupSize;\n");
    } else if (t->uses_subgroup) fail(t, VKMSL_UNSUPPORTED, "subgroup operations in a vertex shader");

    // ---- workgroup size
    if (t->model == VKMSL_STAGE_COMPUTE) {
        for (int k = 0; k < 3; k++) result->workgroup_size[k] = t->local_size[k];
        if (t->has_local_size_id)
            for (int k = 0; k < 3; k++) { int ok = 1; result->workgroup_size[k] = const_u32(t, t->local_size_id[k], &ok); }
        for (uint32_t id = 1; id < t->bound; id++)
            if ((t->e[id].kind == K_CONST || t->e[id].kind == K_SPEC) && (t->e[id].d.deco & D_BUILTIN) &&
                t->e[id].d.builtin == BI_WorkgroupSize && t->e[id].n >= 5)
                for (int k = 0; k < 3; k++) { int ok = 1; result->workgroup_size[k] = const_u32(t, t->e[id].w[2 + k], &ok); }
        for (int k = 0; k < 3; k++) if (!result->workgroup_size[k]) result->workgroup_size[k] = 1;
    }

    // ---- the source
    sb src = { 0 };
    sb_puts(&src, k_prelude);
    emit_helpers(t, &src);
    sb_putn(&src, t->pre.p ? t->pre.p : "", t->pre.n);
    // Descriptor set argument buffers: members at their words, gaps padded.
    if (t->dm_count) qsort(t->dm, t->dm_count, sizeof *t->dm, cmp_dm);
    for (uint32_t s = 0; s < VKMSL_MAX_SETS; s++) {
        if (!t->set_used[s]) continue;
        result->sets_used |= 1u << s;
        sb_printf(&src, "struct spvDS%u {\n", s);
        uint32_t word = 0;
        for (uint32_t i = 0; i < t->dm_count; i++) {
            dmember *m = &t->dm[i];
            if (m->set != s) continue;
            if (m->word < word) { fail(t, VKMSL_INVALID, "set %u: bindings overlap at word %u", s, m->word); break; }
            if (m->word > word) sb_printf(&src, "    ulong _pad%u[%u];\n", word, m->word - word);
            sb_printf(&src, "    %s;\n", m->decl);
            word = m->word + m->words;
        }
        sb_puts(&src, "};\n");
        sb_printf(&params, ",\n    constant spvDS%u& ds%u [[buffer(%u)]]", s, s, s);
    }
    if (result->uses_push_constants) sb_printf(&params, ",\n    constant uchar* spvPC [[buffer(%u)]]", VKMSL_PUSH_BUFFER);
    if (result->uses_aux) sb_printf(&params, ",\n    constant uint* spvAux [[buffer(%u)]]", VKMSL_AUX_BUFFER);
    sb_putn(&src, t->glob.p ? t->glob.p : "", t->glob.n);

    snprintf(result->entry_name, sizeof result->entry_name, "vkmain");
    const char *stage_kw = t->vkernel || t->gs ? "kernel" : t->model == VKMSL_STAGE_VERTEX ? "vertex" :
                           t->model == VKMSL_STAGE_FRAGMENT ? "fragment" : t->model == VKMSL_STAGE_COMPUTE ? "kernel" : NULL;
    if (t->vkernel || t->gs)
        sb_puts(&src, "struct spvGsParams { uint count, instances, first, first_instance; int base_vertex; uint indexed, index16, prims;\n"
                      "    uint topology, invocations, in_slots, out_max_tris; };\n");
    if (!stage_kw) { fail(t, VKMSL_UNSUPPORTED, "stage %u", t->model); goto cleanup; }
    int has_in = in_decl.n > 0, has_out = out_decl.n > 0 && t->model != VKMSL_STAGE_COMPUTE && !t->vkernel && !t->gs;
    if (has_in) sb_printf(&src, "struct spvInput {\n%s};\n", in_decl.p);
    if (has_out) { sb_printf(&src, "struct spvOutput {\n%s};\n", out_decl.p); ret_type = "spvOutput"; }
    if (t->model == VKMSL_STAGE_FRAGMENT && t->early_tests) { sb_puts(&src, "[[early_fragment_tests]] "); result->early_fragment_tests = 1; }
    sb_printf(&src, "%s %s vkmain(", stage_kw, ret_type);
    if (has_in) sb_puts(&params, ",\n    spvInput spvIn [[stage_in]]");
    // (params is a list of ",\n    decl": its first separator is dropped)
    if (params.n > 2) sb_putn(&src, params.p + 2, params.n - 2);
    sb_puts(&src, ") {\n");
    if (has_out) sb_puts(&src, "    spvOutput spvOut = {};\n");
    sb_putn(&src, t->vars.p ? t->vars.p : "", t->vars.n);
    sb_putn(&src, prologue.p ? prologue.p : "", prologue.n);
    sb_putn(&src, in_init.p ? in_init.p : "", in_init.n);
    // The body, its returns expanded to the output stores.
    char *ret = has_out ? xprintf("{\n%s    return spvOut;\n}\n", t->ret.p ? t->ret.p : "") :
                (t->vkernel || t->gs) ? xprintf("{\n%s    return;\n}\n", t->ret.p ? t->ret.p : "") : xstrdup("return;\n");
    // (a geometry shader's EmitVertex: its outputs into spvCur, a triangle once the strip has three vertices)
    char *emit = t->gs ? xprintf("{\n    for (uint i = 0; i < %uu; i++) spvCur[i] = float4(0.0f);\n%s"
        "    if (spvStrip >= 2u && spvTris < %uu) {\n"
        "        device float4 *o = spvGOut + (spvRun * %uu + spvTris) * %uu;\n"
        "        bool odd = (spvStrip & 1u) != 0u;\n"
        "        for (uint i = 0; i < %uu; i++) { o[i] = odd ? spvB[i] : spvA[i]; o[%uu + i] = odd ? spvA[i] : spvB[i]; o[%uu + i] = spvCur[i]; }\n"
        "        spvTris++;\n    }\n"
        "    spvA = spvB; spvB = spvCur; spvStrip++;\n}\n", t->gs_os, t->gs_pack.p ? t->gs_pack.p : "", t->gs_maxt, t->gs_maxt,
        3 * t->gs_os, t->gs_os, t->gs_os, 2 * t->gs_os) : NULL;
    for (const char *p = t->body.p ? t->body.p : ""; *p;) {
        const char *m = strstr(p, "@RET@\n"), *e = emit ? strstr(p, "@EMIT@\n") : NULL;
        if (e && (!m || e < m)) { sb_putn(&src, p, (size_t)(e - p)); sb_puts(&src, emit); p = e + 7; continue; }
        if (!m) { sb_puts(&src, p); break; }
        sb_putn(&src, p, (size_t)(m - p));
        sb_puts(&src, ret);
        p = m + 6;
    }
    free(ret);
    free(emit);
    if (t->vkernel || t->gs) { sb_putn(&src, t->ret.p ? t->ret.p : "", t->ret.n); sb_puts(&src, "}\n"); }
    else if (!has_out) sb_puts(&src, "}\n");
    else { sb_putn(&src, t->ret.p ? t->ret.p : "", t->ret.n); sb_puts(&src, "    return spvOut;\n}\n"); }
    if (t->gs) gs_passthrough(t, &src);
    if (src.oom || t->body.oom || t->vars.oom) fail(t, VKMSL_OUT_OF_MEMORY, "out of memory");
    if (!t->rc) { result->msl = src.p; result->msl_size = src.n; src.p = NULL; }
cleanup:
    free(src.p); free(in_decl.p); free(in_init.p); free(out_decl.p); free(params.p); free(prologue.p);
out:
    for (uint32_t i = 0; t->e && i < t->bound; i++) {
        free(t->e[i].expr); free(t->e[i].aux); free(t->e[i].tname);
        if (t->e[i].ptr) {
            pinfo *p = t->e[i].ptr;
            free(p->lvalue); free(p->base); free(p->offset); free(p->coord); free(p->sample); free(p);
        }
    }
    for (uint32_t i = 0; i < t->dm_count; i++) free(t->dm[i].decl);
    free(t->dm); free(t->e); free(t->members); free(t->blocks); free(t->fparams);
    free(t->pre.p); free(t->glob.p); free(t->vars.p); free(t->body.p); free(t->ret.p); free(t->gs_pack.p);
    return t->rc;
}

void vkmsl_result_free(vkmsl_result *result) {
    if (!result) return;
    free(result->msl);
    result->msl = NULL;
    result->msl_size = 0;
}

// The descriptor bindings a module declares, with the Vulkan descriptor type each variable's type implies (a
// uniform buffer, storage buffer, sampled or storage image, texel buffer, sampler, combined image sampler or input
// attachment) and its array size (0 for an unsized array). Words are left 0.
int vkmsl_reflect(const uint32_t *words, size_t word_count, uint32_t stage, vkmsl_binding *out, uint32_t capacity,
                  uint32_t *count) {
    vkmsl_options opt = { .stage = stage };
    vkmsl_result res;
    memset(&res, 0, sizeof res);
    T tt;
    T *t = &tt;
    memset(t, 0, sizeof *t);
    t->w = words; t->count = word_count; t->opt = &opt; t->res = &res;
    *count = 0;
    if (parse(t)) {
        for (uint32_t id = 1; id < t->bound; id++) {
            ent *v = &t->e[id];
            if (v->kind != K_VAR || !(v->d.deco & D_BINDING)) continue;
            uint32_t sc = v->w[2], pointee = tw(t, v->type)[2], n = 1;
            int arrayed = 0;
            uint32_t elem = descriptor_element(t, pointee, &n, &arrayed);
            uint32_t type = ~0u;
            int eop = type_op(t, elem);
            if (sc == SC_StorageBuffer || (sc == SC_Uniform && (E(t, elem)->d.deco & D_BUFFERBLOCK))) type = VK_DT_STORAGE_BUFFER;
            else if (sc == SC_Uniform) type = VK_DT_UNIFORM_BUFFER;
            else if (eop == OpTypeSampler) type = VK_DT_SAMPLER;
            else if (eop == OpTypeSampledImage) type = tw(t, tw(t, elem)[1])[2] == DIM_Buffer ? VK_DT_UNIFORM_TEXEL : VK_DT_COMBINED;
            else if (eop == OpTypeImage) {
                const uint32_t *iw = tw(t, elem);
                if (iw[2] == DIM_SubpassData) type = VK_DT_INPUT_ATTACHMENT;
                else if (iw[2] == DIM_Buffer) type = iw[6] == 2 ? VK_DT_STORAGE_TEXEL : VK_DT_UNIFORM_TEXEL;
                else type = iw[6] == 2 ? VK_DT_STORAGE_IMAGE : VK_DT_SAMPLED_IMAGE;
            }
            if (type == ~0u) continue;
            if (*count < capacity)
                out[*count] = (vkmsl_binding){ v->d.set, v->d.binding, type, arrayed == 2 ? 0 : n, 0, VKMSL_NO_DYNAMIC };
            (*count)++;
        }
    }
    int rc = t->rc;
    for (uint32_t i = 0; t->e && i < t->bound; i++) { free(t->e[i].expr); free(t->e[i].aux); free(t->e[i].tname); }
    free(t->e); free(t->members); free(t->blocks); free(t->fparams); free(t->pre.p); free(t->glob.p);
    return rc;
}
