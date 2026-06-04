// test_transpile.cpp
// -----------------------------------------------------------------------------
// Differential test for the AST -> kernel transpiler (Program::emitC()).
//
// For a battery of expressions it: compiles each to an AST, emits the C kernel
// body, writes one generated .cpp of `extern "C" double gen_N(const double* v)`
// functions (including ExprKernel.h), compiles that to a shared object, dlopens
// it, then compares the transpiled kernel against the CPU evaluator eval() over
// thousands of random input vectors. If they match, the same emitted body is
// trustworthy as a CUDA/Metal/GLSL kernel (given the matching prelude).
//
// Needs no GPU. Run from the repo root:
//   c++ -std=c++11 -O2 -I ofx/Expression tests/test_transpile.cpp -o /tmp/tt && /tmp/tt
// -----------------------------------------------------------------------------
#include "ExprEval.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <dlfcn.h>

using namespace expreval;

int main() {
    // the plugin's fixed predefined variables, in slot order
    std::vector<std::string> names =
        { "r","g","b","a","x","y","cx","cy","width","height","frame" };

    std::vector<std::string> exprs = {
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

    // ---- build + compile the generated kernels ----
    std::vector<Program> progs(exprs.size());
    std::string gen = "#include \"ExprKernel.h\"\n";
    for (size_t i = 0; i < exprs.size(); ++i) {
        std::string err;
        if (!progs[i].compile(exprs[i], names, err)) {
            std::printf("compile FAIL [%s]: %s\n", exprs[i].c_str(), err.c_str());
            return 1;
        }
        gen += "extern \"C\" double gen_" + std::to_string(i) +
               "(const double* v){ return " + progs[i].emitC() + "; }\n";
    }
    { FILE* f = std::fopen("/tmp/expr_gen.cpp", "w"); std::fwrite(gen.data(), 1, gen.size(), f); std::fclose(f); }

    int rc = std::system("c++ -std=c++11 -O2 -I ofx/Expression -shared -fPIC "
                         "/tmp/expr_gen.cpp -o /tmp/expr_gen.so 2>/tmp/expr_gen.err");
    if (rc != 0) {
        std::printf("generated-kernel compile FAILED:\n");
        std::system("cat /tmp/expr_gen.err");
        return 1;
    }
    void* h = dlopen("/tmp/expr_gen.so", RTLD_NOW);
    if (!h) { std::printf("dlopen failed: %s\n", dlerror()); return 1; }
    typedef double (*Fn)(const double*);

    // ---- differential check vs eval() ----
    std::srand(20260604);
    auto rnd = [](double lo, double hi) { return lo + (hi - lo) * (std::rand() / (double)RAND_MAX); };

    int fails = 0; double worst = 0;
    for (size_t i = 0; i < exprs.size(); ++i) {
        Fn fn = (Fn)dlsym(h, ("gen_" + std::to_string(i)).c_str());
        if (!fn) { std::printf("dlsym fail %zu\n", i); return 1; }
        double localworst = 0;
        for (int t = 0; t < 5000; ++t) {
            double v[11];
            v[0]=rnd(0,1); v[1]=rnd(0,1); v[2]=rnd(0,1); v[3]=rnd(0,1);   // r g b a
            v[4]=rnd(0,1920); v[5]=rnd(0,1080);                           // x y
            v[6]=rnd(-1.5,1.5); v[7]=rnd(-1.5,1.5);                       // cx cy
            v[8]=rnd(16,4096); v[9]=rnd(16,2160); v[10]=rnd(0,250);       // width height frame
            double a = progs[i].eval(v), b = fn(v);
            double e;
            bool af = std::isfinite(a), bf = std::isfinite(b);
            if (!af || !bf) { e = (af != bf) ? 1e9 : 0.0; }              // both non-finite = OK
            else { double abse = std::fabs(a - b); e = std::min(abse, abse / (std::fabs(a) + 1e-9)); }
            localworst = std::max(localworst, e);
        }
        if (localworst > 1e-9) { std::printf("  DIFF [%s]  worst=%.3g\n", exprs[i].c_str(), localworst); ++fails; }
        worst = std::max(worst, localworst);
    }

    std::printf("\ntranspile differential: %zu expressions x 5000 inputs, worst err %.3g, %d failing\n",
                exprs.size(), worst, fails);
    std::printf(fails ? "FAIL\n" : "ALL PASS (transpiled kernels match the CPU evaluator)\n");
    return fails ? 1 : 0;
}
