// ExprKernelCuda.h
// -----------------------------------------------------------------------------
// CUDA (NVRTC) back-end for the transpiled expression kernel emitted by
// Program::emitC() (see ExprEval.h). The CUDA sibling of ExprKernelMetal.h: every
// exh_* helper mirrors the SAME semantics as ExprKernel.h, so the shared emitted
// kernel *body* produces the same result on an NVIDIA GPU as the CPU evaluator.
//
// Precision note — why this mirrors the CPU (double) prelude, not the Metal (float)
// one: CUDA has a real `double` type, and NVRTC's overload resolution rejects the
// mixed `pow(float,double)` / `fmax(float,double)` / `fmod(float,double)` calls the
// emitted body produces (a float v[] read against a double literal) as *ambiguous*
// — Metal accepted them only because Metal has no double. So the CUDA kernels widen
// the float inputs into a `double v[]` (exactly as the CPU oracle does:
// vd[k]=(double)vf[k]) and run the deterministic math in double — unambiguous, and
// a tighter match to the oracle than the float path. Value-noise/random stay
// float-exact (floorf/sinf/f-literals) so CUDA noise == the CPU float noise == Metal
// bit-for-bit. (Cross-backend, CUDA deterministic math is thus ~float-epsilon
// "more accurate" than Metal; both match the double oracle within their precision.)
//
// The prelude is a runtime string compiled with NVRTC at render (cached per
// expression set). Kernels are `extern "C" __global__` so the driver API finds
// them by unmangled name.
//   1. kExprCudaPrelude       — the exh_* device-helper library (mirrors ExprKernel.h).
//   2. buildCudaScalarKernels — `gen_N` over a float v[] input buffer (diff test).
//   3. buildCudaPixelKernel   — the per-pixel `exprKernel` (render path).
// -----------------------------------------------------------------------------
#ifndef EXPR_KERNEL_CUDA_H
#define EXPR_KERNEL_CUDA_H

#include <string>
#include <vector>
#include <utility>

