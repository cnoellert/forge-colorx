#version 120
//
// ColorExpression.glsl
// -----------------------------------------------------------------------------
// A Matchbox (Autodesk Flame / Flare / Smoke) re-creation of Nuke's
// Color > Math > Expression node.
//
// Nuke parses free-typed text per channel at run time. GLSL is compiled and a
// Matchbox UI has no text field, so the artist writes the four channel formulas
// directly in the EXPRESSION BLOCK below, using the SAME variable names and the
// SAME function library that Nuke exposes, then recompiles with shader_builder.
// Animatable user constants (k1..k4, ref) let you tweak common values live with
// no recompile.
//
// Function / variable parity with Nuke is documented in README.md.
// GLSL 120 is targeted so the shader runs on both Linux and macOS Flame.
// -----------------------------------------------------------------------------

// --- Flame built-in uniforms -------------------------------------------------
uniform float adsk_result_w, adsk_result_h;   // output resolution
uniform float adsk_time;                       // current frame (0 if unavailable)
uniform sampler2D front;                       // the image input ("rgba" in Nuke)

// --- User constants (the "knobs" you can animate without recompiling) --------
uniform float k1, k2, k3, k4;                  // generic scalars, refer to as k1..k4
uniform vec3  ref;                             // a reference colour (ref.r, ref.g, ref.b)
uniform bool  clamp_output;                    // clamp result to [0,1]
uniform float mix_amount;                      // blend expr result with the original

// =============================================================================
//  NUKE-COMPATIBLE EXPRESSION LIBRARY
//  These mirror the functions available in Nuke's Expression node. Functions
//  that GLSL 120 already provides with identical semantics (sin, cos, tan, asin,
//  acos, atan, exp, log, pow, sqrt, floor, ceil, abs, min, max, sign, mod,
//  fract, step, smoothstep, mix, clamp(x,a,b), radians, degrees, length) are
//  used as-is; only the ones GLSL lacks (or names differently) are defined here.
// =============================================================================

float pi()              { return 3.141592653589793; }
float e_()              { return 2.718281828459045; }   // 'e' is reserved-ish; use e_()

float lerp(float a, float b, float t) { return mix(a, b, t); }
float pow2(float x)     { return x * x; }
float fabs(float x)     { return abs(x); }
float hypot(float x, float y) { return sqrt(x * x + y * y); }
float log10(float x)    { return log(x) / log(10.0); }
float atan2(float y, float x) { return atan(y, x); }

// C-style fmod: result takes the sign of the dividend (GLSL mod() takes the
// sign of the divisor, which differs from Nuke for negative inputs).
float trunc(float x)    { return (x < 0.0) ? ceil(x) : floor(x); }
float fmod(float x, float y) { return x - y * trunc(x / y); }
float rint(float x)     { return floor(x + 0.5); }      // round to nearest

// Hyperbolic (not built in to GLSL 120)
float sinh(float x)     { return 0.5 * (exp(x) - exp(-x)); }
float cosh(float x)     { return 0.5 * (exp(x) + exp(-x)); }
float tanh(float x)     { return sinh(x) / cosh(x); }

// Single-argument clamp, matching Nuke's clamp(x) == clamp to [0,1]
float clamp01(float x)  { return clamp(x, 0.0, 1.0); }

// Floating-point manipulation (approximate; GLSL has no frexp/ldexp in 120)
float ldexp(float x, float ex)   { return x * exp2(ex); }
float exponent(float x)          { return floor(log2(abs(x) + 1e-30)); }
float mantissa(float x)          { return x / exp2(exponent(x)); }

