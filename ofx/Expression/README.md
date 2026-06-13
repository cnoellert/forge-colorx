# colorx — an OpenFX port of Nuke's Expression node

A genuine re-creation of Nuke's **Color > Math > Expression** node as an OpenFX
plugin. An OFX host gives you real **text fields**, so you type per-channel
expressions and they are parsed and evaluated **at run time** — the actual Nuke
behaviour. The built `.ofx` loads in **Autodesk Flame (2021+)**, Nuke, DaVinci
Resolve/Fusion, Natron, and any other OpenFX host.

📖 **Online reference:** <https://cnoellert.github.io/forge-colorx/> — the variables,
operators, function library, and preset gallery on one browsable page (also reachable
from the node's **Expression syntax** button).

```
Expression/
├── Expression.cpp   the OFX plugin (params, render, channel dispatch)
├── ExprEval.h       self-contained expression compiler/evaluator (no deps)
├── Makefile         build to colorx.ofx.bundle
└── README.md        this file
```

## Build

> **Just want to use it?** Grab a prebuilt macOS/Linux bundle from the
> [latest release](https://github.com/cnoellert/forge-colorx/releases/latest) and
> jump to [Install](#install). Build from source only if you need a platform the
> release doesn't cover.

The plugin uses the standard OpenFX C++ **Support** library. The simplest route
reuses the openfx repo's build rules:

```bash
git clone https://github.com/AcademySoftwareFoundation/openfx
cp -r Expression openfx/Support/Plugins/Expression
cd openfx/Support/Plugins/Expression
make
# Linux  -> colorx.ofx.bundle/Contents/Linux-x86-64/colorx.ofx
# macOS  -> colorx.ofx.bundle/Contents/MacOS/colorx.ofx
```

Requirements: a C++11 compiler and `make`. No third-party libraries — the
expression engine (`ExprEval.h`) is self-contained.

## Install

However you got it — a release download or your own build — you end up with an
`colorx.ofx.bundle`. Install it into the host's OFX plugin path:

1. **Copy the bundle** to the OFX plugin path (both are root-owned, so use `sudo`):
   - **macOS:** `/Library/OFX/Plugins/`
   - **Linux:** `/usr/OFX/Plugins/`

   (Or point the `OFX_PLUGIN_PATH` env var at a folder that contains the bundle.)

2. **macOS only — clear the download quarantine**, or the host silently skips an
   unsigned bundle it didn't build itself:

   ```bash
   sudo xattr -dr com.apple.quarantine "/Library/OFX/Plugins/colorx.ofx.bundle"
   ```

3. **Restart the host application** — OFX plugins are scanned once at launch.

The node then appears under **FORGE > color > colorx** (labelled **colorx**).

> **Upgrading from the old "Expression" node?** This release renames the node to
> **colorx** (menu label + grouping only) — the plugin **identifier is unchanged**, so
> it's a **drop-in** update: existing comps/Batch setups keep working, no re-linking.
> Just **replace** the old `Expression.ofx.bundle` with `colorx.ofx.bundle` in the OFX
> plugin path. Don't leave both installed — they share one identifier, so a host would
> see a duplicate.

On **Linux**, the release ships two bundles: a **RHEL / Flame** build (needs only
glibc 2.29, loads on Flame's RHEL workstations) and a **modern** build (Ubuntu,
newer glibc, for recent Resolve etc.). On a RHEL-based Flame box, use the RHEL one.

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
- **Output** — **Mix** (blend original↔result), **Clamp Output**, the **Preset**
  pulldown (a one-pick effect gallery — see [Presets](#presets) below), and an
  **Expression syntax** button that opens the [online reference](https://cnoellert.github.io/forge-colorx/)
  in your browser.

Two kinds of named token: a **Variable** is *computed* (name + formula, in the block),
a **Constant** is *dialed* (name + slider). Both are usable anywhere in the channels.

### Presets

At the foot of the **Output** page is a **Preset** pulldown — a one-pick effect
gallery. Choosing an entry stamps a whole effect into the node at once: the four
channel expressions, the Variables block, **and** the suggested `k1..k4` knob values
(e.g. **Checkerboard** sets `k1 = 32`; **Gamma** sets `k1 = 2.2`). The entries mirror
[`PRESETS.md`](../../PRESETS.md) — UV pass, cosine-palette rainbow, checkerboard,
plasma, Perlin clouds/marble, duotone, posterize, film grain, scanlines, and more.

It's a starting point, not a mode. Once a preset is loaded every field stays fully
editable, and the knobs are meant to be dialed live (that's why each preset documents
what its `kN` control). Hand-edit any channel or the Variables block and the pulldown
snaps back to **(Custom)** — an honest signal that you've diverged from the preset.
Tweaking a knob does *not* snap it: dialing `k1` is *using* the preset, not leaving it.

Verified live in **Flame 2026.2.2** (Apple Silicon) and **DaVinci Resolve 20.3.1.6**:
picking a preset rewrites the channel/vars/knob fields in both hosts.

**Adding a preset:** add one row to the `kPresets[]` table in `Expression.cpp` — the
pulldown label, the four channel strings, the Variables string (`""` to clear it), and
`setK` plus `k1..k4` — then add the matching documented entry to `PRESETS.md`. The two
are kept in sync by design: the table feeds the pulldown, `PRESETS.md` is the human
reference. (The pulldown is driven by the node's `changedParam` callback, which also
implements the snap-to-`(Custom)` behaviour.)

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

The `k1..k4`/aliases, `ref`, **Mix** and **Clamp Output** let you drive an expression
live with keyframable, nameable constants. **Mix** blends the original image (0) with
the expression result (1); **Clamp Output** clamps the result to `0..1` (applied before
Mix).

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
  signed ~`[-1,1]`. Computed in float with a computable permutation (no table) so it is
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

The CPU path is multithreaded across scanlines via the OFX MultiThread suite, and
each expression is compiled to an AST once per render rather than re-parsed per
pixel — plenty for interactive grading-style use.

On verified hosts the same expression also runs on the **GPU**, transpiled from the
AST: a **Metal** path (advertised to DaVinci Resolve, where it is pixel-verified)
and an opt-in **CUDA** path (Linux, built `make WITH_CUDA=1`). Every other host —
including Flame, whose macOS OFX host is deliberately kept off the Metal path because
it crashes on a Metal-advertising node — uses the CPU path. The GPU and CPU
back-ends share one tableless noise/permutation implementation, so `noise()`,
`fBm()`, `turbulence()` and `random()` match across all three (NVRTC builds pass
`--fmad=false` to hold that parity).
