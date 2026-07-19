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
- **Whole-lightmap-atlas denoise (replaces the old per-face pass).** Denoising each face in isolation made
  OIDN re-estimate a different exposure per face and edge-darken each one, tiling into a faint world-aligned
  grid (a per-cell vignette) under `r_lightmap`. Now, after every face is finished (`SaveLightmapSurfaces`,
  before the byte write), `DenoiseLightmapAtlas` (`light/write.cc`) packs **every** face's float lightmap —
  per lightstyle — into ONE big atlas (height-sorted shelf pack, mirroring `common/bspinfo.cc`'s packer) with
  a replicated-border **gutter** around each face, runs OIDN **once** over the whole atlas (a single
  consistent exposure + real cross-face context), and scatters the denoised RGB back into
  `light_surfaces[].lightmapsByStyle[].samples[].color` (non-occluded luxels only; the per-face flood-fill
  re-fills the rest). The gutter (≥ OIDN's kernel) keeps unrelated faces from bleeding across boundaries.
  Verified: notnormals' 24 524 faces pack into one 4901×4486 atlas denoised in a single call (no grid,
  ~350 MB peak, faster than 24k per-face calls). **Future refinement:** albedo/normal AOV guide buffers
  (per-face texture-average + per-luxel `samples[].normal`) for sharper edge preservation.

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

### feat: static-prop per-vertex lighting — `-propvertexlight` (Source `-StaticPropLighting` analogue)

Bakes the finished world lighting **at each vertex** of every placed IQM prop and writes it, **per placement**,
to a new `RGBPROPLIGHT` BSPX lump. This fixes the classic "the whole prop reads one dark floor sample" look:
an engine can light a prop's sunlit top brightly and darken its shadowed underside, per instance.

- **Usage:** add `-propvertexlight` to the `light` command. It reuses the **same classnames** as
  `-propshadowclasses` (plus the `_light_mesh` entity), so one keyword set drives both baked shadows and baked
  vertex lighting, and they stay consistent. **IQM only** (the engine applies the colors back by vertex index).
- **How it works:** after the world lighting + lightgrid are finalised (and before the BSP is written), for
  every matched prop each IQM vertex is transformed to world space (same `origin`/`angles`/`scale` placement
  math as the occluders) and the lighting there is sampled with `FixPointAndCalcLightgrid` (which nudges
  vertices that sit just inside a surface back onto it). The omnidirectional result is stored as an absolute
  0–255 RGB in global IQM vertex order. The per-vertex sampling is **multithreaded** (`logging::parallel_for`,
  like the lightgrid pass) with a `PropVertexLighting` progress bar — a full per-point light calc over every
  prop vertex is not cheap on sun-heavy maps (≈50 s for ~300k verts with 66 suns + bounce), so it shows an ETA
  instead of appearing to hang. Drop `-propvertexlight` for fast iteration compiles.
- **`RGBPROPLIGHT` lump format** (little-endian): `uint32 version(=1)`, `uint32 record_count`, then per record
  `float origin[3]`, `float angles[3]`, `uint16 namelen`, `char name[namelen]` (the entity `model`),
  `uint32 vertexcount`, `uint8 rgb[vertexcount][3]`. One record per placement (no dedup — each instance is
  unique). ~3 bytes/vertex, e.g. 246 props / ~299k verts ≈ 0.9 MB.
- **Engine side (separate):** the colours are stored **absolute**; a consumer normalises them to a ~1.0-centred
  multiplier and multiplies over the model's live (PBR) lighting, so per-pixel/normalmap lighting is preserved
  and only *redistributed* across the mesh. For FTEQW this is IQM vertex colours fed through a `VC` shader
  permutation (a small engine change, tracked separately).
- **Files:** `light/model_import.{cc,hh}` (`LoadIqmVerts` + `GatherPropVertexLights`), `light/light.cc`
  (`WritePropVertexLights` + call site after `LightGrid`), `include/light/light.hh` + `light/light.cc`
  (the `-propvertexlight` setting).
- **Verified:** a direct-only bake of nettest `notnormals.bsp` produced `RGBPROPLIGHT` with 246 records /
  298 683 vertices (912 733 bytes), the lump round-trips byte-exact, per-vertex colours vary (bright tops vs.
  black undersides), and the existing `DECOUPLED_LM`/lightgrid lumps are untouched.
- **Alignment caveat:** identical to the occluder feature — the placement rotation uses standard `angles`
  (pitch,yaw,roll); engines that render meshes with a flipped pitch (FTE `r_meshpitch -1`) may mismatch the
  *sample positions* of a pitched/rolled prop. Yaw-only props are exact. Colours are always emitted in model
  vertex order, so the colour↔vertex mapping never depends on the transform.

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

Every optional dependency should follow upstream's `find_package … if(FOUND) add_definitions(-DHAVE_X)`
pattern (see `light/CMakeLists.txt`), so a missing dep just disables the feature and never breaks the build.

*(OIDN denoising (per-face AND whole-atlas), external mesh occluders, and static-prop per-vertex lighting
have all moved to the Implemented section above.)*

### 1. Denoise AOV guide buffers

The whole-atlas denoise works without them, but feeding OIDN albedo (per-face texture-average) + normal
(per-luxel `samples[].normal`) guide images would let it preserve real material/geometry edges more sharply
(and reduce reliance on the gutter width).

### 2. Alpha-tested ("fence") mesh shadows + `.md3`

Foliage/grate occluders need the mesh on the Embree *filter* geometry with per-triangle alpha (like Q1
`{`-textures) instead of the current unconditional-occluder slot; plus a `.md3` mesh loader.
