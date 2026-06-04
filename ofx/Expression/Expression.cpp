// Expression.cpp
// -----------------------------------------------------------------------------
// "Expression" - an OpenFX re-creation of Nuke's Color > Math > Expression node.
//
// Four multiline string parameters (r, g, b, a) hold per-channel expressions
// that are parsed once per render (ExprEval.h) and evaluated per pixel. Four
// temp-variable rows (name + expression) mirror Nuke's temporaries. The node
// runs as a filter over its single source clip, multithreaded via the OFX
// Support library. Builds to a .ofx that loads in Flame (2021+), Nuke, Resolve,
// Fusion and any other OpenFX host.
//
// Predefined variables available to every expression:
//   r g b a            input channels (normalised 0..1)
//   x y                pixel coords, origin bottom-left (as in Nuke)
//   cx cy              centred, aspect-preserved coords (0,0 centre, +/-1 at L/R)
//   width height       image size (region of definition)
//   frame              current frame (render time)
//   pi e               constants (also pi(), e())
// plus any temp variables you name, plus the full function library in ExprEval.h.
// -----------------------------------------------------------------------------

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include "ExprEval.h"

// Metal GPU render path (macOS only — the Makefile defines HAVE_METAL and links
// the Metal/Foundation frameworks on Darwin). Pure-C++ bridge; see ExprMetal.h.
#ifdef HAVE_METAL
#include "ExprKernelMetal.h"
#include "ExprMetal.h"
#include <utility>

// OFX-Metal support is HOST-SPECIFIC. We only advertise it and take the GPU path
// on hosts we have actually verified drive the OFX Metal render correctly. Other
// hosts get the CPU path. Concretely: DaVinci Resolve's OFX host enables Metal and
// hands us valid MTLBuffers + a command queue (verified: pixels exact, path=metal),
// but Autodesk Flame's macOS OFX host hard-crashes when given our Metal node — so
// Flame (and any not-yet-verified host) must stay on the CPU path. Gated by the OFX
// host name; expand the allow-list only after verifying a host end-to-end.
static bool hostDrivesMetalSafely()
{
    try {
        OFX::ImageEffectHostDescription* h = OFX::getImageEffectHostDescription();
        if (h && h->hostName.find("Resolve") != std::string::npos) return true; // "DaVinciResolve"
    } catch (...) {}
    return false;
}
#endif

// CUDA GPU render path (Linux only — the Makefile defines HAVE_CUDA and links
// nvrtc/cuda when built WITH_CUDA=1). Pure-C++ bridge; see ExprCuda.h.
#ifdef HAVE_CUDA
#include "ExprKernelCuda.h"
#include "ExprCuda.h"
#include <utility>

// OFX-CUDA is HOST-SPECIFIC, exactly like OFX-Metal. We only advertise it and take
// the GPU path on hosts verified to drive OFX-CUDA correctly — DaVinci Resolve on
// Linux drives OFX-CUDA (hands us device buffers + a stream). Autodesk Flame's OFX
// host is CPU-leaning, so gating to Resolve keeps Flame (and any unverified host)
// safely on the CPU path — the same protection hostDrivesMetalSafely() gives on
// macOS. Expand the allow-list only after verifying a host end-to-end.
static bool hostDrivesCudaSafely()
{
    try {
        OFX::ImageEffectHostDescription* h = OFX::getImageEffectHostDescription();
        if (h && h->hostName.find("Resolve") != std::string::npos) return true; // "DaVinciResolve"
    } catch (...) {}
    return false;
}
#endif

// Opt-in render-path diagnostic (off in normal + CI builds). Build with
// -DEXPR_PATH_LOG to record, per render, the OFX host name, whether the host
// enabled Metal, and which branch actually executed — to confirm whether a given
// host (Resolve, Flame, ...) drives our Metal kernel or the CPU fallback. Append
// to /tmp/expr_path.log; read with `sort /tmp/expr_path.log | uniq -c`.
#ifdef EXPR_PATH_LOG
#include <cstdio>
static void exprPathLog(int metalEnabled, const char* branch)
{
    const char* host = "?";
    try {
        OFX::ImageEffectHostDescription* h = OFX::getImageEffectHostDescription();
        if (h) host = h->hostName.c_str();
    } catch (...) {}
    FILE* f = std::fopen("/tmp/expr_path.log", "a");
    if (!f) return;
    std::fprintf(f, "host=%s metalEnabled=%d branch=%s\n", host, metalEnabled, branch);
    std::fclose(f);
}
static void exprPathLogCuda(int cudaEnabled, const char* branch)
{
    const char* host = "?";
    try {
        OFX::ImageEffectHostDescription* h = OFX::getImageEffectHostDescription();
        if (h) host = h->hostName.c_str();
    } catch (...) {}
    FILE* f = std::fopen("/tmp/expr_path.log", "a");
    if (!f) return;
    std::fprintf(f, "host=%s cudaEnabled=%d branch=%s\n", host, cudaEnabled, branch);
    std::fclose(f);
}
#define EXPR_PATHLOG(me, br) exprPathLog((me), (br))
#define EXPR_PATHLOG_CUDA(ce, br) exprPathLogCuda((ce), (br))
#else
#define EXPR_PATHLOG(me, br) ((void)0)
#define EXPR_PATHLOG_CUDA(ce, br) ((void)0)
#endif

