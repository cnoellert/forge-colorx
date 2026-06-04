// ExprKernelMetal.h
// -----------------------------------------------------------------------------
// Metal (MSL) back-end for the transpiled expression kernel emitted by
// Program::emitC() (see ExprEval.h). This is the GPU sibling of ExprKernel.h:
// every exh_* helper here mirrors the SAME semantics as the matching helper in
// ExprKernel.h, so the emitted kernel *body* — shared verbatim across CPU, CUDA
// and Metal — produces the same result on the GPU as the CPU evaluator does.
//
// The one unavoidable difference: Metal has no `double`, so this prelude is
// single precision (`float`). The differential test tests/test_metal.mm runs the
// emitted kernels on a real Metal device and diffs them against the double CPU
// oracle eval(), reporting the float-precision gap honestly (deterministic math
// matches to ~1e-6; the fract-of-large-product hash in noise()/random() is the
// known precision-sensitive case — see that test's report).
//
// Two things live here:
//   1. kExprMetalPrelude  — the MSL #include + exh_* helper library as a string.
//   2. buildMetalScalarKernels() — wraps emitted bodies in `kernel void gen_N`
//      functions over a v[] input buffer (used by the differential test; the
//      production per-pixel kernel builder will reuse the same prelude).
// The prelude is a runtime string because the OFX Metal path compiles MSL at
// render via newLibraryWithSource: (cached per expression set).
// -----------------------------------------------------------------------------
#ifndef EXPR_KERNEL_METAL_H
#define EXPR_KERNEL_METAL_H

#include <string>
#include <vector>

