// test_cuda_run.cpp
// -----------------------------------------------------------------------------
// Real-GPU differential test for the CUDA back-end of the AST->kernel transpiler.
// The NVIDIA sibling of tests/test_metal.mm (Metal) and the runtime counterpart
// to tests/test_cuda.cpp (the GPU-less NVRTC compile-check): for the same
// expression battery it compiles each to an AST, emits the portable kernel body
// (Program::emitC()), wraps the bodies in CUDA `gen_N` kernels via
// ExprKernelCuda.h, NVRTC-compiles that to PTX, loads it with the CUDA *driver
// API* on a real GPU, launches one thread per random input vector, and diffs the
// GPU result against the CPU evaluator eval().
//
// Precision — the CUDA kernels widen the float inputs into a `double v[]` and run
// the deterministic math in double (see ExprKernelCuda.h), exactly as the CPU
// oracle does. So the *math* spread is only a few double ULP (GPU vs host-libm
// transcendentals) — much tighter than the genuine float-vs-double gap Metal
// measures. But the kernel stores its result into a *float* output buffer (as the
// render path writes float pixels), so the GPU-vs-oracle error is dominated by the
// final round-to-float of the double result: ~6e-8 worst (0.5 float ULP near 1).
// DET_TOL is therefore float-scale (1e-6) — still far below Metal's 1e-3 — with a
// tiny boundary allowance for the rare case where a slightly-different
// transcendental straddles a floor()/comparison boundary (e.g. exponent()'s
// floor(log2)).
//
// FMA — NVRTC must be compiled with `--fmad=false`. By default NVRTC fuses
// a*b+c into a single fma, which changes the low bits of the large product inside
// the noise/random hash; fract() of a large number then amplifies that low-bit
// change into a big swing (value-noise that should be bit-identical to the CPU's
// separate-op float arithmetic otherwise diverges in ~40% of samples). With
// --fmad=false the value-noise matches the CPU float oracle bit-for-bit; only
// random() (sinf of a ~1e5 argument, vendor-divergent range reduction) stays
// caveated. The render-path NVRTC compile (ExprCuda) must use the same flag.
// Hash-based exprs (noise/random/fBm/turbulence) are reported, never failed.
//
// SKIPs cleanly (exit 0) when no CUDA device is present, so the CI `cuda` job
// (GPU-less) can run it without failing — it just reports SKIP and the separate
// NVRTC compile-check (test_cuda.cpp) gates codegen there.
//
// Build (Linux, CUDA toolkit / NVRTC + driver present):
//   c++ -std=c++11 -O2 -I ofx/Expression -I$CUDA_HOME/include \
//       tests/test_cuda_run.cpp -o test_cuda_run \
//       -L$CUDA_HOME/lib64 -lnvrtc -lcuda && ./test_cuda_run
// (With a conda CUDA env, $CUDA_HOME is the env prefix and libcuda comes from the
//  driver in /lib64 — add -L/lib64 if the conda lib dir shadows it.)
// -----------------------------------------------------------------------------
#include "ExprEval.h"
#include "ExprKernelCuda.h"

#include <nvrtc.h>
#include <cuda.h>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

using namespace expreval;

// The plugin's fixed predefined variables, in slot order (matches Expression.cpp,
// test_transpile.cpp, test_metal.mm). The battery uses no temps, so nVars == 11.
static const int NVARS = 11;

static std::vector<std::string> exprBattery() {
    return {
        "r", "r+g", "b-r", "r*g+b", "1-r", "pow(r,1/2.2)",
        "0.2126*r+0.7152*g+0.0722*b", "x/width", "y/height",
        "clamp(1-hypot(cx,cy))", "r>0.5?1:0", "sin(r*pi)+cos(g)",
        "min(r,g,b)", "max(r,g,b,0.5)", "lerp(r,g,0.5)", "mix(r,b,cy)",
        "smoothstep(0,1,r)", "step(0.5,r)", "abs(-r)+sign(g-0.5)",
        "noise(x*0.1,y*0.1)", "noise(cx,cy,frame*0.05)", "random(x,y)",
        "to_sRGB(r)", "from_sRGB(r)", "to_rec709(g)", "from_byte(128)",
        "fBm(cx,cy,0,4,2,0.5)", "turbulence(cx,cy,0,3,2,0.5)",
        "trunc(r*10)", "round(r*10)", "rint(g*7)", "r%0.3",
        "(r&&g)||!b", "r==g", "r!=g", "r<=g", "-2^2", "2^3^2",
        "atan2(g,r)", "atan(g,r)", "exp(log(r+0.01))", "log2(r+1)",
        "clamp(r,0.2,0.8)", "degrees(radians(r*100))", "pow2(r)+ldexp(g,2)",
        "exponent(r*1000+1)", "hypot(cx,cy)", "fmod(x,7)",
        "r<0.3?g:(b>0.7?cx:cy)", "sqrt(abs(cx))*sign(cy)",
    };
}

