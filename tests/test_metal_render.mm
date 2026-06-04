// test_metal_render.mm
// -----------------------------------------------------------------------------
// End-to-end test of the production Metal *pixel* path: buildMetalPixelKernel()
// + metalRender() (ExprMetal.mm). Where test_metal.mm checks the scalar emitted
// bodies, this checks the per-pixel wrapper the OFX render path actually runs —
// source read from a MTLBuffer, the predefined-variable binding (x,y bottom-left;
// centred aspect-preserved cx,cy; width/height/frame), temp evaluation, and the
// channel-remapped write — by allocating real MTLBuffers, dispatching on the GPU,
// and diffing against the same binding evaluated by the CPU Program::eval().
//
// It mirrors ExpressionProcessor in Expression.cpp (bounds origin, var slots,
// float write). If those drift, this catches it. SKIPs (exit 0) with no device.
//
// Build (from repo root):
//   clang++ -std=c++17 -ObjC++ -O2 -I ofx/Expression \
//       tests/test_metal_render.mm ofx/Expression/ExprMetal.mm \
//       -framework Metal -framework Foundation -o /tmp/tmr && /tmp/tmr
// -----------------------------------------------------------------------------
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "ExprEval.h"
#include "ExprKernelMetal.h"
#include "ExprMetal.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

using namespace expreval;

// Variable slots — must match Expression.cpp's enum order.
enum { R=0, G, B, A, X, Y, CX, CY, WIDTH, HEIGHT, FRAME, FIXED };

int main(int argc, char** argv) {
    // --emit F: write a representative production pixel kernel and exit (no GPU),
    // so CI can offline-compile it with `xcrun metal`.
    if (argc >= 3 && std::string(argv[1]) == "--emit") {
        std::vector<std::string> names =
            { "r","g","b","a","x","y","cx","cy","width","height","frame", "lum" };
        Program chan[4], temp; std::string err;
        const char* ex[4] = { "lum", "smoothstep(0,1,r)", "min(r,g,b)", "a" };
        for (int k = 0; k < 4; ++k) chan[k].compile(ex[k], names, err);
        temp.compile("0.2126*r+0.7152*g+0.0722*b", names, err);
        std::string cbody[4] = { chan[0].emitC(), chan[1].emitC(), chan[2].emitC(), chan[3].emitC() };
        std::vector<std::pair<int,std::string> > temps;
        temps.push_back(std::make_pair(11, temp.emitC()));
        std::string msl = buildMetalPixelKernel(cbody, temps, 12);
        FILE* f = std::fopen(argv[2], "w");
        if (!f) { std::printf("cannot open %s\n", argv[2]); return 1; }
        std::fwrite(msl.data(), 1, msl.size(), f); std::fclose(f);
        std::printf("wrote pixel kernel (%zu bytes) to %s\n", msl.size(), argv[2]);
        return 0;
    }
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { std::printf("SKIP: no Metal device\n"); return 0; }
        std::printf("Metal device: %s\n", [[dev name] UTF8String]);
        id<MTLCommandQueue> q = [dev newCommandQueue];

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

        id<MTLBuffer> sbuf = [dev newBufferWithBytes:srcPix.data()
                                              length:srcPix.size()*sizeof(float)
                                             options:MTLResourceStorageModeShared];
        id<MTLBuffer> dbuf = [dev newBufferWithLength:srcPix.size()*sizeof(float)
                                             options:MTLResourceStorageModeShared];

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

            // build MSL exactly as Expression.cpp does
            std::string cbody[4] = { chan[0].emitC(), chan[1].emitC(), chan[2].emitC(), chan[3].emitC() };
            std::vector<std::pair<int,std::string> > temps;
            if (hasTemp) temps.push_back(std::make_pair(LUM, temp.emitC()));
            std::string msl = buildMetalPixelKernel(cbody, temps, nVars);

            // clear dst, dispatch on the GPU through the production bridge
            std::memset([dbuf contents], 0, srcPix.size()*sizeof(float));
            MetalImageDesc sd, dd;
            sd.buffer = (void*)sbuf; sd.x1 = 0; sd.y1 = 0; sd.rowFloats = W*nComps;
            dd.buffer = (void*)dbuf; dd.x1 = 0; dd.y1 = 0; dd.rowFloats = W*nComps;
            if (!metalRender((void*)q, sd, /*hasSrc*/1, dd,
                             /*rw*/0, 0, W, H, nComps, (float)W, (float)H, frame, msl, err)) {
                std::printf("metalRender failed [case %zu]: %s\n", ci, err.c_str());
                return 1;
            }

            // CPU reference: replicate ExpressionProcessor's binding with eval()
            const float* gpu = (const float*)[dbuf contents];
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

        std::printf("\nmetal pixel-path: %zu cases on GPU, worst err %.3g\n", cases.size(), worst);
        std::printf(fails ? "FAIL\n" : "ALL PASS (GPU pixel path matches the CPU binding)\n");
        return fails ? 1 : 0;
    }
}