// --- Colour-space conversions (match Nuke's helpers) -------------------------
float to_sRGB(float c)   { return (c <= 0.0031308) ? c * 12.92
                                                    : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float from_sRGB(float c) { return (c <= 0.04045)   ? c / 12.92
                                                    : pow((c + 0.055) / 1.055, 2.4); }
float to_rec709(float c)   { return (c < 0.018) ? c * 4.5
                                                : 1.099 * pow(c, 0.45) - 0.099; }
float from_rec709(float c) { return (c < 0.081) ? c / 4.5
                                                : pow((c + 0.099) / 1.099, 1.0 / 0.45); }
float to_byte(float c)   { return to_sRGB(c) * 255.0; }
float from_byte(float c) { return from_sRGB(c / 255.0); }

// --- Noise / random: classic Perlin (Gustavson/Ashima, tableless) ------------
// Real gradient (Perlin) noise via a computable permutation (no table), so it
// matches the OFX build's noise to float precision (the OFX C-family uses the
// identical scalar sequence; see ofx/Expression/ExprEval.h). noise() ~[-1,1],
// random() [0,1). Ashima classicnoise3D is public domain.
vec3  mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4  mod289(vec4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
float mod289(float x){ return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4  permute(vec4 x)  { return mod289(((x*34.0)+1.0)*x); }
float permute(float x) { return mod289(((x*34.0)+1.0)*x); }
vec4  taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }
vec3  fade(vec3 t) { return t*t*t*(t*(t*6.0-15.0)+10.0); }

float cnoise(vec3 P) {
    vec3 Pi0 = floor(P), Pi1 = Pi0 + vec3(1.0);
    Pi0 = mod289(Pi0); Pi1 = mod289(Pi1);
    vec3 Pf0 = fract(P), Pf1 = Pf0 - vec3(1.0);
    vec4 ix = vec4(Pi0.x, Pi1.x, Pi0.x, Pi1.x);
    vec4 iy = vec4(Pi0.yy, Pi1.yy);
    vec4 iz0 = Pi0.zzzz, iz1 = Pi1.zzzz;
    vec4 ixy = permute(permute(ix) + iy);
    vec4 ixy0 = permute(ixy + iz0), ixy1 = permute(ixy + iz1);
    vec4 gx0 = ixy0 * (1.0 / 7.0);
    vec4 gy0 = fract(floor(gx0) * (1.0 / 7.0)) - 0.5;
    gx0 = fract(gx0);
    vec4 gz0 = vec4(0.5) - abs(gx0) - abs(gy0);
    vec4 sz0 = step(gz0, vec4(0.0));
    gx0 -= sz0 * (step(0.0, gx0) - 0.5);
    gy0 -= sz0 * (step(0.0, gy0) - 0.5);
    vec4 gx1 = ixy1 * (1.0 / 7.0);
    vec4 gy1 = fract(floor(gx1) * (1.0 / 7.0)) - 0.5;
    gx1 = fract(gx1);
    vec4 gz1 = vec4(0.5) - abs(gx1) - abs(gy1);
    vec4 sz1 = step(gz1, vec4(0.0));
    gx1 -= sz1 * (step(0.0, gx1) - 0.5);
    gy1 -= sz1 * (step(0.0, gy1) - 0.5);
    vec3 g000 = vec3(gx0.x,gy0.x,gz0.x), g100 = vec3(gx0.y,gy0.y,gz0.y);
    vec3 g010 = vec3(gx0.z,gy0.z,gz0.z), g110 = vec3(gx0.w,gy0.w,gz0.w);
    vec3 g001 = vec3(gx1.x,gy1.x,gz1.x), g101 = vec3(gx1.y,gy1.y,gz1.y);
    vec3 g011 = vec3(gx1.z,gy1.z,gz1.z), g111 = vec3(gx1.w,gy1.w,gz1.w);
    vec4 norm0 = taylorInvSqrt(vec4(dot(g000,g000), dot(g010,g010), dot(g100,g100), dot(g110,g110)));
    g000 *= norm0.x; g010 *= norm0.y; g100 *= norm0.z; g110 *= norm0.w;
    vec4 norm1 = taylorInvSqrt(vec4(dot(g001,g001), dot(g011,g011), dot(g101,g101), dot(g111,g111)));
    g001 *= norm1.x; g011 *= norm1.y; g101 *= norm1.z; g111 *= norm1.w;
    float n000 = dot(g000, Pf0);
    float n100 = dot(g100, vec3(Pf1.x, Pf0.y, Pf0.z));
    float n010 = dot(g010, vec3(Pf0.x, Pf1.y, Pf0.z));
    float n110 = dot(g110, vec3(Pf1.x, Pf1.y, Pf0.z));
    float n001 = dot(g001, vec3(Pf0.x, Pf0.y, Pf1.z));
    float n101 = dot(g101, vec3(Pf1.x, Pf0.y, Pf1.z));
    float n011 = dot(g011, vec3(Pf0.x, Pf1.y, Pf1.z));
    float n111 = dot(g111, Pf1);
    vec3 fade_xyz = fade(Pf0);
    vec4 n_z = mix(vec4(n000, n100, n010, n110), vec4(n001, n101, n011, n111), fade_xyz.z);
    vec2 n_yz = mix(n_z.xy, n_z.zw, fade_xyz.y);
    return 2.2 * mix(n_yz.x, n_yz.y, fade_xyz.x);
}
float noise(float x, float y, float z) { return cnoise(vec3(x, y, z)); }
float noise(float x, float y)          { return noise(x, y, 0.0); }
float noise(float x)                   { return noise(x, 0.0, 0.0); }

// random(): tableless permute hash of the integer cell (matches the OFX build's
// random() to float precision). Two permute rounds -> 289*289 distinct values.
float random(float x, float y, float z) {
    float a = mod289(floor(x)), b = mod289(floor(y)), c = mod289(floor(z));
    float h1 = permute(permute(permute(a) + b) + c);
    float h2 = permute(permute(permute(a + 1.0) + b) + c);
    return fract((h1 * 289.0 + h2) * (1.0 / 83521.0));
}
float random(float x, float y) { return random(x, y, 0.0); }
float random(float x)          { return random(x, 0.0, 0.0); }

float fBm(float x, float y, float z, int octaves, float lacunarity, float gain) {
    float sum = 0.0, amp = 1.0, freq = 1.0;
    for (int i = 0; i < 32; i++) {
        if (i >= octaves) break;
        sum  += amp * noise(x * freq, y * freq, z * freq);
        freq *= lacunarity;
        amp  *= gain;
    }
    return sum;
}
float turbulence(float x, float y, float z, int octaves, float lacunarity, float gain) {
    float sum = 0.0, amp = 1.0, freq = 1.0;
    for (int i = 0; i < 32; i++) {
        if (i >= octaves) break;
        sum  += amp * abs(noise(x * freq, y * freq, z * freq));
        freq *= lacunarity;
        amp  *= gain;
    }
    return sum;
}

// =============================================================================
void main(void)
{
    vec2 res = vec2(adsk_result_w, adsk_result_h);
    vec2 uv  = gl_FragCoord.xy / res;

    // ---- Predefined variables, matching Nuke's Expression node --------------
    vec4  rgba = texture2D(front, uv);
    float r = rgba.r;
    float g = rgba.g;
    float b = rgba.b;
    float a = rgba.a;

    float x = gl_FragCoord.x;          // pixel coords, origin bottom-left (as in Nuke)
    float y = gl_FragCoord.y;
    float width  = adsk_result_w;
    float height = adsk_result_h;
    float cx = (x - width  * 0.5) / (width * 0.5);   // centred, aspect-preserved
    float cy = (y - height * 0.5) / (width * 0.5);   // (matches Nuke cx/cy)
    float frame = adsk_time;
    float t = frame;                   // Nuke parity: t is an alias for frame

    // =========================================================================
    //  EXPRESSION BLOCK  --  EDIT THESE FOUR LINES
    //  Write each channel exactly as you would in Nuke's Expression node.
    //  Available: r g b a  x y cx cy  width height frame (alias t)  k1 k2 k3 k4  ref
    //  plus the full function library above (sin, pow, clamp, lerp, noise, ...).
    //
    //  Examples (uncomment / replace the identity below):
    //    Gamma:     float R = pow(r, 1.0/k1);  ...        (k1 = gamma)
    //    Invert:    float R = 1.0 - r;  ...
    //    Luma:      float L = 0.2126*r+0.7152*g+0.0722*b; R=G=B=L;
    //    Gradient:  float R = uv_like = x/width;          (horizontal ramp)
    //    Vignette:  float v = 1.0 - hypot(cx,cy)*k1; R=r*v; G=g*v; B=b*v;
    //    Noise:     float n = noise(x*k1, y*k1, frame*0.1); R=r+n*k2; ...
    // -------------------------------------------------------------------------
    float R = r;
    float G = g;
    float B = b;
    float A = a;
    // =========================================================================

    vec4 result = vec4(R, G, B, A);

    if (clamp_output) result = clamp(result, 0.0, 1.0);
    result = mix(rgba, result, mix_amount);   // mix_amount = 1.0 -> pure expression

    gl_FragColor = result;
}
