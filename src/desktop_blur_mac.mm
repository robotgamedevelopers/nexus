// ScreenCaptureKit supplies reduced-resolution frames. Both Gaussian passes stay on Metal.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <cmath>
#include <algorithm>
#include "platform.h"

static const char* gaussian = R"metal(
#include <metal_stdlib>
using namespace metal;
kernel void blur(texture2d<float, access::read> source [[texture(0)]],
                 texture2d<float, access::write> dest [[texture(1)]],
                 constant int2& axis [[buffer(0)]], constant float& sigma [[buffer(1)]],
                 uint2 p [[thread_position_in_grid]]) {
    int2 size(source.get_width(), source.get_height());
    if (any(int2(p) >= size)) return;
    float4 sum = 0;
    float total = 0;
    int radius = int(ceil(3 * sigma));
    for (int i = -radius; i <= radius; ++i) {
        float w = exp(-0.5f * float(i*i) / (sigma*sigma));
        sum += w * source.read(uint2(clamp(int2(p) + axis*i, int2(0), size-1)));
        total += w;
    }
    dest.write(float4(sum.rgb/total, 1), p);
}
)metal";

@interface NexusDesktopBlur : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic,strong) SCStream* stream;
@property(nonatomic,strong) CAMetalLayer* layer;
@property(nonatomic,strong) NSView* backing;
@property(nonatomic,strong) id<MTLCommandQueue> queue;
@property(nonatomic,strong) id<MTLComputePipelineState> pipeline;
@property(nonatomic,strong) id<MTLTexture> scratch;
@property(nonatomic) CVMetalTextureCacheRef cache;
@property(nonatomic) NSUInteger generation;
@property(nonatomic) BOOL busy;
@property(nonatomic) BOOL ready;
@property(nonatomic) float sigma;
- (void)stop;
- (void)start:(NSView*)view;
@end

@implementation NexusDesktopBlur
- (void)stop {
    ++_generation;
    SCStream* old = _stream;
    _stream = nil;
    [old stopCaptureWithCompletionHandler:^(NSError*) {}];
    _backing.hidden = YES;
    _layer = nil;
    _backing.layer = nil;
    _scratch = nil;
    _busy = NO;
    _ready = YES;
}
- (void)dealloc { if (_cache) CFRelease(_cache); }

