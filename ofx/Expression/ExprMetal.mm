// ExprMetal.mm
// -----------------------------------------------------------------------------
// Objective-C++ implementation of the Metal compute dispatch declared in
// ExprMetal.h. It compiles the generated MSL (cached by source string), encodes
// the exprKernel over the source/destination MTLBuffers the OFX host handed us,
// and runs it on the host's command queue. macOS only.
//
// Not built with ARC — object lifetimes are managed explicitly: cached pipelines
// are retained for the process lifetime; everything transient lives in an
// @autoreleasepool.
// -----------------------------------------------------------------------------
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "ExprMetal.h"

#include <map>
#include <mutex>
#include <string>

namespace {

// Must be byte-identical to `struct ExprMetalUniforms` emitted by
// buildMetalPixelKernel() (same field order and 4-byte int/float types).
struct Uniforms {
    int   rwx1, rwy1, rwx2, rwy2;
    int   srcX1, srcY1, srcRowFloats;
    int   dstX1, dstY1, dstRowFloats;
    int   nComps, hasSrc;
    float fwidth, fheight, frame;
    float k1, k2, k3, k4, refr, refg, refb, mix;
    int   clampOut;
};

// MSL-source -> compiled pipeline cache. Pipelines are retained (non-ARC) and
// kept for the life of the process; the map is tiny (one entry per distinct
// expression set) so this never needs eviction in practice.
std::mutex                                        gCacheMtx;
std::map<std::string, id<MTLComputePipelineState>> gCache;

// Get (compiling + caching on first use) the pipeline for this MSL source.
id<MTLComputePipelineState> pipelineFor(id<MTLDevice> dev,
                                        const std::string& msl,
                                        std::string& err) {
    {
        std::lock_guard<std::mutex> lk(gCacheMtx);
        auto it = gCache.find(msl);
        if (it != gCache.end()) return it->second;
    }
    NSError* nserr = nil;
    NSString* src = [NSString stringWithUTF8String:msl.c_str()];
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&nserr];
    if (!lib) {
        err = "MSL compile failed: ";
        err += nserr ? [[nserr localizedDescription] UTF8String] : "unknown";
        return nil;
    }
    id<MTLFunction> fn = [lib newFunctionWithName:@"exprKernel"];
    id<MTLComputePipelineState> pso =
        fn ? [dev newComputePipelineStateWithFunction:fn error:&nserr] : nil;
    [fn release];
    [lib release];
    if (!pso) {
        err = "pipeline build failed: ";
        err += nserr ? [[nserr localizedDescription] UTF8String] : "no exprKernel";
        return nil;
    }
    {
        std::lock_guard<std::mutex> lk(gCacheMtx);
        auto it = gCache.find(msl);
        if (it != gCache.end()) { [pso release]; return it->second; }  // lost the race
        gCache[msl] = pso;   // keep the +1 reference
    }
    return pso;
}

} // namespace

namespace expreval {

bool metalRender(void* commandQueue,
                 const MetalImageDesc& src, int hasSrc,
                 const MetalImageDesc& dst,
                 int rwX1, int rwY1, int rwX2, int rwY2,
                 int nComps, float width, float height, float frame,
                 const ExprKnobs& knobs,
                 const std::string& mslSource,
                 std::string& err) {
    if (!commandQueue || !dst.buffer) { err = "no command queue / dst buffer"; return false; }
    const int W = rwX2 - rwX1, H = rwY2 - rwY1;
    if (W <= 0 || H <= 0) return true;   // nothing to render

    @autoreleasepool {
        id<MTLCommandQueue> q   = (id<MTLCommandQueue>)commandQueue;
        id<MTLDevice>       dev = [q device];
        id<MTLBuffer>       dbuf = (id<MTLBuffer>)dst.buffer;
        id<MTLBuffer>       sbuf = (hasSrc && src.buffer) ? (id<MTLBuffer>)src.buffer : dbuf;

        id<MTLComputePipelineState> pso = pipelineFor(dev, mslSource, err);
        if (!pso) return false;

        Uniforms u;
        u.rwx1 = rwX1; u.rwy1 = rwY1; u.rwx2 = rwX2; u.rwy2 = rwY2;
        u.srcX1 = src.x1; u.srcY1 = src.y1; u.srcRowFloats = src.rowFloats;
        u.dstX1 = dst.x1; u.dstY1 = dst.y1; u.dstRowFloats = dst.rowFloats;
        u.nComps = nComps; u.hasSrc = (hasSrc && src.buffer) ? 1 : 0;
        u.fwidth = width; u.fheight = height; u.frame = frame;
        u.k1 = knobs.k[0]; u.k2 = knobs.k[1]; u.k3 = knobs.k[2]; u.k4 = knobs.k[3];
        u.refr = knobs.ref[0]; u.refg = knobs.ref[1]; u.refb = knobs.ref[2];
        u.mix = knobs.mix; u.clampOut = knobs.clampOut;

        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setBuffer:sbuf offset:0 atIndex:0];
        [enc setBuffer:dbuf offset:0 atIndex:1];
        [enc setBytes:&u length:sizeof(u) atIndex:2];

        NSUInteger tew = pso.threadExecutionWidth;
        NSUInteger maxT = pso.maxTotalThreadsPerThreadgroup;
        NSUInteger tgw = (tew < (NSUInteger)W) ? tew : (NSUInteger)W;
        if (tgw == 0) tgw = 1;
        NSUInteger tgh = maxT / tgw;
        if (tgh == 0) tgh = 1;
        if (tgh > (NSUInteger)H) tgh = (NSUInteger)H;
        [enc dispatchThreads:MTLSizeMake(W, H, 1)
          threadsPerThreadgroup:MTLSizeMake(tgw, tgh, 1)];
        [enc endEncoding];

        // First version waits for completion so the dst buffer is guaranteed
        // filled before the OFX image is released. The spec permits returning
        // without waiting (the host sequences on its queue); revisit for async
        // once the pixel path is host-verified.
        [cb commit];
        [cb waitUntilCompleted];

        if (cb.status == MTLCommandBufferStatusError) {
            err = "command buffer error: ";
            err += cb.error ? [[cb.error localizedDescription] UTF8String] : "unknown";
            return false;
        }
    }
    return true;
}

} // namespace expreval
