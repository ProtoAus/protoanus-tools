# protoanus-tools — changes vs upstream ericw-tools

Fork of ericw-tools (2.0.0-alpha) — <https://github.com/ProtoAus/protoanus-tools>. GPLv2+.
This file tracks what the fork changes and why, so it can double as release notes.

---

## Implemented

### fix: argument errors now exit non-zero (was exit 0)

**Problem.** `qbsp`, `vis`, and `light` printed their usage/help text and exited with status **0** whenever
an unknown/invalid option or the wrong number of operands was given. Build front-ends treat 0 as success —
so, e.g., a TrenchBroom compile profile with a malformed `light` flag would print help, "succeed", and
silently produce a map with **no lighting** (the failure was invisible). This bit real users.

**Cause.** A parse error throws `settings::parse_exception`, but each tool's `initialize()` caught it and
re-threw `settings::quit_after_help_exception` — the *same* exception `-help` uses — which `main.cc` maps
to `return 0`. The "wrong operand count" path likewise called `print_help(true)` (→ `quit_after_help`).

**Fix.** Added a distinct `settings::quit_after_error_exception` (`include/common/settings.hh`). Both error
paths in each tool now throw it after printing help + the error text, and each `main.cc` catches it and
returns **1**. `-help`/`-h`/`-?` still returns 0.

**Files:** `include/common/settings.hh`; `qbsp/qbsp.cc` + `qbsp/main.cc`; `light/light.cc` + `light/main.cc`;
`vis/vis.cc` + `vis/main.cc`. (`bsputil`/`maputil` call `print_help(true)` directly and could get the same
treatment later.)

**Verify:** `light -notaflag foo.bsp; echo $?` → **1**; `qbsp` with no map arg → **1**; `light -help; echo $?`
→ **0**. **Note for upstream tests:** any test that exercised these error paths and caught
`quit_after_help_exception` should now expect `quit_after_error_exception`.

### feat: OIDN lightmap denoising (`-denoise`)

Optional lightmap denoising via **Intel Open Image Denoise**. Upstream has no denoiser (only `-extra`
supersampling + `-soft` box blur), so clean GI/AO/soft-shadow results need many samples. `-denoise` runs
OIDN's `RTLightmap` filter over each face's oversampled HDR float lightmap (after flood-fill, before
downsample), so low sample counts come out clean.

- **Build:** optional — `find_package(OpenImageDenoise 2 QUIET)`; if found, `liblight` links it and gets
  `-DHAVE_OIDN`, else `-denoise` is a graceful no-op (build never breaks on its absence). Same pattern as
  the existing optional Embree.
- **Runtime gotcha (fixed):** OIDN 2.x loads its CPU **device module** dynamically, looking next to the
  loaded core lib for the *un-prefixed* name `OpenImageDenoise_device_cpu.dll`. MSYS2/mingw builds it as
  `libOpenImageDenoise_device_cpu.dll`, so it was never found ("unsupported device type: CPU"). The
  `light` POST_BUILD now copies OIDN's core + device dlls next to `light.exe`, stripping the `lib` prefix
  from device modules.
- **Files:** `light/denoise.{cc,hh}` (new; OIDN wrapper, thread-safe via a shared device + mutex, small
  faces skipped), `light/write.cc` (hook in `WriteSingleLightmap`), `include/light/light.hh` + `light/light.cc`
  (the `-denoise` setting), `light/CMakeLists.txt` (find/link/define + runtime copy).
- **Verified:** OIDN device loads (detects the CPU), and a bake with `-denoise` vs without produces a
  different (denoised) lightmap. Degrades gracefully when built/run without OIDN.
- **Known limitation / future work:** denoise is currently **per-face** and serialized with a mutex; a
  global lightmap-atlas denoise (reusing `common/bspinfo.cc build_lightmap_atlas`) would give better quality
  on small faces and run once instead of per-face. Albedo/normal AOV guide buffers (from the texture-average
  cache + deluxe normals) are also a future refinement.

### feat: external mesh occluders — `_light_mesh` (Source `-StaticPropPolys` analogue)

`light` can now load real prop meshes and add their triangles to the Embree scene as shadow casters, so
props throw **accurate mesh-shaped shadows** instead of collision-hull/brush-proxy approximations. Upstream
could only import `.map` *brush* prefabs (`misc_external_map`); there was no mesh path at all.

