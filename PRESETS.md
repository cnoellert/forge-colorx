# Expression presets

A gallery of ready-to-paste presets for the **Expression** OFX node. Every preset
here is validated against the expression parser, so it compiles as-is.

📖 Browsable version (with the full function library) at
<https://cnoellert.github.io/forge-colorx/#presets>.

> **Tip:** every preset below is also built into the node's **Preset** pulldown (top
> of the *Channels* page). Picking one stamps its `r/g/b/a`, the `Variables` block,
> and the suggested `k1..k4` values straight into the fields — no copy-paste needed.
> Hand-edit any channel afterwards and the pulldown snaps back to *(Custom)*. This
> page stays the reference for what each preset does and how to drive its knobs.
> (The pulldown is fed by the `kPresets` table in `ofx/Expression/Expression.cpp` —
> keep the two in sync.)

## How to use

- **Channels** — paste each `r =` / `g =` / `b =` / `a =` line into its channel box.
- **Variables** — paste the `Variables:` line into the single Variables box. Multiple
  variables are separated by `;` (Flame collapses newlines, so use `;` there; Resolve/
  Nuke accept newlines too).
- **Knobs** — `k1..k4` are the live sliders; set the suggested values below. `t` is the
  current frame, so presets that use `t` animate. `ref.r/.g/.b` is the **Reference
  Colour** swatch.

A couple of things worth knowing:

- The node is **per-pixel**: an expression only sees the current pixel's `r g b a`
  plus its coordinates. Great for **generators** and **per-pixel grades**; it can't
  blur, sharpen, or sample neighbours.
- `x y` are pixel coords (origin bottom-left). `cx cy` are **centred and
  aspect-preserved** (`0,0` centre, `±1` at the left/right edges) — use these for
  resolution-independent patterns.
- Handy constants: `6.2831853` = 2π, `2.0944` = 2π/3, `4.1888` = 4π/3.

The colour-palette presets use Inigo Quilez's cosine palette,
`colour(t) = a + b·cos(2π(c·t + d))` — see
<https://iquilezles.org/articles/palettes/>.

---

## Technical / UV

### UV (ST) pass
The classic red-horizontal / green-vertical coordinate pass. The `+0.5` samples each
pixel's **centre** (`x` is the integer pixel index), so it's a correct ST/UV map.
```
r = (x+0.5)/width
g = (y+0.5)/height
b = 0
a = 1
```

### Radial gradient
White at centre, falling to black. **k1** = radius (try `1.0`).
```
Variables:  d = hypot(cx,cy)
r = clamp(1 - d/k1)
g = clamp(1 - d/k1)
b = clamp(1 - d/k1)
a = 1
```

### Angle sweep
A 0→1 ramp swept around the centre (a conical gradient).
```
Variables:  ang = (atan2(cy,cx) + pi) / (2*pi)
r = ang
g = ang
b = ang
a = 1
```

---

## Gradients & palettes

### Rainbow (cosine palette)
A smooth horizontal rainbow.
```
Variables:  u = x/width
r = 0.5 + 0.5*cos(6.2831853*(u + 0.00))
g = 0.5 + 0.5*cos(6.2831853*(u + 0.33))
b = 0.5 + 0.5*cos(6.2831853*(u + 0.67))
a = 1
```

### Reference → white gradient
Left = the **Reference Colour** swatch, right = white. Pick a colour to drive it.
```
Variables:  u = x/width
r = lerp(ref.r, 1, u)
g = lerp(ref.g, 1, u)
b = lerp(ref.b, 1, u)
a = 1
```

---

## Patterns

### Checkerboard
**k1** = cell size in pixels (try `32`).
```
Variables:  c = fmod(floor(x/k1) + floor(y/k1), 2)
r = c
g = c
b = c
a = 1
```

### Stripes
Vertical bars. **k1** = period in pixels (try `32`).
```
Variables:  f = x/k1 - floor(x/k1)
r = step(0.5, f)
g = step(0.5, f)
b = step(0.5, f)
a = 1
```