- (void)start:(NSView*)view {
    [self stop];
    if (_sigma <= 0 || !view.window.screen) return;
    if (!CGPreflightScreenCaptureAccess()) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ CGRequestScreenCaptureAccess(); });
        });
        return;
    }
    if (!_queue) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSError* error = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:@(gaussian) options:nil error:&error];
        if (lib) _pipeline = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"blur"] error:&error];
        if (!_pipeline || CVMetalTextureCacheCreate(nullptr,nullptr,device,nullptr,&_cache) != kCVReturnSuccess) {
            NSLog(@"nexus backdrop shader unavailable: %@", error);
            return;
        }
        _queue = [device newCommandQueue];
    }
    NSWindow* window = view.window;
    if (!_backing) {
        NSView* container = [[NSView alloc] initWithFrame:view.bounds];
        NSResponder* focus = window.firstResponder;
        window.contentView = container;
        _backing = [[NSView alloc] initWithFrame:container.bounds];
        _backing.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [container addSubview:_backing];
        view.frame = container.bounds;
        view.autoresizingMask = _backing.autoresizingMask;
        [container addSubview:view];
        [window makeFirstResponder:focus];
    }
    _layer = [CAMetalLayer layer];
    _layer.device = _queue.device;
    _layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    _layer.framebufferOnly = NO;
    _backing.wantsLayer = YES;
    _backing.layer = _layer;
    _backing.hidden = NO;
    _ready = NO;
    NSUInteger generation = _generation;
    NSNumber* displayID = window.screen.deviceDescription[@"NSScreenNumber"];
    CGSize points = window.screen.frame.size;
    // A capture interruption or a denied first frame must never strand the overlay at zero opacity.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2*NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (generation == self.generation && !self.ready) [self stop];
    });
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:YES
        completionHandler:^(SCShareableContent* content, NSError* error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (generation != self.generation) return;
            SCDisplay* target = nil;
            for (SCDisplay* display in content.displays)
                if (display.displayID == displayID.unsignedIntValue) target = display;
            SCRunningApplication* own = nil;
            for (SCRunningApplication* app in content.applications)
                if (app.processID == NSProcessInfo.processInfo.processIdentifier) own = app;
            if (!own || !target || error) { [self stop]; return; }
            SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:target excludingApplications:@[own] exceptingWindows:@[]];
            SCStreamConfiguration* config = [SCStreamConfiguration new];
            config.width = std::max(1L, lround(points.width/4));
            config.height = std::max(1L, lround(points.height/4));
            config.minimumFrameInterval = CMTimeMake(1, 15);
            config.pixelFormat = kCVPixelFormatType_32BGRA;
            config.showsCursor = NO;
            config.capturesAudio = NO;
            self.layer.drawableSize = CGSizeMake(config.width,config.height);
            self.stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:self];
            NSError* outputError = nil;
            if (![self.stream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:dispatch_get_main_queue() error:&outputError]) {
                [self stop]; return;
            }
            [self.stream startCaptureWithCompletionHandler:^(NSError* failure) {
                if (failure) dispatch_async(dispatch_get_main_queue(), ^{
                    if (generation == self.generation) [self stop];
                });
            }];
        });
    }];
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    dispatch_async(dispatch_get_main_queue(), ^{ if (self.stream == stream) [self stop]; });
}
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if (_stream != stream || type != SCStreamOutputTypeScreen) return;
    NSArray* attachments = (__bridge NSArray*)CMSampleBufferGetSampleAttachmentsArray(sample, false);
    NSNumber* status = attachments.firstObject[SCStreamFrameInfoStatus];
    CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
    if (status && (status.integerValue == SCFrameStatusBlank || status.integerValue == SCFrameStatusSuspended)) {
        [self stop];
        return;
    }
    if (_busy || !pixels || !status || status.integerValue != SCFrameStatusComplete) return;
    size_t w = CVPixelBufferGetWidth(pixels), h = CVPixelBufferGetHeight(pixels);
    CVMetalTextureRef wrapper = nullptr;
    if (CVMetalTextureCacheCreateTextureFromImage(nullptr,_cache,pixels,nullptr,MTLPixelFormatBGRA8Unorm,w,h,0,&wrapper) != kCVReturnSuccess) return;
    id<MTLTexture> input = CVMetalTextureGetTexture(wrapper);
    id<CAMetalDrawable> drawable = [_layer nextDrawable];
    if (!input || !drawable || drawable.texture.width != w || drawable.texture.height != h) { CFRelease(wrapper); return; }
    if (!_scratch || _scratch.width != w || _scratch.height != h) {
        auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        desc.storageMode = MTLStorageModePrivate;
        _scratch = [_queue.device newTextureWithDescriptor:desc];
    }
    id<MTLCommandBuffer> cmd = [_queue commandBuffer];
    if (!_scratch || !cmd) { CFRelease(wrapper); return; }
    const float sigma = std::max(0.1f, _sigma/4);
    for (int pass = 0; pass < 2; ++pass) {
        id<MTLComputeCommandEncoder> encoder = [cmd computeCommandEncoder];
        [encoder setComputePipelineState:_pipeline];
        [encoder setTexture:pass ? _scratch : input atIndex:0];
        [encoder setTexture:pass ? drawable.texture : _scratch atIndex:1];
        const int axis[] = {1-pass,pass};
        [encoder setBytes:axis length:sizeof(axis) atIndex:0];
        [encoder setBytes:&sigma length:sizeof(sigma) atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(w,h,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
        [encoder endEncoding];
    }
    _busy = YES;
    [cmd presentDrawable:drawable];
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        CFRelease(wrapper);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self.stream != stream) return;
            self.busy = NO;
            if (finished.status == MTLCommandBufferStatusError) { [self stop]; return; }
            self.backing.hidden = NO;
            self.ready = YES;
        });
    }];
    [cmd commit];
}
@end

namespace nexus {
static NexusDesktopBlur* blur;
void startDesktopBlur(const PrSurface& surface, float sigma) {
    if (surface.kind != PrSurfaceKind_NsView) return;
    if (!blur) blur = [NexusDesktopBlur new];
    blur.sigma = sigma;
    [blur start:(__bridge NSView*)surface.handle];
}
void stopDesktopBlur() { [blur stop]; }
bool desktopBlurReady() { return !blur || blur.ready; }
}
