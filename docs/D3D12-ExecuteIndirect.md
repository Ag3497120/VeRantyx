# D3D12 ExecuteIndirect on Metal

VXRuntime implements D3D12 command signatures and `ExecuteIndirect` in `VXRuntime/src/d3d12.m`. It follows the public
[DirectX indirect drawing specification](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/IndirectDrawing.md)
for tightly packed argument payloads, arbitrary 4-byte-aligned record strides, optional GPU count buffers, the
`min(count, MaxCommandCount)` rule, command ordering, and post-call state resets.

The current command signature accepts `DRAW`, `DRAW_INDEXED`, `DISPATCH`, vertex buffer views for slots 0–15, index
buffer views, root constants, root CBV/SRV/UAV views, and the Tier 1.1 incrementing constant. Signature creation checks
payload size and ordering, requires exactly one terminal draw or dispatch operation, checks root parameter types and
ranges, rejects duplicate or out-of-range incrementing constants, and requires a root signature if and only if the
signature changes root arguments. `ExecuteIndirect` checks that the list is direct or compute, that its resources are
buffers, and that argument/count offsets and maximum argument ranges are valid.
At queue submission, the signature's root signature must match the one bound to the command list. Fixed-signature
indirect dispatches and draws also validate the currently bound pipeline and shader resource states. UPLOAD-backed
signatures that update root arguments or IA views validate each decoded record against its resulting shader and IA
state. GPU-only root/IA inputs are staged and validated during preflight when their command list has not issued prior
GPU work; the readback is ordered after earlier submissions on the same queue. If earlier work in the same list may
have produced those records, replay submits and waits for that prefix, reads the records in queue order, then reruns
whole-list state validation with the staged records before expanding them. Invalid per-record root/IA resource states
skip those records while the signature's post-call resets still apply.

For fixed-stride `DISPATCH`, `DRAW`, or `DRAW_INDEXED` records with no root updates, replay records a common-GPU-IR
indirect-range operation. Draw signatures must also leave vertex and index buffer bindings unchanged. Metal reads
no-count dispatch and draw arguments directly from the original buffer, keeping the D3D12 record stride. For dispatch
and simple draw signatures with a count buffer, a cached Metal compute kernel copies only the command payloads into
scratch storage and zeroes the dispatch group-count X, vertex count, or index count beyond
`min(count, MaxCommandCount)`. Replay then issues the bounded set of Metal indirect commands from that storage. Both
routes run in the same Metal command buffer without CPU readback or a wait; the counted route also reads and clamps
the count on the GPU.
The currently bound pipeline, root state, topology, and IA views are applied before graphics draws.

Signatures that update root arguments or IA views use a CPU compatibility path. UPLOAD records are read directly;
when the argument or count buffer is GPU-only, replay ends the active Metal encoders, blits the bounded argument range
and optional count into a temporary shared buffer, waits for that blit, then expands the records into ordered D3D12
front-end state updates and transient common-GPU-IR draw/dispatch commands. The temporary readback keeps preceding
GPU writes in order, including a count produced earlier in the same command list. This path can stall the CPU and GPU
at each such ExecuteIndirect call. Fixed-signature draw/dispatch ranges continue to use the GPU-side Metal path.
Submission validation uses the same count clamp and record bounds while simulating readable root/IA updates, validates
draw or dispatch resource states, and records post-call resets. GPU-only inputs preceded by GPU work in the same list
are checked during ordered replay after their producer prefix has completed. The public specification permits indirect
buffers from any heap type, so this compatibility path improves execution coverage but still performs CPU readback and
does not provide GPU-side rewriting for root/IA-updating signatures.

The Tier 1.1 counter starts at zero for each call, advances once per executed command, occupies no argument-buffer bytes,
and resets its target DWORD after the call. Raytracing and mesh dispatch signatures, and bundle execution remain
unsupported. Existing ordinary tests cover invalid command signatures and argument offsets, GPU-only root/IA records
from an earlier submission that are staged and rendered after preflight validation, same-list GPU-produced root/IA
records rejected when their vertex resource remains in COPY_DEST, a GPU-written count that clamps a
padded two-record dispatch buffer to one GPU-side indirect dispatch, a GPU-only count buffer that clamps a padded
two-record draw stream without CPU readback, a GPU-only root/IA-updating draw with a GPU-produced count,
no-count two-record non-indexed and indexed draws with padded strides, an indirect indexed draw
with root constants and both buffer views, a zero-count-buffer call that resets touched root constants without drawing,
and incrementing-constant draws that verify the 0-origin counter, the per-call restart, and the post-call reset. These
tests do not establish compatibility with any game.
