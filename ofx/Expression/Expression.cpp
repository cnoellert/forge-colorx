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
template <class PIX, int nComps, int maxValue>
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
                            a = s[0] * invMax;
                        } else {                        // RGB or RGBA
                            r = s[0] * invMax;
                            g = s[1] * invMax;
                            b = s[2] * invMax;
                            if (nComps == 4) a = s[3] * invMax;
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

    template <class PIX, int nComps, int maxValue>
    void process(const OFX::RenderArguments& args, const ExprContext& ctx);

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
            setPersistentMessage(OFX::Message::eMessageError, "",
                "Temp '" + tname[t] + "': " + err);
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
            setPersistentMessage(OFX::Message::eMessageError, "",
                std::string("Channel ") + label[c] + ": " + err);
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
    }
    clearPersistentMessage();

    OfxRectD rod = _dstClip->getRegionOfDefinition(args.time);
    ctx.width  = rod.x2 - rod.x1;
    ctx.height = rod.y2 - rod.y1;
    ctx.frame  = args.time;
}

template <class PIX, int nComps, int maxValue>
void ExpressionPlugin::process(const OFX::RenderArguments& args, const ExprContext& ctx)
{
    std::unique_ptr<OFX::Image> dst(_dstClip->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_srcClip && _srcClip->isConnected()
                                  ? _srcClip->fetchImage(args.time) : 0);

    ExpressionProcessor<PIX, nComps, maxValue> proc(*this);
    proc.setDstImg(dst.get());
    proc.setSrcImg(src.get());
    proc.setContext(&ctx);
    proc.setRenderWindow(args.renderWindow);
    proc.process();
}

void ExpressionPlugin::render(const OFX::RenderArguments& args)
{
    ExprContext ctx;
    buildContext(args, ctx);

    OFX::BitDepthEnum       depth = _dstClip->getPixelDepth();
    OFX::PixelComponentEnum comps = _dstClip->getPixelComponents();

    if (comps == OFX::ePixelComponentRGBA) {
        switch (depth) {
            case OFX::eBitDepthUByte:  process<unsigned char,  4, 255>  (args, ctx); break;
            case OFX::eBitDepthUShort: process<unsigned short, 4, 65535>(args, ctx); break;
            case OFX::eBitDepthFloat:  process<float,          4, 1>    (args, ctx); break;
            default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }
    } else if (comps == OFX::ePixelComponentRGB) {
        switch (depth) {
            case OFX::eBitDepthUByte:  process<unsigned char,  3, 255>  (args, ctx); break;
            case OFX::eBitDepthUShort: process<unsigned short, 3, 65535>(args, ctx); break;
            case OFX::eBitDepthFloat:  process<float,          3, 1>    (args, ctx); break;
            default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }
    } else if (comps == OFX::ePixelComponentAlpha) {
        switch (depth) {
            case OFX::eBitDepthUByte:  process<unsigned char,  1, 255>  (args, ctx); break;
            case OFX::eBitDepthUShort: process<unsigned short, 1, 65535>(args, ctx); break;
            case OFX::eBitDepthFloat:  process<float,          1, 1>    (args, ctx); break;
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
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);

    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(true);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(OFX::eRenderFullySafe);
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
