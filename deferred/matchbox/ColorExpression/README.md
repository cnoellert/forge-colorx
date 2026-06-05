# ColorExpression — a Matchbox port of Nuke's Expression node

A Matchbox shader for Autodesk **Flame / Flare / Smoke** that re-creates the
behaviour of Nuke's **Color > Math > Expression** node: per-channel math on
`r g b a` using the same predefined variables and the same function library.

```
matchbox/ColorExpression/
├── ColorExpression.glsl   the shader (edit the EXPRESSION BLOCK here)
├── ColorExpression.xml    the UI sidecar (knobs, input, mix/clamp)
└── README.md              this file
```

## The one honest difference from Nuke

Nuke parses **free-typed text** for each channel at run time. A Matchbox shader
is **compiled GLSL**, and the Matchbox UI has **no text-entry widget** — only
sliders, checkboxes, colour pickers and popups. So there is no way to type an
arbitrary expression into a box and have it evaluate live.

The faithful equivalent — and how Flame TDs actually do this — is:

1. The full Nuke expression **function library and variables are reproduced in
   GLSL** (below).
2. You write the four channel formulas in the **EXPRESSION BLOCK** of
   `ColorExpression.glsl`, in *Nuke-identical syntax*.
3. You recompile (one command). Animatable **knobs** (`k1..k4`, `ref`) let you
   tweak the common values live, with no recompile.

If you genuinely need run-time-typed expressions, that requires a custom
OpenFX/Action plugin, not a Matchbox shader — out of scope here, happy to
discuss.

## Install / compile

Drop the folder somewhere on your Matchbox path (e.g.
`/opt/Autodesk/presets/<ver>/matchbox/shaders/`, or your user shaders folder),
then validate/compile the GLSL with the bundled tool:

```bash
shader_builder -m ColorExpression.glsl
```

`shader_builder` reports any GLSL errors and (re)generates/validates the sidecar
XML. In Flame, add a **Matchbox** node and load `ColorExpression`.

> GLSL target is `#version 120` so it runs on both Linux and macOS Flame.
> (Linux Flame also supports 130; 120 is the portable subset.)

## How to write an expression

Open `ColorExpression.glsl`, find the **EXPRESSION BLOCK**, and edit:

```glsl
float R = r;        // red   out
float G = g;        // green out
float B = b;        // blue  out
float A = a;        // alpha out
```

Each line can use any variable and any function listed below — exactly as in
Nuke. The result is optionally clamped to `[0,1]` and blended with the original
via the **Mix** knob.

### Predefined variables (same as Nuke)

| Variable            | Meaning                                                        |
|---------------------|---------------------------------------------------------------|
| `r` `g` `b` `a`     | Input channels at the current pixel                           |
| `x` `y`             | Pixel coordinates, origin **bottom-left** (as in Nuke)        |
| `cx` `cy`           | Centred, aspect-preserved coords: `(0,0)` centre, `±1` at L/R edge |
| `width` `height`    | Output resolution                                             |
| `frame`             | Current frame (`adsk_time`)                                   |
| `k1 k2 k3 k4`       | Animatable scalar knobs (UI sliders)                          |
| `ref`              | Animatable reference colour — use `ref.r` `ref.g` `ref.b`      |

### Function parity (Nuke → this shader)

Functions GLSL already provides with identical meaning are used **as-is**:

`sin cos tan asin acos atan(y,x) exp log pow(x,y) sqrt floor ceil abs sign
min(x,y) max(x,y) mod fract step(a,x) smoothstep(a,b,x) mix(a,b,t)
clamp(x,a,b) radians degrees`

Functions GLSL lacks or names differently are **added** in the shader:

