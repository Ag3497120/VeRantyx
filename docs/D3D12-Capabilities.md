# D3D12 format capability queries

`ID3D12Device::CheckFeatureSupport` handles `D3D12_FEATURE_FORMAT_SUPPORT` and
`D3D12_FEATURE_FORMAT_INFO` in `VXRuntime/src/d3d12.m`. The dedicated GPU virtual
address query reports a 40-bit address limit per resource and per process, in
line with the legacy `D3D12_OPTIONS` field.

## Resource placement alignment

`GetResourceAllocationInfo` accepts the public 4 KiB, 64 KiB, and 4 MiB alignment values where the resource
description permits them. Small-resource validation estimates near-equilateral power-of-two tiles from the DXGI
format block size and texture dimensions. It allows up to 16 4 KiB tiles for non-MSAA textures and up to 64 64 KiB
tiles for eligible MSAA textures; texture-array layers do not multiply the most-detailed-mip estimate. Unknown format
block layouts are rejected for small alignment. This follows the [D3D12 resource description rules](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc).
Both allocation-info methods accept only a zero `visibleMask` on this single-node device. Microsoft's
[GetResourceAllocationInfo contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getresourceallocationinfo%28uint_uint_constd3d12_resource_desc%29)
specifies zero for single-GPU operation; the ordinary tests also check that a nonzero mask returns the documented
`UINT64_MAX` size error sentinel.

`CreateHeap` accepts the documented default, 64 KiB, and 4 MiB heap alignments, rejects other values, and rejects
`ALLOW_DISPLAY`, which is valid only for committed resources. `CreatePlacedResource`
enforces `DENY_BUFFERS`, `DENY_RT_DS_TEXTURES`, and `DENY_NON_RT_DS_TEXTURES`, including the corresponding `ALLOW_ONLY_*`
combinations, before attempting Metal placement. This follows the [D3D12 heap description](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_heap_desc)
and [heap flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_flags) contracts. Ordinary D3D12
tests exercise invalid and accepted alignments, plus buffer, RT/DS texture, and non-RT/DS texture rejection on incompatible heaps.

Resource layout validation requires `ROW_MAJOR` for buffers and accepts only `UNKNOWN` for textures. Row-major
cross-adapter textures, tiled `64KB_UNDEFINED_SWIZZLE`, and `64KB_STANDARD_SWIZZLE` are rejected because the runtime
reports cross-adapter row-major, tiled-resource, and standard-swizzle support as unavailable. Allocation-info queries
return the documented `UINT64_MAX` size sentinel for these layouts, and resource creation returns `E_INVALIDARG`.
Ordinary D3D12 tests cover each rejected texture layout and the invalid buffer layout.

Initial resource states and legacy transition-barrier before/after states are checked for known bits, legal read/write
combinations, resource dimension, required resource flags, and resolve sample counts. Vertex/constant-buffer,
index-buffer, stream-output, and indirect-argument states are buffer-only; render-target, depth, resolve, and
shading-rate-source states are texture states, with shading-rate-source restricted to 2D textures. Video and
raytracing-acceleration-structure states are rejected because those execution paths are not implemented. Common copy
and shader-resource states remain valid for both resource kinds where the resource's other flags and heap properties
allow them. The ordinary D3D12 test rejects incompatible initial states and
buffer-only texture transitions, accepts a compatible combined shader-read initial state, and checks a valid texture
shader-read transition. `DENY_SHADER_RESOURCE` is restricted to depth-stencil resources, prevents shader-resource
states, and leaves created SRV descriptors null. Shading-rate-source state requires an R8_UINT 2D image with one mip,
one slice, one sample, UNKNOWN layout, and no render-target, depth-stencil, cross-adapter, simultaneous-access, or
video-reference-only flags. Its access is represented as generic memory read in GPU IR rather than shader read. These
rules follow the public
[`D3D12_RESOURCE_STATES`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_states)
and [`D3D12_RESOURCE_DIMENSION`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_dimension)
definitions, [resource flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_flags),
Microsoft's [VRS image requirements](https://learn.microsoft.com/en-us/windows/win32/direct3d12/vrs), and [resource requirements](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/ResourceHeaps.md).

Resource descriptions accept render-target, depth-stencil, unordered-access, and deny-shader-resource flags only in
valid combinations. Render-target/depth-stencil and simultaneous-access flags are rejected on buffers; unordered
access remains valid for buffers. Unsupported cross-adapter, video-reference, acceleration-structure, tight-alignment
(`0x400`), and unknown flags are rejected by resource creation and allocation-info queries. The ordinary D3D12 test
covers invalid combinations and unsupported flags, as well as the supported simultaneous-access texture and UAV
buffer cases. These constraints follow Microsoft's [resource flag definitions](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_flags) and the DirectX-Specs [tight-alignment proposal](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/D3D12TightPlacedResourceAlignment.md).

MSAA resources are validated as 2D textures with exactly one mip, a render-target or depth-stencil flag, sample
counts in the D3D12 power-of-two range, and no unordered-access or simultaneous-access flag. A small Metal texture
probe with the resource's format and attachment usage keeps allocation-info results and resource creation consistent
with `MULTISAMPLE_QUALITY_LEVELS` and the active device's sample-count support. Invalid descriptions are rejected
before allocating the guest resource.
This matches the [D3D12_RESOURCE_DESC multisampling restrictions](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc)
and the per-format [multisample quality query](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_multisample_quality_levels).