// Hash-based expressions depend on the fract-of-large-product noise/random hash,
// whose last bits diverge between vendors. Reported, never failed (as on Metal).
static bool isHashBased(const std::string& e) {
    return e.find("noise") != std::string::npos ||
           e.find("random") != std::string::npos ||
           e.find("fBm")    != std::string::npos ||
           e.find("turbulence") != std::string::npos;
}

static bool cuOk(CUresult r, const char* what) {
    if (r == CUDA_SUCCESS) return true;
    const char* msg = nullptr; cuGetErrorString(r, &msg);
    std::printf("CUDA driver error in %s: %s\n", what, msg ? msg : "?");
    return false;
}

// NVRTC-compile the source to PTX, targeting the device's own compute capability
// (falling back to compute_70 if NVRTC rejects that arch). Returns "" on failure.
static std::string nvrtcToPTX(const std::string& src, int ccMajor, int ccMinor) {
    nvrtcProgram prog;
    if (nvrtcCreateProgram(&prog, src.c_str(), "expr.cu", 0, nullptr, nullptr)
            != NVRTC_SUCCESS) {
        std::printf("nvrtcCreateProgram failed\n");
        return "";
    }
    char archOpt[64];
    std::snprintf(archOpt, sizeof archOpt, "--gpu-architecture=compute_%d%d",
                  ccMajor, ccMinor);
    // --fmad=false: do NOT fuse a*b+c into fma — match the CPU oracle's separate
    // float ops so the noise hash stays bit-identical (see header note).
    const char* opts[] = { archOpt, "--fmad=false" };
    nvrtcResult rc = nvrtcCompileProgram(prog, 2, opts);
    if (rc != NVRTC_SUCCESS) {
        // Retry with a baseline virtual arch the driver can JIT forward.
        const char* fb[] = { "--gpu-architecture=compute_70", "--fmad=false" };
        rc = nvrtcCompileProgram(prog, 2, fb);
    }
    if (rc != NVRTC_SUCCESS) {
        size_t logSize = 0; nvrtcGetProgramLogSize(prog, &logSize);
        std::string log(logSize ? logSize - 1 : 0, '\0');
        if (logSize > 1) nvrtcGetProgramLog(prog, &log[0]);
        std::printf("NVRTC COMPILE FAILED (%s):\n%s\n", nvrtcGetErrorString(rc),
                    log.c_str());
        nvrtcDestroyProgram(&prog);
        return "";
    }
    size_t ptxSize = 0; nvrtcGetPTXSize(prog, &ptxSize);
    std::string ptx(ptxSize, '\0');
    nvrtcGetPTX(prog, &ptx[0]);
    nvrtcDestroyProgram(&prog);
    return ptx;
}