// ---- IEEE-754 half <-> float (Fusion/Resolve deliver 16-bit float images) ----
static inline float halfToFloat(unsigned short h)
{
    unsigned int s = (h >> 15) & 1u, e = (h >> 10) & 0x1fu, m = h & 0x3ffu, out;
    if (e == 0) {
        if (m == 0) { out = s << 31; }                       // +/- zero
        else {                                               // subnormal
            e = 127 - 15 + 1;
            while ((m & 0x400u) == 0) { m <<= 1; --e; }
            m &= 0x3ffu;
            out = (s << 31) | (e << 23) | (m << 13);
        }
    } else if (e == 0x1fu) {                                 // inf / nan
        out = (s << 31) | (0xffu << 23) | (m << 13);
    } else {                                                 // normal
        out = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
    }
    float f; std::memcpy(&f, &out, 4); return f;
}
static inline unsigned short floatToHalf(float f)
{
    unsigned int x; std::memcpy(&x, &f, 4);
    unsigned int s = (x >> 16) & 0x8000u;
    int          e = (int)((x >> 23) & 0xffu) - 127 + 15;
    unsigned int m = x & 0x7fffffu;
    if (e <= 0) {                                            // subnormal / underflow
        if (e < -10) return (unsigned short)s;
        m |= 0x800000u;
        int shift = 14 - e;
        unsigned int hm = m >> shift;
        if ((m >> (shift - 1)) & 1u) ++hm;                   // round to nearest
        return (unsigned short)(s | hm);
    } else if (e >= 0x1f) {                                  // overflow -> inf
        return (unsigned short)(s | 0x7c00u);
    }
    unsigned short hm = (unsigned short)(s | (e << 10) | (m >> 13));
    if ((m >> 12) & 1u) ++hm;                                // round to nearest
    return hm;
}

#define kPluginName        "Expression"
#define kPluginGrouping    "Color/Math"
#define kPluginDescription "Per-channel math expressions, a re-creation of Nuke's Expression node.\n" \
                           "Variables: r g b a, x y, cx cy, width height, frame (alias t), pi, k1..k4 and\n" \
                           "ref.r/ref.g/ref.b knobs, plus your temps. Mix blends original<->result; Clamp "\
                           "clamps to 0..1."
#define kPluginIdentifier  "tv.diff.Expression"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

// indices of the fixed predefined variables in the variable buffer. r..frame are
// per-pixel / per-render; k1..k4 and ref.r/.g/.b are the user-constant knobs
// (uniform across the frame) that bring the OFX in line with the Matchbox build.
enum { V_R = 0, V_G, V_B, V_A, V_X, V_Y, V_CX, V_CY, V_WIDTH, V_HEIGHT, V_FRAME,
       V_K1, V_K2, V_K3, V_K4, V_REFR, V_REFG, V_REFB, V_FIXED_COUNT };

// ---- the compiled programs + everything the per-pixel loop needs -------------
struct ExprContext {
    std::vector<std::string> names;       // variable names, by slot index
    expreval::Program        chan[4];     // r,g,b,a output expressions
    // Derived bindings, evaluated in order before the channel exprs. This carries
    // BOTH kinds of named token: the named-constant aliases (a name for a k-knob,
    // value driven by the k slider) and the name+formula temps (a value derived
    // from the image). Both end up here as (slot, program) pairs, so the per-pixel
    // loop and the GPU kernel treat them uniformly.
    std::vector<int>              derivedSlot;
    std::vector<expreval::Program> derived;
    int                      nVars = 0;
    double                   width = 0, height = 0, frame = 0;
    // user-constant knobs (mirror the Matchbox: k1..k4, ref colour, mix, clamp)
    double                   k[4]   = {1, 0, 0, 0};   // k1 defaults to 1, rest 0
    double                   ref[3] = {0, 0, 0};
    double                   mixAmt = 1.0;            // 0 = original image, 1 = result
    bool                     clampOut = false;        // clamp result to [0,1]
};

// ============================================================================
//  Per-pixel processor (templated on pixel type / component count / max value)
// ============================================================================
template <class PIX, int nComps, int maxValue, bool HALF = false>
class ExpressionProcessor : public OFX::ImageProcessor {
public:
    explicit ExpressionProcessor(OFX::ImageEffect& instance)
        : OFX::ImageProcessor(instance), _srcImg(0), _ctx(0) {}

