# PASSOFF — forge-colorx

Handoff notes for picking this back up (ideally in a Claude session scoped to
`cnoellert/forge-colorx`, so changes can be committed/pushed directly).

## What this is

A re-creation of Nuke's **Color > Math > Expression** node for Autodesk Flame,
in two flavours that share identical expression semantics:

- **`matchbox/ColorExpression/`** — GPU Matchbox shader (Flame family).
- **`ofx/Expression/`** — OpenFX plugin (Flame 2021+, Nuke, Resolve/Fusion,
  Natron). This is the faithful clone: real text fields, parsed at run time.

Both use the same function library, the same `noise()`, and the same predefined
variables, so an expression behaves the same in either tool.

## Repo layout

```
forge-colorx/
├── README.md                      project overview + chooser table
├── PASSOFF.md                     this file
├── .github/workflows/ci.yml       tests + transpiler diff + per-OS bundle artifacts
├── tests/
│   ├── test_expr.cpp              36 evaluator unit tests
│   └── test_transpile.cpp         AST->kernel transpiler differential test (vs eval)
├── matchbox/ColorExpression/
│   ├── ColorExpression.glsl       #version 120; full Nuke function library;
│   │                              channel formulas in an EXPRESSION BLOCK
│   ├── ColorExpression.xml        UI sidecar (k1..k4, ref colour, mix, clamp)
│   └── README.md                  build/install + Nuke→Matchbox parity table
└── ofx/Expression/
    ├── Expression.cpp             OFX plugin (params, render, channel dispatch)
    ├── ExprEval.h                 Pratt parser + evaluator + emitC() transpiler
    ├── ExprKernel.h               exh_* helper prelude for transpiled kernels
    ├── Info.plist                 OFX bundle plist (required to load)
    ├── Makefile                   builds Expression.ofx.bundle
    └── README.md                  build/install + parity table + examples
```

Build artifacts (`openfx/` SDK clone, `*.ofx`, `*.o`, test binaries) are
`.gitignore`d. The OFX builds against the AcademySoftwareFoundation/openfx
Support tree (cloned on demand).

## Status / what was verified

- **Evaluator: 36/36 unit tests pass** (`tests/test_expr.cpp`) — operator
  precedence incl. `-2^2 == -4` and right-assoc `2^3^2`, ternary, short-circuit
  `&&`/`||`, clamp/min/max/lerp/smoothstep, trig, noise, error rejection.
- **OFX plugin compiles AND links** into a working `Expression.ofx` against the
  Academy Software Foundation OpenFX SDK (verified locally; CI reproduces it).
- **OFX plugin HOST-VERIFIED in DaVinci Resolve Studio 20.3 (macOS arm64).**
  Loaded, param panel populated, and **pixels confirmed correct end-to-end** by
  rendering a known flat input through `r+g / b-r / r*g+b` and reading the EXR:
  - at **32-bit float**: exact `0.6000000 / 0.4000000 / 0.6800000` (HDR-safe,
    unclamped — the float path is a straight `(PIX)v`),
  - at 8-bit it quantises as expected (`0.678 == 173/255`).
