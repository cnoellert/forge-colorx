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
"// noise / random: classic Perlin (Gustavson/Ashima, tableless) — mirrors ev_p* /\n"
"// exh_p* (ExprEval.h / ExprKernel.h / ExprKernelCuda.h). Float-only +,-,*,floor\n"
"// (intermediates < 289); MSL is float-only so this matches the CPU float oracle.\n"
"inline float exh_fract(float x)   { return x - floor(x); }\n"
"inline float exh_pmod289(float x) { return x - floor(x * (1.0f/289.0f)) * 289.0f; }\n"
"inline float exh_ppermute(float x){ return exh_pmod289((x*34.0f + 1.0f) * x); }\n"
"inline float exh_ptinv(float r)   { return 1.79284291400159f - 0.85373472095314f * r; }\n"
"inline float exh_pfade(float t)   { return t*t*t*(t*(t*6.0f - 15.0f) + 10.0f); }\n"
"inline float exh_pnoise3(float Px, float Py, float Pz) {\n"
"    float i0x=floor(Px), i0y=floor(Py), i0z=floor(Pz);\n"
"    float i1x=i0x+1.0f, i1y=i0y+1.0f, i1z=i0z+1.0f;\n"
"    i0x=exh_pmod289(i0x); i0y=exh_pmod289(i0y); i0z=exh_pmod289(i0z);\n"
"    i1x=exh_pmod289(i1x); i1y=exh_pmod289(i1y); i1z=exh_pmod289(i1z);\n"
"    float f0x=Px-floor(Px), f0y=Py-floor(Py), f0z=Pz-floor(Pz);\n"
"    float f1x=f0x-1.0f, f1y=f0y-1.0f, f1z=f0z-1.0f;\n"
"    float ixL[4]={i0x,i1x,i0x,i1x}, iyL[4]={i0y,i0y,i1y,i1y};\n"
"    float gx[8], gy[8], gz[8];\n"
"    for (int L=0; L<4; ++L) {\n"
"        float ixy = exh_ppermute(exh_ppermute(ixL[L]) + iyL[L]);\n"
"        for (int zz=0; zz<2; ++zz) {\n"
"            float p = exh_ppermute(ixy + (zz==0 ? i0z : i1z));\n"
"            float g  = p * (1.0f/7.0f);\n"
"            float ggy = exh_fract(floor(g) * (1.0f/7.0f)) - 0.5f;\n"
"            float ggx = exh_fract(g);\n"
"            float ggz = 0.5f - fabs(ggx) - fabs(ggy);\n"
"            float sz  = (ggz <= 0.0f) ? 1.0f : 0.0f;\n"
"            ggx -= sz * ((ggx >= 0.0f ? 1.0f : 0.0f) - 0.5f);\n"
"            ggy -= sz * ((ggy >= 0.0f ? 1.0f : 0.0f) - 0.5f);\n"
"            int k = L + zz*4;  gx[k]=ggx; gy[k]=ggy; gz[k]=ggz;\n"
"        }\n"
"    }\n"
"    for (int k=0;k<8;++k){ float n=exh_ptinv(gx[k]*gx[k]+gy[k]*gy[k]+gz[k]*gz[k]); gx[k]*=n; gy[k]*=n; gz[k]*=n; }\n"
"    float n000=gx[0]*f0x+gy[0]*f0y+gz[0]*f0z, n100=gx[1]*f1x+gy[1]*f0y+gz[1]*f0z;\n"
"    float n010=gx[2]*f0x+gy[2]*f1y+gz[2]*f0z, n110=gx[3]*f1x+gy[3]*f1y+gz[3]*f0z;\n"
"    float n001=gx[4]*f0x+gy[4]*f0y+gz[4]*f1z, n101=gx[5]*f1x+gy[5]*f0y+gz[5]*f1z;\n"
"    float n011=gx[6]*f0x+gy[6]*f1y+gz[6]*f1z, n111=gx[7]*f1x+gy[7]*f1y+gz[7]*f1z;\n"
"    float wx=exh_pfade(f0x), wy=exh_pfade(f0y), wz=exh_pfade(f0z);\n"
"    float nz0=n000+(n001-n000)*wz, nz1=n100+(n101-n100)*wz, nz2=n010+(n011-n010)*wz, nz3=n110+(n111-n110)*wz;\n"
"    float ny0=nz0+(nz2-nz0)*wy, ny1=nz1+(nz3-nz1)*wy;\n"
"    return 2.2f * (ny0 + (ny1-ny0)*wx);\n"
"}\n"
"inline float exh_noise(float x, float y, float z) { return exh_pnoise3(x, y, z); }\n"
"inline float exh_random(float x, float y, float z) {\n"
"    float a=exh_pmod289(floor(x)), b=exh_pmod289(floor(y)), c=exh_pmod289(floor(z));\n"
"    float h1=exh_ppermute(exh_ppermute(exh_ppermute(a)+b)+c);\n"
"    float h2=exh_ppermute(exh_ppermute(exh_ppermute(a+1.0f)+b)+c);\n"
"    return exh_fract((h1*289.0f + h2) * (1.0f/83521.0f));\n"
"}\n"
"inline float exh_fbm(float x, float y, float z, float oct, float lac, float gain) {\n"
"    int o = (int)oct; float sum = 0.0, amp = 1.0, fr = 1.0;\n"
"    for (int i = 0; i < o && i < 32; ++i) { sum += amp * exh_pnoise3(x*fr, y*fr, z*fr); fr *= lac; amp *= gain; }\n"
"    return sum;\n"
"}\n"
"inline float exh_turb(float x, float y, float z, float oct, float lac, float gain) {\n"
"    int o = (int)oct; float sum = 0.0, amp = 1.0, fr = 1.0;\n"
"    for (int i = 0; i < o && i < 32; ++i) { sum += amp * fabs(exh_pnoise3(x*fr, y*fr, z*fr)); fr *= lac; amp *= gain; }\n"
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

