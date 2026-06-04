// test_cuda.cpp
// -----------------------------------------------------------------------------
// NVRTC compile-check for the CUDA back-end of the AST->kernel transpiler. The
// CUDA analogue of the `xcrun metal` compile-check (and the planned counterpart
// to test_metal's GPU run): it builds the CUDA kernel source from the emitC()
// bodies — both the scalar `gen_N` battery and a representative per-pixel
// `exprKernel` — and compiles each to PTX with NVRTC. No GPU is required (NVRTC
// is a runtime compiler library), so this gates CUDA codegen correctness in CI on
// a GPU-less Linux runner. Runtime numeric parity vs the CPU oracle runs later on
// an actual NVIDIA box (see PASSOFF Phase 3).
//
// Build (Linux, with the CUDA toolkit / NVRTC installed):
//   c++ -std=c++11 -O2 -I ofx/Expression -I/usr/local/cuda/include \
//       tests/test_cuda.cpp -o test_cuda -lnvrtc && ./test_cuda
// (CI uses the distro CUDA package; adjust -I / -L to taste.)
// -----------------------------------------------------------------------------
#include "ExprEval.h"
#include "ExprKernelCuda.h"

#include <nvrtc.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace expreval;

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

// Compile one CUDA source string with NVRTC; print the log and return success.
static bool nvrtcCheck(const char* label, const std::string& src) {
    nvrtcProgram prog;
    nvrtcResult rc = nvrtcCreateProgram(&prog, src.c_str(), "expr.cu", 0, nullptr, nullptr);
    if (rc != NVRTC_SUCCESS) {
        std::printf("  %s: nvrtcCreateProgram failed: %s\n", label, nvrtcGetErrorString(rc));
        return false;
    }
    const char* opts[] = { "--gpu-architecture=compute_70" };
    rc = nvrtcCompileProgram(prog, 1, opts);

    size_t logSize = 0;
    nvrtcGetProgramLogSize(prog, &logSize);
    std::string log(logSize ? logSize - 1 : 0, '\0');
    if (logSize > 1) nvrtcGetProgramLog(prog, &log[0]);

    bool ok = (rc == NVRTC_SUCCESS);
    if (!ok) {
        std::printf("  %s: COMPILE FAILED (%s)\n%s\n", label, nvrtcGetErrorString(rc), log.c_str());
    } else {
        size_t ptxSize = 0; nvrtcGetPTXSize(prog, &ptxSize);
        std::printf("  %s: OK (%zu bytes PTX)%s\n", label, ptxSize,
                    log.empty() ? "" : " [with warnings]");
        if (!log.empty()) std::printf("    log: %s\n", log.c_str());
    }
    nvrtcDestroyProgram(&prog);
    return ok;
}

int main() {
    int major = 0, minor = 0;
    nvrtcVersion(&major, &minor);
    std::printf("NVRTC %d.%d\n", major, minor);

    const std::vector<std::string> exprs = exprBattery();
    const std::vector<std::string> names =
        { "r","g","b","a","x","y","cx","cy","width","height","frame" };

    // compile all expressions -> AST -> emitted bodies
    std::vector<std::string> bodies(exprs.size());
    for (size_t i = 0; i < exprs.size(); ++i) {
        std::string err; Program p;
        if (!p.compile(exprs[i], names, err)) {
            std::printf("expr compile FAIL [%s]: %s\n", exprs[i].c_str(), err.c_str());
            return 1;
        }
        bodies[i] = p.emitC();
    }

    int fails = 0;

    // 1) the scalar differential-kernel battery
    std::printf("scalar kernels (%zu expressions):\n", exprs.size());
    if (!nvrtcCheck("gen_* battery", buildCudaScalarKernels(bodies, NVARS))) ++fails;

    // 2) a representative per-pixel kernel (channels + a temp slot)
    {
        std::vector<std::string> pnames = names; pnames.push_back("lum");
        Program chan[4], temp; std::string err;
        const char* ex[4] = { "lum", "smoothstep(0,1,r)", "min(r,g,b)", "a" };
        for (int k = 0; k < 4; ++k) chan[k].compile(ex[k], pnames, err);
        temp.compile("0.2126*r+0.7152*g+0.0722*b", pnames, err);
        std::string cbody[4] = { chan[0].emitC(), chan[1].emitC(), chan[2].emitC(), chan[3].emitC() };
        std::vector<std::pair<int,std::string> > temps;
        temps.push_back(std::make_pair(11, temp.emitC()));
        std::printf("pixel kernel (4 channels + 1 temp):\n");
        if (!nvrtcCheck("exprKernel", buildCudaPixelKernel(cbody, temps, 12))) ++fails;
    }

    std::printf(fails ? "\nFAIL\n" : "\nALL PASS (CUDA kernels compile to PTX via NVRTC)\n");
    return fails ? 1 : 0;
}