| Nuke                          | Use in this shader            | Notes |
|-------------------------------|-------------------------------|-------|
| `pi()`                        | `pi()`                        | constant π |
| `e`                           | `e_()`                        | `e` collides with GLSL exponent syntax |
| `lerp(a,b,t)`                 | `lerp(a,b,t)`                 | alias of `mix` |
| `pow2(x)`                     | `pow2(x)`                     | `x*x` |
| `fabs(x)`                     | `fabs(x)` (or `abs`)          |       |
| `fmod(x,y)`                   | `fmod(x,y)`                   | C-style: sign of dividend (GLSL `mod` differs for negatives) |
| `hypot(x,y)`                  | `hypot(x,y)`                  |       |
| `log10(x)`                    | `log10(x)`                    |       |
| `atan2(y,x)`                  | `atan2(y,x)` (or `atan(y,x)`) |       |
| `trunc(x)`                    | `trunc(x)`                    | toward zero |
| `rint(x)`                     | `rint(x)`                     | round to nearest |
| `int(x)`                      | `trunc(x)`                    | `int` is a reserved keyword |
| `clamp(x)` (1-arg)            | `clamp01(x)`                  | clamp to `[0,1]` |
| `sinh cosh tanh`              | `sinh cosh tanh`              | defined (not built into 120) |
| `ldexp exponent mantissa`     | same names                    | **approximate** (no `frexp` in GLSL) |
| `noise(x,y,z)`                | `noise(x,y,z)`                | signed ~`[-1,1]`; **classic Perlin** (Gustavson/Ashima, tableless) |
| `random(x,y,z)`              | `random(x,y,z)`               | `[0,1)` tableless permute hash (cell-based) |
| `fBm(x,y,z,oct,lac,gain)`     | `fBm(...)`                    |       |
| `turbulence(x,y,z,oct,lac,gain)` | `turbulence(...)`          |       |
| `from_sRGB to_sRGB`           | same names                    | standard sRGB EOTF/OETF |
| `from_rec709 to_rec709`       | same names                    | Rec.709 OETF |
| `from_byte to_byte`           | same names                    | 8-bit sRGB ↔ linear |

Deviations worth remembering:

- `int(x)` → **`trunc(x)`** (keyword clash).
- `clamp(x)` 1-arg → **`clamp01(x)`** (GLSL `clamp` needs 3 args).
- `e` → **`e_()`**.
- `min`/`max` are 2-argument in GLSL; for 3+ inputs nest them: `max(a,max(b,c))`.
- `noise()` is **classic Perlin** gradient noise (Gustavson/Ashima `cnoise`,
  tableless computable permutation), signed ~`[-1,1]`. It uses the identical
  algorithm/constants as the OFX build, so the two tools match to float precision;
  `random()` is a matching tableless permute hash. Not a bit-exact match to Nuke's
  own Perlin permutation.

## Example expressions

Each example is the contents of the EXPRESSION BLOCK. They read the same as the
equivalent Nuke expression.

**Gamma (`k1` = gamma):**
```glsl
float R = pow(r, 1.0/k1);
float G = pow(g, 1.0/k1);
float B = pow(b, 1.0/k1);
float A = a;
```

**Invert RGB, keep alpha:**
```glsl
float R = 1.0 - r;
float G = 1.0 - g;
float B = 1.0 - b;
float A = a;
```

**Luminance to mono (Rec.709 weights):**
```glsl
float L = 0.2126*r + 0.7152*g + 0.0722*b;
float R = L, G = L, B = L, A = a;
```

**Horizontal gradient in red:**
```glsl
float R = x / width;
float G = g, B = b, A = a;
```

**Radial vignette (`k1` = strength):**
```glsl
float v = clamp01(1.0 - hypot(cx, cy) * k1);
float R = r*v, G = g*v, B = b*v, A = a;
```

**Animated noise grain (`k1` = scale, `k2` = amount):**
```glsl
float n = noise(x*k1, y*k1, frame*0.1);
float R = r + n*k2;
float G = g + n*k2;
float B = b + n*k2;
float A = a;
```

**Tint toward the reference colour (`k1` = amount):**
```glsl
float R = lerp(r, ref.r, k1);
float G = lerp(g, ref.g, k1);
float B = lerp(b, ref.b, k1);
float A = a;
```

## Notes on the XML UI

Confirmed against real Matchbox shaders: `float` sliders, `bool` checkboxes,
`int` fields, `sampler2D` inputs, and `Page`/`Col` layout. The `Reference
Colour` uses the `vec3` + `ValueType="Colour"` colour-picker pattern; if your
Flame build rejects it, replace that one uniform with three plain `float`
uniforms (`refr refg refb`) and update the GLSL accordingly.

`shader_builder -m ColorExpression.glsl` is the source of truth — if it
compiles cleanly there, it will load in Flame.
