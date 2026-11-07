# Graphics pipeline library and shader objects: feasibility

Status: study only, no code. Written 2026-11-07 for the Ocarina of Time stutter work
(`0400486024DEC000`): new areas built 166 pipelines whose NVIDIA driver compile took 40.5 s of
worker time (p50 65 ms, p90 545 ms, max 3.0 s), and native draws wait for every one of them.

## Why it would help

Today one `vkCreateGraphicsPipelines` call per `GraphicsPipelineCacheKey` compiles every stage
from SPIR-V with all fixed state. The same vertex shader is compiled again for each new
combination of fragment shader, render-target formats, blend, topology and so on (the last session
translated vertex shader `46cc6aaba42f700f` in 35 pipelines). With
`VK_EXT_graphics_pipeline_library` (GPL) each stage would be compiled once into a library and
pipelines would be *linked* from libraries in well under a millisecond with
`graphicsPipelineLibraryFastLinking`. `VK_EXT_shader_object` removes pipelines altogether.

The RTX 4060 Laptop on driver 615.78.08 exposes `VK_EXT_graphics_pipeline_library` with
`graphicsPipelineLibraryFastLinking = true` and
`graphicsPipelineLibraryIndependentInterpolationDecoration = true`, `VK_EXT_shader_object`,
`VK_EXT_extended_dynamic_state{,2,3}`, `VK_EXT_vertex_input_dynamic_state` and
`VK_EXT_descriptor_buffer` (`vulkaninfo`).

## DISABLE_OPTIMIZATION is not a shortcut on NVIDIA

`tools/nx2/pipeline_compile_bench.cpp` compiled 394 vertex+fragment pipelines from the Ocarina
cache (`vulkan.fork-v54.bin`, SPIR-V from shader-sweep's offline profile, salted modules,
`__GL_SHADER_DISK_CACHE=0`, one thread, RTX 4060 Laptop, 615.78.08, machine under load from
parallel builds):

| Flags | Total | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| none | 91.2 s | 32.5 ms | 136.5 ms | 5.4 s | 11.5 s |
| `DISABLE_OPTIMIZATION` | 91.4 s | 32.1 ms | 136.1 ms | 5.7 s | 13.7 s |

`VK_KHR_pipeline_executable_properties` reports identical register counts and binary sizes for
all 394 pairs: the driver ignores the flag. Its disk cache also returns the same entry for both
(an optimized build after an unoptimized one of the same SPIR-V takes 0.3 ms). A two-tier
compile (`EDEN_NX2_FAST_FIRST_COMPILE`) therefore cannot shorten held draws on this driver;
only fewer or cheaper compiles (GPL fast linking, fewer pipeline variants) can.

## Driver-level reuse

Every `vkCreateGraphicsPipelines` receives `PipelineCache::vulkan_pipeline_cache`, but on NVIDIA
and AMD proprietary drivers `DriverCachesPipelines` leaves it null on purpose and the driver's own
disk cache (`~/.cache/nvidia/GLCache/<per-application hash>/`, 218 MB for Lichii here) does the
reuse. It is keyed per application path, so a build run from another directory starts cold.
The NVIDIA README documents `__GL_SHADER_DISK_CACHE`, `_PATH` and `_SIZE` (without a default
size) and warns that running without `_SIZE` after a larger one wipes an over-size cache; it does
not document `__GL_SHADER_DISK_CACHE_SKIP_CLEANUP`. Setting `__GL_SHADER_DISK_CACHE_SIZE` (e.g.
10 GB) globally, and always launching the same binary path, keeps the boot preload warm.

## What our SPIR-V bakes in today

A stage's SPIR-V is a function of its guest program **and** `Shader::RuntimeInfo`
(`MakeRuntimeInfo` in `vk_pipeline_cache.cpp`), its `Shader::Profile`, and the binding counters
carried from the previous stage. Each of these must become stage-local, a specialization
constant, or dynamic state before a stage can be compiled once and reused.

