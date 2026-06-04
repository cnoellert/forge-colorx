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

// --- Noise / random (Nuke: noise() signed ~[-1,1], random() [0,1]) -----------
float hash3(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float vnoise(vec3 p) {                          // smooth value noise in [0,1]
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i + vec3(0,0,0)), hash3(i + vec3(1,0,0)), u.x),
                   mix(hash3(i + vec3(0,1,0)), hash3(i + vec3(1,1,0)), u.x), u.y),
               mix(mix(hash3(i + vec3(0,0,1)), hash3(i + vec3(1,0,1)), u.x),
                   mix(hash3(i + vec3(0,1,1)), hash3(i + vec3(1,1,1)), u.x), u.y), u.z);
}
float noise(float x, float y, float z) { return vnoise(vec3(x, y, z)) * 2.0 - 1.0; }
float noise(float x, float y)          { return noise(x, y, 0.0); }
float noise(float x)                   { return noise(x, 0.0, 0.0); }

float random(float x, float y, float z) {
    return fract(sin(dot(vec3(x, y, z), vec3(12.9898, 78.233, 37.719))) * 43758.5453);
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

    // =========================================================================
    //  EXPRESSION BLOCK  --  EDIT THESE FOUR LINES
    //  Write each channel exactly as you would in Nuke's Expression node.
    //  Available: r g b a  x y cx cy  width height frame  k1 k2 k3 k4  ref
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
