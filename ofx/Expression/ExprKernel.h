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
static inline double exh_fract(double x) { return x - floor(x); }
static inline double exh_hash3(double x, double y, double z) {
    double px = exh_fract(x * 0.3183099 + 0.1) * 17.0;
    double py = exh_fract(y * 0.3183099 + 0.1) * 17.0;
    double pz = exh_fract(z * 0.3183099 + 0.1) * 17.0;
    return exh_fract(px * py * pz * (px + py + pz));
}
static inline double exh_vnoise(double x, double y, double z) {
    double ix = floor(x), iy = floor(y), iz = floor(z);
    double fx = x - ix, fy = y - iy, fz = z - iz;
    double ux = fx * fx * (3.0 - 2.0 * fx);
    double uy = fy * fy * (3.0 - 2.0 * fy);
    double uz = fz * fz * (3.0 - 2.0 * fz);
    double c000 = exh_hash3(ix,     iy,     iz),     c100 = exh_hash3(ix+1.0, iy,     iz);
    double c010 = exh_hash3(ix,     iy+1.0, iz),     c110 = exh_hash3(ix+1.0, iy+1.0, iz);
    double c001 = exh_hash3(ix,     iy,     iz+1.0), c101 = exh_hash3(ix+1.0, iy,     iz+1.0);
    double c011 = exh_hash3(ix,     iy+1.0, iz+1.0), c111 = exh_hash3(ix+1.0, iy+1.0, iz+1.0);
    double x00 = c000 + (c100 - c000) * ux, x10 = c010 + (c110 - c010) * ux;
    double x01 = c001 + (c101 - c001) * ux, x11 = c011 + (c111 - c011) * ux;
    double y0 = x00 + (x10 - x00) * uy, y1 = x01 + (x11 - x01) * uy;
    return y0 + (y1 - y0) * uz;
}
static inline double exh_noise(double x, double y, double z) { return exh_vnoise(x, y, z) * 2.0 - 1.0; }
static inline double exh_random(double x, double y, double z) {
    double s = sin(x * 12.9898 + y * 78.233 + z * 37.719) * 43758.5453;
    return exh_fract(s);
}
static inline double exh_fbm(double x, double y, double z, double oct, double lac, double gain) {
    int o = (int)oct; double sum = 0, amp = 1, fr = 1;
    for (int i = 0; i < o && i < 32; ++i) { sum += amp * exh_noise(x*fr, y*fr, z*fr); fr *= lac; amp *= gain; }
    return sum;
}
static inline double exh_turb(double x, double y, double z, double oct, double lac, double gain) {
    int o = (int)oct; double sum = 0, amp = 1, fr = 1;
    for (int i = 0; i < o && i < 32; ++i) { sum += amp * fabs(exh_noise(x*fr, y*fr, z*fr)); fr *= lac; amp *= gain; }
    return sum;
}

// --- colour-space helpers ----------------------------------------------------
static inline double exh_to_srgb(double c)     { return (c <= 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0/2.4) - 0.055; }
static inline double exh_from_srgb(double c)   { return (c <= 0.04045)   ? c / 12.92 : pow((c + 0.055)/1.055, 2.4); }
static inline double exh_to_rec709(double c)   { return (c < 0.018) ? c * 4.5 : 1.099 * pow(c, 0.45) - 0.099; }
static inline double exh_from_rec709(double c) { return (c < 0.081) ? c / 4.5 : pow((c + 0.099)/1.099, 1.0/0.45); }
static inline double exh_from_byte(double x)   { return exh_from_srgb(x / 255.0); }
static inline double exh_to_byte(double x)     { return exh_to_srgb(x) * 255.0; }

#endif // EXPR_KERNEL_H