| Input | Stage | Source | What GPL/shader objects need |
|---|---|---|---|
| `generic_input_types` (attribute float/sint/uint) | vertex | `key.state.attributes` / `attribute_types` | Fold into the vertex-shader cache key (few variants per shader), or read attributes as uint and bitcast by a spec constant |
| `fixed_state_point_size` | vertex/geometry | `key.state.point_size` when topology is points | Spec constant, or a push-constant point size |
| `xfb_varyings`, `xfb_count` | vertex/geometry | `key.state` when `xfb_enabled` | Part of the stage key (XFB layout is rare and stable per shader) |
| `convert_depth_mode` (GL NDC) | pre-raster | `key.state.ndc_minus_one_to_one` | Spec constant or `VK_EXT_depth_clip_control` (dynamic with EDS3) |
| `y_negate` | pre-raster/fragment | `key.state.y_negate` | Spec constant or push constant (we already push render-area data) |
| `input_topology` | geometry | `key.state.topology` | Geometry-only; stage key |
| tessellation primitive/spacing/winding | tess eval | `key.state` | Stage key (spec constants are not allowed for execution modes) |
| `previous_stage_stores`, legacy mapping, `per_primitive_generics` | every stage after the first | the previous **program** | The real blocker: a fragment shader's interface depends on what the vertex shader writes. Must be replaced by a fixed interface (declare all 32 generics, rely on `IndependentInterpolationDecoration`) |
| `alpha_test_func`, `alpha_test_reference` | fragment | `key.state` | Spec constants (DXVK does this) |
| `dual_source_blend` | fragment | `key.state.attachment0_dual_source_blend` | Part of the fragment-output key |
| `color_output_types` | fragment | render-target formats (MoltenVK only today) | Fragment-output key |
| `force_early_z` | fragment | `key.state.early_z` | Execution mode: stage key (two variants at most) |
| binding numbers | every stage | `Shader::Backend::Bindings` accumulated across stages in `EmitShaderModule` | The second blocker: a stage's bindings start where the previous stage's ended, so the same fragment shader gets different bindings next to a different vertex shader |
| rescaling / render-area push constants | every stage | `RescalingLayout`, `RenderAreaLayout` | Already stage-independent |

## Descriptor layout constraints

Each `GraphicsPipeline` builds **one** descriptor set layout from all of its stages
(`DescriptorLayoutBuilder` over `stage_infos`), then picks push descriptors, a descriptor buffer
or a pooled set for that layout, and a pipeline layout with a single all-graphics push-constant
range. GPL requires the libraries that are linked together to use compatible layouts, so either:

1. one set per stage and `VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT` (what DXVK does),
   with bindings numbered from zero inside each stage's set; or