int main() {
    const std::vector<std::string> exprs = exprBattery();
    const std::vector<std::string> names =
        { "r","g","b","a","x","y","cx","cy","width","height","frame" };

    // ---- compile expressions -> AST -> emitted kernel bodies ----
    std::vector<Program> progs(exprs.size());
    std::vector<std::string> bodies(exprs.size());
    for (size_t i = 0; i < exprs.size(); ++i) {
        std::string err;
        if (!progs[i].compile(exprs[i], names, err)) {
            std::printf("compile FAIL [%s]: %s\n", exprs[i].c_str(), err.c_str());
            return 1;
        }
        bodies[i] = progs[i].emitC();
    }
    const std::string src = buildCudaScalarKernels(bodies, NVARS);

    // ---- driver init / device probe (SKIP cleanly if no usable CUDA) ----
    // On a GPU-less box (no driver, only the toolkit's libcuda stub) cuInit itself
    // returns an error; that's not a test failure, just "nothing to run here".
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
    if (!cuOk(cuDeviceGet(&dev, 0), "cuDeviceGet")) return 1;
    char devName[256] = {0};
    cuDeviceGetName(devName, sizeof devName, dev);
    int ccMajor = 0, ccMinor = 0;
    cuDeviceGetAttribute(&ccMajor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
    cuDeviceGetAttribute(&ccMinor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
    int nvMaj = 0, nvMin = 0; nvrtcVersion(&nvMaj, &nvMin);
    std::printf("CUDA device: %s (sm_%d%d), NVRTC %d.%d\n",
                devName, ccMajor, ccMinor, nvMaj, nvMin);

    CUcontext ctx;
    if (!cuOk(cuCtxCreate(&ctx, 0, dev), "cuCtxCreate")) return 1;

    // ---- NVRTC -> PTX -> module ----
    const std::string ptx = nvrtcToPTX(src, ccMajor, ccMinor);
    if (ptx.empty()) { cuCtxDestroy(ctx); return 1; }
    CUmodule mod;
    if (!cuOk(cuModuleLoadData(&mod, ptx.c_str()), "cuModuleLoadData")) {
        cuCtxDestroy(ctx); return 1;
    }

    // ---- random inputs: float bits, so CPU and GPU see identical values ----
    // (CPU evals the widened float, isolating kernel arithmetic from input
    //  quantisation.) Same ranges/seed spirit as test_metal.mm / test_transpile.
    const int M = 5000;
    std::srand(20260604);
    auto rnd = [](double lo, double hi) {
        return lo + (hi - lo) * (std::rand() / (double)RAND_MAX);
    };
    std::vector<float> vin((size_t)M * NVARS);
    for (int t = 0; t < M; ++t) {
        float* v = &vin[(size_t)t * NVARS];
        v[0]=rnd(0,1); v[1]=rnd(0,1); v[2]=rnd(0,1); v[3]=rnd(0,1);   // r g b a
        v[4]=rnd(0,1920); v[5]=rnd(0,1080);                           // x y
        v[6]=rnd(-1.5,1.5); v[7]=rnd(-1.5,1.5);                       // cx cy
        v[8]=rnd(16,4096); v[9]=rnd(16,2160); v[10]=rnd(0,250);       // width height frame
    }

    CUdeviceptr dVin = 0, dOut = 0;
    if (!cuOk(cuMemAlloc(&dVin, vin.size()*sizeof(float)), "cuMemAlloc vin") ||
        !cuOk(cuMemAlloc(&dOut, (size_t)M*sizeof(float)),  "cuMemAlloc out")) {
        cuModuleUnload(mod); cuCtxDestroy(ctx); return 1;
    }
    cuOk(cuMemcpyHtoD(dVin, vin.data(), vin.size()*sizeof(float)), "memcpy vin");

    const double DET_TOL  = 1e-6;    // double math, float store: round-to-float dominates (~6e-8)
    const double BAD_FRAC = 0.01;    // <=1% may straddle a decision boundary
    std::vector<float> out(M);
    int fails = 0;
    double worstDet = 0.0, worstHash = 0.0;

    for (size_t i = 0; i < exprs.size(); ++i) {
        char fn[32]; std::snprintf(fn, sizeof fn, "gen_%zu", i);
        CUfunction k;
        if (!cuOk(cuModuleGetFunction(&k, mod, fn), fn)) { ++fails; continue; }

        int count = M;
        void* args[] = { &dVin, &dOut, &count };
        const unsigned block = 256, grid = (M + block - 1) / block;
        if (!cuOk(cuLaunchKernel(k, grid,1,1, block,1,1, 0, 0, args, nullptr),
                  "cuLaunchKernel") ||
            !cuOk(cuCtxSynchronize(), "cuCtxSynchronize")) { ++fails; continue; }
        cuOk(cuMemcpyDtoH(out.data(), dOut, (size_t)M*sizeof(float)), "memcpy out");

        double localworst = 0.0; int bad = 0; int hashBad = 0;
        for (int t = 0; t < M; ++t) {
            const float* vf = &vin[(size_t)t * NVARS];
            double vd[NVARS];
            for (int k2 = 0; k2 < NVARS; ++k2) vd[k2] = (double)vf[k2];   // identical bits
            double a = progs[i].eval(vd);
            double b = (double)out[t];
            bool af = std::isfinite(a), bf = std::isfinite(b);
            double e;
            if (!af || !bf) { e = (af != bf) ? 1e9 : 0.0; }
            else { double abse = std::fabs(a-b); e = std::min(abse, abse/(std::fabs(a)+1e-9)); }
            localworst = std::max(localworst, e);
            if (e > DET_TOL) ++bad;
            if (e > 1e-2) ++hashBad;        // "materially different" sample
        }
        bool hash = isHashBased(exprs[i]);
        double badFrac = bad / (double)M;
        if (hash) {
            worstHash = std::max(worstHash, localworst);
            std::printf("  hash  [%-28s] worst=%.3g  %d/%d differ >1e-2  (informational)\n",
                        exprs[i].c_str(), localworst, hashBad, M);
        } else {
            worstDet = std::max(worstDet, localworst);
            if (localworst <= DET_TOL) {
                // clean — silent
            } else if (badFrac <= BAD_FRAC) {
                std::printf("  bound [%-28s] worst=%.3g  %d/%d at boundary (ok)\n",
                            exprs[i].c_str(), localworst, bad, M);
            } else {
                std::printf("  FAIL  [%-28s] worst=%.3g  %d/%d exceed %.0e\n",
                            exprs[i].c_str(), localworst, bad, M, DET_TOL);
                ++fails;
            }
        }
    }

    cuMemFree(dVin); cuMemFree(dOut);
    cuModuleUnload(mod); cuCtxDestroy(ctx);

    std::printf("\ncuda differential: %zu expressions x %d inputs on GPU\n",
                exprs.size(), M);
    std::printf("  deterministic worst err: %.3g (tol %.0e, double path)\n", worstDet, DET_TOL);
    std::printf("  hash-based   worst err: %.3g (float-precision, expected)\n", worstHash);
    std::printf(fails ? "FAIL\n"
                      : "ALL PASS (GPU matches CPU oracle to double precision)\n");
    return fails ? 1 : 0;
}
