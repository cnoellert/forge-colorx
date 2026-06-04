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

// --- noise / random: classic Perlin (Gustavson/Ashima, tableless) ------------
// Must match ev_* in ExprEval.h bit-for-bit. Real gradient (Perlin) noise via a
// computable permutation (no table), all float +,-,*,floor (intermediates < 289,
// float-exact) -> noise AND random() are cross-backend parity-clean. Mirrors
// ev_p* in ExprEval.h and the exh_* preludes in ExprKernelCuda.h / ExprKernelMetal.h;
// the differential tests guard against drift.
static inline float exh_fractf(float x)   { return x - floorf(x); }
static inline float exh_pmod289(float x)  { return x - floorf(x * (1.0f/289.0f)) * 289.0f; }
static inline float exh_ppermute(float x) { return exh_pmod289((x*34.0f + 1.0f) * x); }
static inline float exh_ptinv(float r)    { return 1.79284291400159f - 0.85373472095314f * r; }
static inline float exh_pfade(float t)    { return t*t*t*(t*(t*6.0f - 15.0f) + 10.0f); }
static inline float exh_pnoise3(float Px, float Py, float Pz) {
    float i0x=floorf(Px), i0y=floorf(Py), i0z=floorf(Pz);
    float i1x=i0x+1.0f, i1y=i0y+1.0f, i1z=i0z+1.0f;
    i0x=exh_pmod289(i0x); i0y=exh_pmod289(i0y); i0z=exh_pmod289(i0z);
    i1x=exh_pmod289(i1x); i1y=exh_pmod289(i1y); i1z=exh_pmod289(i1z);
    float f0x=Px-floorf(Px), f0y=Py-floorf(Py), f0z=Pz-floorf(Pz);
    float f1x=f0x-1.0f, f1y=f0y-1.0f, f1z=f0z-1.0f;
    float ixL[4]={i0x,i1x,i0x,i1x}, iyL[4]={i0y,i0y,i1y,i1y};
    float gx[8], gy[8], gz[8];
    for (int L=0; L<4; ++L) {
        float ixy = exh_ppermute(exh_ppermute(ixL[L]) + iyL[L]);
        for (int zz=0; zz<2; ++zz) {
            float p = exh_ppermute(ixy + (zz==0 ? i0z : i1z));
            float g  = p * (1.0f/7.0f);
            float ggy = exh_fractf(floorf(g) * (1.0f/7.0f)) - 0.5f;
            float ggx = exh_fractf(g);
            float ggz = 0.5f - fabsf(ggx) - fabsf(ggy);
            float sz  = (ggz <= 0.0f) ? 1.0f : 0.0f;
            ggx -= sz * ((ggx >= 0.0f ? 1.0f : 0.0f) - 0.5f);
            ggy -= sz * ((ggy >= 0.0f ? 1.0f : 0.0f) - 0.5f);
            int k = L + zz*4;  gx[k]=ggx; gy[k]=ggy; gz[k]=ggz;
        }
    }
    for (int k=0;k<8;++k){ float n=exh_ptinv(gx[k]*gx[k]+gy[k]*gy[k]+gz[k]*gz[k]); gx[k]*=n; gy[k]*=n; gz[k]*=n; }
    float n000=gx[0]*f0x+gy[0]*f0y+gz[0]*f0z, n100=gx[1]*f1x+gy[1]*f0y+gz[1]*f0z;
    float n010=gx[2]*f0x+gy[2]*f1y+gz[2]*f0z, n110=gx[3]*f1x+gy[3]*f1y+gz[3]*f0z;
    float n001=gx[4]*f0x+gy[4]*f0y+gz[4]*f1z, n101=gx[5]*f1x+gy[5]*f0y+gz[5]*f1z;
    float n011=gx[6]*f0x+gy[6]*f1y+gz[6]*f1z, n111=gx[7]*f1x+gy[7]*f1y+gz[7]*f1z;
    float wx=exh_pfade(f0x), wy=exh_pfade(f0y), wz=exh_pfade(f0z);
    float nz0=n000+(n001-n000)*wz, nz1=n100+(n101-n100)*wz, nz2=n010+(n011-n010)*wz, nz3=n110+(n111-n110)*wz;
    float ny0=nz0+(nz2-nz0)*wy, ny1=nz1+(nz3-nz1)*wy;
    return 2.2f * (ny0 + (ny1-ny0)*wx);
}
static inline double exh_noise(double x, double y, double z) {
    return (double)exh_pnoise3((float)x, (float)y, (float)z);
}
static inline double exh_random(double x, double y, double z) {
    float a=exh_pmod289(floorf((float)x)), b=exh_pmod289(floorf((float)y)), c=exh_pmod289(floorf((float)z));
    float h1=exh_ppermute(exh_ppermute(exh_ppermute(a)+b)+c);
    float h2=exh_ppermute(exh_ppermute(exh_ppermute(a+1.0f)+b)+c);
    return (double)exh_fractf((h1*289.0f + h2) * (1.0f/83521.0f));
}
static inline double exh_fbm(double x, double y, double z, double oct, double lac, double gain) {
    int o = (int)oct; float sum = 0.0f, amp = 1.0f, fr = 1.0f;
    float fx = (float)x, fy = (float)y, fz = (float)z, fl = (float)lac, fg = (float)gain;
    for (int i = 0; i < o && i < 32; ++i) {
        sum += amp * exh_pnoise3(fx*fr, fy*fr, fz*fr); fr *= fl; amp *= fg;
    }
    return (double)sum;
}
static inline double exh_turb(double x, double y, double z, double oct, double lac, double gain) {
    int o = (int)oct; float sum = 0.0f, amp = 1.0f, fr = 1.0f;
    float fx = (float)x, fy = (float)y, fz = (float)z, fl = (float)lac, fg = (float)gain;
    for (int i = 0; i < o && i < 32; ++i) {
        sum += amp * fabsf(exh_pnoise3(fx*fr, fy*fr, fz*fr)); fr *= fl; amp *= fg;
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