    void setSrcImg(OFX::Image* v) { _srcImg = v; }
    void setContext(const ExprContext* c) { _ctx = c; }

    virtual void multiThreadProcessImages(OfxRectI window)
    {
        const bool isFloat = (maxValue == 1);
        const double invMax = isFloat ? 1.0 : 1.0 / double(maxValue);
        std::vector<double> vars(_ctx->nVars, 0.0);

        vars[V_WIDTH]  = _ctx->width;
        vars[V_HEIGHT] = _ctx->height;
        vars[V_FRAME]  = _ctx->frame;
        vars[V_K1] = _ctx->k[0]; vars[V_K2] = _ctx->k[1];
        vars[V_K3] = _ctx->k[2]; vars[V_K4] = _ctx->k[3];
        vars[V_REFR] = _ctx->ref[0]; vars[V_REFG] = _ctx->ref[1]; vars[V_REFB] = _ctx->ref[2];
        const double mixAmt = _ctx->mixAmt; const bool clampOut = _ctx->clampOut;
        const double halfW = _ctx->width  * 0.5;
        const double halfH = _ctx->height * 0.5;
        const double invHalfW = (halfW != 0.0) ? 1.0 / halfW : 0.0;

        for (int y = window.y1; y < window.y2; ++y) {
            if (_effect.abort()) break;
            PIX* dstPix = (PIX*)_dstImg->getPixelAddress(window.x1, y);

            for (int x = window.x1; x < window.x2; ++x, dstPix += nComps) {
                // ---- read source (0 outside bounds / when unconnected) ----
                double r = 0, g = 0, b = 0, a = (nComps == 4 || nComps == 1) ? 0.0 : 1.0;
                if (_srcImg) {
                    PIX* s = (PIX*)_srcImg->getPixelAddress(x, y);
                    if (s) {
                        if (nComps == 1) {              // alpha only
                            a = HALF ? halfToFloat((unsigned short)s[0]) : s[0] * invMax;
                        } else {                        // RGB or RGBA
                            r = HALF ? halfToFloat((unsigned short)s[0]) : s[0] * invMax;
                            g = HALF ? halfToFloat((unsigned short)s[1]) : s[1] * invMax;
                            b = HALF ? halfToFloat((unsigned short)s[2]) : s[2] * invMax;
                            if (nComps == 4) a = HALF ? halfToFloat((unsigned short)s[3]) : s[3] * invMax;
                        }
                    }
                }

                // ---- bind predefined variables ----
                vars[V_R] = r; vars[V_G] = g; vars[V_B] = b; vars[V_A] = a;
                vars[V_X] = (double)x; vars[V_Y] = (double)y;
                vars[V_CX] = ((double)x - halfW) * invHalfW;
                vars[V_CY] = ((double)y - halfH) * invHalfW;   // divide by halfW -> aspect preserved

                // ---- derived bindings (named constants + temps), in order ----
                for (size_t d = 0; d < _ctx->derived.size(); ++d)
                    vars[_ctx->derivedSlot[d]] = _ctx->derived[d].eval(&vars[0]);

                // ---- channel expressions ----
                double out[4];
                out[0] = _ctx->chan[0].eval(&vars[0]);
                out[1] = _ctx->chan[1].eval(&vars[0]);
                out[2] = _ctx->chan[2].eval(&vars[0]);
                out[3] = _ctx->chan[3].eval(&vars[0]);

                // ---- output knobs: clamp then mix vs the original (matches the
                //      Matchbox main(): clamp first, then mix(orig, result, amt)) ----
                if (clampOut) for (int c = 0; c < 4; ++c)
                    out[c] = out[c] < 0.0 ? 0.0 : (out[c] > 1.0 ? 1.0 : out[c]);
                if (mixAmt != 1.0) {
                    const double orig[4] = { r, g, b, a };
                    for (int c = 0; c < 4; ++c) out[c] = orig[c] + (out[c] - orig[c]) * mixAmt;
                }

                // ---- write, remapping channels to the actual component layout ----
                if (nComps == 1) {
                    dstPix[0] = convert(out[3], isFloat);            // alpha
                } else {
                    dstPix[0] = convert(out[0], isFloat);
                    dstPix[1] = convert(out[1], isFloat);
                    dstPix[2] = convert(out[2], isFloat);
                    if (nComps == 4) dstPix[3] = convert(out[3], isFloat);
                }
            }
        }
    }

private:
    static PIX convert(double v, bool isFloat) {
        if (HALF) return (PIX)floatToHalf((float)v);        // 16-bit float, no clamp
        if (isFloat) return (PIX)v;                         // HDR-safe, no clamp
        double s = std::floor(v * maxValue + 0.5);
        if (s < 0) s = 0;
        if (s > maxValue) s = maxValue;
        return (PIX)s;
    }

    OFX::Image*        _srcImg;
    const ExprContext* _ctx;
};

