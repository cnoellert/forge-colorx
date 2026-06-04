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
#define EXPR_PATHLOG(me, br) exprPathLog((me), (br))
#else
#define EXPR_PATHLOG(me, br) ((void)0)
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
                           "Variables: r g b a, x y, cx cy, width height, frame, pi, plus your temps."
#define kPluginIdentifier  "tv.diff.Expression"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

// number of temp-variable rows, matching Nuke's Expression tab
#define kNumTemps 4

// indices of the fixed predefined variables in the variable buffer
enum { V_R = 0, V_G, V_B, V_A, V_X, V_Y, V_CX, V_CY, V_WIDTH, V_HEIGHT, V_FRAME, V_FIXED_COUNT };

// ---- the compiled programs + everything the per-pixel loop needs -------------
struct ExprContext {
    std::vector<std::string> names;       // variable names, by slot index
    expreval::Program        chan[4];     // r,g,b,a output expressions
    expreval::Program        temps[kNumTemps];
    int                      tempSlot[kNumTemps]; // -1 if that row is unused
    int                      nVars = 0;
    double                   width = 0, height = 0, frame = 0;
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

                // ---- temp variables, evaluated in order ----
                for (int t = 0; t < kNumTemps; ++t)
                    if (_ctx->tempSlot[t] >= 0)
                        vars[_ctx->tempSlot[t]] = _ctx->temps[t].eval(&vars[0]);

                // ---- channel expressions ----
                double out[4];
                out[0] = _ctx->chan[0].eval(&vars[0]);
                out[1] = _ctx->chan[1].eval(&vars[0]);
                out[2] = _ctx->chan[2].eval(&vars[0]);
                out[3] = _ctx->chan[3].eval(&vars[0]);

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
        for (int t = 0; t < kNumTemps; ++t) {
            char n[16], e[16];
            std::sprintf(n, "tempName%d", t);
            std::sprintf(e, "tempExpr%d", t);
            _tempName[t] = fetchStringParam(n);
            _tempExpr[t] = fetchStringParam(e);
        }
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

    OFX::Clip*        _dstClip;
    OFX::Clip*        _srcClip;
    OFX::StringParam* _exprR; OFX::StringParam* _exprG;
    OFX::StringParam* _exprB; OFX::StringParam* _exprA;
    OFX::StringParam* _tempName[kNumTemps];
    OFX::StringParam* _tempExpr[kNumTemps];
};

void ExpressionPlugin::buildContext(const OFX::RenderArguments& args, ExprContext& ctx)
{
    // fixed predefined variable names, in slot order
    static const char* fixed[V_FIXED_COUNT] =
        { "r","g","b","a","x","y","cx","cy","width","height","frame" };
    ctx.names.assign(fixed, fixed + V_FIXED_COUNT);

    // append the named temp variables
    std::string tname[kNumTemps], texpr[kNumTemps];
    for (int t = 0; t < kNumTemps; ++t) {
        _tempName[t]->getValueAtTime(args.time, tname[t]);
        _tempExpr[t]->getValueAtTime(args.time, texpr[t]);
        ctx.tempSlot[t] = -1;
        if (!tname[t].empty()) {
            ctx.tempSlot[t] = (int)ctx.names.size();
            ctx.names.push_back(tname[t]);
        }
    }
    ctx.nVars = (int)ctx.names.size();

    std::string err;
    for (int t = 0; t < kNumTemps; ++t) {
        if (ctx.tempSlot[t] >= 0 && !ctx.temps[t].compile(texpr[t], ctx.names, err)) {
            try { setPersistentMessage(OFX::Message::eMessageError, "",
                "Temp '" + tname[t] + "': " + err); } catch (...) {}
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
    }

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
    for (int t = 0; t < kNumTemps; ++t)
        if (ctx.tempSlot[t] >= 0)
            temps.push_back(std::make_pair(ctx.tempSlot[t], ctx.temps[t].emitC()));
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

    std::string err;
    bool ok = expreval::metalRender(
        args.pMetalCmdQ, sd, hasSrc, dd,
        args.renderWindow.x1, args.renderWindow.y1,
        args.renderWindow.x2, args.renderWindow.y2,
        nComps, (float)ctx.width, (float)ctx.height, (float)ctx.frame,
        msl, err);
    if (!ok) { try { setPersistentMessage(OFX::Message::eMessageError, "",
                       "Metal render failed (CPU fallback): " + err); } catch (...) {} }
    return ok;
}
#endif // HAVE_METAL

void ExpressionPlugin::render(const OFX::RenderArguments& args)
{
    ExprContext ctx;
    buildContext(args, ctx);

#ifdef HAVE_METAL
    // Prefer the GPU when the host is driving a Metal render; fall back to CPU.
    if (args.isEnabledMetalRender && args.pMetalCmdQ) {
        bool gpu = renderMetal(args, ctx);
        EXPR_PATHLOG(1, gpu ? "metal" : "metal-failed->cpu");
        if (gpu) return;
    } else {
        EXPR_PATHLOG(args.isEnabledMetalRender ? 1 : 0, "cpu");
    }
#else
    EXPR_PATHLOG(0, "cpu-no-haveMetal");
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
    // Advertise the Metal render path; the host then MAY hand us float MTLBuffers
    // + a command queue at render (we fall back to CPU whenever it doesn't).
    try { desc.setSupportsMetalRender(true); } catch (...) {}
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

    OFX::PageParamDescriptor* page = desc.definePageParam("Controls");

    defineExprParam(desc, page, "exprR", "r =", "r");
    defineExprParam(desc, page, "exprG", "g =", "g");
    defineExprParam(desc, page, "exprB", "b =", "b");
    defineExprParam(desc, page, "exprA", "a =", "a");

    for (int t = 0; t < kNumTemps; ++t) {
        char n[16], e[16], ln[24], le[24];
        std::sprintf(n, "tempName%d", t);
        std::sprintf(e, "tempExpr%d", t);
        std::sprintf(ln, "temp name %d", t);
        std::sprintf(le, "temp expr %d", t);
        OFX::StringParamDescriptor* pn = desc.defineStringParam(n);
        pn->setLabel(ln);
        pn->setStringType(OFX::eStringTypeSingleLine);
        pn->setDefault("");
        pn->setAnimates(false);
        page->addChild(*pn);
        defineExprParam(desc, page, e, le, "");
    }
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
