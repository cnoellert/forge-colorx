// ExprMetal.h
// -----------------------------------------------------------------------------
// Pure-C++ bridge to the Metal compute dispatch in ExprMetal.mm. The interface
// uses only void* and PODs (no Objective-C / Metal types), so Expression.cpp can
// stay a portable .cpp and call it behind #ifdef HAVE_METAL. The .mm side casts
// the void*s back to id<MTLCommandQueue> / id<MTLBuffer>.
//
// Only compiled/linked on macOS (the Makefile adds ExprMetal.o + the Metal /
// Foundation frameworks on Darwin). The CUDA/Linux path is unaffected.
// -----------------------------------------------------------------------------
#ifndef EXPR_METAL_H
#define EXPR_METAL_H

#include <string>

namespace expreval {

// One image plane as the OFX Metal render passes it: a MTLBuffer whose contents
// begin at the image bounds origin (x1,y1), row stride measured in floats.
struct MetalImageDesc {
    void* buffer = nullptr;   // id<MTLBuffer>
    int   x1 = 0, y1 = 0;     // image bounds origin (OFX, bottom-left)
    int   rowFloats = 0;      // row stride / sizeof(float)
};

// User-constant knobs (mirror the Matchbox): k1..k4 + ref colour bound as the
// expression variables k1..k4 / ref.r/.g/.b; mix blends original<->result;
// clampOut clamps the result to [0,1]. Passed into the kernel uniforms.
struct ExprKnobs {
    float k[4]   = {1, 0, 0, 0};
    float ref[3] = {0, 0, 0};
    float mix    = 1.0f;
    int   clampOut = 0;
};

// Dispatch one render tile on the GPU using the host-supplied command queue.
// `mslSource` must define `kernel void exprKernel(...)` exactly as produced by
// buildMetalPixelKernel(). Compiled pipelines are cached by MSL source, so the
// per-render cost after the first frame of an expression is just the dispatch.
// Returns true on success; on false `err` explains why and the caller should
// fall back to the CPU render path.
bool metalRender(void* commandQueue,
                 const MetalImageDesc& src, int hasSrc,
                 const MetalImageDesc& dst,
                 int rwX1, int rwY1, int rwX2, int rwY2,
                 int nComps, float width, float height, float frame,
                 const ExprKnobs& knobs,
                 const std::string& mslSource,
                 std::string& err);

} // namespace expreval
#endif // EXPR_METAL_H