// ---- production per-pixel kernel ---------------------------------------------
// The uniforms struct passed to the kernel. ExprMetal.mm defines a byte-identical
// C++ struct (same field order/types) — keep them in lockstep.
//   struct ExprMetalUniforms {
//     int rwx1,rwy1,rwx2,rwy2;  src/dst origins + row strides (in floats);
//     int srcX1,srcY1,srcRowFloats; int dstX1,dstY1,dstRowFloats;
//     int nComps,hasSrc; float fwidth,fheight,frame; };
//
// Build the full MSL for one expression set: read the source pixel from its
// MTLBuffer, bind the predefined variables exactly as ExpressionProcessor does
// on the CPU (origin bottom-left x,y; centred aspect-preserved cx,cy), evaluate
// the temps in order then the four channel expressions, and write the result.
// `temps` are (slot, emitC() body) pairs in evaluation order; `chan[4]` are the
// emitC() bodies for r,g,b,a; `nVars` sizes the v[] array (fixed vars + temps).
inline std::string buildMetalPixelKernel(const std::string chan[4],
        const std::vector<std::pair<int,std::string>>& temps, int nVars) {
    const std::string nv = std::to_string(nVars);
    std::string s = kExprMetalPrelude;
    s +=
"\n"
"struct ExprMetalUniforms {\n"
"    int rwx1, rwy1, rwx2, rwy2;\n"
"    int srcX1, srcY1, srcRowFloats;\n"
"    int dstX1, dstY1, dstRowFloats;\n"
"    int nComps, hasSrc;\n"
"    float fwidth, fheight, frame;\n"
"};\n"
"\n"
"kernel void exprKernel(device const float* src [[buffer(0)]],\n"
"                       device float* dst [[buffer(1)]],\n"
"                       constant ExprMetalUniforms& U [[buffer(2)]],\n"
"                       uint2 gid [[thread_position_in_grid]]) {\n"
"    int gx = U.rwx1 + (int)gid.x;\n"
"    int gy = U.rwy1 + (int)gid.y;\n"
"    if (gx >= U.rwx2 || gy >= U.rwy2) return;\n"
"    float r = 0.0, g = 0.0, b = 0.0, a = (U.nComps == 4 || U.nComps == 1) ? 0.0 : 1.0;\n"
"    if (U.hasSrc != 0) {\n"
"        int si = (gy - U.srcY1) * U.srcRowFloats + (gx - U.srcX1) * U.nComps;\n"
"        if (U.nComps == 1) { a = src[si]; }\n"
"        else { r = src[si]; g = src[si+1]; b = src[si+2]; if (U.nComps == 4) a = src[si+3]; }\n"
"    }\n"
"    float v[" + nv + "];\n"
"    v[0] = r; v[1] = g; v[2] = b; v[3] = a;\n"
"    v[4] = (float)gx; v[5] = (float)gy;\n"
"    float halfW = U.fwidth * 0.5; float halfH = U.fheight * 0.5;\n"
"    float invHalfW = (halfW != 0.0) ? 1.0 / halfW : 0.0;\n"
"    v[6] = ((float)gx - halfW) * invHalfW;\n"
"    v[7] = ((float)gy - halfH) * invHalfW;\n"
"    v[8] = U.fwidth; v[9] = U.fheight; v[10] = U.frame;\n";
    for (size_t t = 0; t < temps.size(); ++t)
        s += "    v[" + std::to_string(temps[t].first) + "] = (" + temps[t].second + ");\n";
    s +=
"    float o0 = (" + chan[0] + ");\n"
"    float o1 = (" + chan[1] + ");\n"
"    float o2 = (" + chan[2] + ");\n"
"    float o3 = (" + chan[3] + ");\n"
"    int di = (gy - U.dstY1) * U.dstRowFloats + (gx - U.dstX1) * U.nComps;\n"
"    if (U.nComps == 1) { dst[di] = o3; }\n"
"    else { dst[di] = o0; dst[di+1] = o1; dst[di+2] = o2; if (U.nComps == 4) dst[di+3] = o3; }\n"
"}\n";
    return s;
}

} // namespace expreval
#endif // EXPR_KERNEL_METAL_H
