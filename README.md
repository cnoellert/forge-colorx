# forge-colorx

A re-creation of Nuke's **Color > Math > Expression** node as an **OpenFX plugin** —
real per-channel text fields, parsed and evaluated at run time, so an expression
behaves just as it does in Nuke. The built `.ofx` loads in **Autodesk Flame (2021+)**,
DaVinci Resolve/Fusion, Nuke, Natron, and any other OpenFX host. In a host it appears
as the **colorx** node under **FORGE › color › colorx**.

```
forge-colorx/
└── ofx/Expression/             OpenFX plugin (Flame 2021+, Nuke, Resolve, …)
    ├── Expression.cpp
    ├── ExprEval.h              self-contained expression compiler/evaluator
    ├── Makefile
    └── README.md
```

## What it does

- **Typed per-channel expressions** (`r= g= b= a=`) with a full Nuke-parity function
  library — trig, `pow`, `clamp`, `lerp`, `step`/`smoothstep`, `noise`/`fBm`/`turbulence`,
  colour-space helpers, and more.
- **Derived variables**, four nameable/animatable knobs (`k1..k4`), and a Reference Colour.
- A **Preset pulldown** that loads ready-made effects — channels, variables, and knob
  values together — in one pick.
- **GPU where it's safe:** Metal on DaVinci Resolve, opt-in CUDA on Linux/NVIDIA; the
  multithreaded **CPU** path everywhere else (including Flame).

📖 **[Reference manual](https://cnoellert.github.io/forge-colorx/)** — a browsable page with
the variables, operators, the full function library, and the preset gallery.

See [`ofx/Expression/README.md`](ofx/Expression/README.md) for build, install, the full
Nuke→here function-parity table, and worked examples. For a gallery of effects — also
built into the Preset pulldown — see [`PRESETS.md`](PRESETS.md).

## Install

Grab a prebuilt bundle for macOS or Linux from the
[**latest release**](https://github.com/cnoellert/forge-colorx/releases/latest), or
build from source — see the plugin README.

> **Upgrading from the old "Expression" node?** The node was renamed to **colorx**
> (label + menu only); its plugin identifier is unchanged, so it's a **drop-in** update —
> existing comps keep working, no re-linking. Just replace the old
> `Expression.ofx.bundle` with `colorx.ofx.bundle` (don't keep both installed).

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