- **OFX plugin HOST-VERIFIED in Autodesk Flame 2026.2.2 (macOS arm64).** Loaded
  as an `OpenFX` Batch node (`change_plugin('Expression')` → our `r=/g=/b=/a=`
  tabs, no rejection by Flame's picky OFX host), ran without crashing, and a
  **typed `r = 0.5` rendered to an exact `0.500000`** with the other channels
  passing through — confirming typed-expression evaluation works in Flame. The
  `clearPersistentMessage` guard held here too (Flame's OFX host is also strict).
- **Cross-platform builds via CI.** Every push builds proper `.ofx.bundle`s and
  uploads them as artifacts: `Expression.ofx-linux-x86-64` (ELF x86-64, exports
  `OfxGetPlugin`, `Contents/Linux-x86-64/`) and `Expression.ofx-macos-arm64`.
  Linux is **binary-verified** (compiles/links/correct arch); pixel-verify on a
  real Linux Flame/Resolve is still pending (see below).
- **GPU render path — Phase 1 done (the transpiler).** `Program::emitC()` turns
  the compiled AST into a portable kernel body; `tests/test_transpile.cpp`
  compiles the emitted kernels and diffs them against `eval()` over 50
  expressions × 5000 random inputs → **worst error 2.22e-16, 0 failing**. This
  is CI-gated and needs no GPU. (Phases 2–4 below.)
- **Matchbox GLSL**: ASCII-clean, XML well-formed. Targets `#version 120` for
  Linux+macOS Flame. `shader_builder` (`/opt/Autodesk/flame_2027/bin`) accepts it
  (exit 0) but a true GPU compile (load as a Matchbox node in Flame) is still
  unverified.

### Fixes made during host verification (see git log)

These were required to actually run in Resolve and are now in the source:
- **Added `Info.plist`** to the OFX bundle (was missing — bundle wouldn't load).
- **Fixed the `Makefile`**: `PLUGINOBJECTS` must list only `Expression.o`; the
  OFX Support objects come from `Makefile.master`'s `SUPPORTOBJECTS`.
- **Guarded the OFX message suite.** Resolve's Fusion OFX host does **not**
  implement it, so an unconditional `clearPersistentMessage()` threw
  `kOfxStatErrUnsupported` on every render → black frame + "render failed". Now
  wrapped in try/catch (also the error-path `setPersistentMessage` calls).
- **`getRegionOfDefinition()` guarded** with a render-window fallback (defensive).
- **16-bit float (half) support added** to the render dispatch (`eBitDepthHalf`),
  since Fusion is half-native in some paths. Handles byte/short/half/float.

### NOT yet verified (next person / next session)

- **Linux pixel-verification.** The Linux x86-64 bundle builds on CI but hasn't
  been *loaded* in a real Linux Flame/Resolve. The user HAS a Linux Flame/Resolve
  workstation (NVIDIA) — that's the test bed. The Resolve scripting harness ports
  over with Linux paths: `RESOLVE_SCRIPT_API=/opt/resolve/Developer/Scripting`,
  `RESOLVE_SCRIPT_LIB=/opt/resolve/libs/Fusion/fusionscript.so`.
- **Matchbox node loaded in Flame** (GPU compile) — `shader_builder` accepts it
  but it hasn't been loaded as a Matchbox node in Flame.
- **Universal macOS build** (arm64 + x86_64) for Intel-Mac — `Makefile.master`
  has a Universal mode.
- **Host caveat (both Resolve & Flame):** neither host populates the OFX
  describe-time string defaults into the editable fields, and neither exposes the
  string params to its scripting API. This is a host quirk, **not** a plugin bug
  — typed-in-UI expressions evaluate correctly (verified: Flame `r=0.5` → exact
  `0.5`). Automated pixel tests therefore drive expressions via param *defaults*.

### Open loose ends (small)

- A throwaway Flame batch group **`FCX_OFX_TEST`** was left on the portofino
  desktop (Colour Source → OpenFX → Write File). Safe to delete.
- `/Library/OFX/Plugins/Expression.ofx.bundle` currently holds the **diagnostic**
  build (test defaults `r+g…`, tries to log to `~/expr_debug.log`). Swap in the
  clean build before real use: `make -C openfx/Support/Plugins/Expression
  DEBUGFLAG=-O2` then `sudo cp -R …/Darwin-64-debug/Expression.ofx.bundle
  /Library/OFX/Plugins/` and relaunch the host. (`rm ~/expr_debug.log` too.)

## How to build

**OFX**
```bash
git clone https://github.com/AcademySoftwareFoundation/openfx
cp -r ofx/Expression openfx/Support/Plugins/Expression
cd openfx/Support/Plugins/Expression && make
# -> Expression.ofx.bundle/Contents/<arch>/Expression.ofx  → /usr/OFX/Plugins
```

**Matchbox**
```bash
shader_builder -m matchbox/ColorExpression/ColorExpression.glsl
# then load ColorExpression as a Matchbox node in Flame
```

macOS note: build with `make DEBUGFLAG=-O2` (optimised, and keeps
`DEBUGNAME=debug` so the release-only `osxDeploy.sh` step is skipped). Resolve/
Flame scan `/Library/OFX/Plugins` on macOS; **a host only re-reads OFX at launch**
(replacing the file under a running host does nothing — full relaunch needed).

**Run the tests** (from repo root)
```bash
g++ -std=c++11 -O2 -I ofx/Expression tests/test_expr.cpp      -o /tmp/te && /tmp/te
c++ -std=c++11 -O2 -I ofx/Expression tests/test_transpile.cpp -o /tmp/tt && /tmp/tt
```

## GPU render path (in progress)

Goal: real-time GPU render for the OFX. **CUDA on Linux, Metal on Mac** (both via
the OFX GPU render suites in `ofxGPURender.h`); Flame's GPU path is its own
Matchbox GLSL (Flame's OFX host is CPU-leaning and likely won't do OFX-GPU).

Design: **one codegen, three thin shims.** `Program::emitC()` emits a portable
kernel *body* over a `v[]` array calling `exh_*` helpers. Each back-end supplies
a prelude exposing the same `exh_*` names in its language, plus a wrapper. The
CPU evaluator is the **numeric oracle** for every target.

- **Phase 1 ✅ (done):** `emitC()` + `ExprKernel.h` (CPU prelude) +
  `test_transpile.cpp` differential test (2.22e-16, CI-gated).
- **Phase 2 ⏭ (next — Metal, local on this Mac):**
  1. `ExprKernelMetal.h` — `exh_*` in Metal Shading Language (port `ExprKernel.h`;
     `float`, `metal::` builtins). 2. Emit a `kernel void` wrapper around the
     `emitC()` body (varies/uniforms → threadgroup buffer). 3. In `Expression.cpp`,
     detect `kOfxImageEffectPropMetalEnabled`/`args` Metal command queue, compile
     MSL at render via `newLibraryWithSource` (cache by expr string), dispatch.
     4. Verify in Resolve/Mac here, EXR-diff vs the CPU path. Keep CPU fallback.
- **Phase 3 (CUDA):** `ExprKernelCuda.h`; NVRTC compile; add an `nvcc/nvrtc`
  *compile-check* CI job (no GPU needed); runtime parity on the user's Linux box.
- **Phase 4:** wire GPU into `render()` with CPU fallback; close GLSL/Matchbox.

OFX GPU refs: `openfx/include/ofxGPURender.h`, and the OpenFX/Support GPU bits.
Resolve supports OFX Metal (Mac) + CUDA (Linux); test there with the harness in
[[resolve-ofx-verification]] memory.

## Key design decisions (so they aren't re-litigated)

- **Matchbox can't be a literal clone.** A Matchbox UI has no text-entry widget
  and GLSL is compiled, so you can't type an arbitrary expression and have it
  evaluate live. The Matchbox build instead reproduces Nuke's whole function
  library in GLSL and puts the four channel formulas in an editable source
  block (recompile to apply), with `k1..k4`/`ref` knobs for live tweaks. The
  **OFX** build is where typed-at-runtime expressions actually happen.
- **`noise()` is a smooth signed value-noise approximation** (centred near 0),
  deliberately identical between the two builds for cross-tool parity. It is not
  bit-exact to Nuke's Perlin. If exactness matters, swap in a real Perlin/Simplex
  in both `ExprEval.h` (`ev_noise`) and `ColorExpression.glsl` (`noise`).
- **Coordinate parity with Nuke:** origin bottom-left for `x,y`; `cx,cy` are
  centred and aspect-preserved (divide both by half-width).
- **Channel normalisation (OFX):** byte/short clips are scaled to 0..1 in and
  written back at depth (clamped); float passes through unclamped (HDR-safe).
- **`int()` → `trunc()`, `clamp(x)` 1-arg → clamp to [0,1], `e` → `e()`** — see
  the per-build READMEs for the full deviation list.

## Suggested next steps (in priority order)

1. **GPU Phase 2 — Metal back-end** (see the GPU section above). The immediate
   focus. Build/verify locally in Resolve on this Mac.
2. **GPU Phase 3 — CUDA**, with runtime parity on the user's Linux workstation.
3. Linux pixel-verification of the OFX in a real Linux Flame/Resolve.
4. Load/verify the Matchbox node in Flame (GPU compile).
5. Nice-to-haves: user-constant params (`k1..k4`/`ref`) in the OFX UI; real
   Perlin (both builds); universal macOS build; tag **v1.0**.

## Session history (newest first)

- GPU Phase 1: AST→kernel transpiler + differential oracle test (CI-gated).
- CI: per-OS `.ofx.bundle` artifacts (linux-x86-64 + macos-arm64).
- OFX host-verified in Flame 2026.2.2 (typed `r=0.5` → exact 0.5).
- OFX host-verified in Resolve Studio 20.3; fixed the message-suite crash
  (`clearPersistentMessage`), added `Info.plist`, half-float, Makefile fix.
- Flattened the repo to the git root; dropped the snapshot tarball.

## Continuing with Claude

This session is scoped to `cnoellert/forge-colorx` and commits/pushes directly to
`main`. There are **memory files** under the project's memory dir worth reading:
`resolve-launch-control`, `resolve-ofx-verification`, `flame-ofx-bridge` (host
quirks + how to drive/verify the plugin). The live Flame bridge is the
`forge-core` MCP (`flame_ping`, `flame_execute_python`). Good first ask to resume:
**"continue GPU Phase 2 — the Metal back-end."**