Implemented feature queries require the exact structure size for the selected
feature. `D3D12_OPTIONS2` reports depth bounds and programmable sample positions
as unsupported. `D3D12_OPTIONS3` reports copy-queue timestamps only when the
active Metal device exposes timestamp counters sampled between blit operations,
and reports immediate writes for direct, compute, and copy command lists. Those
writes require a four-byte-aligned buffer GPU address and `COPY_DEST` state; the
runtime stages each 32-bit value in shared Metal memory and encodes an ordered
blit copy at the recorded command position. Marker modes use encoder boundaries,
which may serialize more strongly than their D3D12 scheduling hints.
The runtime uses Metal's blit-boundary sampling for the copy-queue timestamp
heap, `EndQuery`, and `ResolveQueryData` path. Stage-boundary support alone is
not enough for this capability. `WriteBufferImmediate` is implemented through
ordered blit copies; fully typed format casting, view instancing and barycentrics
remain unsupported until their command and shader paths are implemented. This
lets applications select fallback paths for capabilities that the Metal
translation layer cannot provide.

The legacy architecture query and `D3D12_FEATURE_ARCHITECTURE1` derive tile-based
rendering from Apple GPU family support and UMA from `MTLDevice.hasUnifiedMemory`.
Cache-coherent UMA is reported for Apple-family unified-memory devices; isolated
MMU is conservatively reported as unsupported because the guest address-space
bridge does not promise CPU page-table protection semantics.

Feature-level queries and `D3D12CreateDevice` are capped at 11_1. The runtime reports
Tiled Resources Tier 0 and `OutputMergerLogicOp = FALSE`; Microsoft's feature-level
table requires Tier 2 tiled resources, min/max filters, and output-merger logic
operations for 12_0. Returning 12_0 while those required features are unavailable
would let applications select an unsupported path. The query chooses the highest
requested known level no higher than the cap, and device creation rejects a higher
minimum level. This follows Microsoft's [hardware feature-level table](https://learn.microsoft.com/en-us/windows/win32/direct3d12/hardware-feature-levels).

