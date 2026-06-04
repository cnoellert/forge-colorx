// ExprKernel.h
// -----------------------------------------------------------------------------
// Portable helper prelude for the transpiled expression kernel emitted by
// Program::emitC() (see ExprEval.h). Every exh_* function here mirrors the exact
// semantics of the matching case in ExprEval.h's evalNode(), so a transpiled
// kernel produces the same pixels as the CPU evaluator. The differential test
// tests/test_transpile.cpp compiles emitted kernels against this prelude and
// asserts parity with eval() over thousands of random inputs — guarding drift.
//
// This is the CPU/C++ prelude (double precision). The GPU back-ends (CUDA, Metal,
// GLSL) will provide their own preludes exposing the SAME exh_* names/semantics,
// so the emitted kernel body is shared across all targets.
// -----------------------------------------------------------------------------
#ifndef EXPR_KERNEL_H
#define EXPR_KERNEL_H

#include <math.h>

static inline double exh_log2(double x)      { return log(x) / log(2.0); }
static inline double exh_trunc(double x)     { return x < 0 ? ceil(x) : floor(x); }
static inline double exh_round(double x)     { return floor(x + 0.5); }      // rint/round
static inline double exh_sign(double x)      { return (double)((x > 0) - (x < 0)); }
static inline double exh_exponent(double x)  { return floor(log2(fabs(x) + 1e-30)); }
static inline double exh_mantissa(double x)  { return x / exp2(floor(log2(fabs(x) + 1e-30))); }
static inline double exh_pow2(double x)      { return x * x; }
static inline double exh_step(double e, double x) { return x < e ? 0.0 : 1.0; }   // step(edge,x)
static inline double exh_ldexp(double a, double b){ return a * exp2(b); }
static inline double exh_clamp1(double x)    { return x < 0 ? 0.0 : (x > 1 ? 1.0 : x); }
static inline double exh_clamp3(double x, double a, double b) { return x < a ? a : (x > b ? b : x); }
static inline double exh_lerp(double a, double b, double t)   { return a + (b - a) * t; }
static inline double exh_smoothstep(double a, double b, double x) {
    double t = (x - a) / (b - a); t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * (3 - 2 * t);
}

// --- value noise / random (must match ev_* in ExprEval.h bit-for-bit) --------
// Computed in FLOAT (not double): the GPU back-ends are float-only, so the noise
// sub-library is float on every target to render identical noise on CPU and GPU.
// value-noise is +,-,*,floor only -> IEEE-exact and bit-identical; random() uses
// sinf (a tiny cross-vendor ULP residual vs the GPU). Mirrors ev_*f in ExprEval.h
// and exh_* in ExprKernelMetal.h; the differential tests guard against drift.
static inline float exh_fractf(float x) { return x - floorf(x); }
static inline float exh_hash3f(float x, float y, float z) {
    float px = exh_fractf(x * 0.3183099f + 0.1f) * 17.0f;
    float py = exh_fractf(y * 0.3183099f + 0.1f) * 17.0f;
    float pz = exh_fractf(z * 0.3183099f + 0.1f) * 17.0f;
    return exh_fractf(px * py * pz * (px + py + pz));
}
static inline float exh_vnoisef(float x, float y, float z) {
    float ix = floorf(x), iy = floorf(y), iz = floorf(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;
    float ux = fx * fx * (3.0f - 2.0f * fx);
    float uy = fy * fy * (3.0f - 2.0f * fy);
    float uz = fz * fz * (3.0f - 2.0f * fz);
    float c000 = exh_hash3f(ix,      iy,      iz),      c100 = exh_hash3f(ix+1.0f, iy,      iz);
    float c010 = exh_hash3f(ix,      iy+1.0f, iz),      c110 = exh_hash3f(ix+1.0f, iy+1.0f, iz);
    float c001 = exh_hash3f(ix,      iy,      iz+1.0f), c101 = exh_hash3f(ix+1.0f, iy,      iz+1.0f);
    float c011 = exh_hash3f(ix,      iy+1.0f, iz+1.0f), c111 = exh_hash3f(ix+1.0f, iy+1.0f, iz+1.0f);
    float x00 = c000 + (c100 - c000) * ux, x10 = c010 + (c110 - c010) * ux;
    float x01 = c001 + (c101 - c001) * ux, x11 = c011 + (c111 - c011) * ux;
    float y0 = x00 + (x10 - x00) * uy, y1 = x01 + (x11 - x01) * uy;
    return y0 + (y1 - y0) * uz;
}
static inline double exh_noise(double x, double y, double z) {
    return (double)(exh_vnoisef((float)x, (float)y, (float)z) * 2.0f - 1.0f);
}
static inline double exh_random(double x, double y, double z) {
    float s = sinf((float)x * 12.9898f + (float)y * 78.233f + (float)z * 37.719f) * 43758.5453f;
    return (double)exh_fractf(s);
}
static inline double exh_fbm(double x, double y, double z, double oct, double lac, double gain) {
    int o = (int)oct; float sum = 0.0f, amp = 1.0f, fr = 1.0f;
    float fx = (float)x, fy = (float)y, fz = (float)z, fl = (float)lac, fg = (float)gain;
    for (int i = 0; i < o && i < 32; ++i) {
        sum += amp * (exh_vnoisef(fx*fr, fy*fr, fz*fr) * 2.0f - 1.0f); fr *= fl; amp *= fg;
    }
    return (double)sum;
}
static inline double exh_turb(double x, double y, double z, double oct, double lac, double gain) {
    int o = (int)oct; float sum = 0.0f, amp = 1.0f, fr = 1.0f;
    float fx = (float)x, fy = (float)y, fz = (float)z, fl = (float)lac, fg = (float)gain;
    for (int i = 0; i < o && i < 32; ++i) {
        sum += amp * fabsf(exh_vnoisef(fx*fr, fy*fr, fz*fr) * 2.0f - 1.0f); fr *= fl; amp *= fg;
    }
    return (double)sum;
}

// --- colour-space helpers ----------------------------------------------------
static inline double exh_to_srgb(double c)     { return (c <= 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0/2.4) - 0.055; }
static inline double exh_from_srgb(double c)   { return (c <= 0.04045)   ? c / 12.92 : pow((c + 0.055)/1.055, 2.4); }
static inline double exh_to_rec709(double c)   { return (c < 0.018) ? c * 4.5 : 1.099 * pow(c, 0.45) - 0.099; }
static inline double exh_from_rec709(double c) { return (c < 0.081) ? c / 4.5 : pow((c + 0.099)/1.099, 1.0/0.45); }
static inline double exh_from_byte(double x)   { return exh_from_srgb(x / 255.0); }
static inline double exh_to_byte(double x)     { return exh_to_srgb(x) * 255.0; }

#endif // EXPR_KERNEL_H