// ============================================================================
//  Plugin instance
// ============================================================================
class ExpressionPlugin : public OFX::ImageEffect {
public:
    explicit ExpressionPlugin(OfxImageEffectHandle handle)
        : OFX::ImageEffect(handle)
    {
        _dstClip = fetchClip(kOfxImageEffectOutputClipName);
        _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
        _exprR = fetchStringParam("exprR");
        _exprG = fetchStringParam("exprG");
        _exprB = fetchStringParam("exprB");
        _exprA = fetchStringParam("exprA");
        _vars  = fetchStringParam("vars");
        _k1 = fetchDoubleParam("k1"); _k2 = fetchDoubleParam("k2");
        _k3 = fetchDoubleParam("k3"); _k4 = fetchDoubleParam("k4");
        _ref = fetchRGBParam("ref");
        _mix = fetchDoubleParam("mix");
        _clamp = fetchBooleanParam("clampOutput");
    }

    virtual void render(const OFX::RenderArguments& args);

private:
    void buildContext(const OFX::RenderArguments& args, ExprContext& ctx);

    template <class PIX, int nComps, int maxValue, bool HALF = false>
    void process(const OFX::RenderArguments& args, const ExprContext& ctx);

#ifdef HAVE_METAL
    // GPU render via Metal. Returns false (caller falls back to CPU) if the host
    // isn't giving float Metal buffers or anything goes wrong.
    bool renderMetal(const OFX::RenderArguments& args, const ExprContext& ctx);
#endif
#ifdef HAVE_CUDA
    // GPU render via CUDA. Returns false (caller falls back to CPU) if the host
    // isn't giving float device buffers or anything goes wrong.
    bool renderCuda(const OFX::RenderArguments& args, const ExprContext& ctx);
#endif

    OFX::Clip*        _dstClip;
    OFX::Clip*        _srcClip;
    OFX::StringParam* _exprR; OFX::StringParam* _exprG;
    OFX::StringParam* _exprB; OFX::StringParam* _exprA;
    OFX::StringParam* _vars;              // multi-line "name = expression" block
    OFX::DoubleParam* _k1; OFX::DoubleParam* _k2;
    OFX::DoubleParam* _k3; OFX::DoubleParam* _k4;
    OFX::RGBParam*    _ref;
    OFX::DoubleParam* _mix;
    OFX::BooleanParam* _clamp;
};