- **Usage — existing props (recommended for many props):** add `-propshadowclasses "prop_static prop_detail
  ..."` to the `light` command (or set the `_propshadowclasses` worldspawn key). `light` then reads every
  entity of those classnames and casts its `model` as a shadow, using the entity's own `model`/`origin`/
  `angles`/`scale` keys — **no new entities and no per-prop edits**. (Verified: notnormals' 246 `prop_static`/
  detail/dynamic entities → 217 382 triangles of real prop mesh cast into the lightmap.)
- **Usage — one-offs:** a dedicated `_light_mesh` point entity (keys `model`, `origin`, `angles` = pitch yaw
  roll like `misc_external_map`, optional `_scale`) always casts, regardless of `-propshadowclasses`.
- Model paths (`.obj`/`.iqm`) resolve through the compiler's file search (`-path`).
- **Alignment caveat:** rotation uses the standard `angles` (pitch yaw roll) convention; engines that render
  meshes with a flipped pitch (e.g. FTE `r_meshpitch -1`) may see pitched/rolled props' shadows misaligned.
  Yaw-only props are unaffected.
- **Loaders:** minimal in-repo readers, no new dependency — Wavefront **`.obj`** (positions + faces,
  fan-triangulated) and **IQM v2** (`INTERQUAKEMODEL`, base-frame `IQM_POSITION` float3 verts + triangle
  list). Dispatched by extension.
- **How it hooks in:** `GatherMeshOccluders` (in `light/model_import.cc`) collects world-space triangles as
  `winding3f_t`s; `Embree_TraceInit` (`light/trace_embree.cc`) adds them via `CreateGeometryFromWindings`,
  the *same* unconditional-occluder path already used for skip-brush shadows (no intersect filter attached →
  they always occlude).
- **Files:** `light/model_import.{cc,hh}` (new), `light/trace_embree.cc` (hook), `light/CMakeLists.txt`.
- **Verified:** a `.obj` cube (12 tris) and a real nettest `.iqm` crate (2 796 tris) each load and cast a
  shadow that provably changes the floor lightmap (and vanishes when the mesh file is removed).
- **Future work:** alpha-tested ("fence") shadows for foliage would need the mesh added to the *filter*
  geometry with per-triangle texture/alpha info (like Q1 `{`-textures), rather than as an unconditional
  occluder; and a `.md3` loader.

---

## Investigated — no change needed

### `-lightgrid_dist` already works (it is not clamped)

`lightgrid_dist` is a `setting_vec3` (`include/light/light.hh`); the `{32,32,32}` in its constructor
(`light/light.cc`) is the **default vector**, not `(default,min,max)`. It is consumed directly
(`light/lightgrid.cc`: `data.grid_dist = light_options.lightgrid_dist.value()`), with no clamp — so
`-lightgrid_dist 16 16 16` (finer, smoother model lighting; bigger/slower lump) or `48 48 48` (coarser,
faster) already take effect. No fix required.

---

## Roadmap (planned — design notes, not yet implemented)

These are the reasons the fork exists; captured here with concrete hooks so implementation is
paint-by-numbers once a build environment (CMake + Embree 4 + oneTBB, and OIDN for the first item) is set up.
Every optional dependency should follow upstream's `find_package … if(FOUND) add_definitions(-DHAVE_X)`
pattern (see `light/CMakeLists.txt`), so a missing dep just disables the feature and never breaks the build.

*(OIDN denoising has moved to the Implemented section above.)*

*(External mesh occluders have moved to the Implemented section above.)*

### 1. Static-prop per-vertex lighting  (Source `-StaticPropLighting` analogue)

Pairs with #2. After the world bake, sample lighting at each placed prop's mesh vertices and emit
per-vertex RGB (new BSPX lump or sidecar, documented format) so a prop's sunlit top stays bright while its
underside darkens — fixing the "whole prop reads one dark floor sample" problem. Engine-side consumption
(FTEQW: IQM vertex color + `rgbgen exactvertex`) is a separate, documented follow-up.

- **New files:** `light/staticprop_lighting.cc` + a format doc under `docs/`.

**Build status note:** Stages 1–3 above beyond the implemented fix require a working CMake + Embree 4 +
oneTBB (+ OIDN) toolchain; the git submodules (`fmt`, `jsoncpp`, `pareto`, `nanobench`) must be initialised
(`git submodule update --init --recursive`) before configuring.
