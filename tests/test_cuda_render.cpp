// test_cuda_render.cpp
// -----------------------------------------------------------------------------
// End-to-end test of the production CUDA *pixel* path: buildCudaPixelKernel()
// + cudaRender() (ExprCuda.cpp). The CUDA sibling of tests/test_metal_render.mm.
// Where test_cuda_run.cpp checks the scalar emitted bodies, this checks the
// per-pixel wrapper the OFX render path actually runs — source read from a device
// buffer, the predefined-variable binding (x,y bottom-left; centred
// aspect-preserved cx,cy; width/height/frame), temp evaluation, and the
// channel-remapped write — by allocating real CUDA device buffers, dispatching on
// the GPU through the production cudaRender() bridge, and diffing against the same
// binding evaluated by the CPU Program::eval().
//
// It mirrors ExpressionProcessor in Expression.cpp (bounds origin, var slots,
// float write) exactly as test_metal_render.mm does. If those drift, this catches
// it. SKIPs (exit 0) when no CUDA device/driver is present.
//
// Build (Linux, CUDA toolkit / NVRTC + driver present):
//   c++ -std=c++11 -O2 -I ofx/Expression -I$NVRTC_INC -I$CUDA_INC \
//       tests/test_cuda_render.cpp ofx/Expression/ExprCuda.cpp -o test_cuda_render \
//       -L$NVRTC_LIB -l:libnvrtc.so.12 -lcuda && ./test_cuda_render
// -----------------------------------------------------------------------------
#include "ExprEval.h"
#include "ExprKernelCuda.h"
#include "ExprCuda.h"

#include <cuda.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

using namespace expreval;

// Variable slots — must match Expression.cpp's enum order.
enum { R=0, G, B, A, X, Y, CX, CY, WIDTH, HEIGHT, FRAME, FIXED };