`GetCustomHeapProperties` reports the matching custom heap properties for
DEFAULT, UPLOAD, and READBACK on the single exposed node. DEFAULT is CPU
inaccessible; UPLOAD is write-back on the reported cache-coherent UMA path and
write-combined otherwise; READBACK is write-back. `ID3D12Resource::Map` exposes
buffer memory only for UPLOAD, READBACK, and CPU-visible CUSTOM heaps, requires
subresource zero, and validates the optional range against the buffer size.
The abstract UPLOAD and READBACK heap types accept buffers only; CPU-accessible
textures require a CUSTOM heap with explicit CPU page and memory pool properties.
Resource creation requires optimized clear values to be null for buffers and
accepts typed formats from the resource's compatible format family for textures.
Texture `Map` tracks nested mappings per subresource and returns a null CPU
pointer for opaque layouts. `WriteToSubresource` and `ReadFromSubresource` copy
mapped buffer ranges directly and transfer mapped, single-sample, single-plane
textures through a synchronous Metal blit; texture calls require `COMMON` state
and validate subresource bounds, box alignment, and row/depth pitches. Nonempty
planar and multisampled texture CPU transfers remain unsupported; empty-box
requests are returned as no-ops after validating the subresource index. This path uses the
public D3D12 Map and CPU transfer rules plus Metal's private-texture staging and
blit mechanism:
[`WriteToSubresource`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-writetosubresource),
[`ReadFromSubresource`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-readfromsubresource),
[`MTLTexture`](https://developer.apple.com/documentation/metal/mtltexture), and
[`MTLBlitCommandEncoder`](https://developer.apple.com/documentation/metal/mtlblitcommandencoder).
These heap properties follow Microsoft's
[`GetCustomHeapProperties`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getcustomheapproperties%28uint_d3d12_heap_type%29)
and [CPU page property](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_cpu_page_property) definitions.

`D3D12_OPTIONS1.Int64ShaderOps` is true only when the Metal device supports Apple
GPU family 3 or later. The DXIL translator maps 64-bit integer values to Metal
`long` and `ulong`, and Apple's feature table lists 64-bit integer math from
Apple family 3. `WaveOps` stays false while the full DXIL wave-operation and
helper-lane behavior is not available across the advertised shader stages.
This lets applications avoid selecting 64-bit shader paths on older Metal GPUs.
See Apple's [Metal feature set table](https://developer.apple.com/metal/limits/).

`CreateFence` supports local monitored fences backed by `MTLSharedEvent`. It rejects
unknown flag bits as invalid and returns unsupported for shared, cross-adapter, and
non-monitored fence modes, which have no matching handle or synchronization path in
this runtime. Fence creation fails if Metal cannot create the event or listener;
queue `Signal` and `Wait` validate that the fence belongs to the same device and
report command-buffer allocation failures instead of silently dropping the operation.
See Microsoft's [`D3D12_FENCE_FLAGS`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_fence_flags)
and [command-queue synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/executing-and-synchronizing-command-lists).

`D3D12_OPTIONS.ResourceBindingTier` reports Tier 3. DXIL `CreateHandleFromHeap`
lowers to an indexed pointer into the shader-visible CBV/SRV/UAV or sampler
descriptor heap, and descriptor-table ranges resolve against the full bound
heap, including unbounded ranges. The Metal descriptor representation keeps
unpopulated entries zeroed; state validation ignores those entries unless a
live descriptor names a resource. Shader-visible sampler heaps are limited to
the D3D12 maximum of 2048 descriptors. The D3D12 feature test checks Tier 3 and
that the sampler-heap boundary accepts 2048 and rejects 2049. This report is
about the resource-binding behavior exposed by VXRuntime's D3D12 implementation.

Format support is derived from the active `MTLDevice`: the runtime probes texture
and typed-buffer creation with the usages it actually requests, and tests color
blending by creating a small Metal render pipeline. Results are cached per
device and DXGI format. Flags that the current translator or command path does
not implement, including format casting, video, and tiled resources, stay clear
even when Metal has a related feature. The DXIL translator lowers TextureGather
and TextureGatherCmp for 2D, array, cube, and cube-array textures; 2D offsets
outside Metal's `[-8, 7]` range are rejected. `SHADER_GATHER` is reported for
sample-access, non-depth formats that can create a 2D Metal texture, matching
the currently lowered color gather path. `SHADER_GATHER_COMPARISON` is reported
alongside depth comparison sampling when the depth texture probe succeeds.
The format-query test checks gather flag dependencies across every exposed DXGI
format and requires color gather for RGBA8 plus comparison gather for D32; the
D3D12 runtime test executes `GatherGreen` with an explicit offset against an
uploaded RGBA8 texture and executes `GatherCmp` against a cleared D32 texture,
then checks every output pixel. The DXIL gather fixture also compiles its
generated MSL for 2D, array, cube, and cube-array shapes. Structured-buffer UAV
counters are handled separately from format support and lower to Metal device
atomics.

UAV clears currently support typed and raw buffer views plus the
supported texture views.
`R32_TYPELESS` raw-buffer `ClearUnorderedAccessViewUint` clears only the selected
DWORD range, repeating `Values[0]`; the raw-buffer path records that operation as
the shared GPU IR fill command and stages a bounded pattern for Metal blits.
Structured-buffer clears are rejected: the D3D12 debug layer reports
`CLEARUNORDEREDACCESSVIEW_INCOMPATIBLE_WITH_STRUCTURED_BUFFERS` for both clear
methods. VXRuntime returns `E_INVALIDARG` from command-list `Close` for these
calls. Applications can initialize a structured UAV with a compute shader.
Typed `ClearUnorderedAccessViewUint` supports signed and unsigned integer views;
the lower 8, 16, or 32 bits of each input DWORD are preserved for the
corresponding signed or unsigned component width for `R`, `RG`, and `RGBA`
formats. This includes the signed extension required by `R8_SINT` and
`R16_SINT` Metal writes.
Float clears are supported for typed `R16_FLOAT`, `R16G16_FLOAT`,
`R16G16B16A16_FLOAT`, `R32_FLOAT`, `R32G32_FLOAT`, and
`R32G32B32A32_FLOAT` buffer views; raw float clears, structured-buffer clears,
and buffer rectangles are rejected. The D3D12 float-buffer tests seed
each channel, clear a nonzero range, and verify the clear plus untouched prefix
and suffix, including half-float conversion. The integer tests seed the buffer,
clear a nonzero subrange with four distinct integer clear values, then check every channel in
the cleared range and the untouched prefix and suffix for `R`, `RG`, and `RGBA`
8-, 16-, and 32-bit signed and unsigned typed buffers. Texture tests check full
and rectangular clears at the same component widths. Packed
`R10G10B10A2_UINT` buffer clears are also covered, with ten low bits from each
color value and two from alpha packed in [DXGI bit order](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi-format).
Microsoft's API contracts describe both clear operations as compute-like and
require the resource to be in UAV state:
[ClearUnorderedAccessViewFloat](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearunorderedaccessviewfloat),
[ClearUnorderedAccessViewUint](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearunorderedaccessviewuint).
The raw-buffer first-value behavior is tested directly. D3D11's separate
structured-buffer clear behavior is not applied to D3D12.

`ClearRenderTargetView` and `ClearDepthStencilView` lower to common GPU IR clear
commands after D3D12 records the selected view and validates its API resource
state. The common stream retains the Metal texture view and snapshots the
rectangle list; its D3D12 backend callback clips each rectangle to the selected
attachment and executes only those areas while preserving pixels outside them.
An empty list uses Metal's attachment load-action clear path.
The D3D12 runtime tests check overlapping color rectangles and a partial D32
depth clear by copying the attachments back and inspecting every pixel; they
also check depth clamping, subnormal normalization, and D32S8 stencil clearing
by using the cleared stencil values to gate a full-screen draw. Clear float
inputs normalize subnormal values to zero while retaining NaNs, and depth
values are clamped to the [0, 1] range. This matches the public API contract
that a null rectangle list clears the full view and a supplied list selects the
portions to clear:
[ClearRenderTargetView](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearrendertargetview),
[ClearDepthStencilView](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-cleardepthstencilview).

Graphics and compute PSO creation checks DXIL resource registers against the
provided root signature. Every reflected SRV, UAV, CBV, and sampler range must
be covered in the same register space, with visibility that includes the
shader stage. The stage-specific `DENY_*_SHADER_ROOT_ACCESS` flags also reject
shaders that use root bindings from a denied stage. Draw and dispatch validation
repeats the check against the root signature currently bound on the command
list, so rebinding an incompatible signature rejects the entire command list
before its clears or draws execute. Tests cover a missing register range, a
pixel shader denied root access, and an incompatible root-signature rebind.
This follows Microsoft's
[root-signature flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_root_signature_flags),
[root-signature creation and PSO compatibility](https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-root-signature),
and [shader visibility](https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-root-signature#shader-visibility) rules.
The runtime advertises root signature version 1.1 as its highest fully supported
version. The versioned serializer and RTS0 reader also accept version 1.2 static
samplers with non-normalized coordinates, lowering them to Metal's unnormalized
sampler mode for point or min/mag-linear filters, clamp or transparent-border
addressing, zero LOD range, and no comparison or anisotropy. Version 1.2
integer border colors remain unsupported because Metal exposes only the
floating-point border-color modes; the feature query therefore continues to
report 1.1. The legacy serializer accepts only version 1.0; callers use the
versioned serializer for 1.1 and 1.2 descriptions. Version 1.2 extends
`STATIC_SAMPLER_DESC1` with flags and uses the
RTS0 v3 static-sampler encoding. These mappings follow Microsoft's
[root signature version enumeration](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d_root_signature_version)
and [root signature 1.2 layout](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/VulkanOn12.md),
the [RTS0 container format](https://llvm.org/docs/DirectX/DXContainer.html), and Apple's
[`MTLSamplerDescriptor.normalizedCoordinates`](https://developer.apple.com/documentation/metal/mtlsamplerdescriptor/normalizedcoordinates)
constraints. Ordinary D3D12 tests serialize and create a version-1.2 root signature with non-normalized coordinates,
reject unsigned-integer border colors, verify that the legacy serializer rejects versions above 1.0, and query the
advertised root-signature version. Dynamic and static sampler `MipLODBias` values map to
`MTLSamplerDescriptor.lodBias` on macOS 26 and Apple GPU family 10; other Metal families retain zero bias because
they do not expose this feature. The ordinary sampler draw fixture supplies a nonzero bias to both sampler forms
while sampling a single-mip texture. See Apple's [`lodBias` property](https://developer.apple.com/documentation/metal/mtlsamplerdescriptor/lodbias)
and [Metal feature set table](https://developer.apple.com/metal/limits/).

Texture SRVs normalize mip, array-slice, and plane fields for 1D, 2D, 3D,
multisample-array, cube, and cube-array views. `MipLevels = UINT_MAX` selects
the remaining mips; zero mip or array counts and out-of-bounds ranges are
rejected. `TextureCube` SRVs expose six faces, and `TextureCubeArray` SRVs
convert `NumCubes` to six Metal view slices per cube while preserving
`First2DArrayFace`. The front end rejects a range that exceeds the resource
instead of silently shortening it. Finite nonnegative `ResourceMinLODClamp`
values travel in the existing 32-byte SRV descriptor and are combined with
DXIL's shader-level min-LOD operands for filtered sampling. Explicit-LOD
samples combine the SRV clamp with the requested LOD. Since Metal exposes
per-sample min-LOD clamping only on Mac2 or Apple6 GPU families, nonzero SRV
clamps are left null on older GPUs and on 1D views; shaders with a dynamic
shader-level clamp are rejected when the device lacks that feature. Integer
`TextureLoad` now applies its static offsets and checks the selected mip's
coordinates, array layer, or sample index before reading; accesses outside
those bounds return a zero vector as Direct3D requires. The runtime test checks
2D positive/negative coordinates, invalid mip levels, and positive/negative
static offsets against an initialized texture. Nonzero `ResourceMinLODClamp`
continues to reject `TextureGather` and `TextureLoad` because those paths do not
yet preserve the clamp semantics; this matches Microsoft's out-of-range
`Load` contract and Apple's texture-read min-LOD requirement
([HLSL `Load`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-load),
[Metal texture minLOD](https://developer.apple.com/documentation/metal/mtltexture/minlod)).
Nonzero clamps combined with `MostDetailedMip != 0` are rejected because
D3D12 changes the meaning of `MipLevels` in that case; Microsoft recommends
leaving one of those fields at zero. This follows Microsoft's
[ResourceMinLODClamp rules](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_tex2d_srv)
and Apple's documented
[per-sample min-LOD GPU-family requirement](https://developer.apple.com/documentation/metal/restricting-access-to-specific-mipmaps).
The range-normalization test covers clamp parsing and rejects negative or NaN
values, along with mip and slice fields, null-descriptor dimensions,
multisample arrays, cube sizes, bounds, and overflow. A null SRV descriptor
inherits the resource's texture dimension and covers the full texture where
that default is valid.
This follows Microsoft's
[default SRV rules](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createshaderresourceview),
and its
[cube-array SRV fields](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_texcube_array_srv)
and Apple's requirement that a Metal cube view contains six slices and a cube
array view contains a multiple of six slices
([Metal texture-view API](https://developer.apple.com/documentation/metal/mtltexture/newtextureviewwithpixelformat:texturetype:levels:slices:)).

`ID3D12GraphicsCommandList1::ResolveSubresourceRegion` supports full-subresource
color average resolves with the Metal render-pass resolve path. Partial color
average resolves preserve `pSrcRect`, `DstX` and `DstY` through common GPU IR;
a cached Metal fragment pipeline reads and averages every sample for supported
normalized and floating-point render-target formats, and writes only the
destination rectangle. Color MIN/MAX resolves use typed Metal fragment
pipelines for the currently supported normalized/floating-point and integer
render-target formats. Full and partial color MIN/MAX use the same sample
reduction; partial regions load/store the destination and preserve pixels
outside the selected rectangle. D16 and D32 depth MIN/MAX resolves use Metal's depth
resolve filter on Apple GPU family 3 or later. D16 and D32 depth AVERAGE
resolves use a cached Metal fragment pipeline that reads each multisample depth
value and writes the arithmetic mean through fragment depth output. The common
GPU IR carries the resolve region and mode to the Metal path.
`CheckFeatureSupport(FORMAT_SUPPORT)` reports depth `MULTISAMPLE_RENDERTARGET`
only after a matching multisample Metal depth texture can be created, and
reports `MULTISAMPLE_RESOLVE` for D16/D32 on Apple GPU family 3 or later,
matching the implemented resolve paths. D16/D32 depth resolve also
selects the requested slice when the source and destination are MSAA array
resources. Partial depth AVERAGE/MIN/MAX uses a depth-writing fragment pipeline
for D16/D32, with a load/store pass that preserves pixels outside the resolved
rectangle. The varying-sample RGBA8 array test checks partial color MIN/MAX and
verifies both resolved values and preserved pixels; an RGBA8_UINT case compiles
and executes the typed integer shader path for partial MIN/MAX and full MAX.
Color resolve formats
outside the Metal shader's typed format set, partial D24S8 and D32S8 resolves,
depth average outside D16/D32, stencil-plane and decompress resolves remain
unsupported. D3D12 runtime tests
check full color average, a 3x3 color region from source offset (1,1) to
destination offset (3,2) with all outside pixels preserved, legacy depth
average and `ResolveSubresourceRegion` AVERAGE/MIN/MAX into separate D16 or D32
outputs, and varying-sample 4x D16/D32 array slice selection and values at each
format's precision. The array test resolves a 2x2 depth rectangle using all
three modes and verifies pixels outside it retain the initial depth. Tests also
verify depth format-support flags and rejection of unsupported modes. Microsoft's
[ResolveSubresourceRegion contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist1-resolvesubresourceregion)
defines the full-subresource default and [resolve modes](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resolve_mode). Apple's [Metal feature tables](https://developer.apple.com/metal/capabilities/)
list MSAA depth resolve for Apple GPU family 3 and later.

Texture3D UAVs normalize `MipSlice`, `FirstWSlice`, and `WSize` against the
selected mip's depth. `WSize = UINT_MAX` exposes the remaining depth slices
starting at `FirstWSlice`; a null descriptor defaults to the resource's first
mip and full depth. DXIL `TextureLoad`, `TextureStore`, texture atomics, and
`GetDimensions` use that view-relative depth: accesses translate the relative
W coordinate to the resource slice, reject coordinates outside the view, and
report the view's W size. The D3D12 runtime test uses a four-slice texture and
a `[1, 3)` UAV view to check a store, an out-of-range load/store, an atomic
update, `GetDimensions`, and a null full-range view. This follows Microsoft's
[Texture3D UAV fields](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_tex3d_uav),
[UAV view contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_unordered_access_view_desc),
[default UAV descriptor behavior](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createunorderedaccessview),
and [RWTexture3D::GetDimensions](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-object-rwtexture3d-getdimensions).

DXIL memory-only fences lower to Metal `atomic_thread_fence` with the device or
threadgroup scope selected by the barrier mode. UAV fences include both Metal's
device-memory and texture-memory flags so buffer and texture accesses are
ordered. That intrinsic is available in
Metal shading language 3.2, so D3D12 PSOs containing one compile as MSL 3.2 on
macOS 15 and later; the runtime rejects those PSOs on older macOS versions.
The DXIL fixture tool compiles ordinary translated shaders as MSL 3.1 and
fence-using shaders as MSL 3.2; on older systems it still checks translation
but skips Metal compilation for that fence fixture.
Execution barriers continue to use Metal's `threadgroup_barrier`. This preserves
the distinction Microsoft specifies between a memory barrier and its
`WithGroupSync` counterpart; for example,
[DeviceMemoryBarrier](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/devicememorybarrier)
does not carry the all-threads-reached requirement of
[DeviceMemoryBarrierWithGroupSync](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/devicememorybarrierwithgroupsync).
Apple documents MSL version selection through
[MTLCompileOptions.languageVersion](https://developer.apple.com/documentation/metal/mtlcompileoptions/languageversion)
and [MTLLanguageVersion 3.2](https://developer.apple.com/documentation/metal/mtllanguageversion/version3_2).

Metal's 1D texture sampling interface accepts only implicit-derivative sampling.
DXIL `SampleBias`, `SampleLevel`, `SampleGrad`, and non-undefined `Sample` min-LOD
clamps on 1D and 1D-array resources, along with nonzero or dynamic texel offsets,
therefore fail translation with an explicit diagnostic instead of silently
dropping their sampling information. The shader fixture keeps these cases as
negative translation checks; supported dimensions continue through the normal
Metal compilation path.

Comparison sampling lowers depth 2D, 2D-array, cube, and cube-array resources to
Metal's matching `sample_compare` overloads. DXIL permits additional comparison
resource shapes, but Metal has no 1D depth texture type, so those remain explicit
translation errors. The supported cube shapes are checked by compiling generated
MSL from the comparison fixture.

D3D12's static PSO sample mask is applied to pixel-shader output coverage: a
shader-written `SV_Coverage` value is ANDed with the PSO mask, and shaders without
`SV_Coverage` receive the mask as their Metal `[[sample_mask]]` output. A
non-default mask on a pipeline without a pixel shader is rejected because this
backend has no fragment stage in which to apply it. Metal has no matching
output-merger state for D3D12 logic operations, conservative rasterization, or
forced sample count, so pipelines requesting those modes are rejected explicitly
instead of being created with different behavior. Invalid logic-operation blends,
independent logic operations, and invalid forced-sample-count or
conservative-raster values are rejected as invalid PSO descriptions.
The D3D12 draw test renders with sample zero masked off and checks all 4096 pixels
remain at the render-target clear color; it also checks the unsupported and
invalid PSO cases. The field and validation rules follow Microsoft's
[graphics PSO](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_graphics_pipeline_state_desc),
[render-target blend](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_render_target_blend_desc),
and [rasterizer](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_rasterizer_desc)
descriptions.

DXIL typed UAV atomics are translated for R32_SINT and R32_UINT buffers and
1D/2D/3D textures, including array views. The runtime advertises the matching
`FORMAT_SUPPORT2` atomic bits only when the Metal device supports the Apple 6
GPU family and creation probes succeed for all translated texture shapes and a
texture buffer. Signed and unsigned min/max bits are reported only for their
matching DXGI integer format. Out-of-range accesses return zero without changing
the resource; compare-exchange retries Metal's weak operation when it reports a
spurious failure. Runtime tests dispatch min/max, exchange, and compare-exchange
against raw and structured buffers, typed R32_UINT/R32_SINT buffers, and 1D,
1D-array, 2D, 2D-array, and 3D R32_UINT textures, then check both the returned
old values and the stored results. Metal compilation fixtures cover all translated
texture shapes.

`FORMAT_SUPPORT2_UAV_TYPED_STORE` and `UAV_TYPED_LOAD` are probed independently.
The store probe uses Metal's write-only texture access; the DXIL translator
selects `access::write` for UAVs that only receive stores, while reads and
atomics retain read/write access. This exposes store-only formats without
requiring Metal's read/write texture tier. Typed loads use the mandatory R32
formats where Metal tier 1 permits them, the 15-format D3D12 additional set
only when every format in that set passes its Metal read/write probe, and
per-format probes for D3D12's individually optional formats. Texture-buffer
views are also checked with the corresponding access mode. These rules follow
Microsoft's [typed UAV load format sets](https://learn.microsoft.com/en-us/windows/win32/direct3d12/typed-unordered-access-view-loads)
and [separate format support bits](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_format_support2),
plus Apple's [texture-buffer access table](https://developer.apple.com/metal/capabilities/)
and [shader-write usage](https://developer.apple.com/documentation/metal/mtltextureusage/shaderwrite).

Format plane counts describe DXGI format layout and are independent of whether
Metal can create a resource for that format. The tests cover ordinary RGBA8 and
R32 format support, single-plane color, two-plane NV12 and depth/stencil, the
three-plane V208 layout, and rejection of `DXGI_FORMAT_UNKNOWN`.

`ID3D12GraphicsCommandList2::WriteBufferImmediate` accepts all three public
modes and records one 32-bit write per parameter. Queue submission resolves and
retains each destination buffer, checks its state at the command's position in
the list, and writes through the Metal blit stream. The D3D12 suite checks the
default, marker-in, and marker-out forms on direct, compute, and copy lists,
plus rejection of a direct list submitted to a copy queue. The API contract is
described by Microsoft's [WriteBufferImmediate method](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist2-writebufferimmediate),
[parameter structure](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_writebufferimmediate_parameter),
and [mode enumeration](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_writebufferimmediate_mode).

## Command allocator and list lifetime

An allocator can be associated with at most one command list that is currently
recording. `Close` releases that recording association; `Reset` reopens a closed
list with a compatible allocator. Resetting an allocator fails while its list is
recording or any submitted Metal command buffer that used it is still in flight.
Each allocator reset advances a generation, so a closed list recorded against an
older generation is rejected at submission. The D3D12 suite checks the active
recording, duplicate-list, blocked-queue, and post-completion reuse cases. These
rules follow Microsoft's
[command allocator Reset](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandallocator-reset)
and [graphics command list Reset](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-reset)
contracts.

Command-queue creation validates Direct, Compute, and Copy types, the public
priority values, supported flags, and the single-node zero mask. A request for
global real-time priority returns `DXGI_ERROR_UNSUPPORTED` because Metal cannot
provide that scheduling guarantee. Normal and high priority descriptions are
retained by `GetDesc`; Metal does not otherwise expose D3D12 queue scheduling
priority. The tests check invalid values, the unsupported real-time request, and
creation with high priority and the GPU-timeout flag. See Microsoft's
[queue description](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_command_queue_desc)
and [priority enumeration](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_command_queue_priority).

## Wave operations

The DXIL translator lowers `WavePrefixBitCount`, the sum, product, minimum,
and maximum forms of `WavePrefixOp`, `WaveMatch`, and the SM 6.5
`WaveMultiPrefix*` operations. Ballots, reductions, prefixes, first-lane reads,
and lane-match masks exclude inactive lanes. Pixel-shader wave operations
exclude helper lanes by default; the SM 6.7 feature flag opts them back in.
Prefix min/max and the SM 6.5 partitioned operations use explicit lane shuffles
and compare group masks after removing inactive lanes.
`D3D12_OPTIONS1.WaveOps` remains false because the complete advertised wave
operation set and its stage, helper-lane, and wave-width semantics are not yet
implemented and validated as a whole. These lowerings use Metal's native SIMD
group width. The reader parses the SFI0 feature-info part and honors the SM 6.7
`WaveOpsIncludeHelperLanes` flag by including helper lanes in pixel wave
operations when the flag is set. This translates the requested participation
policy, but does not independently guarantee Metal schedules helper lanes until
the final wave operation. The tests check generated MSL and compile compute and
pixel fixtures. GPU tests compare full-wave and partial-wave compute results
against expected values for ballots, reductions, exclusive prefixes, lane
reads, `WaveMatch`, and all SM 6.5 multi-prefix forms; a 33-thread group leaves
inactive lanes in its final wave. Pixel helper-lane policy and behavior with
inactive pixel lanes still lack a GPU reference comparison.
The common DXIL compute path lowers `WaveGetLaneIndex` and `WaveGetLaneCount` to
API-neutral subgroup built-ins and emits Metal's `thread_index_in_simdgroup`
and `threads_per_simdgroup` input attributes. It also lowers
`WaveActiveAnyTrue`/`WaveActiveAllTrue` to shared subgroup-vote operations and
MSL `simd_any`/`simd_all`, and compute `WaveActiveMin`/`WaveActiveMax` for
32-bit signed and unsigned integers to shared subgroup min/max operations and
MSL `simd_min`/`simd_max`. DXIL's signless integer representation is bitcast
through signed MSL `int` for signed reductions. The tests check all four
integer reductions and compile the generated MSL. This narrow common-IR slice
does not change the conservative `WaveOps` report or Vulkan feature reporting;
it does not test D3D12 command-list execution. The lane-query signatures follow Microsoft's
[`WaveGetLaneIndex`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/wavegetlaneindex)
and [`WaveGetLaneCount`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/wavegetlanecount) definitions;
the vote operations follow [`WaveActiveAnyTrue`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/waveanytrue)
and [`WaveActiveAllTrue`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/wavealltrue),
[`WaveActiveMin`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/waveactivemin),
and [`WaveActiveMax`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/waveactivemax). MSL's
`simd_any`/`simd_all` active-thread rules are documented in Apple's [Metal Shading Language specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf).
See Microsoft's
[Shader Model 6.5 wave-intrinsic specification](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_5.html)
and [SM 6.7 helper-lane option](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_7_Wave_Ops_Include_Helper_Lanes.html).

## Command-list bundles

Direct command lists can execute closed graphics bundles. VXRuntime snapshots the bundle commands when
`ExecuteBundle` is recorded, carries the parent's target and root state into the bundle, and leaves graphics
state changes from the bundle active afterward. A bundle must set its own graphics PSO and primitive topology
before drawing. Descriptor heaps set inside the bundle must match the heaps bound by the parent. The supported
subset is graphics PSO/root/IA state, direct draw calls, and one-operation Draw/DrawIndexed `ExecuteIndirect`
signatures with no count buffer or root/IA updates. Commands prohibited in bundles fail `Close`. Bundles cannot be
submitted directly; the other `ExecuteIndirect` bundle forms remain unsupported.

The D3D12 PSO path also attempts the common DXIL-to-MSL vertex route for a vertex shader that writes a complete
float32 `SV_Position`, reads only generic one-row float32 inputs with one to four components, has no resource bindings,
and has a matching D3D12 input-layout element for every used input. Input signature IDs retain their reflected Metal
attribute locations. Unsupported shaders and layouts continue through the direct DXIL-to-MSL shader path. This is a
narrow graphics-I/O slice; it does not make the full D3D12 graphics shader path common.

For a single render target, default sample mask and no pixel-shader resource bindings, the PSO path also attempts the
common fragment route. That shader may read scalar float32 `SV_Position` components and bool `SV_IsFrontFace`, then
write a complete float32 `SV_Target0`. The latter maps DXIL's `I1`/`IsFrontFace` signature input to Metal's
`[[front_facing]]` builtin and accepts both packed start row zero and the `-1` system-generated sentinel used by
newer shader models. Unsupported signatures remain on direct DXIL-to-MSL.

Graphics PSOs also lower each shader-matched input element's byte width, input slot, attribute offset, per-vertex or
per-instance classification, and instance step rate into common GPU IR metadata. Direct draws retain their bound VBV
and IBV ranges. The common recorder checks statically known nonindexed vertex and instance fetch extents and checks
the selected index bytes against the IBV; indexed vertex IDs are GPU data, so the exact vertex fetch range cannot be
validated before execution. Ordinary command-list `ExecuteIndirect` captures the IA views at record time, while bundle
indirect draws use the inherited IA state during replay. Format interpretation stays in the D3D12 input assembler and
Metal vertex descriptor. Input-layout creation rejects nonzero `InstanceDataStepRate` on per-vertex elements, as required
by Microsoft's [`D3D12_INPUT_ELEMENT_DESC` contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_input_element_desc).

## What engines call at start-up and per frame

`tests/d3d12_engine.c` walks the sequence Unreal Engine and Unity follow. DXGI exposes `IDXGIFactory5`–`7`
(`CheckFeatureSupport(PRESENT_ALLOW_TEARING)` is TRUE because a sync-interval-0 present does not wait for the display;
`EnumAdapterByGpuPreference` returns the one adapter for every preference), `IDXGIAdapter4::GetDesc3`, and
`IDXGIOutput1`–`6`; `GetDesc1` describes the display as 8-bit SDR (G22/P709), so engines keep HDR off.
`IDXGISwapChain4::SetHDRMetaData` validates and ignores its metadata. A D3D12 swap chain created with
`FRAME_LATENCY_WAITABLE_OBJECT` hands out a semaphore holding one count per frame the program may queue
(`SetMaximumFrameLatency`, 1 by default); each present returns its count when the GPU has drawn it.

`CheckFeatureSupport` answers `D3D12_OPTIONS4` to `OPTIONS18`, `SHADER_CACHE`, `COMMAND_QUEUE_PRIORITY`, `EXISTING_HEAPS`,
`SERIALIZATION`, `CROSS_NODE`, `DISPLAYABLE` and the protected-session queries at their exact sizes, reporting the
features as absent (render pass tier 0, no raytracing, mesh shaders, VRS, enhanced barriers or 64-bit atomics), as the
Windows runtime does on hardware without them. `ID3D12Device2::CreatePipelineState` reads pipeline state streams
(subobject types 0x00–0x16, D3D12's defaults for the ones left out; view instancing and depth bounds are refused)
into the classic graphics or compute description. `CreateCommandList1` creates a closed list without an allocator.
Render passes follow tier 0, the tier on which Windows emulates them: `BeginRenderPass` binds the targets and clears
those whose beginning access is CLEAR, `EndRenderPass` resolves those whose ending access is RESOLVE (the source in
`RENDER_TARGET` state, as the API requires). PIX events and markers, `DiscardResource`, `OMSetDepthBounds`,
`SetSamplePositions` and `SetViewInstanceMask` do nothing; `SetPredication` with a buffer runs the predicated work
anyway (logged once). `CreatePipelineLibrary` returns `DXGI_ERROR_UNSUPPORTED`, which engines take as "no PSO
library"; `SetEventOnMultipleFenceCompletion` waits for all or any of its fences.
Single-sample 2D color textures of the resolvable formats are made with Metal's render-target usage whatever their
flags, so any of them can be a resolve destination, as in D3D12 (Metal resolves only into render-target textures).

## fxc's shaders (DXBC) in Direct3D 12 pipelines

Unreal Engine 4 and Unity compile HLSL with fxc for Direct3D 12 by default, so their pipelines carry DXBC (shader model
4 and 5), not DXIL. Graphics and compute pipelines accept both: `dxbc_translate_d3d12` (`src/dxbc.c`) emits the same
MSL as for Direct3D 11 but takes the resources from the root argument buffer DXIL shaders use (`VxRoot` at
`[[buffer(0)]]`, one slot per declared register in DXIL's order: SRVs, UAVs, constant buffers, samplers, space 0),
and reports them in a `dxil_msl` reflection, so root signatures, descriptor tables, root constants and descriptors,
static samplers and validation treat both kinds alike. Textures, samplers and typed UAVs are read from the descriptors'
Metal resource ids, buffers from their GPU addresses (sizes for bounds and `GetDimensions`), append/consume counters
from the descriptor's counter address. Not yet: geometry, hull and domain stages, a static sample mask with a DXBC pixel
shader, non-default SRV component mappings, and shader model 5.1 register spaces and resource arrays.
`build/dxbc2msl -d3d12` and `build/dxbcscan -12` check the translation (223 DXBC shaders from Half-Life: Alyx's and
SteamVR's DLLs translate and compile); `tests/d3d12_fxc.c` draws and dispatches with the D3D11 test shaders.

Root parameters and static samplers are matched to a shader by register and space and by `ShaderVisibility`: a
graphics stage sees ALL and its own parameters (vertex and pixel shaders of one pipeline commonly use the same
register, b0 of each stage, for different parameters); compute shaders see every parameter.

## Replay cost at game scale

`make d3d12-bench` (`tests/d3d12_bench.c`) records a frame of 40 render passes of 25 textured draws and 20 dispatches
over a thousand textures in a shader-visible heap of game size. Every resource's memory is in one `MTLResidencySet`
(macOS 15 and later; placed resources count their heap) that the device's queues carry, so encoders no longer declare
every resource and walk the descriptor heap; draws and dispatches still declare what their bindings name, which is
what Metal's hazard tracking follows (`VXRT_D3D12_NO_RESIDENCY_SET=1` restores the old per-encoder declarations).
Root constants and root descriptors go to 1 MB scratch chunks kept in a pool. Command allocators are judged idle by
a per-queue timeline event (as fences are), not by Metal's later completion handlers, which made `Reset` fail right
after the program's fence said the GPU was done.

## D3D11On12 (SteamVR's path for Direct3D 12 games)

SteamVR's `vrclient` submits a Direct3D 12 game's eye textures through `D3D11On12CreateDevice` and
`ID3D11On12Device::CreateWrappedResource`. `src/d3d11on12.m` makes an ordinary Direct3D 11 device whose Metal command
queue is the given Direct3D 12 queue's, so its work is ordered after the Direct3D 12 work submitted before it and before
the work submitted after its Flush, as D3D11On12 promises. Wrapped 2D textures and buffers are Direct3D 11 objects on
the Direct3D 12 resource's Metal object; `ReleaseWrappedResources` sets the resource's tracked Direct3D 12 state to the
wrap's output state. `ID3D11On12Device1::GetD3D12Device` works; `UnwrapUnderlyingResource` and wrapped 1D/3D textures
do not yet. `tests/d3d11on12.c` checks both orderings and the state hand-back without waits in between. Not yet run
with SteamVR and a Direct3D 12 game.
