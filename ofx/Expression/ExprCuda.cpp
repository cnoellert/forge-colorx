// ExprCuda.cpp
// -----------------------------------------------------------------------------
// CUDA compute dispatch for the OFX render path — the CUDA sibling of
// ExprMetal.mm. Compiles the per-pixel kernel source (buildCudaPixelKernel, see
// ExprKernelCuda.h) to PTX with NVRTC, loads it with the CUDA driver API into the
// host's current context, and launches it on the host-supplied stream
// (args.pCudaStream). NVRTC modules are cached by source string so only the first
// frame of a given expression set pays compilation.
//
// The host (e.g. DaVinci Resolve on Linux) owns the CUDA context, the stream, and
// the device image buffers (getPixelData() returns device pointers under CUDA
// render). We never allocate or copy image memory here — we just launch over the
// host's buffers, exactly as the Metal path uses the host's MTLBuffers.
//
// Compiled/linked only when WITH_CUDA=1 (Linux + CUDA toolkit); see the Makefile.
// -----------------------------------------------------------------------------
#include "ExprCuda.h"

#include <nvrtc.h>
#include <cuda.h>

#include <cstdio>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace expreval {

// Byte-identical to the ExprCudaUniforms defined in the generated kernel source
// (ExprKernelCuda.h::buildCudaPixelKernel) — 12 ints + 3 floats, all 4-byte, no
// padding. Keep the field order/types in lockstep with that struct and with
// ExprMetalUniforms.
struct ExprCudaUniforms {
    int rwx1, rwy1, rwx2, rwy2;
    int srcX1, srcY1, srcRowFloats;
    int dstX1, dstY1, dstRowFloats;
    int nComps, hasSrc;
    float fwidth, fheight, frame;
};