int main() {
    // ---- driver init / device probe (SKIP cleanly if no usable CUDA) ----
    if (cuInit(0) != CUDA_SUCCESS) {
        std::printf("SKIP: CUDA driver unavailable (no GPU / driver on this host)\n");
        return 0;
    }
    int devCount = 0;
    if (cuDeviceGetCount(&devCount) != CUDA_SUCCESS || devCount == 0) {
        std::printf("SKIP: no CUDA device available (GPU-less runner)\n");
        return 0;
    }
    CUdevice dev;
    cuDeviceGet(&dev, 0);
    char devName[256] = {0};
    cuDeviceGetName(devName, sizeof devName, dev);
    std::printf("CUDA device: %s\n", devName);

    CUcontext ctx;
    if (cuCtxCreate(&ctx, 0, dev) != CUDA_SUCCESS) {
        std::printf("cuCtxCreate failed\n"); return 1;
    }
    CUstream stream;
    cuStreamCreate(&stream, CU_STREAM_DEFAULT);

    const int W = 64, H = 48, nComps = 4;
    const float frame = 7.0f;

    // fixed predefined variable names + one temp ("lum")
    std::vector<std::string> names =
        { "r","g","b","a","x","y","cx","cy","width","height","frame", "lum" };
    const int LUM = 11, nVars = 12;

    // source pattern (RGBA float), bounds origin (0,0)
    std::vector<float> srcPix((size_t)W*H*nComps);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float* p = &srcPix[((size_t)y*W + x)*nComps];
            p[0] = (float)x / W;          // r ramps in x
            p[1] = (float)y / H;          // g ramps in y
            p[2] = 0.25f;                 // b flat
            p[3] = 1.0f;                  // a
        }

    // expression sets to exercise the pixel path (lum is a temp)
    struct Case { const char* r; const char* g; const char* b; const char* a; const char* lum; };
    std::vector<Case> cases = {
        { "r", "g", "b", "a", "" },                               // passthrough
        { "g", "b", "r", "a", "" },                               // channel swap
        { "cx", "cy", "0.5", "1", "" },                           // coordinate math
        { "x/width", "y/height", "0", "1", "" },                  // x,y / size
        { "1-r", "r*g+b", "min(r,g,b)", "1", "" },                // mixed math
        { "lum", "lum", "lum", "1", "0.2126*r+0.7152*g+0.0722*b"},// temp -> grey
    };

    const size_t bytes = srcPix.size()*sizeof(float);
    CUdeviceptr dSrc = 0, dDst = 0;
    if (cuMemAlloc(&dSrc, bytes) != CUDA_SUCCESS ||
        cuMemAlloc(&dDst, bytes) != CUDA_SUCCESS) {
        std::printf("cuMemAlloc failed\n"); return 1;
    }
    cuMemcpyHtoD(dSrc, srcPix.data(), bytes);

    std::vector<float> outPix(srcPix.size());
    int fails = 0; double worst = 0;
    for (size_t ci = 0; ci < cases.size(); ++ci) {
        const Case& c = cases[ci];
        bool hasTemp = c.lum[0] != '\0';

        // compile the four channels (+ the temp) against the var names
        Program chan[4], temp; std::string err;
        const char* ex[4] = { c.r, c.g, c.b, c.a };
        for (int k = 0; k < 4; ++k)
            if (!chan[k].compile(ex[k], names, err)) { std::printf("compile fail %s: %s\n", ex[k], err.c_str()); return 1; }
        if (hasTemp && !temp.compile(c.lum, names, err)) { std::printf("temp fail: %s\n", err.c_str()); return 1; }

        // build CUDA source exactly as Expression.cpp::renderCuda does
        std::string cbody[4] = { chan[0].emitC(), chan[1].emitC(), chan[2].emitC(), chan[3].emitC() };
        std::vector<std::pair<int,std::string> > temps;
        if (hasTemp) temps.push_back(std::make_pair(LUM, temp.emitC()));
        std::string cu = buildCudaPixelKernel(cbody, temps, nVars);

        // clear dst on device, dispatch through the production bridge
        cuMemsetD8(dDst, 0, bytes);
        CudaImageDesc sd, dd;
        sd.buffer = (void*)(uintptr_t)dSrc; sd.x1 = 0; sd.y1 = 0; sd.rowFloats = W*nComps;
        dd.buffer = (void*)(uintptr_t)dDst; dd.x1 = 0; dd.y1 = 0; dd.rowFloats = W*nComps;
        if (!cudaRender((void*)stream, sd, /*hasSrc*/1, dd,
                        /*rw*/0, 0, W, H, nComps, (float)W, (float)H, frame, cu, err)) {
            std::printf("cudaRender failed [case %zu]: %s\n", ci, err.c_str());
            return 1;
        }
        cuMemcpyDtoH(outPix.data(), dDst, bytes);

        // CPU reference: replicate ExpressionProcessor's binding with eval()
        const float* gpu = outPix.data();
        double halfW = W*0.5, halfH = H*0.5, invHalfW = halfW != 0 ? 1.0/halfW : 0.0;
        double localworst = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const float* sp = &srcPix[((size_t)y*W + x)*nComps];
                std::vector<double> v(nVars, 0.0);
                v[R]=sp[0]; v[G]=sp[1]; v[B]=sp[2]; v[A]=sp[3];
                v[X]=x; v[Y]=y;
                v[CX]=(x-halfW)*invHalfW; v[CY]=(y-halfH)*invHalfW;
                v[WIDTH]=W; v[HEIGHT]=H; v[FRAME]=frame;
                if (hasTemp) v[LUM] = temp.eval(&v[0]);
                double want[4] = { chan[0].eval(&v[0]), chan[1].eval(&v[0]),
                                   chan[2].eval(&v[0]), chan[3].eval(&v[0]) };
                const float* gp = &gpu[((size_t)y*W + x)*nComps];
                for (int k = 0; k < 4; ++k) {
                    double e = std::fabs(want[k] - (double)gp[k]);
                    e = std::min(e, e / (std::fabs(want[k]) + 1e-9));
                    localworst = std::max(localworst, e);
                }
            }
        }
        worst = std::max(worst, localworst);
        const char* tag = localworst <= 1e-4 ? "ok  " : "FAIL";
        if (localworst > 1e-4) ++fails;
        std::printf("  %s [r=%-8s g=%-8s b=%-10s%s] worst=%.3g\n",
                    tag, c.r, c.g, c.b, hasTemp ? " +lum" : "", localworst);
    }

    cuMemFree(dSrc); cuMemFree(dDst);
    cuStreamDestroy(stream); cuCtxDestroy(ctx);

    std::printf("\ncuda pixel-path: %zu cases on GPU, worst err %.3g\n", cases.size(), worst);
    std::printf(fails ? "FAIL\n" : "ALL PASS (GPU pixel path matches the CPU binding)\n");
    return fails ? 1 : 0;
}
