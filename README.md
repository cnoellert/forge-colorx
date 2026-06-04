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
| Execution | GPU | CPU (multithreaded) |
| Hosts | Flame family | Flame + every OpenFX host |
| Install | drop the files on the Matchbox path | build the `.ofx`, drop the bundle |

The **OFX** build is the faithful clone (Matchbox has no text widget, so a true
"type any expression" box is impossible there — see `matchbox/.../README.md`).
The **Matchbox** build is for when you want GPU speed and a fixed expression.

See each subfolder's `README.md` for build, install, the full Nuke→here
function-parity table, and worked examples.

## Status

- OFX expression engine: 36/36 unit tests pass; the plugin compiles and links
  into a working `.ofx` against the Academy OpenFX SDK.
- Not yet verified live inside a host (param UI + on-image pixels) — needs an
  OFX host / Flame to confirm.