2. a universal layout with fixed binding ranges per stage (simpler, wastes descriptors, and
   conflicts with push descriptors' `maxPushDescriptors` limit).

Either one changes `GuestDescriptorQueue` filling, the update templates, the descriptor-buffer
offsets (`SetDescriptorBufferOffsetsEXT` per set) and `ConfigureImpl`'s per-stage loops.

## How DXVK does it

DXVK (2.0+) compiles each shader into a GPL library when the application creates the shader:
vertex-input and fragment-output state come from small separate libraries, the pre-rasterization
and fragment-shader libraries come from the shader alone with state-dependent behaviour
(alpha test, sample rate, etc.) turned into specialization constants and dynamic state. At draw
time it fast-links (`VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT` off) and records the draw
immediately, then compiles the fully optimized monolithic pipeline on a background thread and
switches to it when ready. It uses one descriptor set per stage with independent sets. Draws are
never skipped, so there are no missing objects and the stall is the link time. When a shader
cannot be expressed as a library, DXVK falls back to a synchronous monolithic compile.

`VK_EXT_shader_object` has the same prerequisites (stage-local SPIR-V, per-stage set layouts) and
additionally needs every piece of fixed state as dynamic state (EDS3 everywhere, vertex input
dynamic, etc.). NVIDIA implements it natively and well; it is a larger rewrite of
`ConfigureDraw`/scheduler state tracking than GPL, and there is no optimized background pipeline
to swap to, so steady-state code quality depends entirely on the driver.

## Recommended staged path

1. **Measure** (done in this branch): driver build totals in the 10 s runtime report;
   `tools/nx2/pipeline_compile_bench.cpp` times real game SPIR-V with and without
   `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT` (no difference on NVIDIA, above). Add `EDEN_NX2_PIPELINE_DIFF=1` runs to learn
   which key fields multiply pipelines per shader.
2. **Stage-local binding numbers** (M, low risk alone): number each stage's bindings from zero
   in a per-stage set. This is a prerequisite for both GPL and shader objects and can ship on its
   own behind a pipeline-cache version bump.
3. **Fixed stage interface** (M): emit all 32 generic outputs/inputs regardless of the previous
   stage, keyed so that `previous_stage_stores` no longer enters the fragment SPIR-V; verify with
   the shader sweep and frame dumps that no interpolation or default-value behaviour changes.
4. **Specialization constants** for alpha test, point size, y-negate and NDC (S-M each).
5. **GPL behind a switch** (L): pre-raster + fragment libraries per (program, stage key), vertex
   input via `VK_EXT_vertex_input_dynamic_state` (already used when available), fragment output
   library per (formats, blend, dual-source) key; fast-link on a miss, optimized background build
   and swap (reuse the retirement scheme of `EDEN_NX2_FAST_FIRST_COMPILE`). Fall back to the
   current monolithic path for tessellation, geometry, transform feedback and mesh pipelines.
6. Shader objects only if GPL linking still shows up in profiles.

Estimated effort: steps 2-4 about two to three weeks of careful work including shader-sweep and
frame-dump validation across the known games; step 5 another two to three weeks. Risk: binding
and interface changes touch every game, so each step needs the full shader sweep and A/B frame
dumps before it can become unconditional.

## Step 2: fixed stage interfaces (`EDEN_NX2_GPL_FIXED_INTERFACES=1`)

Only takes effect together with `EDEN_NX2_GPL_STAGE_BINDINGS=1` (and only in pipelines that
keep the stage binding ranges). Unset, the emitted SPIR-V is byte-identical to before.

Plain vertex+fragment pipelines (vertex A+B or B, optional fragment, no transform feedback,
`maxVertexOutputComponents >= 128`) use `Shader::Maxwell::UseFixedStageInterface`:

- The vertex stage declares all 32 generic outputs at locations 0-31. Ones it does not write
  hold (0, 0, 0, 1), which the vertex prologue already stores into every declared output.
  Outputs carry no interpolation decorations, as before.
- The fragment stage declares every generic it loads, with the same locations, types and
  `Flat`/`NoPerspective` decorations as today (they come from the fragment program alone).
  `previous_stage_stores`, the previous stage's legacy mapping and `per_primitive_generics`
  no longer enter it.
- A fragment stage that loads legacy varyings (colours, fog, fixed-function texcoords) or
  indexes its inputs keeps the linked interface: the legacy-to-generic slot comes from the
  vertex stage's free slots, and an indexed load of an unwritten generic reads 0 in `w` today.

Unwritten inputs: a fragment input the vertex stage does not write used to be a constant
(0, 0, 0, 1); it now reads the vertex stage's default write. Flat inputs and the 0 components
are exact. A smooth `w` is 1.0 interpolated, which is exact if the rasteriser computes
`(1/w)/(1/w)` exactly; this is the one value to check with a frame dump.

Kept on the linked path: tessellation, geometry (including layer emulation), mesh/task and
transform-feedback pipelines. Resolution scaling > 1x still couples stages through the
rescaling push-constant bit indices (`Bindings::texture_scaling_index`); GPL must either
stay off while scaling is active or give each stage a fixed base in the rescaling words.
GPL linking also needs `graphicsPipelineLibraryIndependentInterpolationDecoration` (true on
the RTX 4060 / 615.78.08), since the vertex outputs cannot know the fragment's `Flat`.

Offline check (Ocarina of Time cache, `vulkan.fork-v54.bin`, 600 vertex+fragment pipelines,
shader-sweep `--previous-environment` / `--fixed-interfaces`): 592 compile and pass spirv-val in
both modes (8 lack captured storage-pointer context) and all 592 fixed pairs build on the RTX
4060 (98.8 s total vs 96.0 s linked, same p99). Each of the 330 fragment shaders emitted after
6 random vertex shaders gave 1-6 distinct modules linked (only 13 had one) and exactly one fixed,
except one shader with legacy or indexed loads. 8 of the 592 cache pipelines read an unwritten
generic: 7 flat, one smooth `w`.

Step 3 (state to specialization constants) still has to move out of the stage SPIR-V:
vertex `generic_input_types`, `fixed_state_point_size`, `convert_depth_mode` and `y_negate`;
fragment alpha test, `dual_source_blend` and `force_early_z`; and the vertex stage's
`has_geometry` dependency (point size, depth mode) for pipelines with a geometry stage.
