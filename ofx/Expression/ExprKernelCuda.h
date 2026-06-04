// ExprKernelCuda.h
// -----------------------------------------------------------------------------
// CUDA (NVRTC) back-end for the transpiled expression kernel emitted by
// Program::emitC() (see ExprEval.h). The CUDA sibling of ExprKernelMetal.h: every
// exh_* helper mirrors the SAME semantics as ExprKernel.h / ExprKernelMetal.h, so
// the shared emitted kernel *body* produces the same result on an NVIDIA GPU as
// the CPU evaluator does.
//
// Like Metal, the GPU path is single precision where it matters for cross-target
// parity: the value-noise / random helpers are written float-exact (floorf/sinf,
// f-suffixed literals) so CUDA noise matches the CPU's float noise (ev_*f in
// ExprEval.h) bit-for-bit — value-noise is +,-,*,floor only. The deterministic
// helpers mirror the Metal/CPU bodies; the v[] buffer and the output are float, so
// the result lands at float precision regardless of intermediate width.
//
// The prelude is a runtime string compiled with NVRTC at render (cached per
// expression set) — the same shape as the Metal back-end. Kernels are
// `extern "C" __global__` so the driver API can look them up by unmangled name.
//
//   1. kExprCudaPrelude       — the exh_* device-helper library.
//   2. buildCudaScalarKernels — `gen_N` over a v[] input buffer (differential test).
//   3. buildCudaPixelKernel   — the per-pixel `exprKernel` (render path).
// -----------------------------------------------------------------------------
#ifndef EXPR_KERNEL_CUDA_H
#define EXPR_KERNEL_CUDA_H

#include <string>
#include <vector>
#include <utility>