static std::string trimws(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// A user variable name: a plain identifier (letters/digits/underscore, not starting
// with a digit). Keeps names unambiguous in expressions; ref.r etc. are predefined.
static bool validIdent(const std::string& s)
{
    if (s.empty()) return false;
    char c0 = s[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_')) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

void ExpressionPlugin::buildContext(const OFX::RenderArguments& args, ExprContext& ctx)
{
    // fixed predefined variable names, in slot order (must match the V_* enum)
    static const char* fixed[V_FIXED_COUNT] =
        { "r","g","b","a","x","y","cx","cy","width","height","frame",
          "k1","k2","k3","k4","ref.r","ref.g","ref.b" };
    ctx.names.assign(fixed, fixed + V_FIXED_COUNT);

    // Build the derived bindings from the multi-line Variables block — one
    // "name = expression" per line, evaluated in order before the channels so a
    // later line can use an earlier one (and any line can use k1..k4 / ref.r..).
    // This is both kinds of named token in one field: `gamma = k1` is a named
    // constant driven by the slider; `lum = 0.2126*r+...` is a derived variable.
    // Blank lines and lines starting with '#' are ignored. (names_ stores the
    // address of the ctx.names vector object, stable across push_backs, so
    // compiling against the growing list is safe.)
    ctx.derived.clear();
    ctx.derivedSlot.clear();
    std::string err;

    std::string varsSrc;
    _vars->getValueAtTime(args.time, varsSrc);
    size_t pos = 0; int lineNo = 0;
    while (pos < varsSrc.size()) {
        size_t eol = varsSrc.find('\n', pos);
        std::string line = varsSrc.substr(pos, (eol == std::string::npos) ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? varsSrc.size() : eol + 1;
        ++lineNo;
        line = trimws(line);
        if (line.empty() || line[0] == '#') continue;

        char ln[16]; std::sprintf(ln, "%d", lineNo);
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            try { setPersistentMessage(OFX::Message::eMessageError, "",
                std::string("Variables line ") + ln + ": expected 'name = expression'"); } catch (...) {}
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
        std::string name = trimws(line.substr(0, eq));
        std::string expr = trimws(line.substr(eq + 1));
        if (!validIdent(name)) {
            try { setPersistentMessage(OFX::Message::eMessageError, "",
                std::string("Variables line ") + ln + ": '" + name + "' is not a valid name"); } catch (...) {}
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
        int slot = (int)ctx.names.size();
        ctx.names.push_back(name);
        expreval::Program p;
        if (!p.compile(expr, ctx.names, err)) {
            try { setPersistentMessage(OFX::Message::eMessageError, "",
                "Variable '" + name + "': " + err); } catch (...) {}
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
        ctx.derivedSlot.push_back(slot);
        ctx.derived.push_back(p);
    }
    ctx.nVars = (int)ctx.names.size();

    std::string es[4];
    _exprR->getValueAtTime(args.time, es[0]);
    _exprG->getValueAtTime(args.time, es[1]);
    _exprB->getValueAtTime(args.time, es[2]);
    _exprA->getValueAtTime(args.time, es[3]);
    static const char* label[4] = { "r", "g", "b", "a" };
    for (int c = 0; c < 4; ++c) {
        if (!ctx.chan[c].compile(es[c], ctx.names, err)) {
            // setPersistentMessage is wrapped: some hosts (e.g. Resolve's Fusion
            // OFX) don't implement the message suite and throw on it.
            try { setPersistentMessage(OFX::Message::eMessageError, "",
                std::string("Channel ") + label[c] + ": " + err); } catch (...) {}
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
    }
    // Resolve's Fusion OFX host lacks the message suite, so clearPersistentMessage()
    // throws kOfxStatErrUnsupported there. Swallow it so render() can proceed.
    try { clearPersistentMessage(); } catch (...) {}

    // getRegionOfDefinition can be unsupported mid-render on some hosts; fall back
    // to the render window (== full image for a non-tiled host) if it throws.
    OfxRectD rod;
    try {
        rod = _dstClip->getRegionOfDefinition(args.time);
    } catch (...) {
        rod.x1 = args.renderWindow.x1; rod.y1 = args.renderWindow.y1;
        rod.x2 = args.renderWindow.x2; rod.y2 = args.renderWindow.y2;
    }
    ctx.width  = rod.x2 - rod.x1;
    ctx.height = rod.y2 - rod.y1;
    ctx.frame  = args.time;

    // user-constant knobs (k1..k4, ref colour, mix, clamp)
    _k1->getValueAtTime(args.time, ctx.k[0]); _k2->getValueAtTime(args.time, ctx.k[1]);
    _k3->getValueAtTime(args.time, ctx.k[2]); _k4->getValueAtTime(args.time, ctx.k[3]);
    _ref->getValueAtTime(args.time, ctx.ref[0], ctx.ref[1], ctx.ref[2]);
    _mix->getValueAtTime(args.time, ctx.mixAmt);
    _clamp->getValueAtTime(args.time, ctx.clampOut);
}

template <class PIX, int nComps, int maxValue, bool HALF>
void ExpressionPlugin::process(const OFX::RenderArguments& args, const ExprContext& ctx)
{
    std::unique_ptr<OFX::Image> dst(_dstClip->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_srcClip && _srcClip->isConnected()
                                  ? _srcClip->fetchImage(args.time) : 0);

    ExpressionProcessor<PIX, nComps, maxValue, HALF> proc(*this);
    proc.setDstImg(dst.get());
    proc.setSrcImg(src.get());
    proc.setContext(&ctx);
    proc.setRenderWindow(args.renderWindow);
    proc.process();
}

#ifdef HAVE_METAL
bool ExpressionPlugin::renderMetal(const OFX::RenderArguments& args, const ExprContext& ctx)
{
    // GPU path is float-only (Resolve's Metal render delivers float buffers).
    if (_dstClip->getPixelDepth() != OFX::eBitDepthFloat) return false;

    OFX::PixelComponentEnum comps = _dstClip->getPixelComponents();
    int nComps = (comps == OFX::ePixelComponentRGBA)  ? 4
               : (comps == OFX::ePixelComponentRGB)   ? 3
               : (comps == OFX::ePixelComponentAlpha) ? 1 : 0;
    if (nComps == 0) return false;

    // Transpile the compiled programs to one MSL kernel for this expression set.
    std::string chan[4] = { ctx.chan[0].emitC(), ctx.chan[1].emitC(),
                            ctx.chan[2].emitC(), ctx.chan[3].emitC() };
    std::vector<std::pair<int, std::string> > temps;
    for (size_t d = 0; d < ctx.derived.size(); ++d)
        temps.push_back(std::make_pair(ctx.derivedSlot[d], ctx.derived[d].emitC()));
    std::string msl = expreval::buildMetalPixelKernel(chan, temps, ctx.nVars);

    std::unique_ptr<OFX::Image> dst(_dstClip->fetchImage(args.time));
    if (!dst.get()) return false;
    std::unique_ptr<OFX::Image> src(_srcClip && _srcClip->isConnected()
                                  ? _srcClip->fetchImage(args.time) : 0);

    expreval::MetalImageDesc dd, sd;
    OfxRectI db = dst->getBounds();
    dd.buffer = dst->getPixelData();
    dd.x1 = db.x1; dd.y1 = db.y1;
    dd.rowFloats = dst->getRowBytes() / (int)sizeof(float);
    if (!dd.buffer) return false;

    int hasSrc = 0;
    if (src.get()) {
        OfxRectI sb = src->getBounds();
        sd.buffer = src->getPixelData();
        sd.x1 = sb.x1; sd.y1 = sb.y1;
        sd.rowFloats = src->getRowBytes() / (int)sizeof(float);
        hasSrc = (sd.buffer != 0) ? 1 : 0;
    }

    expreval::ExprKnobs knobs;
    for (int i = 0; i < 4; ++i) knobs.k[i] = (float)ctx.k[i];
    for (int i = 0; i < 3; ++i) knobs.ref[i] = (float)ctx.ref[i];
    knobs.mix = (float)ctx.mixAmt; knobs.clampOut = ctx.clampOut ? 1 : 0;

    std::string err;
    bool ok = expreval::metalRender(
        args.pMetalCmdQ, sd, hasSrc, dd,
        args.renderWindow.x1, args.renderWindow.y1,
        args.renderWindow.x2, args.renderWindow.y2,
        nComps, (float)ctx.width, (float)ctx.height, (float)ctx.frame,
        knobs, msl, err);
    if (!ok) { try { setPersistentMessage(OFX::Message::eMessageError, "",
                       "Metal render failed (CPU fallback): " + err); } catch (...) {} }
    return ok;
}
#endif // HAVE_METAL

#ifdef HAVE_CUDA
bool ExpressionPlugin::renderCuda(const OFX::RenderArguments& args, const ExprContext& ctx)
{
    // GPU path is float-only (Resolve's CUDA render delivers float device buffers).
    if (_dstClip->getPixelDepth() != OFX::eBitDepthFloat) return false;

    OFX::PixelComponentEnum comps = _dstClip->getPixelComponents();
    int nComps = (comps == OFX::ePixelComponentRGBA)  ? 4
               : (comps == OFX::ePixelComponentRGB)   ? 3
               : (comps == OFX::ePixelComponentAlpha) ? 1 : 0;
    if (nComps == 0) return false;

    // Transpile the compiled programs to one CUDA kernel for this expression set.
    std::string chan[4] = { ctx.chan[0].emitC(), ctx.chan[1].emitC(),
                            ctx.chan[2].emitC(), ctx.chan[3].emitC() };
    std::vector<std::pair<int, std::string> > temps;
    for (size_t d = 0; d < ctx.derived.size(); ++d)
        temps.push_back(std::make_pair(ctx.derivedSlot[d], ctx.derived[d].emitC()));
    std::string cu = expreval::buildCudaPixelKernel(chan, temps, ctx.nVars);

    std::unique_ptr<OFX::Image> dst(_dstClip->fetchImage(args.time));
    if (!dst.get()) return false;
    std::unique_ptr<OFX::Image> src(_srcClip && _srcClip->isConnected()
                                  ? _srcClip->fetchImage(args.time) : 0);

    expreval::CudaImageDesc dd, sd;
    OfxRectI db = dst->getBounds();
    dd.buffer = dst->getPixelData();
    dd.x1 = db.x1; dd.y1 = db.y1;
    dd.rowFloats = dst->getRowBytes() / (int)sizeof(float);
    if (!dd.buffer) return false;

    int hasSrc = 0;
    if (src.get()) {
        OfxRectI sb = src->getBounds();
        sd.buffer = src->getPixelData();
        sd.x1 = sb.x1; sd.y1 = sb.y1;
        sd.rowFloats = src->getRowBytes() / (int)sizeof(float);
        hasSrc = (sd.buffer != 0) ? 1 : 0;
    }

    expreval::ExprKnobs knobs;
    for (int i = 0; i < 4; ++i) knobs.k[i] = (float)ctx.k[i];
    for (int i = 0; i < 3; ++i) knobs.ref[i] = (float)ctx.ref[i];
    knobs.mix = (float)ctx.mixAmt; knobs.clampOut = ctx.clampOut ? 1 : 0;

    std::string err;
    bool ok = expreval::cudaRender(
        args.pCudaStream, sd, hasSrc, dd,
        args.renderWindow.x1, args.renderWindow.y1,
        args.renderWindow.x2, args.renderWindow.y2,
        nComps, (float)ctx.width, (float)ctx.height, (float)ctx.frame,
        knobs, cu, err);
    if (!ok) { try { setPersistentMessage(OFX::Message::eMessageError, "",
                       "CUDA render failed (CPU fallback): " + err); } catch (...) {} }
    return ok;
}
#endif // HAVE_CUDA

void ExpressionPlugin::render(const OFX::RenderArguments& args)
{
    ExprContext ctx;
    buildContext(args, ctx);

#ifdef HAVE_METAL
    // Prefer the GPU only on a verified host (hostDrivesMetalSafely) that is
    // actually driving a Metal render; otherwise CPU. The host gate is what keeps
    // Flame (which crashes in the Metal dispatch) safely on the CPU path.
    if (hostDrivesMetalSafely() && args.isEnabledMetalRender && args.pMetalCmdQ) {
        bool gpu = renderMetal(args, ctx);
        EXPR_PATHLOG(1, gpu ? "metal" : "metal-failed->cpu");
        if (gpu) return;
    } else {
        EXPR_PATHLOG(args.isEnabledMetalRender ? 1 : 0, "cpu");
    }
#else
    EXPR_PATHLOG(0, "cpu-no-haveMetal");
#endif

#ifdef HAVE_CUDA
    // Same shape as the Metal branch: prefer the GPU only on a verified host
    // (hostDrivesCudaSafely) actually driving a CUDA render; otherwise CPU. The
    // host gate keeps CPU-leaning hosts (Flame) safely off the CUDA path.
    if (hostDrivesCudaSafely() && args.isEnabledCudaRender && args.pCudaStream) {
        bool gpu = renderCuda(args, ctx);
        EXPR_PATHLOG_CUDA(1, gpu ? "cuda" : "cuda-failed->cpu");
        if (gpu) return;
    } else {
        EXPR_PATHLOG_CUDA(args.isEnabledCudaRender ? 1 : 0, "cpu");
    }
#endif

    OFX::BitDepthEnum       depth = _dstClip->getPixelDepth();
    OFX::PixelComponentEnum comps = _dstClip->getPixelComponents();

    if (comps == OFX::ePixelComponentRGBA) {
        switch (depth) {
            case OFX::eBitDepthUByte:  process<unsigned char,  4, 255>      (args, ctx); break;
            case OFX::eBitDepthUShort: process<unsigned short, 4, 65535>    (args, ctx); break;
            case OFX::eBitDepthHalf:   process<unsigned short, 4, 1, true>  (args, ctx); break;
            case OFX::eBitDepthFloat:  process<float,          4, 1>        (args, ctx); break;
            default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }
    } else if (comps == OFX::ePixelComponentRGB) {
        switch (depth) {
            case OFX::eBitDepthUByte:  process<unsigned char,  3, 255>      (args, ctx); break;
            case OFX::eBitDepthUShort: process<unsigned short, 3, 65535>    (args, ctx); break;
            case OFX::eBitDepthHalf:   process<unsigned short, 3, 1, true>  (args, ctx); break;
            case OFX::eBitDepthFloat:  process<float,          3, 1>        (args, ctx); break;
            default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }
    } else if (comps == OFX::ePixelComponentAlpha) {
        switch (depth) {
            case OFX::eBitDepthUByte:  process<unsigned char,  1, 255>      (args, ctx); break;
            case OFX::eBitDepthUShort: process<unsigned short, 1, 65535>    (args, ctx); break;
            case OFX::eBitDepthHalf:   process<unsigned short, 1, 1, true>  (args, ctx); break;
            case OFX::eBitDepthFloat:  process<float,          1, 1>        (args, ctx); break;
            default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }
    } else {
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

// ============================================================================
//  Factory
// ============================================================================
mDeclarePluginFactory(ExpressionPluginFactory, {}, {});

static void defineExprParam(OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                            const char* name, const char* label, const char* def)
{
    OFX::StringParamDescriptor* p = desc.defineStringParam(name);
    p->setLabel(label);
    p->setStringType(OFX::eStringTypeMultiLine);
    p->setDefault(def);
    p->setAnimates(true);
    page->addChild(*p);
}

void ExpressionPluginFactory::describe(OFX::ImageEffectDescriptor& desc)
{
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    desc.addSupportedContext(OFX::eContextFilter);
    desc.addSupportedContext(OFX::eContextGeneral);
    desc.addSupportedBitDepth(OFX::eBitDepthUByte);
    desc.addSupportedBitDepth(OFX::eBitDepthUShort);
    desc.addSupportedBitDepth(OFX::eBitDepthHalf);
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);

    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(true);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(OFX::eRenderFullySafe);

#ifdef HAVE_METAL
    // Advertise the Metal render path ONLY to hosts we've verified handle it (see
    // hostDrivesMetalSafely) — Flame's macOS OFX host crashes on our Metal node, so
    // we must not advertise Metal to it. Verified hosts then MAY hand us float
    // MTLBuffers + a command queue at render (we fall back to CPU if they don't).
    if (hostDrivesMetalSafely()) { try { desc.setSupportsMetalRender(true); } catch (...) {} }
#endif

#ifdef HAVE_CUDA
    // Advertise CUDA render ONLY to verified hosts (see hostDrivesCudaSafely) —
    // Resolve on Linux drives OFX-CUDA; CPU-leaning hosts must not be offered it.
    // Verified hosts then MAY hand us float device buffers + a stream at render
    // (we fall back to CPU if they don't).
    if (hostDrivesCudaSafely()) { try { desc.setSupportsCudaRender(true); } catch (...) {} }
#endif
}

void ExpressionPluginFactory::describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum /*context*/)
{
    OFX::ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src->addSupportedComponent(OFX::ePixelComponentRGBA);
    src->addSupportedComponent(OFX::ePixelComponentRGB);
    src->addSupportedComponent(OFX::ePixelComponentAlpha);
    src->setTemporalClipAccess(false);
    src->setSupportsTiles(true);
    src->setIsMask(false);

    OFX::ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(OFX::ePixelComponentRGBA);
    dst->addSupportedComponent(OFX::ePixelComponentRGB);
    dst->addSupportedComponent(OFX::ePixelComponentAlpha);
    dst->setSupportsTiles(true);

    // Four separate pages so the panel reads clearly. Hosts that honour OFX pages
    // show them as tabs; Flame labels each auto-laid-out column group by its page
    // name (so the previously-ambiguous boxes now sit under Channels / Variables /
    // Constants / Output).
    OFX::PageParamDescriptor* pgChan  = desc.definePageParam("Channels");
    OFX::PageParamDescriptor* pgVars  = desc.definePageParam("Variables");
    OFX::PageParamDescriptor* pgConst = desc.definePageParam("Constants");
    OFX::PageParamDescriptor* pgOut   = desc.definePageParam("Output");

    // --- Channels: the four output expressions ---
    defineExprParam(desc, pgChan, "exprR", "r =", "r");
    defineExprParam(desc, pgChan, "exprG", "g =", "g");
    defineExprParam(desc, pgChan, "exprB", "b =", "b");
    defineExprParam(desc, pgChan, "exprA", "a =", "a");

    // --- Variables: ONE multi-line block, one "name = expression" per line,
    //     evaluated top to bottom before the channels. Holds both kinds of named
    //     token: `gamma = k1` (a named constant driven by the slider) and
    //     `lum = 0.2126*r+...` (a value derived from the image). Self-labelling, so
    //     it stays clean even on hosts that don't render text-field labels. ---
    {
        OFX::StringParamDescriptor* v = desc.defineStringParam("vars");
        v->setLabel("variables");
        v->setStringType(OFX::eStringTypeMultiLine);
        v->setDefault("# one per line:  name = expression  (e.g.  gamma = k1)");
        v->setHint("Declare variables, one 'name = expression' per line; use the names in your "
                   "channel expressions. Reference knobs as k1..k4 (e.g. gamma = k1). "
                   "Blank lines and lines starting with # are ignored.");
        v->setAnimates(false);
        pgVars->addChild(*v);
    }

    // --- Constants: animatable k-knobs. Reference them as k1..k4, or alias one in
    //     the Variables block (e.g. gamma = k1) to use a friendly name. ---
    for (int i = 0; i < 4; ++i) {
        char kn[8], lk[8];
        std::sprintf(kn, "k%d", i + 1);
        std::sprintf(lk, "k%d", i + 1);
        OFX::DoubleParamDescriptor* p = desc.defineDoubleParam(kn);
        p->setLabel(lk);
        p->setDefault(i == 0 ? 1.0 : 0.0);
        p->setRange(-1e6, 1e6);
        p->setDisplayRange(-1.0, 1.0);
        p->setIncrement(0.001);
        p->setHint("Animatable scalar; reference as kN (or alias it in the Variables block).");
        p->setAnimates(true);
        pgConst->addChild(*p);
    }
    OFX::RGBParamDescriptor* ref = desc.defineRGBParam("ref");
    ref->setLabel("Reference Colour");
    ref->setDefault(0.0, 0.0, 0.0);
    ref->setHint("Reference colour; reference it as ref.r ref.g ref.b in your expressions.");
    ref->setAnimates(true);
    pgConst->addChild(*ref);

    // --- Output: post-process the result (clamp first, then mix vs the original) ---
    OFX::DoubleParamDescriptor* mix = desc.defineDoubleParam("mix");
    mix->setLabel("Mix");
    mix->setDefault(1.0);
    mix->setRange(0.0, 1.0);
    mix->setDisplayRange(0.0, 1.0);
    mix->setIncrement(0.001);
    mix->setHint("Blend between the original image (0) and the expression result (1).");
    mix->setAnimates(true);
    pgOut->addChild(*mix);

    OFX::BooleanParamDescriptor* clampOut = desc.defineBooleanParam("clampOutput");
    clampOut->setLabel("Clamp Output");
    clampOut->setDefault(false);
    clampOut->setHint("Clamp the result to the 0..1 range (like Nuke's clamp(x)).");
    clampOut->setAnimates(true);
    pgOut->addChild(*clampOut);
}

OFX::ImageEffect* ExpressionPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/)
{
    return new ExpressionPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray& ids)
{
    static ExpressionPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
