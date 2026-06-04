// ExprCuda.h
// -----------------------------------------------------------------------------
// Pure-C++ bridge to the CUDA compute dispatch in ExprCuda.cpp. The CUDA sibling
// of ExprMetal.h: the interface uses only void* and PODs (no CUDA types), so
// Expression.cpp stays a portable .cpp and calls it behind #ifdef HAVE_CUDA. The
// .cpp side casts the void*s back to CUstream / CUdeviceptr and drives NVRTC +
// the CUDA driver API.
//
// Only compiled/linked on Linux with a CUDA toolkit present (the Makefile adds
// ExprCuda.o + -lnvrtc/-lcuda when WITH_CUDA=1). The macOS/Metal path and the
// CPU-only Linux build are unaffected.
// -----------------------------------------------------------------------------
#ifndef EXPR_CUDA_H
#define EXPR_CUDA_H

#include <string>

namespace expreval {

// One image plane as the OFX CUDA render passes it: getPixelData() returns a CUDA
// *device* pointer when CUDA render is enabled, whose contents begin at the image
// bounds origin (x1,y1); row stride measured in floats. Mirrors MetalImageDesc.
struct CudaImageDesc {
    void* buffer = nullptr;   // CUdeviceptr (device memory)
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

// Dispatch one render tile on the GPU using the host-supplied CUDA stream.
// `cudaSource` must define `extern "C" __global__ void exprKernel(...)` exactly as
// produced by buildCudaPixelKernel(). NVRTC-compiled modules are cached by source
// string (compiled with --fmad=false for CPU noise parity — see ExprKernelCuda.h),
// so the per-render cost after the first frame of an expression is just the launch.
// Returns true on success; on false `err` explains why and the caller should fall
// back to the CPU render path.
bool cudaRender(void* stream,
                const CudaImageDesc& src, int hasSrc,
                const CudaImageDesc& dst,
                int rwX1, int rwY1, int rwX2, int rwY2,
                int nComps, float width, float height, float frame,
                const ExprKnobs& knobs,
                const std::string& cudaSource,
                std::string& err);

} // namespace expreval
#endif // EXPR_CUDA_H