namespace expreval {

// exh_* device helpers. Deterministic helpers mirror the Metal/CPU bodies; the
// noise helpers are float-exact (match ev_*f in ExprEval.h) for CPU<->GPU noise
// parity. CUDA device math (sin/pow/fabs/fmin/fmax/fmod/hypot/log/exp2/...) is
// available unqualified, so the emitted body's bare calls resolve directly.
static const char* kExprCudaPrelude =
"typedef float FLT;\n"
"\n"
"__device__ static float exh_log2(float x)      { return log(x) / log(2.0); }\n"
"__device__ static float exh_trunc(float x)     { return x < 0 ? ceil(x) : floor(x); }\n"
"__device__ static float exh_round(float x)     { return floor(x + 0.5); }\n"
"__device__ static float exh_sign(float x)      { return (float)(((x > 0) ? 1 : 0) - ((x < 0) ? 1 : 0)); }\n"
"__device__ static float exh_exponent(float x)  { return floor(log2(fabs(x) + 1e-30)); }\n"
"__device__ static float exh_mantissa(float x)  { return x / exp2(floor(log2(fabs(x) + 1e-30))); }\n"
"__device__ static float exh_pow2(float x)      { return x * x; }\n"
"__device__ static float exh_step(float e, float x) { return x < e ? 0.0 : 1.0; }\n"
"__device__ static float exh_ldexp(float a, float b){ return a * exp2(b); }\n"
"__device__ static float exh_clamp1(float x)    { return x < 0 ? 0.0 : (x > 1 ? 1.0 : x); }\n"
"__device__ static float exh_clamp3(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }\n"
"__device__ static float exh_lerp(float a, float b, float t)   { return a + (b - a) * t; }\n"
"__device__ static float exh_smoothstep(float a, float b, float x) {\n"
"    float t = (x - a) / (b - a); t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * (3 - 2 * t);\n"
"}\n"
"\n"
"// --- value noise / random: float-exact, matches ev_*f (ExprEval.h) ----------\n"
"__device__ static float exh_fractf(float x) { return x - floorf(x); }\n"
"__device__ static float exh_hash3f(float x, float y, float z) {\n"
"    float px = exh_fractf(x * 0.3183099f + 0.1f) * 17.0f;\n"
"    float py = exh_fractf(y * 0.3183099f + 0.1f) * 17.0f;\n"
"    float pz = exh_fractf(z * 0.3183099f + 0.1f) * 17.0f;\n"
"    return exh_fractf(px * py * pz * (px + py + pz));\n"
"}\n"
"__device__ static float exh_vnoisef(float x, float y, float z) {\n"
"    float ix = floorf(x), iy = floorf(y), iz = floorf(z);\n"
"    float fx = x - ix, fy = y - iy, fz = z - iz;\n"
"    float ux = fx * fx * (3.0f - 2.0f * fx);\n"
"    float uy = fy * fy * (3.0f - 2.0f * fy);\n"
"    float uz = fz * fz * (3.0f - 2.0f * fz);\n"
"    float c000 = exh_hash3f(ix,      iy,      iz),      c100 = exh_hash3f(ix+1.0f, iy,      iz);\n"
"    float c010 = exh_hash3f(ix,      iy+1.0f, iz),      c110 = exh_hash3f(ix+1.0f, iy+1.0f, iz);\n"
"    float c001 = exh_hash3f(ix,      iy,      iz+1.0f),  c101 = exh_hash3f(ix+1.0f, iy,      iz+1.0f);\n"
"    float c011 = exh_hash3f(ix,      iy+1.0f, iz+1.0f),  c111 = exh_hash3f(ix+1.0f, iy+1.0f, iz+1.0f);\n"
"    float x00 = c000 + (c100 - c000) * ux, x10 = c010 + (c110 - c010) * ux;\n"
"    float x01 = c001 + (c101 - c001) * ux, x11 = c011 + (c111 - c011) * ux;\n"
"    float y0 = x00 + (x10 - x00) * uy, y1 = x01 + (x11 - x01) * uy;\n"
"    return y0 + (y1 - y0) * uz;\n"
"}\n"
"__device__ static float exh_noise(float x, float y, float z) { return exh_vnoisef(x, y, z) * 2.0f - 1.0f; }\n"
"__device__ static float exh_random(float x, float y, float z) {\n"
"    float s = sinf(x * 12.9898f + y * 78.233f + z * 37.719f) * 43758.5453f;\n"
"    return exh_fractf(s);\n"
"}\n"
"__device__ static float exh_fbm(float x, float y, float z, float oct, float lac, float gain) {\n"
"    int o = (int)oct; float sum = 0.0f, amp = 1.0f, fr = 1.0f;\n"
"    for (int i = 0; i < o && i < 32; ++i) { sum += amp * (exh_vnoisef(x*fr, y*fr, z*fr) * 2.0f - 1.0f); fr *= lac; amp *= gain; }\n"
"    return sum;\n"
"}\n"
"__device__ static float exh_turb(float x, float y, float z, float oct, float lac, float gain) {\n"
"    int o = (int)oct; float sum = 0.0f, amp = 1.0f, fr = 1.0f;\n"
"    for (int i = 0; i < o && i < 32; ++i) { sum += amp * fabsf(exh_vnoisef(x*fr, y*fr, z*fr) * 2.0f - 1.0f); fr *= lac; amp *= gain; }\n"
"    return sum;\n"
"}\n"
"\n"
"__device__ static float exh_to_srgb(float c)     { return (c <= 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0/2.4) - 0.055; }\n"
"__device__ static float exh_from_srgb(float c)   { return (c <= 0.04045)   ? c / 12.92 : pow((c + 0.055)/1.055, 2.4); }\n"
"__device__ static float exh_to_rec709(float c)   { return (c < 0.018) ? c * 4.5 : 1.099 * pow(c, 0.45) - 0.099; }\n"
"__device__ static float exh_from_rec709(float c) { return (c < 0.081) ? c / 4.5 : pow((c + 0.099)/1.099, 1.0/0.45); }\n"
"__device__ static float exh_from_byte(float x)   { return exh_from_srgb(x / 255.0); }\n"
"__device__ static float exh_to_byte(float x)     { return exh_to_srgb(x) * 255.0; }\n";

// `gen_N(const float* vin, float* out, int count)` — one thread per nVars-float
// input vector. The differential-test form (mirrors buildMetalScalarKernels).
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
        s += "    const float* v = vin + tid * " + nv + ";\n";
        s += "    out[tid] = (" + bodies[i] + ");\n";
        s += "}\n";
    }
    return s;
}

// The per-pixel kernel. Uniforms struct is byte-identical (field order/types) to
// ExprMetalUniforms and to ExprCuda's host-side struct — keep all three in step.
// Binds the predefined variables exactly as ExpressionProcessor does on the CPU.
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
"    float r = 0.0, g = 0.0, b = 0.0, a = (U.nComps == 4 || U.nComps == 1) ? 0.0 : 1.0;\n"
"    if (U.hasSrc != 0) {\n"
"        int si = (gy - U.srcY1) * U.srcRowFloats + (gx - U.srcX1) * U.nComps;\n"
"        if (U.nComps == 1) { a = src[si]; }\n"
"        else { r = src[si]; g = src[si+1]; b = src[si+2]; if (U.nComps == 4) a = src[si+3]; }\n"
"    }\n"
"    float v[" + nv + "];\n"
"    v[0] = r; v[1] = g; v[2] = b; v[3] = a;\n"
"    v[4] = (float)gx; v[5] = (float)gy;\n"
"    float halfW = U.fwidth * 0.5, halfH = U.fheight * 0.5;\n"
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
#endif // EXPR_KERNEL_CUDA_H
