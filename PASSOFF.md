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
├── .github/workflows/ci.yml       runs unit tests + compiles the .ofx on push
├── tests/test_expr.cpp            36 evaluator unit tests
├── matchbox/ColorExpression/
│   ├── ColorExpression.glsl       #version 120; full Nuke function library;
│   │                              channel formulas in an EXPRESSION BLOCK
│   ├── ColorExpression.xml        UI sidecar (k1..k4, ref colour, mix, clamp)
│   └── README.md                  build/install + Nuke→Matchbox parity table
└── ofx/Expression/
    ├── Expression.cpp             OFX plugin (params, render, channel dispatch)
    ├── ExprEval.h                 self-contained Pratt parser + evaluator
    ├── Makefile                   builds Expression.ofx.bundle
    └── README.md                  build/install + parity table + examples
```

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
- **Matchbox GLSL**: ASCII-clean, XML well-formed. Targets `#version 120` for
  Linux+macOS Flame.

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

- **Flame specifically.** Verified in Resolve (OFX); not yet loaded in Flame
  itself (2021+ OFX, or the Matchbox build). Cross-platform builds still TODO:
  **x86_64 Linux + Flame on both platforms** is the eventual target.
- Matchbox `shader_builder -m ColorExpression.glsl` has not been run on a Flame
  box (no `shader_builder` available where this was authored).
- **Test-harness caveat:** Fusion's *scripting* `SetInput()` does not push OFX
  string-param values into the plugin instance (the plugin reads describe-time
  defaults). This is a Fusion scripting-bridge quirk, **not** a plugin bug —
  typed-in-UI params use the standard `getValueAtTime` path. Automated pixel
  tests therefore drive the expressions via the param *defaults*.

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

**Run the unit tests**
```bash
g++ -std=c++11 -O2 -I ofx/Expression tests/test_expr.cpp -o /tmp/test_expr && /tmp/test_expr
```

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

## Suggested next steps

1. Load both in a host and verify visually (priority).
2. Decide on exact-Perlin vs the current approximation.
3. Optional: GLSL-transpile the OFX AST for GPU-speed OFX render at 4K.
4. Optional: add a colour-knob / user-constant params to the OFX UI to mirror
   the Matchbox `k1..k4`/`ref` for animation without retyping.
5. Tag a v1.0 once host-verified.

## To continue with Claude in the right repo

Start a new Claude Code session scoped to `cnoellert/forge-colorx`, then it can
read/commit/push here directly (this prior session was scoped to a different
repo and could only hand off files). Good first asks: "verify the OFX plugin in
a host", "swap noise() for exact Perlin in both builds", or "add user-constant
params to the OFX UI".
