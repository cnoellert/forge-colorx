# Expression — an OpenFX port of Nuke's Expression node

A genuine re-creation of Nuke's **Color > Math > Expression** node as an OpenFX
plugin. Unlike a Matchbox shader, an OFX host gives you real **text fields**, so
you type per-channel expressions and they are parsed and evaluated **at run
time** — the actual Nuke behaviour. The built `.ofx` loads in **Autodesk Flame
(2021+)**, Nuke, DaVinci Resolve/Fusion, Natron, and any other OpenFX host.

```
Expression/
├── Expression.cpp   the OFX plugin (params, render, channel dispatch)
├── ExprEval.h       self-contained expression compiler/evaluator (no deps)
├── Makefile         build to Expression.ofx.bundle
└── README.md        this file
```

## Why OFX instead of Matchbox

| | Matchbox | OFX (this) |
|---|---|---|
| Per-channel **typed** expressions, live | No (compiled GLSL, no text field) | **Yes** |
| Runs on | Flame family | Flame + every OFX host |
| Execution | GPU | CPU (multithreaded) |
| Install | drop files | build per platform, drop `.ofx.bundle` |

The Matchbox version (separate deliverable) stays useful when you want GPU speed
and don't need to retype expressions; this OFX version is the faithful clone.

## Build

The plugin uses the standard OpenFX C++ **Support** library. The simplest route
reuses the openfx repo's build rules:

```bash
git clone https://github.com/AcademySoftwareFoundation/openfx
cp -r Expression openfx/Support/Plugins/Expression
cd openfx/Support/Plugins/Expression
make
# Linux  -> Expression.ofx.bundle/Contents/Linux-x86-64/Expression.ofx
# macOS  -> Expression.ofx.bundle/Contents/MacOS-x86-64/Expression.ofx
```

Requirements: a C++11 compiler and `make`. No third-party libraries — the
expression engine (`ExprEval.h`) is self-contained.

### Install

Copy `Expression.ofx.bundle` to the OFX plugin path:

- **Linux:** `/usr/OFX/Plugins/`
- **macOS:** `/Library/OFX/Plugins/`

or set `OFX_PLUGIN_PATH` to a folder containing it. Flame picks up OFX plugins
from there; the node appears under **Color/Math > Expression**.

## Using it

The UI is split into four pages:

- **Channels** — the four output expressions `r =` `g =` `b =` `a =`, one box each.
- **Variables** — ONE multi-line block of **`name = formula`** statements (separate
  with `;` or a newline), a value **derived by a formula** (luminance, a vignette
  falloff, …) reusable in the channels. Evaluated in order before the channels. One
  self-labelling box (each line says what it is) rather than separate name/formula
  fields, so it stays readable where Flame won't render text-field labels.
- **Constants** — four animatable knobs `k1..k4`, each with an optional **`kN name`**
  alias box, plus the **Reference Colour**. The slider drives the value; the name is
  a friendly token. Name `k1` "gamma" and both `gamma` and `k1` resolve to that
  slider. (These stay as separate boxes because the labelled `k` slider anchors the
  alias box next to it.)
- **Output** — **Mix** (blend original↔result) and **Clamp Output**.

Two kinds of named token: a **Variable** is *computed* (name + formula, in the block),
a **Constant** is *dialed* (name + slider). Both are usable anywhere in the channels.

### Predefined variables

| Variable         | Meaning                                                   |
|------------------|-----------------------------------------------------------|
| `r g b a`        | Input channels, normalised 0..1                           |
| `x y`            | Pixel coordinates, origin bottom-left (as in Nuke)        |
| `cx cy`          | Centred, aspect-preserved coords: `(0,0)` centre, `±1` L/R |
| `width height`   | Image size (region of definition)                         |
| `frame` / `t`    | Current frame (render time); `t` is a Nuke-parity alias    |
| `pi e`           | Constants (also callable as `pi()` `e()`)                 |
| `k1 k2 k3 k4`    | Constant knobs (Constants page; `k1` defaults to 1, rest 0) |
| *your `kN` aliases* | Whatever you name a constant knob (e.g. `gamma`)         |
| `ref.r ref.g ref.b` | The Reference Colour knob (RGB), default `0`            |
| *your variables* | Whatever you name in the Variables rows                    |