### Concentric rings
**k1** = ring frequency (try `6`).
```
Variables:  d = hypot(cx,cy)
r = 0.5 + 0.5*sin(d*k1*6.2831853)
g = 0.5 + 0.5*sin(d*k1*6.2831853)
b = 0.5 + 0.5*sin(d*k1*6.2831853)
a = 1
```

### Flower / rose
A petalled mask. **k2** = number of petals (try `5`).
```
Variables:  ang = atan2(cy,cx); rad = hypot(cx,cy); m = step(rad, 0.4 + 0.3*cos(ang*k2))
r = m
g = m
b = m
a = 1
```

### Plasma (animated)
Retro demoscene plasma — animates on `t`. **k1** = scale (try `3`).
```
Variables:  v = sin(cx*k1*3) + sin(cy*k1*3 + t*0.1) + sin((cx+cy)*k1*2) + sin(hypot(cx,cy)*k1*4 - t*0.13)
r = 0.5 + 0.5*sin(v)
g = 0.5 + 0.5*sin(v + 2.0944)
b = 0.5 + 0.5*sin(v + 4.1888)
a = 1
```

---

## Noise (Perlin)

### Clouds (animated fBm)
Soft drifting clouds. **k1** = scale (try `3`).
```
Variables:  n = fBm(cx*k1, cy*k1, t*0.05, 5, 2, 0.5); c = clamp(n*0.5 + 0.5)
r = c
g = c
b = c
a = 1
```

### Marble
Veined marble from turbulence. **k1** = scale (try `3`), **k2** = vein frequency (try `2`).
```
Variables:  tu = turbulence(cx*k1, cy*k1, t*0.05, 5, 2, 0.5); m = 0.5 + 0.5*sin((cx + tu*2)*6.2831853*k2)
r = m
g = m
b = m
a = 1
```

### Colorized noise
Perlin noise pushed through the cosine palette. **k1** = scale (try `3`).
```
Variables:  n = fBm(cx*k1, cy*k1, t*0.05, 5, 2, 0.5)*0.5 + 0.5
r = 0.5 + 0.5*cos(6.2831853*(n + 0.00))
g = 0.5 + 0.5*cos(6.2831853*(n + 0.33))
b = 0.5 + 0.5*cos(6.2831853*(n + 0.67))
a = 1
```

### Film grain (on the input)
Adds per-pixel grain to the image. **k1** = amount (try `0.1`). The channel offsets
decorrelate the grain so it isn't grey.
```
r = r + (random(x,    y,    t) - 0.5)*k1
g = g + (random(x+11, y,    t) - 0.5)*k1
b = b + (random(x,    y+7,  t) - 0.5)*k1
a = a
```

---

## Per-pixel colour grades (on the input)

### Gamma
**k1** = gamma (try `2.2`).
```
r = pow(r, 1/k1)
g = pow(g, 1/k1)
b = pow(b, 1/k1)
a = a
```

### Saturation
**k1** = saturation (`0` = greyscale, `1` = unchanged, `>1` = boosted).
```
Variables:  lum = 0.2126*r + 0.7152*g + 0.0722*b
r = lum + (r - lum)*k1
g = lum + (g - lum)*k1
b = lum + (b - lum)*k1
a = a
```

### Duotone
Maps shadows→**Reference Colour**, highlights→white, by luminance.
```
Variables:  lum = 0.2126*r + 0.7152*g + 0.0722*b
r = lerp(ref.r, 1, lum)
g = lerp(ref.g, 1, lum)
b = lerp(ref.b, 1, lum)
a = a
```

### Posterize
Quantise to bands. **k1** = number of levels (try `6`).
```
r = floor(r*k1)/k1
g = floor(g*k1)/k1
b = floor(b*k1)/k1
a = a
```

### Vignette
Darken the edges. **k1** = strength (try `0.7`).
```
Variables:  d = hypot(cx,cy); v = clamp(1 - d*k1)
r = r*v
g = g*v
b = b*v
a = a
```

### Scanlines (CRT)
**k1** = line frequency (try `1.5`).
```
Variables:  s = 0.6 + 0.4*sin(y*k1)
r = r*s
g = g*s
b = b*s
a = a
```
