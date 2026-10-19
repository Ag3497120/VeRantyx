# Vulkan and SPIR-V work

VXRuntime will implement Vulkan's C API in its own `vulkan-1.dll` module and lower GPU work to Metal through the shared
GPU intermediate representation. Vulkan objects and rules remain in the Vulkan front end; resource execution and
backend-facing operations can be shared with D3D11 and D3D12 where their semantics match.

`VXRuntime/src/vulkan.c` registers an in-tree `vulkan-1.dll` frontend and reports Vulkan 1.0 with no advertised
extensions or layers. It creates a compute/transfer-only device on unified-memory Metal, exposes one queue, maps
host-visible coherent Vulkan memory to Metal shared buffers, and supports record-time inline buffer updates, repeated-pattern buffer fills, buffer copy, scoped memory/buffer/image barriers,
command pools and buffers, queue submission, binary semaphores and fences. Its transfer image slice supports linear-tiled
and optimal-tiled single-mip, single-layer, single-sample 2D color images for transfer source/destination usage in R8, RG8, RGBA8, BGRA8,
RGBA16F and R32F formats. `vkGetImageSubresourceLayout` reports the Metal-required padded row pitch for linear images;
optimal-tiled images keep their backing layout opaque, as required by Vulkan's tiling model. Image memory
requirements account for the padded backing row pitch used internally by Metal. Both image
tilings currently use a Metal texture backed by the bound shared-buffer allocation. Buffer-to-image and image-to-buffer regions lower to
common GPU IR texture copies. `vkCmdCopyImage` also blits between same-format linear images and rejects overlapping
source/destination memory ranges and repeated destination ranges when images alias one allocation. Image layout
barriers preserve layout, access, stage and subresource-range metadata and order the Metal blit/compute encoders. The
frontend records image transitions and image-layout uses alongside the common stream; queue-submit preflight replays
them in submit order, requires each old layout to match the current image layout (except `UNDEFINED`, which discards
prior contents), and checks copy/clear layout arguments before executing any command buffer in the batch. New image
layouts are committed only after successful synchronous execution. This follows the Vulkan
[`VkImageMemoryBarrier` layout rule](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageMemoryBarrier.html).
`vkCmdPipelineBarrier` with no memory, buffer or image barriers still records its stage-scoped execution dependency
in common GPU IR with empty access scopes, preserving the command's ordering in the Metal submission stream. This
matches the [Vulkan pipeline-barrier execution scopes](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPipelineBarrier.html).
`vkCreateImageView` and `vkDestroyImageView` now implement the Vulkan object and lifetime path for those images. They
accept 2D views with the image's exact format, color aspect, the sole mip/layer (or the corresponding `REMAINING`
range), and valid component swizzles; the view retains its image until it is destroyed. When Metal confirms
read/write texture support, the frontend also creates optimal-tiled RGBA8 storage images and accepts their identity
views in `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` writes with `GENERAL` layout. Descriptor-set layouts, pools, image writes,
retained image lifetimes, dispatch-time descriptor snapshots and Metal texture-slot binding are handled by the
Vulkan/common-backend path. Queue-submit preflight checks that each dispatched storage image is in the descriptor's
`GENERAL` layout at that point in the recorded layout sequence. The in-tree SPIR-V reader and MSL emitter now lower
`OpImageRead` and `OpImageWrite` for single-sample, non-arrayed 2D `Rgba8` storage images with float32 texels and no
optional image operands. The common Metal test dispatches a read-then-write kernel and checks the copied pixel.
The Vulkan integration test transitions two RGBA8 images to `GENERAL`, binds the reflected storage-image
descriptors, dispatches the same read-then-write shader, transitions the output to transfer-source layout, and checks
the copied bytes through a host-visible buffer. Optimal-tiled RGBA8 images with sampled usage can also be bound as
`VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` descriptors in `GENERAL` or `SHADER_READ_ONLY_OPTIMAL` layout. The SPIR-V reader
and MSL emitter lower single-sample, non-arrayed 2D `Sampled=1` float32 images with `OpImageFetch`, int2 texel
coordinates, and Lod 0; unsupported image operands are rejected. The Metal and Vulkan integration tests fetch one
texel, write it to a storage image, and verify the bytes after transfer readback. Sampler objects and filtered
sampling are not implemented.
`vkCmdClearColorImage` lowers a full clear of the currently supported single-mip/single-layer color image through the
common GPU IR clear operation; it checks transfer-destination usage, the accepted transfer-destination/general layout,
color aspect and supported subresource range. It shares the common Metal executor with D3D12 attachment clears. This
follows the public Vulkan [`vkCmdClearColorImage` contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdClearColorImage.html).
The exposed memory type is host-visible and host-coherent. `vkMapMemory` tracks the mapped subrange, and
`vkFlushMappedMemoryRanges` / `vkInvalidateMappedMemoryRanges` validate each range against that mapping and use host
memory-order fences; no cache operation is needed for this coherent Metal shared-memory type. The reported
`nonCoherentAtomSize` is 1, the Vulkan minimum. This follows the Vulkan
[`VkMappedMemoryRange` rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkMappedMemoryRange.html)
and the [host-coherent memory contract](https://docs.vulkan.org/spec/latest/chapters/memory.html).

Fence status and reset operations are synchronized across host threads; `vkWaitForFences` implements wait-all/wait-any,
zero-time polling, finite nanosecond timeouts, and blocking until a queue submission signals a fence.
Vulkan 1.0 transfer operations have no corresponding `VkFormatProperties`
feature bits, so `vkGetPhysicalDeviceImageFormatProperties` is the query for this supported linear transfer subset.
The compute path accepts read-only uniform-buffer descriptors with a single `uint` member at byte offset zero, plus storage-buffer and dynamic-storage-buffer descriptors, including fixed-size descriptor arrays, creates a compute pipeline from the selected SPIR-V entry point, lowers it through the
shared GPU IR and MSL emitter, and submits direct or indirect dispatches on Metal. Device creation rejects unimplemented extensions and
features except `robustBufferAccess`, which is enforced for scalar 32-bit `uint`, `int`, and `float` storage-buffer
shader paths:
generated MSL returns zero for out-of-range loads and drops out-of-range stores using each bound descriptor's byte range,
following the core behavior described in the [Khronos Vulkan robustness guide](https://docs.vulkan.org/guide/latest/robustness.html).
Compute-stage push constants use the common push-constant ranges and command stream. Pipeline layouts validate nonempty,
four-byte-aligned, nonoverlapping ranges within the reported 128-byte `maxPushConstantsSize`; shader ranges must be
covered by the layout. `vkCmdPushConstants` validates stage/range coverage and records a copy of the updated bytes, and
the Metal backend binds the command buffer's push-constant storage in a reserved runtime-data buffer slot. That buffer
also carries descriptor byte-range metadata when storage-buffer bounds checks are needed. SPIR-V
push-constant reads currently lower 32-bit integer/float scalar or vector members, nested struct offsets, and fixed
arrays or column-major matrices when access-chain indices are compile-time constants. Dynamic indices and row-major
matrix access remain unsupported.
Other storage/sampled-image shader shapes, formats, access operands and dimensions beyond the single-sample,
non-arrayed 2D `Rgba8` subset, filtered sampling, Vulkan samplers, depth/stencil image clears, per-subresource layout
tracking beyond the current single-mip/single-layer images,
graphics queues, uniform-buffer structs beyond that one-scalar shape, dynamic uniform buffers, timeline semaphores, swapchains and WSI are not
implemented; this frontend is not yet suitable for general Vulkan games or claimed as a conformant Vulkan
implementation. Descriptor-set updates advance a tracked generation; command buffers that bound
the old generation are rejected at end-recording or queue submission, matching Vulkan's invalidation rule for
ordinary descriptor bindings.
Recorded buffer and image commands and compute dispatches track the objects they may access, including buffers resolved
through descriptor sets, bound pipelines and pipeline layouts. Destroying a referenced buffer, image, pipeline or
pipeline layout invalidates each non-pending command buffer; retained internal objects remain alive until those command
buffers are reset or freed. The ordinary Vulkan tests cover direct buffer commands, descriptor-mediated dispatches,
image clears, pipeline destruction and pipeline-layout destruction, then verify queue submission rejects each invalid
command buffer. This follows the Vulkan
[command-buffer invalidation rules](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html).
Freeing a descriptor set or resetting/destroying its pool now releases the pool's ownership even when an executable
command buffer retains the set. The command buffer then fails end-recording or queue submission, and its retained set
state is released when the command buffer is reset or freed. Ordinary tests cover all three pool operations.

For uniform and storage-buffer descriptors, `VkDescriptorSetLayoutBinding::descriptorCount` reserves a bounded set of slots and
`vkUpdateDescriptorSets` writes the requested `dstArrayElement` range from its `VkDescriptorBufferInfo` array and
continues across consecutive compatible storage-buffer bindings when a write spans their boundary. Descriptor-copy
operations copy uniform or storage-buffer descriptors between compatible ranges; a call applies its writes before its
copies and preserves undefined source elements as undefined.
Dynamic storage-buffer bindings use Vulkan's binding-number/array-element offset order. The frontend requires the
exact offset count, checks 16-byte alignment and each written descriptor's effective range, and snapshots offsets
into the common descriptor state at dispatch. Descriptor pools account for uniform, static storage-buffer and dynamic
storage-buffer capacity separately, and the queried uniform/storage descriptor limits match the supported Metal slot count.
Pipeline layouts and descriptor pools count each descriptor element against the 30-slot Metal limit. The current
shader path lowers one fixed-size outer descriptor array when each `OpAccessChain` uses a compile-time constant element
index; dynamic indices, runtime or nested descriptor arrays, and arrays captured by helper functions are rejected.
This follows Vulkan's layout and write model while keeping shader translation explicit about its narrower Metal
binding path, as described in the [Khronos descriptor-set specification](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html)
and [descriptor-array guide](https://docs.vulkan.org/guide/latest/descriptor_arrays.html).

`vkCmdFillBuffer` records a 32-bit repeated pattern in the common GPU IR and executes in command order through a Metal blit upload. It validates transfer-destination usage, four-byte offset and size alignment, range, and `VK_WHOLE_SIZE` rounding before recording. This follows the Khronos [`vkCmdFillBuffer` reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdFillBuffer.html).

Fence tests cover zero-time polling, a finite timeout, a host wait unblocked by a separate queue-submission thread,
status/reset, and the existing submit/wait path. This follows the Vulkan
[vkWaitForFences](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForFences.html),
[vkGetFenceStatus](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetFenceStatus.html), and
[vkQueueSubmit](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html) semantics. Queue work is
still submitted synchronously by the current Metal backend; this does not imply a nonblocking or multi-queue path.

The physical-device query reports a 30-entry compute uniform-buffer limit, a 128-byte push-constant limit, conservative minimum image dimensions and array layers, a 128-invocation compute
workgroup limit, Vulkan's required 65535 dispatch-group count per axis, Metal's actual per-axis threadgroup sizes, and
the required 64-byte minimum mapping alignment. Most of the Vulkan 1.0 limit table remains incomplete and should not
be treated as a conformance claim.

The first SPIR-V component is `VXRuntime/src/spirv.c`. It copies words into host-endian storage, reads either byte
order, checks the five-word module header, and safely walks the variable-length instruction stream. It extracts entry
point names, execution models and interface IDs, records literal `LocalSize` or ID-based `LocalSizeId` metadata, and
retains scalar specialization-constant `SpecId` decorations for Vulkan pipeline creation.
It indexes scalar void, bool, integer and floating-point types, vectors, matrices, images, samplers, sampled images,
arrays, runtime arrays, structs, pointers and function signatures. It retains scalar, null, composite and module-level
undefined values, checks composite constituent count/type, and rejects specialization constants used by ordinary
composite constants. It applies direct
`OpDecorate` and `OpMemberDecorate` records plus `OpDecorationGroup`, `OpGroupDecorate` and `OpGroupMemberDecorate`
forms for tracked descriptor set/binding, array stride, block/member layout, row/column major matrices, built-ins,
locations, components, indices and read/write qualifiers. It exposes module variables in declaration order and checks
group definition/application order and target kinds. It also
checks the relationships needed by that metadata, including variable/pointer storage classes and referenced types.
It also indexes `OpFunction` declarations, verifies parameter types against function signatures, records each `OpLabel`
block's bounded word range, and rejects duplicate function, parameter, label and tracked instruction-result IDs. It
records result types for a common subset of core instructions. It checks that `OpBranch` and `OpBranchConditional`
targets and the `OpSwitch` default target resolve to labels owned by the same function. When the selector's type is
known, it also checks `OpSwitch` literal/target layout from the integer width and validates every case label. It checks
the SPIR-V 1.6 conditional-target rule and branch-weight sums. It rejects malformed function endings or blocks without
a terminator. Function and block iterators stop at their recorded boundaries while keeping instruction operands in the
original host-endian module.
The descriptor reflection helper classifies uniform/storage buffers, sampled/storage images, samplers, combined image
samplers, input attachments and texel buffers, and reports fixed descriptor-array counts or runtime-array presence.
`spirv_ir.c` maps those declarations into the common GPU layout with namespace zero, Vulkan set/binding numbers,
the selected entry point's stage, and an unbounded count for runtime descriptor arrays. It also maps a single
push-constant block made from explicitly laid-out scalar, vector, matrix, fixed-array, and nested-struct members to
an aligned common byte range. It conservatively assigns every module-level descriptor and push-constant declaration
to that selected stage. Runtime-sized or specialization-sized push-constant arrays, unsupported scalar types,
multiple push-constant variables, and malformed or incomplete layout metadata return unsupported. The Vulkan front end
validates its supported set-layout subset, storage-buffer range/alignment and selected compute entry point. Reflection
still conservatively includes module-level resource declarations, so entry points sharing a module with unrelated
resources may be rejected.

`spirv_build_gpu_ir_shader` now creates an owned common typed shader object for a selected entry point and its
statically reachable function calls. It checks the Vulkan entry-point signature and rejects recursive call graphs.
Literal `LocalSize` dimensions are copied as a fixed threadgroup size. The Vulkan pipeline frontend applies
`VkSpecializationInfo` map entries to scalar specialization constants, resolves `LocalSizeId` defaults and overrides,
then creates an immutable specialized common shader variant. The shared IR operation keeps the original shader
immutable and materializes defaults as fixed constants. The current MSL subset supports boolean and 32-bit scalar
specialization constants; specialization composites, derived specialization operations, and wider scalar constants
remain unsupported.
It maps core scalar/container types, scalar, null and composite constants, module-level undefined values, function
parameters/calls, variables (including descriptor and stage-interface decorations), loads/stores with the core
Volatile, Aligned and Nontemporal memory operands, access chains, runtime-array length, copies, common conversions, vector/composite
construction, static vector-lane extraction and insertion, dynamic vector extraction and insertion, shuffles, matrix transpose, component-wise arithmetic including 32-bit integer `OpBitCount`, `OpBitReverse`, `OpBitFieldInsert`, `OpBitFieldSExtract` and `OpBitFieldUExtract`, float vector-times-scalar scaling, vector dot products and vector float remainder/modulo, comparisons, select/phi,
runtime-array length, control/memory barriers, structured merge instructions, branches, switch literals, and returns. Barrier records
preserve the source operation and scope/semantics IDs. The MSL emitter lowers compute `OpControlBarrier` with
Workgroup or Subgroup execution scope to `threadgroup_barrier` or `simdgroup_barrier`; it lowers supported
`OpMemoryBarrier` scopes to `atomic_thread_fence` with Metal's matching device, threadgroup, or simdgroup scope.
Vulkan `UniformMemory`, `WorkgroupMemory`, and `ImageMemory` map to Metal device, threadgroup, and texture memory
flags. Unsupported storage classes and scopes are rejected. Non-relaxed SPIR-V fence orders use Metal's
`memory_order_seq_cst`, which is stronger than Acquire, Release, or AcquireRelease but preserves their ordering
constraints. The reader checks that barrier operands are scalar 32-bit integer constants; full Vulkan capability,
memory-model, and execution-environment validation remains incomplete. Load/store lowering retains volatility,
power-of-two alignment, and non-temporal hints. Pointer visibility/availability and extension memory operands remain
unsupported until an API/backend mapping can preserve their semantics. Composite paths and shuffle selectors are
validated against the declared types before lowering. Struct members retain explicit offsets, matrix strides/major
order, interface decorations, and read/write qualifiers. The common object retains the original SPIR-V opcode while
assigning API-neutral operation IDs; its constructor validates type/member references, function signatures, ID/operand
references, and block terminators. Instructions outside this subset return `GPU_IR_UNSUPPORTED` instead of being
skipped. The reader also indexes `OpExtInstImport` names and IDs. For the `GLSL.std.450` set, the current compute
lowering accepts 32-bit float scalar and 2–4 lane vector `FAbs`, `Floor`, `Ceil`, `Trunc`, `Fract`, `Round`,
`RoundEven`, `Sqrt`, `InverseSqrt`, `Sin`, `Cos`, `Tan`, `Asin`, `Acos`, `Atan`, `Sinh`, `Cosh`, `Tanh`, `Asinh`,
`Acosh`, `Atanh`, `Exp`, `Log`, `Exp2`, `Log2`, `Pow`, `FMin`, `FMax`, `FClamp`, and `Fma`, and emits the
corresponding Metal math calls. It also lowers 32-bit integer scalar and 2–4 lane vector `FindILsb`, `FindSMsb`, and
`FindUMsb`, including their all-bits-set result for zero (and for signed MSB inputs 0 and -1). The MSL backend computes
MSB indices as `31 - clz(value)`. Metal execution tests verify ordinary scalar/vector results and vector zero/negative
edge cases. Other
extended-instruction sets and instructions remain unsupported. See the Khronos
[`GLSL.std.450` specification](https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html).
Sampled-image operations, atomics, pointer-scope and extension load/store
operands, and many conversion/arithmetic forms are not lowered yet.
The Metal integration shader feeds its results into the storage-buffer output; readback checks that the existing ten
results truncate to 19, the added standard math results truncate to 9, scalar/vector bit-count checks contribute 36,
`OpBitReverse(7)` contributes `0xe0000000`, scalar/vector bit-field cases contribute 188, and vector bit scans contribute
5 after zero and signed `-1` edge cases; scalar bit scans contribute another 4. The bit-field cases cover
signed and unsigned extraction from a negative signed value, insertion into an unsigned word, and vector extraction.
The standalone `spirv_check.py` path also translates and Metal-compiles that fixture.

The Metal emitter handles 32-bit scalar and 2–4 lane vector `OpConvertFToU`, `OpConvertFToS`, `OpConvertSToF`,
`OpConvertUToF`, `OpSConvert`, `OpUConvert`, `OpFConvert`, and same-width `OpBitcast`. Float-to-integer conversions
use Metal's truncating cast, matching SPIR-V's round-toward-zero rule; wider conversions and other vector widths
remain unsupported by this emitter.

The Metal emitter supports `OpBitCount` for 32-bit integer scalar and 2–4 lane vector bases and results with matching
component counts. It converts signed input lanes to unsigned integers before Metal `popcount`, then casts the unsigned
count to the declared result type. `OpBitReverse` supports 32-bit integer scalar and 2–4 lane vector types when the
base and result types match. `OpBitFieldInsert`, `OpBitFieldSExtract` and `OpBitFieldUExtract` support matching
32-bit integer scalar or 2–4 lane vector base/result values with 32-bit scalar offset and count values. The lowering
uses Metal's `insert_bits` and `extract_bits` operations; unsigned extraction reinterprets signed inputs before
extracting so its upper bits stay zero. Out-of-range offsets or counts retain the source languages' undefined/poison
behavior. Other integer widths remain unsupported by these arithmetic lowerings.

`OpArrayLength` requires the SPIR-V-specified unsigned 32-bit or 64-bit result and a pointer to a structure whose
last member is a runtime array. The current Metal lowering supports the existing single-member storage-buffer shape
(32-bit scalar elements with stride 4) and a 32-bit result. It derives the element count from the descriptor's bound byte
range, rounding down any trailing partial element; this keeps robust accesses and the reported length within
`VkDescriptorBufferInfo::range`. The 64-bit SPIR-V form passes reader and common-IR validation but remains unsupported
by the current MSL emitter.

This remains a partial reader and lowering frontend, not a complete SPIR-V or Vulkan validator. SSA dominance and
definition-before-use rules, `OpDecorateId`, capabilities, extensions, and Vulkan execution-environment rules are not
fully validated.
Grouped decorations are expanded only for tracked literal-valued metadata; other decoration kinds and their semantics
remain unvalidated. Unknown instructions remain safely iterable, but their semantics are not validated or lowered.
Dynamic buffer descriptor variants and other Vulkan descriptor and feature rules are not inferred. `gpu_ir_msl.c`
emits a deliberately narrow compute subset from common IR: one void entry point, including multi-block control flow through an MSL state machine, `GlobalInvocationId`, `LocalInvocationId`, `WorkgroupId`, `NumWorkgroups`, `WorkgroupSize` and `LocalInvocationIndex` mapped to Metal kernel input attributes, function-scope scalar/vector variables; set-0 storage-buffer bindings whose resources contain runtime arrays of 32-bit `uint`, `int`, or `float` scalars with a 4-byte stride, including one fixed-size outer descriptor array with constant indexing; single-sample, non-arrayed 2D `Rgba8` storage-image `OpImageRead` and `OpImageWrite` with no optional image operands; sampled-image `OpImageFetch` for float32 2D images at Lod 0 with int2 coordinates; scalar and 2–4 lane vector arithmetic and logic, scalar integer and floating comparisons, scalar-condition and 2–4 lane vector-boolean-condition `OpSelect`, 2–4 lane vector construction, static extraction/insertion for vector lanes, and dynamic extraction/insertion with 32-bit integer indices, access/load/store/copy/return, workgroup and subgroup control barriers, and scoped memory fences. The state machine preserves branches, conditional branches, 32-bit integer `OpSwitch`, and scalar/vector loop-carried `OpPhi` values, while checking that phi inputs match each block’s predecessor set and snapshotting all incoming values before committing phi results, which preserves simultaneous SSA assignments for cross-dependent loop-carried values. Helper functions with scalar/vector parameters and scalar/vector or void results are supported; Function- and Workgroup-storage pointer parameters to supported scalar/vector/fixed-array values retain their MSL address spaces. Single-block bodies emit straight-line MSL, while multi-block bodies use the same state machine. Nested calls have type-checked arguments. Storage-buffer and supported RGBA8 storage/sampled-image bindings, supported compute built-ins, and supported Workgroup scalar/vector variables or fixed arrays are threaded into helper calls as hidden MSL parameters, preserving descriptor-range checks. StorageBuffer pointer parameters, pointer returns, pointer-valued phis, entry-point return values, unsupported shader types, and operations outside the listed lowering subset remain unsupported. Built-in variables are accepted only with the scalar or `uint3` type required by each built-in. Loads and stores check each full 4-byte element against the descriptor range before accessing Metal memory. When pipeline layout is supplied, finite compute-visible storage-buffer bindings from multiple descriptor sets, including fixed-size arrays, map densely to Metal slots 0–29; slot 30 carries the byte-range table. Supported storage and sampled-image bindings map densely to shared Metal texture slots 0–3. The reusable Metal backend accepts ordered common streams with record-time buffer updates and copies, direct or indirect compute dispatch, descriptor-state binds, and buffer-scoped barriers; resource copies currently support buffers only. A Metal integration path builds the common descriptor layout and immutable descriptor snapshot, records a bind and dispatch in the common GPU stream, then submits through `gpu_ir_metal.m`. The 64-element buffer is checked after uint3 multiplication and a lane-dependent comparison/select; a second region verifies that local and workgroup IDs reconstruct the global X index and that `LocalInvocationIndex` is exposed correctly. The same Metal integration shader executes workgroup/subgroup control barriers and device/workgroup memory fences, including combined uniform/image memory flags. Loop/phi and nested helper-call fixtures, including a multi-block helper with Function/Workgroup pointer arguments that reads a storage buffer through `GlobalInvocationId`, uses a phi value, and captures Workgroup scalar/fixed-array storage, check lowering output and compile generated MSL without running a GPU workload.
Function-scope variables also support fixed arrays of scalar or vector values up to 4096 elements, with 32-bit integer indices.
Module-scope `Workgroup` scalar/vector variables and fixed arrays lower to Metal `threadgroup` storage. The Vulkan
`maxComputeSharedMemorySize` query reports Metal's `maxThreadgroupMemoryLength`, and pipeline creation rejects compiled
static threadgroup storage that exceeds that limit.
Unsupported shader layouts, specialization forms or widths, unknown threadgroup sizes and memory flags are rejected.
The Vulkan frontend currently requires fixed or pipeline-specialized local sizes with no more than 128 invocations, one
buffer descriptor type per binding, the fixed one-`uint` uniform block shape above, fixed-size single-level descriptor
arrays with compile-time constant shader indices, and at most 30 buffer descriptor elements across four sets. This reflects the
current Metal binding path rather than Vulkan's full descriptor model.
`tools/spirv2msl.m` exposes this in-tree translation path for shader inspection and optional Metal compilation;
`tools/spirv_check.py` classifies a shader corpus as supported, unsupported, or invalid. These tools check only
VXRuntime's current lowering subset and do not claim full SPIR-V or Vulkan validation.

The format follows Khronos's public [SPIR-V unified specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html):
the module starts with five header words, and each instruction's first word packs its word count and opcode. The
reader is dependency-free and preserves unknown opcodes by their declared size so future semantic work can add
support without changing byte-stream handling.

Next stages:

1. Expand SPIR-V control-flow coverage, broader arithmetic and atomics, each
   checked against ordinary shader programs.
2. Add sampler objects, filtered sampling, more sampled/storage image formats, and broader image operations only after
   validating the Metal format and access behavior for each exposed case.
3. Improve Vulkan physical-device properties and validate the exposed limits against the Vulkan 1.0 requirements.
4. Add graphics pipelines and WSI after their formats, synchronization and presentation behavior have backend support.