The `k1..k4`/aliases, `ref`, **Mix** and **Clamp Output** mirror the companion
Matchbox build, so the OFX is a superset: type any expression *and* drive it live
with keyframable, nameable constants. **Mix** blends the original image (0) with the
expression result (1); **Clamp Output** clamps the result to `0..1` (applied before
Mix, matching the Matchbox).

For byte/short clips the channels are normalised to 0..1 on the way in and
written back at the clip's bit depth (clamped). Float clips pass through
unclamped (HDR-safe).

### Operators

`+ - * / %`  `^` (power, right-assoc)  `< <= > >= == !=`  `&& ||`  `!`  `?:`
(ternary). Precedence follows C, with `^` binding tighter than unary minus
(`-2^2 == -4`).

### Function library (matches Nuke)

`sin cos tan asin acos atan(x) atan(y,x)/atan2 sinh cosh tanh exp log log10 log2
sqrt abs/fabs floor ceil trunc round rint sign radians degrees pow pow2 fmod
hypot ldexp exponent mantissa min(...) max(...) clamp(x) clamp(x,a,b) lerp/mix
smoothstep(a,b,x) step(edge,x) noise(x[,y[,z]]) random(x[,y[,z]])
fBm(x,y,z,oct,lac,gain) turbulence(x,y,z,oct,lac,gain) from_sRGB to_sRGB
from_rec709 to_rec709 from_byte to_byte`

Notes / deliberate parity choices:

- `int(x)` is accepted as an alias of `trunc(x)`.
- `clamp(x)` (1-arg) clamps to `[0,1]`; `clamp(x,a,b)` is the 3-arg form.
- `min`/`max` are variadic (`max(r,g,b)` works).
- `fmod` is C-style (sign of the dividend).
- `noise()` is **classic Perlin** gradient noise (Gustavson/Ashima, tableless),
  signed ~`[-1,1]`, matching the companion Matchbox shader for cross-tool
  consistency. Computed in float with a computable permutation (no table) so it is
  parity-clean across the CPU/CUDA/Metal back-ends; `random()` likewise uses a
  tableless permute hash (cell-based — feed pixel coords for per-pixel values).
  Not a bit-exact match to Nuke's own Perlin permutation.
- An empty channel field evaluates to `0` (as in Nuke). The defaults are the
  identity (`r`/`g`/`b`/`a`), i.e. a pass-through.
- A syntax error is reported on the node (persistent error message) naming the
  offending channel/temp and the parse error.

### Example expressions

| Goal | r | g | b | a |
|---|---|---|---|---|
| Pass-through | `r` | `g` | `b` | `a` |
| Invert RGB | `1-r` | `1-g` | `1-b` | `a` |
| Gamma 2.2 | `pow(r,1/2.2)` | `pow(g,1/2.2)` | `pow(b,1/2.2)` | `a` |
| Luma (mono) | `0.2126*r+0.7152*g+0.0722*b` | (same) | (same) | `a` |
| Horizontal ramp | `x/width` | `g` | `b` | `a` |
| Radial vignette | `r*clamp(1-hypot(cx,cy))` | `g*clamp(1-hypot(cx,cy))` | `b*clamp(1-hypot(cx,cy))` | `a` |
| Threshold matte | `r` | `g` | `b` | `r>0.5?1:0` |

Vignette tidied with a variable — in the Variables box put `v = clamp(1 - hypot(cx,cy))`,
then `r = r*v`, `g = g*v`, `b = b*v`.

**More presets:** see [`PRESETS.md`](../../PRESETS.md) at the repo root — a validated,
copy-paste gallery (UV pass, rainbow/cosine palettes, checkerboard, plasma, Perlin
clouds/marble, duotone, posterize, film grain, scanlines, …) with suggested knob
values.

## Performance

Evaluation is CPU, multithreaded across scanlines via the OFX MultiThread suite,
and each expression is compiled to an AST once per render rather than re-parsed
per pixel. That's plenty for interactive grading-style use. If you later need 4K
real-time, the path would be to transpile the AST to GLSL — but that's an
optimization, not required for a faithful clone.