// Module cache keyed by CUDA source. Modules belong to the context they were
// loaded into; the host keeps one CUDA context for the render thread, so caching
// the resolved CUfunction is safe across frames.
namespace {
struct CachedKernel { CUmodule mod; CUfunction fn; };
std::map<std::string, CachedKernel> g_cache;
std::mutex g_mutex;

// NVRTC-compile `src` to PTX. --fmad=false is REQUIRED for CPU/GPU noise parity
// (fma fusion perturbs the noise hash; see ExprKernelCuda.h / test_cuda_run.cpp).
bool compileToPTX(const std::string& src, std::string& ptx, std::string& err) {
    nvrtcProgram prog;
    if (nvrtcCreateProgram(&prog, src.c_str(), "expr.cu", 0, nullptr, nullptr)
            != NVRTC_SUCCESS) { err = "nvrtcCreateProgram failed"; return false; }

    char arch[64] = "--gpu-architecture=compute_70";
    CUdevice dev;
    if (cuCtxGetDevice(&dev) == CUDA_SUCCESS) {
        int mj = 0, mn = 0;
        cuDeviceGetAttribute(&mj, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
        cuDeviceGetAttribute(&mn, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
        if (mj > 0) std::snprintf(arch, sizeof arch,
                                  "--gpu-architecture=compute_%d%d", mj, mn);
    }
    const char* opts[] = { arch, "--fmad=false" };
    nvrtcResult rc = nvrtcCompileProgram(prog, 2, opts);
    if (rc != NVRTC_SUCCESS) {   // fall back to a virtual arch the driver can JIT
        const char* fb[] = { "--gpu-architecture=compute_70", "--fmad=false" };
        rc = nvrtcCompileProgram(prog, 2, fb);
    }
    if (rc != NVRTC_SUCCESS) {
        size_t n = 0; nvrtcGetProgramLogSize(prog, &n);
        std::string log(n ? n - 1 : 0, '\0');
        if (n > 1) nvrtcGetProgramLog(prog, &log[0]);
        err = std::string("NVRTC compile failed: ") + log;
        nvrtcDestroyProgram(&prog);
        return false;
    }
    size_t ptxSize = 0; nvrtcGetPTXSize(prog, &ptxSize);
    ptx.assign(ptxSize, '\0');
    nvrtcGetPTX(prog, &ptx[0]);
    nvrtcDestroyProgram(&prog);
    return true;
}

// Resolve (compile + load + cache) the exprKernel for this source. Returns the
// CUfunction or nullptr (with `err` set).
CUfunction getKernel(const std::string& cudaSource, std::string& err) {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::map<std::string, CachedKernel>::iterator it = g_cache.find(cudaSource);
    if (it != g_cache.end()) return it->second.fn;

    std::string ptx;
    if (!compileToPTX(cudaSource, ptx, err)) return nullptr;

    CUmodule mod;
    if (cuModuleLoadData(&mod, ptx.c_str()) != CUDA_SUCCESS) {
        err = "cuModuleLoadData failed"; return nullptr;
    }
    CUfunction fn;
    if (cuModuleGetFunction(&fn, mod, "exprKernel") != CUDA_SUCCESS) {
        err = "cuModuleGetFunction(exprKernel) failed";
        cuModuleUnload(mod); return nullptr;
    }
    CachedKernel ck; ck.mod = mod; ck.fn = fn;
    g_cache[cudaSource] = ck;
    return fn;
}
} // anonymous namespace

bool cudaRender(void* stream,
                const CudaImageDesc& src, int hasSrc,
                const CudaImageDesc& dst,
                int rwX1, int rwY1, int rwX2, int rwY2,
                int nComps, float width, float height, float frame,
                const std::string& cudaSource,
                std::string& err) {
    // The host has already initialised CUDA and made its context current on this
    // render thread; cuInit is idempotent and cheap as a safety net.
    cuInit(0);
    CUcontext cur = nullptr;
    if (cuCtxGetCurrent(&cur) != CUDA_SUCCESS || cur == nullptr) {
        err = "no current CUDA context"; return false;
    }

    CUfunction fn = getKernel(cudaSource, err);
    if (!fn) return false;

    ExprCudaUniforms U;
    U.rwx1 = rwX1; U.rwy1 = rwY1; U.rwx2 = rwX2; U.rwy2 = rwY2;
    U.srcX1 = src.x1; U.srcY1 = src.y1; U.srcRowFloats = src.rowFloats;
    U.dstX1 = dst.x1; U.dstY1 = dst.y1; U.dstRowFloats = dst.rowFloats;
    U.nComps = nComps; U.hasSrc = hasSrc;
    U.fwidth = width; U.fheight = height; U.frame = frame;

    CUdeviceptr dSrc = (CUdeviceptr)(uintptr_t)src.buffer;
    CUdeviceptr dDst = (CUdeviceptr)(uintptr_t)dst.buffer;
    void* kargs[] = { &dSrc, &dDst, &U };

    const unsigned bx = 16, by = 16;
    const int w = rwX2 - rwX1, h = rwY2 - rwY1;
    if (w <= 0 || h <= 0) return true;   // empty tile: nothing to do
    const unsigned gx = (unsigned)((w + bx - 1) / bx);
    const unsigned gy = (unsigned)((h + by - 1) / by);
    CUstream s = (CUstream)stream;

    CUresult rc = cuLaunchKernel(fn, gx, gy, 1, bx, by, 1, 0, s, kargs, nullptr);
    if (rc != CUDA_SUCCESS) {
        const char* m = nullptr; cuGetErrorString(rc, &m);
        err = std::string("cuLaunchKernel failed: ") + (m ? m : "?");
        return false;
    }
    // v1: wait for completion (analogue of Metal's waitUntilCompleted). The OFX
    // spec allows returning while the stream is in flight; switch to async once
    // the path is host-confirmed (see PASSOFF).
    rc = cuStreamSynchronize(s);
    if (rc != CUDA_SUCCESS) {
        const char* m = nullptr; cuGetErrorString(rc, &m);
        err = std::string("cuStreamSynchronize failed: ") + (m ? m : "?");
        return false;
    }
    return true;
}

} // namespace expreval