namespace expreval {

// The MSL helper prelude. Float precision; metal:: builtins via `using namespace
// metal` cover the bare sin/cos/pow/fabs/fmin/fmax/fmod/hypot/log/... that the
// emitted body calls directly. Each exh_* matches ExprKernel.h line-for-line.
static const char* kExprMetalPrelude =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"// hypot is not in metal_stdlib; provide it under the name emitC() emits.\n"
"inline float hypot(float a, float b) { return sqrt(a * a + b * b); }\n"
"\n"
"inline float exh_log2(float x)      { return log(x) / log(2.0); }\n"
"inline float exh_trunc(float x)     { return x < 0.0 ? ceil(x) : floor(x); }\n"
"inline float exh_round(float x)     { return floor(x + 0.5); }\n"
"inline float exh_sign(float x)      { return (float)(((x > 0.0) ? 1 : 0) - ((x < 0.0) ? 1 : 0)); }\n"
"inline float exh_exponent(float x)  { return floor(log2(fabs(x) + 1e-30)); }\n"
"inline float exh_mantissa(float x)  { return x / exp2(floor(log2(fabs(x) + 1e-30))); }\n"
"inline float exh_pow2(float x)      { return x * x; }\n"
"inline float exh_step(float e, float x) { return x < e ? 0.0 : 1.0; }\n"
"inline float exh_ldexp(float a, float b){ return a * exp2(b); }\n"
"inline float exh_clamp1(float x)    { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }\n"
"inline float exh_clamp3(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }\n"
"inline float exh_lerp(float a, float b, float t)   { return a + (b - a) * t; }\n"
"inline float exh_smoothstep(float a, float b, float x) {\n"
"    float t = (x - a) / (b - a); t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t); return t * t * (3.0 - 2.0 * t);\n"
"}\n"
"\n"
"inline float exh_fract(float x) { return x - floor(x); }\n"
"inline float exh_hash3(float x, float y, float z) {\n"
"    float px = exh_fract(x * 0.3183099 + 0.1) * 17.0;\n"
"    float py = exh_fract(y * 0.3183099 + 0.1) * 17.0;\n"
"    float pz = exh_fract(z * 0.3183099 + 0.1) * 17.0;\n"
"    return exh_fract(px * py * pz * (px + py + pz));\n"
"}\n"
"inline float exh_vnoise(float x, float y, float z) {\n"
"    float ix = floor(x), iy = floor(y), iz = floor(z);\n"
"    float fx = x - ix, fy = y - iy, fz = z - iz;\n"
"    float ux = fx * fx * (3.0 - 2.0 * fx);\n"
"    float uy = fy * fy * (3.0 - 2.0 * fy);\n"
"    float uz = fz * fz * (3.0 - 2.0 * fz);\n"
"    float c000 = exh_hash3(ix,       iy,       iz),       c100 = exh_hash3(ix+1.0, iy,       iz);\n"
"    float c010 = exh_hash3(ix,       iy+1.0,   iz),       c110 = exh_hash3(ix+1.0, iy+1.0,   iz);\n"
"    float c001 = exh_hash3(ix,       iy,       iz+1.0),   c101 = exh_hash3(ix+1.0, iy,       iz+1.0);\n"
"    float c011 = exh_hash3(ix,       iy+1.0,   iz+1.0),   c111 = exh_hash3(ix+1.0, iy+1.0,   iz+1.0);\n"
"    float x00 = c000 + (c100 - c000) * ux, x10 = c010 + (c110 - c010) * ux;\n"
"    float x01 = c001 + (c101 - c001) * ux, x11 = c011 + (c111 - c011) * ux;\n"
"    float y0 = x00 + (x10 - x00) * uy, y1 = x01 + (x11 - x01) * uy;\n"
"    return y0 + (y1 - y0) * uz;\n"
"}\n"
"inline float exh_noise(float x, float y, float z) { return exh_vnoise(x, y, z) * 2.0 - 1.0; }\n"
"inline float exh_random(float x, float y, float z) {\n"
"    float s = sin(x * 12.9898 + y * 78.233 + z * 37.719) * 43758.5453;\n"
"    return exh_fract(s);\n"
"}\n"
"inline float exh_fbm(float x, float y, float z, float oct, float lac, float gain) {\n"
"    int o = (int)oct; float sum = 0.0, amp = 1.0, fr = 1.0;\n"
"    for (int i = 0; i < o && i < 32; ++i) { sum += amp * exh_noise(x*fr, y*fr, z*fr); fr *= lac; amp *= gain; }\n"
"    return sum;\n"
"}\n"
"inline float exh_turb(float x, float y, float z, float oct, float lac, float gain) {\n"
"    int o = (int)oct; float sum = 0.0, amp = 1.0, fr = 1.0;\n"
"    for (int i = 0; i < o && i < 32; ++i) { sum += amp * fabs(exh_noise(x*fr, y*fr, z*fr)); fr *= lac; amp *= gain; }\n"
"    return sum;\n"
"}\n"
"\n"
"inline float exh_to_srgb(float c)     { return (c <= 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0/2.4) - 0.055; }\n"
"inline float exh_from_srgb(float c)   { return (c <= 0.04045)   ? c / 12.92 : pow((c + 0.055)/1.055, 2.4); }\n"
"inline float exh_to_rec709(float c)   { return (c < 0.018) ? c * 4.5 : 1.099 * pow(c, 0.45) - 0.099; }\n"
"inline float exh_from_rec709(float c) { return (c < 0.081) ? c / 4.5 : pow((c + 0.099)/1.099, 1.0/0.45); }\n"
"inline float exh_from_byte(float x)   { return exh_from_srgb(x / 255.0); }\n"
"inline float exh_to_byte(float x)     { return exh_to_srgb(x) * 255.0; }\n";

// Wrap a set of emitted kernel bodies (Program::emitC() strings) as standalone
// `kernel void gen_N(...)` compute functions, each evaluating one expression
// over a packed input buffer of `nVars`-float vectors (one thread per vector).
// This is the form the differential test dispatches; the production per-pixel
// kernel reuses kExprMetalPrelude with a texture/buffer wrapper instead.
inline std::string buildMetalScalarKernels(const std::vector<std::string>& bodies, int nVars) {
    std::string s = kExprMetalPrelude;
    s += "\n";
    const std::string nv = std::to_string(nVars);
    for (size_t i = 0; i < bodies.size(); ++i) {
        const std::string id = std::to_string(i);
        s += "kernel void gen_" + id +
             "(device const float* vin [[buffer(0)]],"
             " device float* out [[buffer(1)]],"
             " constant uint& count [[buffer(2)]],"
             " uint tid [[thread_position_in_grid]]) {\n";
        s += "    if (tid >= count) return;\n";
        s += "    device const float* v = vin + tid * " + nv + "u;\n";
        s += "    out[tid] = (" + bodies[i] + ");\n";
        s += "}\n";
    }
    return s;
}

} // namespace expreval
#endif // EXPR_KERNEL_METAL_H