namespace expreval {

// Mirrors ExprKernel.h exactly (double deterministic helpers; float-internal
// value-noise) with __device__ qualifiers. CUDA device math (log/exp2/pow/sin/
// floor/fabs/floorf/sinf/fabsf/...) is available unqualified, as are the bare
// fmin/fmax/fmod/hypot the emitted body calls — all (double,double) here.
static const char* kExprCudaPrelude =
"__device__ static double exh_log2(double x)      { return log(x) / log(2.0); }\n"
"__device__ static double exh_trunc(double x)     { return x < 0 ? ceil(x) : floor(x); }\n"
"__device__ static double exh_round(double x)     { return floor(x + 0.5); }\n"
"__device__ static double exh_sign(double x)      { return (double)((x > 0) - (x < 0)); }\n"
"__device__ static double exh_exponent(double x)  { return floor(log2(fabs(x) + 1e-30)); }\n"
"__device__ static double exh_mantissa(double x)  { return x / exp2(floor(log2(fabs(x) + 1e-30))); }\n"
"__device__ static double exh_pow2(double x)      { return x * x; }\n"
"__device__ static double exh_step(double e, double x) { return x < e ? 0.0 : 1.0; }\n"
"__device__ static double exh_ldexp(double a, double b){ return a * exp2(b); }\n"
"__device__ static double exh_clamp1(double x)    { return x < 0 ? 0.0 : (x > 1 ? 1.0 : x); }\n"
"__device__ static double exh_clamp3(double x, double a, double b) { return x < a ? a : (x > b ? b : x); }\n"
"__device__ static double exh_lerp(double a, double b, double t)   { return a + (b - a) * t; }\n"
"__device__ static double exh_smoothstep(double a, double b, double x) {\n"
"    double t = (x - a) / (b - a); t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * (3 - 2 * t);\n"
"}\n"
"\n"
"// --- noise / random: classic Perlin (Gustavson/Ashima, tableless) -----------\n"
"// Mirrors ev_p* in ExprEval.h and exh_p* in ExprKernel.h. Float-only +,-,*,floor\n"
"// (intermediates < 289, float-exact); NVRTC is compiled with --fmad=false so the\n"
"// noise/random hash matches the CPU oracle bit-for-bit (no fma fusion).\n"
"__device__ static float exh_fractf(float x)   { return x - floorf(x); }\n"
"__device__ static float exh_pmod289(float x)  { return x - floorf(x * (1.0f/289.0f)) * 289.0f; }\n"
"__device__ static float exh_ppermute(float x) { return exh_pmod289((x*34.0f + 1.0f) * x); }\n"
"__device__ static float exh_ptinv(float r)    { return 1.79284291400159f - 0.85373472095314f * r; }\n"
"__device__ static float exh_pfade(float t)    { return t*t*t*(t*(t*6.0f - 15.0f) + 10.0f); }\n"
"__device__ static float exh_pnoise3(float Px, float Py, float Pz) {\n"
"    float i0x=floorf(Px), i0y=floorf(Py), i0z=floorf(Pz);\n"
"    float i1x=i0x+1.0f, i1y=i0y+1.0f, i1z=i0z+1.0f;\n"
"    i0x=exh_pmod289(i0x); i0y=exh_pmod289(i0y); i0z=exh_pmod289(i0z);\n"
"    i1x=exh_pmod289(i1x); i1y=exh_pmod289(i1y); i1z=exh_pmod289(i1z);\n"
"    float f0x=Px-floorf(Px), f0y=Py-floorf(Py), f0z=Pz-floorf(Pz);\n"
"    float f1x=f0x-1.0f, f1y=f0y-1.0f, f1z=f0z-1.0f;\n"
"    float ixL[4]={i0x,i1x,i0x,i1x}, iyL[4]={i0y,i0y,i1y,i1y};\n"
"    float gx[8], gy[8], gz[8];\n"
"    for (int L=0; L<4; ++L) {\n"
"        float ixy = exh_ppermute(exh_ppermute(ixL[L]) + iyL[L]);\n"
"        for (int zz=0; zz<2; ++zz) {\n"
"            float p = exh_ppermute(ixy + (zz==0 ? i0z : i1z));\n"
"            float g  = p * (1.0f/7.0f);\n"
"            float ggy = exh_fractf(floorf(g) * (1.0f/7.0f)) - 0.5f;\n"
"            float ggx = exh_fractf(g);\n"
"            float ggz = 0.5f - fabsf(ggx) - fabsf(ggy);\n"
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
"// noise/random/fBm/turbulence: float-internal, returned as double (matches\n"
"// ExprKernel.h) so they compose with the double deterministic math.\n"
"__device__ static double exh_noise(double x, double y, double z) {\n"
"    return (double)exh_pnoise3((float)x, (float)y, (float)z);\n"
"}\n"
"__device__ static double exh_random(double x, double y, double z) {\n"
"    float a=exh_pmod289(floorf((float)x)), b=exh_pmod289(floorf((float)y)), c=exh_pmod289(floorf((float)z));\n"
"    float h1=exh_ppermute(exh_ppermute(exh_ppermute(a)+b)+c);\n"
"    float h2=exh_ppermute(exh_ppermute(exh_ppermute(a+1.0f)+b)+c);\n"
"    return (double)exh_fractf((h1*289.0f + h2) * (1.0f/83521.0f));\n"
"}\n"
"__device__ static double exh_fbm(double x, double y, double z, double oct, double lac, double gain) {\n"
"    int o = (int)oct; float sum = 0.0f, amp = 1.0f, fr = 1.0f;\n"
"    float fx = (float)x, fy = (float)y, fz = (float)z, fl = (float)lac, fg = (float)gain;\n"
"    for (int i = 0; i < o && i < 32; ++i) { sum += amp * exh_pnoise3(fx*fr, fy*fr, fz*fr); fr *= fl; amp *= fg; }\n"
"    return (double)sum;\n"
"}\n"
"__device__ static double exh_turb(double x, double y, double z, double oct, double lac, double gain) {\n"
"    int o = (int)oct; float sum = 0.0f, amp = 1.0f, fr = 1.0f;\n"
"    float fx = (float)x, fy = (float)y, fz = (float)z, fl = (float)lac, fg = (float)gain;\n"
"    for (int i = 0; i < o && i < 32; ++i) { sum += amp * fabsf(exh_pnoise3(fx*fr, fy*fr, fz*fr)); fr *= fl; amp *= fg; }\n"
"    return (double)sum;\n"
"}\n"
"\n"
"__device__ static double exh_to_srgb(double c)     { return (c <= 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0/2.4) - 0.055; }\n"
"__device__ static double exh_from_srgb(double c)   { return (c <= 0.04045)   ? c / 12.92 : pow((c + 0.055)/1.055, 2.4); }\n"
"__device__ static double exh_to_rec709(double c)   { return (c < 0.018) ? c * 4.5 : 1.099 * pow(c, 0.45) - 0.099; }\n"
"__device__ static double exh_from_rec709(double c) { return (c < 0.081) ? c / 4.5 : pow((c + 0.099)/1.099, 1.0/0.45); }\n"
"__device__ static double exh_from_byte(double x)   { return exh_from_srgb(x / 255.0); }\n"
"__device__ static double exh_to_byte(double x)     { return exh_to_srgb(x) * 255.0; }\n";

// `gen_N(const float* vin, float* out, int count)` — one thread per nVars-float
// input vector. Widens the float inputs into a double v[] (as the CPU oracle does),
// so the emitted body's math is (double,double) — NVRTC-unambiguous.
inline std::string buildCudaScalarKernels(const std::vector<std::string>& bodies, int nVars) {
    std::string s = kExprCudaPrelude;
    s += "\n";
    const std::string nv = std::to_string(nVars);
    for (size_t i = 0; i < bodies.size(); ++i) {
        const std::string id = std::to_string(i);
        s += "extern \"C\" __global__ void gen_" + id +
             "(const float* vin, float* out, int count) {\n";
        s += "    int tid = blockIdx.x * blockDim.x + threadIdx.x;\n";
        s += "    if (tid >= count) return;\n";
        s += "    double v[" + nv + "];\n";
        s += "    for (int k = 0; k < " + nv + "; ++k) v[k] = (double)vin[tid * " + nv + " + k];\n";
        s += "    out[tid] = (float)(" + bodies[i] + ");\n";
        s += "}\n";
    }
    return s;
}

// The per-pixel kernel. Uniforms struct is byte-identical (field order/types) to
// ExprMetalUniforms and to ExprCuda's host-side struct — keep all in step. Binds
// the predefined variables exactly as ExpressionProcessor does on the CPU; the v[]
// is double (deterministic math in double), src/dst buffers are float.
inline std::string buildCudaPixelKernel(const std::string chan[4],
        const std::vector<std::pair<int,std::string> >& temps, int nVars) {
    const std::string nv = std::to_string(nVars);
    std::string s = kExprCudaPrelude;
    s +=
"\n"
"struct ExprCudaUniforms {\n"
"    int rwx1, rwy1, rwx2, rwy2;\n"
"    int srcX1, srcY1, srcRowFloats;\n"
"    int dstX1, dstY1, dstRowFloats;\n"
"    int nComps, hasSrc;\n"
"    float fwidth, fheight, frame;\n"
"};\n"
"\n"
"extern \"C\" __global__ void exprKernel(const float* src, float* dst, ExprCudaUniforms U) {\n"
"    int gx = U.rwx1 + (int)(blockIdx.x * blockDim.x + threadIdx.x);\n"
"    int gy = U.rwy1 + (int)(blockIdx.y * blockDim.y + threadIdx.y);\n"
"    if (gx >= U.rwx2 || gy >= U.rwy2) return;\n"
"    double r = 0.0, g = 0.0, b = 0.0, a = (U.nComps == 4 || U.nComps == 1) ? 0.0 : 1.0;\n"
"    if (U.hasSrc != 0) {\n"
"        int si = (gy - U.srcY1) * U.srcRowFloats + (gx - U.srcX1) * U.nComps;\n"
"        if (U.nComps == 1) { a = src[si]; }\n"
"        else { r = src[si]; g = src[si+1]; b = src[si+2]; if (U.nComps == 4) a = src[si+3]; }\n"
"    }\n"
"    double v[" + nv + "];\n"
"    v[0] = r; v[1] = g; v[2] = b; v[3] = a;\n"
"    v[4] = (double)gx; v[5] = (double)gy;\n"
"    double halfW = U.fwidth * 0.5, halfH = U.fheight * 0.5;\n"
"    double invHalfW = (halfW != 0.0) ? 1.0 / halfW : 0.0;\n"
"    v[6] = ((double)gx - halfW) * invHalfW;\n"
"    v[7] = ((double)gy - halfH) * invHalfW;\n"
"    v[8] = U.fwidth; v[9] = U.fheight; v[10] = U.frame;\n";
    for (size_t t = 0; t < temps.size(); ++t)
        s += "    v[" + std::to_string(temps[t].first) + "] = (" + temps[t].second + ");\n";
    s +=
"    double o0 = (" + chan[0] + ");\n"
"    double o1 = (" + chan[1] + ");\n"
"    double o2 = (" + chan[2] + ");\n"
"    double o3 = (" + chan[3] + ");\n"
"    int di = (gy - U.dstY1) * U.dstRowFloats + (gx - U.dstX1) * U.nComps;\n"
"    if (U.nComps == 1) { dst[di] = (float)o3; }\n"
"    else { dst[di] = (float)o0; dst[di+1] = (float)o1; dst[di+2] = (float)o2; if (U.nComps == 4) dst[di+3] = (float)o3; }\n"
"}\n";
    return s;
}

} // namespace expreval
#endif // EXPR_KERNEL_CUDA_H
