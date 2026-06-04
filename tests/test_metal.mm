// test_metal.mm
// -----------------------------------------------------------------------------
// Real-GPU differential test for the Metal back-end of the AST->kernel
// transpiler. It is the GPU sibling of tests/test_transpile.cpp: for the same
// expression battery it compiles each to an AST, emits the portable kernel body
// (Program::emitC()), wraps the bodies in MSL `kernel void gen_N` functions via
// ExprKernelMetal.h, compiles that MSL on a real Metal device at runtime
// (newLibraryWithSource:), dispatches one thread per random input vector, and
// diffs the GPU result against the CPU evaluator eval().
//
// Metal is float-only, so unlike test_transpile.cpp (double-vs-double, machine
// epsilon) this measures a genuine float-vs-double gap. We report it honestly:
//   - deterministic math must match to a tight float tolerance (1e-3), with a
//     tiny allowance for inputs that straddle a decision boundary (floor/step/
//     comparisons can round to opposite sides in float vs double);
//   - the fract-of-large-product hash in noise()/random()/fBm/turbulence is
//     inherently precision-sensitive and only reported, never failed.
//
// Modes:
//   test_metal            run on the GPU and diff (SKIP+exit 0 if no device,
//                         so CI on GPU-less macOS runners stays green).
//   test_metal --emit F   write the generated MSL to F and exit (no device
//                         needed) — used by CI for an offline `xcrun metal`
//                         compile-check.
//
// Build (from repo root):
//   clang++ -std=c++17 -ObjC++ -O2 -I ofx/Expression tests/test_metal.mm \
//       -framework Metal -framework Foundation -o /tmp/tm && /tmp/tm
// -----------------------------------------------------------------------------
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "ExprEval.h"
#include "ExprKernelMetal.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

using namespace expreval;

// The plugin's fixed predefined variables, in slot order (matches Expression.cpp
// and test_transpile.cpp). The battery uses no temps, so nVars == 11.
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

// A "hash-based" expression depends on the fract-of-large-product noise/random
// hash, whose last bits diverge between float and double. Reported, never failed.
static bool isHashBased(const std::string& e) {
    return e.find("noise") != std::string::npos ||
           e.find("random") != std::string::npos ||
           e.find("fBm")    != std::string::npos ||
           e.find("turbulence") != std::string::npos;
}

int main(int argc, char** argv) {
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
    const std::string msl = buildMetalScalarKernels(bodies, NVARS);

    // ---- --emit mode: write the MSL and exit (no GPU needed) ----
    if (argc >= 3 && std::string(argv[1]) == "--emit") {
        FILE* f = std::fopen(argv[2], "w");
        if (!f) { std::printf("cannot open %s\n", argv[2]); return 1; }
        std::fwrite(msl.data(), 1, msl.size(), f);
        std::fclose(f);
        std::printf("wrote %zu bytes of MSL (%zu kernels) to %s\n",
                    msl.size(), exprs.size(), argv[2]);
        return 0;
    }

    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            std::printf("SKIP: no Metal device available (GPU-less runner)\n");
            return 0;   // not a failure — CI on headless macOS has no GPU
        }
        std::printf("Metal device: %s\n", [[dev name] UTF8String]);

        NSError* err = nil;
        NSString* src = [NSString stringWithUTF8String:msl.c_str()];
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
        if (!lib) {
            std::printf("MSL COMPILE FAILED:\n%s\n",
                        [[err localizedDescription] UTF8String]);
            return 1;
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];

        // ---- random inputs: float bits, so CPU and GPU see identical values ----
        // (CPU evals the widened float, isolating kernel arithmetic from input
        //  quantisation.) Same ranges/seed spirit as test_transpile.cpp.
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
        id<MTLBuffer> bin  = [dev newBufferWithBytes:vin.data()
                                              length:vin.size()*sizeof(float)
                                             options:MTLResourceStorageModeShared];
        id<MTLBuffer> bout = [dev newBufferWithLength:M*sizeof(float)
                                             options:MTLResourceStorageModeShared];
        uint32_t count = (uint32_t)M;
        id<MTLBuffer> bcnt = [dev newBufferWithBytes:&count length:sizeof(count)
                                             options:MTLResourceStorageModeShared];

        const double DET_TOL = 1e-3;       // float-precision tolerance, deterministic math
        const double BAD_FRAC = 0.01;      // <=1% may straddle a decision boundary
        int fails = 0;
        double worstDet = 0.0, worstHash = 0.0;

        for (size_t i = 0; i < exprs.size(); ++i) {
            NSString* fn = [NSString stringWithFormat:@"gen_%zu", i];
            id<MTLFunction> f = [lib newFunctionWithName:fn];
            if (!f) { std::printf("missing kernel gen_%zu\n", i); return 1; }
            id<MTLComputePipelineState> pso =
                [dev newComputePipelineStateWithFunction:f error:&err];
            if (!pso) {
                std::printf("pipeline gen_%zu failed: %s\n", i,
                            [[err localizedDescription] UTF8String]);
                return 1;
            }
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:bin  offset:0 atIndex:0];
            [enc setBuffer:bout offset:0 atIndex:1];
            [enc setBuffer:bcnt offset:0 atIndex:2];
            NSUInteger tg = pso.maxTotalThreadsPerThreadgroup;
            if (tg > (NSUInteger)M) tg = M;
            [enc dispatchThreads:MTLSizeMake(M,1,1)
              threadsPerThreadgroup:MTLSizeMake(tg,1,1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];

            const float* gpu = (const float*)[bout contents];
            double localworst = 0.0; int bad = 0; int hashBad = 0;
            for (int t = 0; t < M; ++t) {
                const float* vf = &vin[(size_t)t * NVARS];
                double vd[NVARS];
                for (int k = 0; k < NVARS; ++k) vd[k] = (double)vf[k];   // identical bits
                double a = progs[i].eval(vd);
                double b = (double)gpu[t];
                bool af = std::isfinite(a), bf = std::isfinite(b);
                double e;
                if (!af || !bf) { e = (af != bf) ? 1e9 : 0.0; }
                else { double abse = std::fabs(a-b); e = std::min(abse, abse/(std::fabs(a)+1e-9)); }
                localworst = std::max(localworst, e);
                if (e > DET_TOL) ++bad;
                if (e > 1e-2) ++hashBad;        // "materially different" pixel
            }
            bool hash = isHashBased(exprs[i]);
            double badFrac = bad / (double)M;
            if (hash) {
                worstHash = std::max(worstHash, localworst);
                // distinguish rare cell-boundary straddles (value-noise) from
                // broadly-divergent large-argument sin (random()).
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

        std::printf("\nmetal differential: %zu expressions x %d inputs on GPU\n",
                    exprs.size(), M);
        std::printf("  deterministic worst err: %.3g (tol %.0e)\n", worstDet, DET_TOL);
        std::printf("  hash-based   worst err: %.3g (float-precision, expected)\n", worstHash);
        std::printf(fails ? "FAIL\n"
                          : "ALL PASS (GPU matches CPU to float precision)\n");
        return fails ? 1 : 0;
    }
}
