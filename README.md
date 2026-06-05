# forge-colorx

A re-creation of Nuke's **Color > Math > Expression** node for Autodesk Flame,
in two flavours that share the same expression semantics (same function set,
same `noise()`, same variable names — so an expression behaves the same in
either):

```
forge-colorx/
├── matchbox/ColorExpression/   GPU Matchbox shader (Flame family)
│   ├── ColorExpression.glsl
│   ├── ColorExpression.xml
│   └── README.md
└── ofx/Expression/             OpenFX plugin (Flame 2021+, Nuke, Resolve, ...)
    ├── Expression.cpp
    ├── ExprEval.h              self-contained expression compiler/evaluator
    ├── Makefile
    └── README.md
```

## Which one

| | `matchbox/` | `ofx/` |
|---|---|---|
| Per-channel **typed** expressions, live | No — edit the GLSL block + recompile | **Yes** — real text fields, parsed at run time |
| Execution | GPU | CPU (multithreaded) + GPU on verified hosts (Metal/CUDA) |
| Hosts | Flame family | Flame + every OpenFX host |
| Install | drop the files on the Matchbox path | build the `.ofx`, drop the bundle |

The **OFX** build is the faithful clone (Matchbox has no text widget, so a true
"type any expression" box is impossible there — see `matchbox/.../README.md`).
The **Matchbox** build is for when you want GPU speed and a fixed expression.

See each subfolder's `README.md` for build, install, the full Nuke→here
function-parity table, and worked examples. The OFX build ships a **Preset
pulldown** (foot of the node's Output page) that loads any of those effects with
one pick — channels, variables, and knob values together. For the same gallery as
a copy-paste reference (UV pass, rainbow/cosine palettes, checkerboard, plasma,
Perlin clouds/marble, duotone, posterize, film grain, scanlines, …) see
[`PRESETS.md`](PRESETS.md).

## Status

- OFX expression engine: the self-contained `tests/test_expr.cpp` suite passes
  (0 failures); the plugin compiles and links into a working `.ofx` against the
  Academy OpenFX SDK.
- **Verified live in a host** (param UI + on-image pixels): loads, renders, and
  drives its params inside **Flame 2026.2.2** (Apple Silicon, CPU path) and
  **DaVinci Resolve 20.3.1.6** (Metal GPU path, pixel-verified). The **Preset**
  pulldown stamps effects correctly in both.
- GPU paths: **Metal** on verified hosts (Resolve), opt-in **CUDA** on Linux
  (`make WITH_CUDA=1`, verified on an RTX 5000 Ada). Flame stays on the CPU path
  by design — its macOS OFX host crashes on a Metal-advertising node, so Metal is
  gated to a host allow-list.
