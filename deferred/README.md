# deferred

Work that's parked, not abandoned — kept in the repo (with full git history) but
out of the active project's way.

## `matchbox/ColorExpression/`

A GPU **Matchbox** shader port of the Expression node for the Flame family. It shares
the expression semantics (function library, `noise()`, variable names) with the OFX
plugin, but a Matchbox has no text widget, so its expression is fixed in the GLSL
rather than typed live.

The OFX plugin in [`ofx/Expression/`](../ofx/Expression/) is the faithful, live-typed
clone and the focus of the project. The Matchbox is deferred until there's a concrete
need for a GPU-fixed Flame-family build; if that comes up, the source here is the
starting point.
