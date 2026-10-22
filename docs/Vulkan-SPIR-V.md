# Vulkan and SPIR-V work

VXRuntime will implement Vulkan's C API in its own `vulkan-1.dll` module and lower GPU work to Metal through the shared
GPU intermediate representation. Vulkan objects and rules remain in the Vulkan front end; resource execution and
backend-facing operations can be shared with D3D11 and D3D12 where their semantics match.

`VXRuntime/src/vulkan.c` registers an in-tree `vulkan-1.dll` frontend and reports Vulkan 1.0 with no layers. It
advertises the `VK_KHR_maintenance1` and `VK_KHR_swapchain` device extensions; enabling swapchain support requires
the instance to enable both Win32 surface prerequisites. It creates a graphics/compute/transfer device on unified-memory
Metal, exposes one queue, maps
host-visible coherent Vulkan memory to Metal shared buffers, and supports record-time inline buffer updates, repeated-pattern buffer fills, buffer copy, scoped memory/buffer/image barriers,
command pools and buffers, queue submission, binary semaphores and fences. Its transfer image slice supports linear-tiled
and optimal-tiled single-mip, single-sample 2D color images for transfer source/destination usage in R8, RG8, RGBA8, BGRA8,
RGBA16F, R32F, R32_UINT and R32_SINT formats. Transfer-only images may have up to 256 layers. The local Metal validation run rejected a buffer-backed `MTLTextureType2DArray`, so the frontend backs each Vulkan layer with a separate 2D Metal texture created by [`MTLBuffer.makeTexture(descriptor:offset:bytesPerRow:)`](https://developer.apple.com/documentation/metal/mtlbuffer/maketexture%28descriptor%3Aoffset%3Abytesperrow%3A), at that layer's offset in the same bound buffer. Sampled and storage images remain single-layer. `vkGetImageSubresourceLayout` reports the Metal-required padded row pitch for linear images;
optimal-tiled images keep their backing layout opaque, as required by Vulkan's tiling model. Image memory
requirements account for the padded backing row pitch used internally by Metal. Both image
tilings currently use Metal textures backed by the bound shared-buffer allocation. The sampled-image format set now includes R8, RG8, RGBA8, BGRA8, RGBA16F and R32F for optimal tiling; the runtime tests use `OpImageFetch` to verify missing-channel defaults, BGRA component order and RGBA16F half-to-float conversion through RGBA8 storage-image readback. These formats remain transfer-only with linear tiling. This follows Vulkan's format descriptions, missing-component substitution, and per-tiling feature-query model ([format specification](https://docs.vulkan.org/spec/latest/chapters/formats.html), [texel component substitution](https://docs.vulkan.org/spec/latest/chapters/images.html), [`VkFormatProperties`](https://docs.vulkan.org/refpages/latest/refpages/source/VkFormatProperties.html)). Buffer-to-image and image-to-buffer regions lower to common GPU IR texture copies; their `VkBufferImageCopy` row length and image height determine buffer row and image pitches. The ordinary roundtrip test uses `bufferRowLength = 6` and `bufferImageHeight = 4` for a 4×3 texel region and verifies padded row addressing in both directions, following the [Vulkan `VkBufferImageCopy` contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkBufferImageCopy.html). `vkCmdCopyImage` uses each [Vulkan subresource's base layer and layer count](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageSubresourceLayers.html) to copy same-format regions across selected array layers and rejects overlapping
source/destination memory ranges and repeated destination ranges when images alias one allocation. Image layout
barriers preserve layout, access, stage and supported subresource-range metadata and order the Metal blit/compute encoders. The frontend tracks layout independently for each array layer and accepts supported partial-layer barriers. It records image transitions and image-layout uses alongside the common stream; queue-submit preflight replays
them in submit order, requires each old layout to match the current image layout (except `UNDEFINED`, which discards
prior contents), and checks copy/clear layout arguments before executing any command buffer in the batch. New image
layouts are committed only after successful synchronous execution. This follows the Vulkan
[`VkImageMemoryBarrier` layout rule](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageMemoryBarrier.html).
Image views can select one array layer for the current transfer-image view path. Transfer commands and barriers can address any supported layer; sampled/storage views and shaders still support only single-layer images.
`vkCmdPipelineBarrier` with no memory, buffer or image barriers still records its stage-scoped execution dependency
in common GPU IR with empty access scopes, preserving the command's ordering in the Metal submission stream. This
matches the [Vulkan pipeline-barrier execution scopes](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPipelineBarrier.html).
The barrier and binary-semaphore wait-stage masks also accept the advertised graphics queue's vertex-input,
vertex-shader, fragment-shader, early/late-fragment-test, color-attachment-output, and `ALL_GRAPHICS` stages. They map
to the corresponding API-neutral GPU IR synchronization scopes; optional geometry and tessellation stage masks remain
rejected because the device does not enable those features. This follows Vulkan's [pipeline stage definitions](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineStageFlagBits.html)
and the [queue-submit stage-mask rule](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html).
Core Vulkan access masks for index and vertex inputs, shader and input-attachment accesses, color/depth attachments,
transfers, host memory, and generic memory now map to the corresponding GPU IR access classes. Barrier recording checks
that each non-generic access is paired with a stage that can perform it, including the special host-stage rule; the
ordinary tests cover valid graphics accesses and reject transfer-at-fragment and host-at-`ALL_COMMANDS` pairs. This
follows the Vulkan [access-type table](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-pipeline-stages-access-types)
and [`VkAccessFlagBits`](https://docs.vulkan.org/refpages/latest/refpages/source/VkAccessFlagBits.html).
`vkCreateImageView` and `vkDestroyImageView` now implement the Vulkan object and lifetime path for those images. They
accept 2D views with the image's exact format, color aspect, one selected array layer (or `REMAINING` when that selects one layer), and valid component swizzles; the view retains its image until it is destroyed. Sampled-image descriptor writes snapshot the view's four component selectors into common GPU IR, and the Metal backend applies them through a texture view for texel reads and samples. Storage-image descriptors use identity mapping as required by the Vulkan specification. This follows the [Vulkan component-mapping rule](https://docs.vulkan.org/refpages/latest/refpages/source/VkComponentMapping.html) using Metal's [swizzled texture-view API](https://developer.apple.com/documentation/metal/mtltexture/newtextureviewwithpixelformat:texturetype:levels:slices:swizzle:). An ordinary Vulkan GPU test verifies sampled `OpImageFetch` with R/G/B/A remapping and zero/one substitution. When Metal confirms
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
texel, write it to a storage image, and verify the bytes after transfer readback. `OpImageQuerySize` and
`OpImageQuerySizeLod` also lower through common GPU IR for single-sample 2D float32 sampled images and RGBA8 storage
images. For sampled 2D array images, the reader validates the three-component result and the MSL emitter appends
`get_array_size()` after width and height. The arrayed fixture is translator and Metal-compile coverage only: Vulkan
sampled-image creation and views still support one layer, so no Vulkan runtime array query is claimed. The reader preserves
the 32-bit integer LOD operand, and the MSL emitter passes it to the bound Metal texture's width and height queries.
The shader fixtures cover a dynamic LOD from `GlobalInvocationId.x`, LOD 1, and the arrayed result shape. The Vulkan
integration case dispatches one work item, so its dynamic value is 0; the current Vulkan image-view path exposes one
mip and one layer. The reader also accepts `OpImageQuerySamples` for 2D float32 `Sampled=1` multisampled images, including arrayed images; synthetic SPIR-V and Metal GPU readback verify both 2D and 2D-array texture queries. The Vulkan frontend still cannot create or bind multisampled images, so there is no Vulkan execution coverage for this operation. Other dimensions and storage-image arrays remain unsupported. Vulkan
integration tests run both query opcodes on a 2×2 sampled
image and use the returned extent to fetch and write its bottom-right texel, then verify the pixel by transfer
readback. `OpImageQueryLevels` also lowers for supported 2D sampled and storage images. The current Vulkan image-view
path exposes one mip level, so MSL returns the Metal texture's mip count, which is 1; Vulkan integration tests query
both sampled and storage images, use each result to select the bottom-right texel, and check it after transfer
readback. This matches Vulkan's view-level
`levelCount` rule for the current one-level views. The mapping follows the Khronos
[`OpImageQueryLevels`](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpImageQueryLevels),
[`OpImageQuerySize`](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpImageQuerySize)
and [`OpImageQuerySizeLod`](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpImageQuerySizeLod)
rules, with the single-mip image constraint matching the [Vulkan image-query specification](https://docs.vulkan.org/spec/latest/chapters/images.html).
The one-mip limit is also enforced by the Metal storage path: Vulkan memory is currently a shared `MTLBuffer`, and
Apple's [`newTextureWithDescriptor:offset:bytesPerRow:` API](https://developer.apple.com/documentation/metal/mtlbuffer/maketexture%28descriptor%3Aoffset%3Abytesperrow%3A%29)
requires buffer-backed textures to have `mipmapLevelCount` 1. The Vulkan tests assert the advertised image-format
limit and reject a two-mip image. Supporting real mip chains needs a different allocation path that keeps
`VkDeviceMemory` mapping and aliasing semantics while backing a Metal texture with multiple levels, plus per-mip
subresource layout and transition tracking; the current LOD 1 fixture is therefore translator/Metal-compile coverage,
not Vulkan execution coverage.
Sampler objects and filtered
sampling are supported for a narrow compute subset: `vkCreateSampler` maps supported filter, mip, address, border and
LOD settings to retained `MTLSamplerState` objects. `samplerAnisotropy` is reported only when a Metal sampler probe
accepts a value above one, must be enabled at device creation, and accepts clamps from 1 through the queried device
limit (up to 16); the Vulkan float clamp is rounded to Metal's integer sample count. Combined image-sampler descriptors
and separate sampled-image plus
sampler descriptors lower through common IR. The MSL emitter supports `OpImageSampleExplicitLod` for single-sample,
non-arrayed 2D float32 images, float2 coordinates and float LOD, with no optional operands beyond the explicit LOD.
It also supports fragment-stage `OpImageSampleImplicitLod` with no optional Image Operands for that same image and
coordinate subset. The implicit path computes `dfdx`/`dfdy` before descriptor-index bounds guards and passes them to
Metal as `gradient2d`. Ordinary Metal tests compile both combined and separate descriptor forms; the Vulkan
integration test records and submits both forms and checks sampled pixels through readback. This does not cover
implicit LOD in other stages, dimensions, coordinate types, or Image Operands such as Bias, MinLod, or offsets.
Sampler comparison and unsupported border/address modes remain unsupported. The anisotropy
capability and range follow the [Vulkan sampler rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkSamplerCreateInfo.html)
and Apple's [`MTLSamplerDescriptor.maxAnisotropy`](https://developer.apple.com/documentation/metal/mtlsamplerdescriptor/maxanisotropy);
fractional Vulkan clamps cannot be represented exactly by Metal's integer property.
Fixed-size combined image-sampler arrays and separate sampled-image/sampler arrays support dynamically uniform 32-bit
indices when `shaderSampledImageArrayDynamicIndexing` is reported and enabled. MSL selects among the bound textures and
samplers and guards each selected access against the corresponding array length. Runtime tests index a combined array
with `WorkgroupId.x`, and separately index sampled-image and sampler arrays with `WorkgroupId.x` and
`1 - WorkgroupId.x`; both dispatch two workgroups and verify distinct sampled pixels through transfer readback.
Dynamic indices whose uniformity the frontend cannot prove are rejected.
Fixed-size storage-image arrays support dynamically uniform 32-bit indices when
`shaderStorageImageArrayDynamicIndexing` is reported and enabled. MSL selects among the bound Metal textures and
guards the access against the array length. An ordinary Metal/Vulkan integration test indexes the array with
`WorkgroupId.x`, dispatches one invocation per workgroup, and reads both written images back.
`vkCmdClearColorImage` lowers full-image and array-layer-range clears through the common typed GPU IR clear operation; it checks transfer-destination usage, the accepted transfer-destination/general layout,
color aspect, mip range and array-layer bounds. R32_UINT/R32_SINT use the matching clear-union member, and Metal writes the exact typed value to each selected layer texture. The Vulkan integration test uploads and reads back two layers, copies one layer to another, then clears one layer and verifies mapped-memory readback. The operation shares the common Metal executor with D3D12 attachment clears and follows the public Vulkan [`vkCmdClearColorImage` contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdClearColorImage.html).
The exposed memory type is host-visible and host-coherent. `vkMapMemory` tracks the mapped subrange, and
`vkFlushMappedMemoryRanges` / `vkInvalidateMappedMemoryRanges` validate each range against that mapping and use host
memory-order fences; no cache operation is needed for this coherent Metal shared-memory type. The reported
`nonCoherentAtomSize` is 1, the Vulkan minimum. This follows the Vulkan
[`VkMappedMemoryRange` rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkMappedMemoryRange.html)
and the [host-coherent memory contract](https://docs.vulkan.org/spec/latest/chapters/memory.html).

Fence status and reset operations are synchronized across host threads; `vkWaitForFences` implements wait-all/wait-any,
zero-time polling, finite nanosecond timeouts, and blocking until a queue submission signals a fence.
Queue submissions execute synchronously through Metal on the calling thread, and the device synchronization lock
linearizes binary semaphore updates with host acquire/present and fence waits. Pending queue work and waits for future
semaphore signals are not modeled yet.
`VK_KHR_maintenance1` revision 2 exposes transfer-source and transfer-destination format feature bits for the
supported image formats and tilings. Transfer-only image creation requires enabling that device extension; without
it, Vulkan 1.0's implied transfer support applies only where the tiling's format feature mask contains a non-transfer
feature (currently optimal RGBA8). Descriptor-pool capacity failures use `VK_ERROR_OUT_OF_POOL_MEMORY_KHR` on
extension-enabled devices, and `vkTrimCommandPoolKHR` releases retained empty-stream and pool-index capacity. This
The graphics subset exposes the core single-viewport `vkCmdSetViewport` and `vkCmdSetScissor` commands and
float32 scalar/vector vertex input plus scalar 32-bit signed/unsigned integer attributes through binding descriptions,
attributes, and `vkCmdBindVertexBuffers`; 3D images
and the broader Vulkan functionality described below remain unavailable.
Descriptor-set layouts accept visibility in the supported compute, vertex, and fragment stages, and pipeline layouts
preserve that visibility in the shared GPU IR. Graphics pipelines consume fragment-stage combined image-sampler,
sampled-image, and sampler descriptors, including fixed-size arrays. The frontend validates them against the Vulkan
set layout, captures their values at each draw, checks the sampled image layout for image-bearing descriptors, and
binds stage-local Metal texture and sampler slots. It does not yet support graphics storage images, graphics buffer
descriptors, or push constants. The ordinary graphics descriptor tests compile each pipeline and record a draw; they
do not submit those draws or verify sampled pixels.
The compute path accepts read-only uniform-buffer descriptors whose block members are direct 32-bit integer or float scalars with explicit, aligned, nonoverlapping offsets, plus storage-buffer and dynamic-storage-buffer descriptors, including fixed-size descriptor arrays. Both ordinary and dynamic uniform-buffer descriptors are tested through descriptor snapshots and compute dispatch. It creates a compute pipeline from the selected SPIR-V entry point, lowers it through the
shared GPU IR and MSL emitter, and submits direct or indirect dispatches on Metal. Device creation accepts
`VK_KHR_maintenance1`, plus `VK_KHR_swapchain` when the instance enabled both Win32 surface prerequisites; other
unimplemented extensions are rejected. Device creation also accepts
features except `robustBufferAccess`, `fullDrawIndexUint32`, `shaderSampledImageArrayDynamicIndexing`,
`shaderStorageBufferArrayDynamicIndexing`, and, when storage textures are supported,
`shaderStorageImageArrayDynamicIndexing`. The sampled-image feature covers fixed-size sampler, sampled-image and
combined-image-sampler arrays; only indices proven uniform from supported workgroup values and uniform arithmetic are
accepted. Robust buffer access is enforced for scalar 32-bit `uint`, `int`, and `float` storage- and uniform-buffer
shader paths:
generated MSL returns zero for out-of-range loads and drops out-of-range stores using each bound descriptor's byte range,
following the core behavior described in the [Khronos Vulkan robustness guide](https://docs.vulkan.org/guide/latest/robustness.html).
Compute-stage push constants use the common push-constant ranges and command stream. Pipeline layouts validate nonempty,
four-byte-aligned, nonoverlapping ranges within the reported 128-byte `maxPushConstantsSize`; shader ranges must be
covered by the layout. `vkCmdPushConstants` validates stage/range coverage and records a copy of the updated bytes, and
the Metal backend binds the command buffer's push-constant storage in a reserved runtime-data buffer slot. That buffer
also carries descriptor byte-range metadata when buffer bounds checks are needed. SPIR-V
push-constant reads currently lower 32-bit integer/float scalar or vector members, nested struct offsets, and fixed
arrays or column-major matrices when access-chain indices are compile-time constants. Dynamic indices and row-major
matrix access remain unsupported.
Other storage/sampled-image shader shapes, formats, access operands and dimensions beyond the single-sample,
non-arrayed 2D `Rgba8` storage and float32 sampled-image shader subset (Vulkan sampled formats include R8, RG8, RGBA8,
BGRA8, RGBA16F and R32F), sampled/storage Vulkan array images, unsupported sampler features and sampling forms,
depth/stencil image clears, graphics pipelines beyond the documented one-attachment triangle slice, vertex formats and shader inputs outside the float32 scalar/vector and scalar 32-bit signed/unsigned integer subset,
non-float or non-default-interpolation varyings, uniform-buffer block vectors, array members, nested structs or non-32-bit scalar members,
timeline semaphores and broader WSI features remain unsupported; this frontend is not yet suitable for general Vulkan
games or claimed as a conformant Vulkan implementation. The Win32 WSI path creates surfaces for live VXRuntime windows,
reports the supported color format, FIFO mode, and surface capabilities, and provides the limited swapchain, acquire,
and Metal-layer presentation path described below. Descriptor-set updates advance a tracked generation; command buffers that bound
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
shader path lowers one fixed-size outer storage-buffer descriptor array with compile-time constant or dynamically
uniform 32-bit `OpAccessChain` indices. The Vulkan device reports and requires the core
`shaderStorageBufferArrayDynamicIndexing` feature for dynamic indexing. Fixed-size storage-image descriptor arrays also
accept dynamically uniform 32-bit indices when `shaderStorageImageArrayDynamicIndexing` is enabled. Fixed-size
sampled-image, sampler and combined-image-sampler arrays accept dynamically uniform 32-bit indices when
`shaderSampledImageArrayDynamicIndexing` is enabled. Runtime or nested descriptor arrays, dynamic indices whose
uniformity cannot be proven, and descriptor arrays captured by helper functions are rejected. This follows Vulkan's
feature contract and descriptor layout/write model while keeping shader translation explicit about its narrower Metal
binding path, as described in the [Khronos descriptor-set specification](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html),
[descriptor-array guide](https://docs.vulkan.org/guide/latest/descriptor_arrays.html), and
[`VkPhysicalDeviceFeatures`](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html).
The Metal integration test dispatches three workgroups against a two-element array: it verifies both selected writes,
checks that the out-of-range third index has no effect, and repeats the case with dynamic-buffer offsets.

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

Graphics interface limits follow the Metal GPU family exposed by the selected device: `maxFragmentInputComponents`
is 60 on Apple2/Apple3 and 124 on Apple4-or-later/ Mac2 devices, and `maxVertexOutputComponents` adds the four
`Position` components. The graphics pipeline validator checks interface locations, component totals, exact float32
scalar/vector type matches and the supported interpolation subset against those reported values. Sparse `Location`
numbers are checked against the translator's supported location range, separately from the aggregate component limit.
Apple's public [Metal feature-set table](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf) documents
the fragment stage-in limits; Vulkan's location assignment and inter-stage matching rules are in its
[shader-interface chapter](https://docs.vulkan.org/spec/latest/chapters/interfaces.html).

The first SPIR-V component is `VXRuntime/src/spirv.c`. It copies words into host-endian storage, reads either byte
order, checks the five-word module header, and safely walks the variable-length instruction stream. It extracts entry
point names, execution models and interface IDs, records literal `LocalSize` or ID-based `LocalSizeId` metadata, and
retains scalar specialization-constant `SpecId` decorations for Vulkan pipeline creation. It records the single
`OpMemoryModel` declaration; Vulkan shader-module creation currently accepts only `Logical` + `GLSL450`, because this
Vulkan 1.0 frontend does not expose physical storage-buffer addressing or the Vulkan memory-model feature. The
addressing-model requirement follows the [Vulkan SPIR-V environment](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html).
It indexes scalar void, bool, integer and floating-point types, vectors, matrices, images, samplers, sampled images,
arrays, runtime arrays, structs, pointers and function signatures. It retains scalar, null, composite and module-level
undefined values, checks composite constituent count/type, and rejects specialization constants used by ordinary
composite constants. It applies direct
`OpDecorate` and `OpMemberDecorate` records plus `OpDecorationGroup`, `OpGroupDecorate` and `OpGroupMemberDecorate`
forms for tracked descriptor set/binding, array stride, block/member layout, row/column major matrices, built-ins,
locations, components, indices, interpolation/invariance qualifiers and read/write qualifiers. It exposes module
variables in declaration order and checks group definition/application order and target kinds. It also
checks the relationships needed by that metadata, including variable/pointer storage classes and referenced types.
It also indexes `OpFunction` declarations, verifies parameter types against function signatures, records each `OpLabel`
block's bounded word range, and rejects duplicate function, parameter, label and tracked instruction-result IDs. It
records result types for a common subset of core instructions. It checks that `OpBranch` and `OpBranchConditional`
targets and the `OpSwitch` default target resolve to labels owned by the same function. When the selector's type is
known, it also checks `OpSwitch` literal/target layout from the integer width and validates every case label. It checks
the SPIR-V 1.6 conditional-target rule and branch-weight sums. It rejects malformed function endings or blocks without
a terminator. Function and block iterators stop at their recorded boundaries while keeping instruction operands in the
original host-endian module.
The common shader reader also maps SPIR-V `Position`, `VertexIndex`, `InstanceIndex`, `FragCoord`, `FrontFacing` and
`FragDepth` built-ins to API-neutral semantics while preserving each global variable's normalized input/output storage
class and location/component/index decorations. Reader fixtures exercise Vertex and Fragment entry-point modules;
the builder also retains whether each I/O variable belongs to the selected `OpEntryPoint` interface, so modules with
multiple entries do not leak unrelated inputs and outputs into one another. A first MSL graphics emitter lowers a
vertex shader using `VertexIndex`/`InstanceIndex` to a `Position` output and a fragment shader using `FragCoord`/
`FrontFacing` to one float4 `Location 0` color. The Vulkan frontend now lowers matching smooth float32 scalar/vector
user varyings through common IR, validates fragment inputs against vertex outputs, maps float32 scalar and vector
attributes plus scalar 32-bit signed/unsigned integer attributes to Metal stage input, and retains bound vertex buffers
for direct, indexed, and indirect draws. Its graphics
test supplies a clip-space position stream and verifies rendered pixels through host readback. Vulkan `InstanceIndex`
retains the `firstInstance` offset and maps directly to Metal `instance_id`; this remains distinct from D3D12
`SV_InstanceID`, which is draw-relative and needs a `base_instance` adjustment in MSL. The common-IR MSL test checks
the mapping, while a Metal readback fixture uses one instance with `firstInstance = 1` to verify that the offset reaches
the shader. See the [Vulkan `InstanceIndex` definition](https://registry.khronos.org/vulkan/specs/latest/man/html/InstanceIndex.html)
and [Metal's `baseInstance` draw argument](https://developer.apple.com/documentation/metal/mtlrendercommandencoder/drawprimitives%28type%3Avertexstart%3Avertexcount%3Ainstancecount%3Abaseinstance%3A).
It also records indexed
draws with bound uint16/uint32 index buffers, `firstIndex`, `vertexOffset` and `firstInstance`, retaining the underlying
memory resource through command execution. Vertex input currently supports `R32_SFLOAT`, `R32G32_SFLOAT`,
`R32G32B32_SFLOAT`, `R32G32B32A32_SFLOAT`, `R32_SINT`, and `R32_UINT`, up to 16 bindings and attributes,
2047-byte attribute offsets and 2048-byte strides; per-instance input uses Vulkan's core step rate of one. These
queried limits and format features
follow the Vulkan [vertex input description](https://docs.vulkan.org/spec/latest/chapters/fxvertex.html),
[device limits](https://docs.vulkan.org/spec/latest/chapters/limits.html), and
[`VkFormatProperties`](https://docs.vulkan.org/refpages/latest/refpages/source/VkFormatProperties.html). Float32
vertex formats may have a different component count from the matching float32 shader input; Metal's vertex fetch
expands missing components with the specified defaults or discards unused components, matching Vulkan's component
assignment rules ([Metal vertex attribute conversion](https://developer.apple.com/documentation/metal/mtlvertexattributedescriptor/format)). It currently
accepts only triangle-list/strip topology, one RGBA8/BGRA8
color attachment, one static or dynamic viewport and scissor, fill/no-cull rasterization, single-sample rendering,
disabled depth, and fixed-function color blending for that one attachment. It maps Vulkan's standard blend factors,
ADD/SUBTRACT/REVERSE_SUBTRACT/MIN/MAX operations, channel write mask, and static blend constants to Metal. Dynamic
blend constants are supported through `VK_DYNAMIC_STATE_BLEND_CONSTANTS` and `vkCmdSetBlendConstants`; the command-buffer
values are captured by direct and indirect draws, and a draw is rejected if an enabled blend equation uses a constant
factor before the value is set. Logic operations, dual-source blend factors, and multiple color attachments remain
unsupported. Blend constants are retained without clamping to `[0, 1]`.
The graphics readback fixture uses constant-color/constant-alpha factors and all-one constants, preserving its
expected pixels while exercising Metal's blend-color state.
The dynamic-state subset supports `VK_DYNAMIC_STATE_VIEWPORT`, `VK_DYNAMIC_STATE_SCISSOR`, and
`VK_DYNAMIC_STATE_BLEND_CONSTANTS`; viewport/scissor commands accept the single supported value at index zero. Other
fixed-function states, non-float or packed vertex formats, and unsupported user `Location` interfaces return unsupported. The ordinary
Metal graphics test separately checks the common-IR backend path. Stage linkage and builtin mappings follow the
public [SPIR-V unified specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html) and
[Vulkan shader-interface specification](https://docs.vulkan.org/spec/latest/chapters/interfaces.html); pipeline
creation and draw parameters follow the public
[Vulkan graphics-pipeline specification](https://docs.vulkan.org/spec/latest/chapters/pipelines.html) and
[`vkCmdDraw` contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDraw.html),
[`vkCmdBindIndexBuffer`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBindIndexBuffer.html), and
[`vkCmdDrawIndexed`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndexed.html).
The common MSL emitter exposes layout-aware vertex/fragment variants; the fragment variant lowers combined
image-sampler descriptors and separate sampled-image plus sampler descriptors for the supported
[`OpImageSampleExplicitLod`](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpImageSampleExplicitLod) and
[`OpImageSampleImplicitLod`](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpImageSampleImplicitLod)
forms into Metal texture/sampler arguments. CPU-only fixtures check SPIR-V reflection, slots, and derivative MSL.
Vulkan graphics pipelines validate those fragment descriptor types against the set layout, and the Metal render
encoder binds their draw-time descriptor snapshots. The ordinary integration test uploads a known RGBA8 texel,
records both combined-image-sampler and separate-image/sampler draws, submits each command buffer, copies the
rendered attachment back, and checks the sampled pixels against the uploaded value while confirming pixels outside
the render area remain unchanged. It exercises the sampled-image layout transitions, descriptor binding, draw,
submission, and readback path together.
Fragment shaders may end a block with SPIR-V `OpKill`; the reader lowers it to the common fragment-termination
terminator, and the single-block graphics MSL path emits `discard_fragment()`. The CPU integration fixture verifies
SPIR-V 1.0 parsing, common-IR block termination, and generated MSL, which is compiled by `make check`. Multi-block
fragment bodies and `OpTerminateInvocation` feature/version validation remain unsupported. `OpKill` follows the
Khronos [SPIR-V instruction contract](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpKill).
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
Literal `LocalSize` dimensions are copied as a fixed threadgroup size. Compute, vertex, and fragment pipeline stages
apply their own `VkSpecializationInfo` map entries to scalar specialization constants; compute additionally resolves
`LocalSizeId` defaults and overrides. Each stage then gets an immutable specialized common shader variant. The shared
IR operation keeps the original shader immutable and materializes defaults as fixed constants. The current MSL subset
supports boolean and 32-bit scalar specialization constants, plus 32-bit integer `OpSpecConstantOp` add, subtract,
multiply, unsigned and signed division, unsigned modulo, logical/arithmetic shifts, bitwise-or, bitwise-xor, and
bitwise-and expressions. Their defaults are resolved by the reader, and Vulkan recomputes derived values from the
pipeline's direct `SpecId` overrides. Division by zero and signed division overflow are rejected because SPIR-V
defines those results as undefined; shift amounts at or above the operand width are also rejected. Specialization
composites, other derived operations, and wider scalar constants remain unsupported. These cases follow the public
[SPIR-V specialization-operation rules](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpSpecConstantOp)
and [integer division definitions](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpSDiv).
It maps core scalar/container types, scalar, null and composite constants, module-level undefined values, function
parameters/calls, variables (including descriptor and stage-interface decorations), loads/stores with the core
Volatile, Aligned and Nontemporal memory operands, access chains, runtime-array length, copies, common conversions, vector/composite
construction, static vector-lane extraction and insertion, dynamic vector extraction and insertion, shuffles, matrix transpose, component-wise arithmetic including 32-bit integer `OpBitCount`, `OpBitReverse`, `OpBitFieldInsert`, `OpBitFieldSExtract` and `OpBitFieldUExtract`, float vector-times-scalar scaling, vector dot products, vector float remainder/modulo, subgroup-scope `OpGroupNonUniformAny`/`OpGroupNonUniformAll`, and `GLSL.std.450` `FMix`, comparisons, select/phi,
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
skipped. The common shader MSL emitter lowers subgroup any/all to `simd_any`/`simd_all`, integer min/max
reductions to `simd_min`/`simd_max`, product to `simd_product`, and bitwise AND/OR/XOR to `simd_and`/`simd_or`/`simd_xor`.
This reader/compiler subset does not advertise Vulkan subgroup features or route Vulkan runtime pipelines through
this common shader path yet. It accepts 32-bit integer `OpGroupNonUniformIMul`, `SMin`/`UMin`/`SMax`/`UMax`,
and `BitwiseAnd`/`BitwiseOr`/`BitwiseXor` only with `GroupNonUniform` and `GroupNonUniformArithmetic`, `Subgroup`
scope and the `Reduce` group operation. Tests cover each operation and reject a non-Subgroup scope or a missing
arithmetic capability.
The reader also indexes `OpExtInstImport` names and IDs. For the `GLSL.std.450` set, the current compute
lowering accepts 32-bit float scalar and 2–4 lane vector `FAbs`, `Floor`, `Ceil`, `Trunc`, `Fract`, `Round`,
`RoundEven`, `Sqrt`, `InverseSqrt`, `Sin`, `Cos`, `Tan`, `Asin`, `Acos`, `Atan`, `Sinh`, `Cosh`, `Tanh`, `Asinh`,
`Acosh`, `Atanh`, `Exp`, `Log`, `Exp2`, `Log2`, `Pow`, `FMin`, `FMax`, `FClamp`, `FMix`, `Step`, `SmoothStep`, and `Fma`, and emits the
corresponding Metal math calls with the GLSL operand order and component-wise behavior in Khronos's
[GLSL extended-instruction specification](https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html). It also lowers 32-bit integer scalar and 2–4 lane vector `FindILsb`, `FindSMsb`, and
`FindUMsb`, including their all-bits-set result for zero (and for signed MSB inputs 0 and -1). The MSL backend computes
MSB indices as `31 - clz(value)`. Metal execution tests verify ordinary scalar/vector results and vector zero/negative
edge cases. Other
extended-instruction sets and instructions remain unsupported. See the Khronos
[`GLSL.std.450` specification](https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html).
Sampled-image operations, pointer-scope and extension load/store
operands, and many conversion/arithmetic forms are not lowered yet.

The atomic subset lowers `OpAtomicLoad`, `OpAtomicStore`, `OpAtomicExchange`, `OpAtomicIAdd`, `OpAtomicISub`,
`OpAtomicIIncrement`/`IDecrement`, `OpAtomicSMin`/`UMin`, `OpAtomicSMax`/`UMax`, `OpAtomicAnd`/`Or`/`Xor`, and
`OpAtomicCompareExchange` to common GPU IR for 32-bit scalar elements in a StorageBuffer, with Device scope. It
accepts Relaxed, Acquire, Release, and AcquireRelease memory orders with the required `UniformMemory` semantics bit;
SequentiallyConsistent and additional semantics bits remain unsupported. Compare-exchange failure accepts Relaxed or
Acquire, and validation rejects failure orders stronger than success. Store and read-modify-write operations require
writable storage; atomic load is allowed on read-only storage. Atomic load returns its loaded value and atomic store
has no result, matching SPIR-V's distinct instruction forms. MSL's fetch-and-modify functions in this path use
Relaxed atomics, so the backend emits a device-scope sequentially consistent fence before release operations and
after acquire operations. This preserves the requested ordering with a stronger fence; Relaxed operations emit no
extra fence. Compare-exchange retains its separate Equal and Unequal semantics in common IR and retries MSL's weak
primitive on spurious failure, preserving the strong SPIR-V behavior. Each operation checks the full four-byte
element against the descriptor range before accessing it; an out-of-range pointer leaves the result at zero.
Vulkan's ordinary 32-bit integer atomics need no optional feature. Runtime tests execute 64 contending
AcquireRelease/Acquire compare-exchanges and 63 invocations for each added read-modify-write opcode with
AcquireRelease ordering, plus an Acquire load and Release store; readback checks old-value propagation and the
exchange, subtraction, signed and unsigned min/max, bitwise, and increment/decrement results. Atomic flag operations,
non-32-bit types, other storage classes and scopes, SequentiallyConsistent orders, and additional memory-semantics
bits remain rejected. This is a deliberately narrow subset of the public
[SPIR-V atomic instruction contracts](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpAtomicIAdd),
[Vulkan atomic support](https://docs.vulkan.org/guide/latest/atomics.html), and Apple's
[Metal Shading Language atomic and fence functions](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf);
full Vulkan memory-model and execution-environment validation remains incomplete.
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
The SPIR-V reader preserves `SubgroupSize` and `SubgroupLocalInvocationId` in the shared shader IR, where they
share semantics with DXIL wave-lane queries. This Vulkan 1.0 frontend does not expose subgroup properties or
subgroup extensions, so compute and graphics pipeline creation rejects shaders that declare these built-ins.
Grouped decorations are expanded only for tracked literal-valued metadata; other decoration kinds and their semantics
remain unvalidated. Unknown instructions remain safely iterable, but their semantics are not validated or lowered.
Other Vulkan descriptor and feature rules are not inferred beyond those explicitly implemented by this frontend. `gpu_ir_msl.c`
emits a deliberately narrow compute subset from common IR: one void entry point, including multi-block control flow through an MSL state machine, `GlobalInvocationId`, `LocalInvocationId`, `WorkgroupId`, `NumWorkgroups`, `WorkgroupSize` and `LocalInvocationIndex` mapped to Metal kernel input attributes, function-scope scalar/vector variables; set-0 storage-buffer bindings whose resources contain runtime arrays of 32-bit `uint`, `int`, or `float` scalars with a 4-byte stride, including one fixed-size outer descriptor array with constant or feature-enabled dynamically uniform 32-bit indexing; fixed-size storage-image, sampled-image, sampler and combined image-sampler arrays with constant or feature-enabled dynamically uniform 32-bit indices; single-sample, non-arrayed 2D `Rgba8` storage-image `OpImageRead` and `OpImageWrite` with no optional image operands; sampled-image `OpImageFetch` for float32 2D images at Lod 0 with int2 coordinates; explicit-LOD sampling for float32 2D images with float2 coordinates; scalar and 2–4 lane vector arithmetic and logic, scalar integer/floating comparisons and 2–4 lane component-wise floating comparisons, including vector `OpIsNan`, `OpIsInf`, `OpIsFinite`, and `OpIsNormal`, the `OpOrdered`/`OpUnordered` predicates, and all `OpFOrd*`/`OpFUnord*` comparisons, scalar-condition and 2–4 lane vector-boolean-condition `OpSelect`, 2–4 lane vector construction, static extraction/insertion for vector lanes, and dynamic extraction/insertion with 32-bit integer indices, access/load/store/copy/return, workgroup and subgroup control barriers, and scoped memory fences. The state machine preserves branches, conditional branches, 32-bit integer `OpSwitch`, and scalar/vector loop-carried `OpPhi` values, while checking that phi inputs match each block’s predecessor set and snapshotting all incoming values before committing phi results, which preserves simultaneous SSA assignments for cross-dependent loop-carried values. Helper functions with scalar/vector parameters and scalar/vector or void results are supported; Function- and Workgroup-storage pointer parameters to supported scalar/vector/fixed-array values retain their MSL address spaces. Single-block bodies emit straight-line MSL, while multi-block bodies use the same state machine. Nested calls have type-checked arguments. Storage-buffer and supported RGBA8 storage/sampled-image bindings, scalar sampler/combined-image bindings, supported compute built-ins, and supported Workgroup scalar/vector variables or fixed arrays are threaded into helper calls as hidden MSL parameters, preserving descriptor-range checks. Descriptor arrays captured by helper functions, StorageBuffer pointer parameters, pointer returns, pointer-valued phis, entry-point return values, unsupported shader types, and operations outside the listed lowering subset remain unsupported. Built-in variables are accepted only with the scalar or `uint3` type required by each built-in. Loads and stores check each full 4-byte element against the descriptor range before accessing Metal memory. When pipeline layout is supplied, finite compute-visible storage-buffer bindings from multiple descriptor sets, including fixed-size arrays, map densely to Metal slots 0–29; slot 30 carries the byte-range table. Supported storage and sampled-image bindings map densely to Metal texture slots, and samplers to sampler slots, subject to the Metal limits reported for the device. The reusable Metal backend accepts ordered common streams with record-time buffer updates and copies, direct or indirect compute dispatch, descriptor-state binds, and buffer-scoped barriers; resource copies currently support buffers only. A Metal integration path builds the common descriptor layout and immutable descriptor snapshot, records a bind and dispatch in the common GPU stream, then submits through `gpu_ir_metal.m`. The 64-element buffer is checked after uint3 multiplication and a lane-dependent comparison/select; a second region verifies that local and workgroup IDs reconstruct the global X index and that `LocalInvocationIndex` is exposed correctly. The same Metal integration shader executes workgroup/subgroup control barriers and device/workgroup memory fences, including combined uniform/image memory flags. Loop/phi and nested helper-call fixtures, including a multi-block helper with Function/Workgroup pointer arguments that reads a storage buffer through `GlobalInvocationId`, uses a phi value, and captures Workgroup scalar/fixed-array storage, check lowering output and compile generated MSL without running a GPU workload.
The synthetic `float2` MSL fixture emits all four vector classification operations, both `OpOrdered`/`OpUnordered`
predicates, and all `OpFOrd*`/`OpFUnord*` comparisons. The Metal compiler checks this generated source; the fixture
does not execute a GPU workload or verify runtime values.
Function-scope variables also support fixed arrays of scalar or vector values up to 4096 elements, with 32-bit integer indices.
Module-scope `Workgroup` scalar/vector variables and fixed arrays lower to Metal `threadgroup` storage. The Vulkan
`maxComputeSharedMemorySize` query reports Metal's `maxThreadgroupMemoryLength`, and pipeline creation rejects compiled
static threadgroup storage that exceeds that limit.
Unsupported shader layouts, specialization forms or widths, unknown threadgroup sizes and memory flags are rejected.
The Vulkan frontend currently requires fixed or pipeline-specialized local sizes with no more than 128 invocations, one
descriptor type per binding, the fixed one-`uint` uniform block shape above, fixed-size single-level descriptor arrays
with constant or feature-enabled dynamically uniform 32-bit indices for sampled-image, sampler, combined-image-sampler
and storage-image descriptors, and constant or feature-enabled dynamically uniform 32-bit indices for storage-buffer
descriptors, at most 30 buffer descriptor elements across four sets, and image/sampler counts within
the advertised Metal texture/sampler limits. This reflects the
current Metal binding path rather than Vulkan's full descriptor model.
`tools/spirv2msl.m` exposes this in-tree translation path for shader inspection and optional Metal compilation;
`tools/spirv_check.py` classifies a shader corpus as supported, unsupported, or invalid. These tools check only
VXRuntime's current lowering subset and do not claim full SPIR-V or Vulkan validation.

The graphics frontend has an initial render-pass vertical slice. Optimal image formats advertise
`VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT` only when Metal can create a render-target texture and compile the typed
clear pipeline for that format. The implemented render pass accepts one single-sample color attachment, one graphics
subpass, and no dependencies; framebuffers bind one 2D image-view layer. `vkCmdBeginRenderPass` records the supported
initial-to-color-attachment transition and records the common GPU IR begin/end render-pass commands. The command retains
the Vulkan 1.0 load/store actions and typed clear value; Metal performs a clipped clear before opening its render
encoder, preserving pixels outside `renderArea`. It keeps concrete values for `DONT_CARE` because Vulkan leaves those
in-area contents undefined and Metal's native discard action is not limited to the render area. `vkCmdEndRenderPass`
records the color-attachment-to-final-layout transition. The first graphics-pipeline slice accepts builtin-driven
vertex shaders and fragment outputs, plus fragment combined-image-sampler, sampled-image, and sampler bindings as
described above; it also accepts
one static or dynamic viewport and scissor (including clipped subrects and maintenance1
negative-height viewports), triangle list or strip topology, fill/no-cull, one
RGBA8/BGRA8 output, fixed-function blend state for its single attachment, and no depth or logic-op state. Standard
blend factors and operations, channel write masks, and static pipeline constants are retained in common GPU IR.
`VK_DYNAMIC_STATE_BLEND_CONSTANTS` snapshots `vkCmdSetBlendConstants` values into each direct or indirect draw; the
frontend requires a set value when an enabled blend equation uses a constant factor. Logic operations, dual-source
factors, and multiple attachments remain unsupported. `vkCmdDraw` records sequential draws, and
`vkCmdBindIndexBuffer` plus `vkCmdDrawIndexed` record uint16/uint32 indexed draws in common GPU IR. Binding checks the
index-buffer usage, range, and effective memory-offset alignment; draw recording checks the selected index range. The
graphics readback fixture uses uint32 index values above 24 bits with a negative `vertexOffset`, exercising the full
index range while selecting the same three shader vertex IDs.
At draw recording, Vulkan snapshots the bound vertex buffers and the byte ranges remaining from each binding offset.
Common GPU IR checks that every declared input binding is present and verifies the full byte extent for statically
known nonindexed vertex and instance fetches. For indexed draws the index range is known and checked, but the vertex
indices are GPU data; the indirect command fields and any indexed vertex values are also read by the GPU, so their
final vertex fetch ranges cannot be proven by CPU-side recording. Vulkan vertex input supports the mapped 8/16-bit
UNORM, SNORM, SFLOAT, UINT and SINT formats plus 32-bit float, integer and unsigned-integer vectors. The frontend
uses common GPU IR metadata for byte width and numeric conversion class, and reports `VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT`
only for formats that have a corresponding Metal vertex format. Ordinary tests check format queries and graphics-pipeline
creation for normalized, half-float and narrow integer inputs; the shared Metal raster test verifies UNORM conversion
and pixel output. Scaled, sRGB, packed and 64-bit vertex formats remain unsupported. These mappings follow the
[`VkFormat` definitions](https://docs.vulkan.org/refpages/latest/refpages/source/VkFormat.html),
[`VkFormatFeatureFlagBits`](https://docs.vulkan.org/refpages/latest/refpages/source/VkFormatFeatureFlagBits.html),
and Apple's [`MTLVertexFormat` interpretation](https://developer.apple.com/documentation/metal/mtlvertexformat).
Vulkan scissor rectangles are intersected with the active render area before Metal encoding. Viewport bounds are
reported as `[-8192, 8191]`, satisfying the Vulkan range rule for 4096-pixel viewport dimensions; these states follow
the [`VkViewport`](https://docs.vulkan.org/refpages/latest/refpages/source/VkViewport.html),
[`vkCmdSetViewport`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdSetViewport.html),
[`vkCmdSetScissor`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdSetScissor.html), and
[`VkPipelineViewportStateCreateInfo`](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineViewportStateCreateInfo.html)
contracts. Dynamic values are retained as command-buffer state and required by a draw only when its pipeline declares
the corresponding dynamic state.
The physical-device query reports `fullDrawIndexUint32 = VK_TRUE` and `maxDrawIndexedIndexValue = UINT32_MAX`, as
required by the [Vulkan feature and limit contract](https://docs.vulkan.org/spec/latest/chapters/features.html).
Primitive restart remains unsupported by this pipeline subset.
Runtime tests clear a full image, clear a smaller rectangle in a second pass, draw sequentially and through an index
buffer into that clipped area, and verify preserved and updated pixels by copying the image back to host memory.
Fixed-count `vkCmdDrawIndirect` and `vkCmdDrawIndexedIndirect` now record common GPU IR commands; Metal reads the
Vulkan-compatible 16-byte and 20-byte argument records directly during render encoding. The indexed form captures the
bound index buffer, type and offset, while both forms snapshot the active viewport and scissor. The frontend reports
`multiDrawIndirect`, `drawIndirectFirstInstance`, and a `maxDrawIndirectCount` of 4096, validates usage, alignment,
stride, range and feature enablement, and tests both forms with two records plus malformed-stride rejection. This
follows Khronos's [`vkCmdDrawIndirect`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndirect.html),
[`vkCmdDrawIndexedIndirect`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndexedIndirect.html),
and [indirect argument structure](https://docs.vulkan.org/refpages/latest/refpages/source/VkDrawIndexedIndirectCommand.html)
contracts.
`VK_ATTACHMENT_LOAD_OP_LOAD` with `initialLayout == VK_IMAGE_LAYOUT_UNDEFINED` is rejected per Vulkan's valid-usage
rule. User vertex formats outside the mapped subset, non-float or non-default-interpolation varyings, indirect-count extensions, depth, resolve or input attachments,
multiple subpasses, subpass dependencies and multisampling remain unsupported.

The first Vulkan WSI slice advertises `VK_KHR_swapchain` alongside the Win32 surface extensions. It creates two or
three internally owned BGRA8 UNORM images with color-attachment usage, FIFO presentation, the window's current client
extent, and exclusive sharing on the single advertised queue family. `vkAcquireNextImageKHR` rotates through available
images, signals binary semaphores or fences, and supports zero, finite and infinite waits. A failed replacement call
retires its supplied `oldSwapchain`, as specified. `vkQueuePresentKHR` accepts acquired images only after their layout
has reached `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`; the Metal backend samples that texture into the window's `CAMetalLayer`.
When multiple swapchains are presented, the overall result follows Vulkan's error precedence rather than array order.
The path does not support concurrent sharing, non-color image usage, other formats or color spaces, non-FIFO present
modes, or extra swapchain/present `pNext` structures. The ordinary Vulkan test covers extension enumeration,
swapchain creation/image enumeration, acquire signals, exhaustion and old-swapchain retirement; a CPU-only check covers
multi-swapchain result precedence. It does not exercise a successful window present; the current running VXRuntime
processes prevented that Metal/UI runtime test in this pass.
These limits follow the public [`VkSwapchainCreateInfoKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html),
[`vkAcquireNextImageKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/vkAcquireNextImageKHR.html), and
[`vkQueuePresentKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html) contracts.

The format follows Khronos's public [SPIR-V unified specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html):
the module starts with five header words, and each instruction's first word packs its word count and opcode. The
reader is dependency-free and preserves unknown opcodes by their declared size so future semantic work can add
support without changing byte-stream handling.

Next stages:

1. Expand SPIR-V control-flow coverage, broader arithmetic and atomics, each
   checked against ordinary shader programs.
2. Expand dynamic image/sampler descriptor indexing, sampled/storage image formats, sampler features and image operations only after
   validating the Metal format, binding and access behavior for each exposed case.
3. Improve Vulkan physical-device properties and validate the exposed limits against the Vulkan 1.0 requirements.
4. Expand the initial graphics slice with additional vertex formats and interpolation modes, indirect-count commands,
   depth and broader attachment state; exercise successful WSI presentation and add resize/recreation coverage.
