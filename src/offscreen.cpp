#include "offscreen.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pr/compress_select.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/draw_select.h"
#include "pr/file.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/image.h"
#include "pr/image_select.h"
#include "pr/layout_scene.h"
#include "pr/text.h"

namespace nexus {

namespace {

struct Build {
    View* view;
    PrKit* kit;
};

void buildOffscreen(void* context, PrUi*, const PrUiRound*) {
    Build& b = *static_cast<Build*>(context);
    buildWindow(*b.view, b.kit, kPrLayoutNodeNone);
}

bool writePng(std::vector<uint8_t>& bytes, PrUVec2 size, const char* path) {
    // The render is premultiplied; the encoder takes straight alpha.
    for (size_t i = 0; i < bytes.size(); i += 4) {
        uint8_t alpha = bytes[i + 3];
        if (alpha != 0 && alpha != 255) {
            for (int c = 0; c < 3; ++c)
                bytes[i + c] = uint8_t(std::min(255, bytes[i + c] * 255 / alpha));
        }
    }
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* codec = compress != nullptr ? prImageCodecSetCreateDefault(compress) : nullptr;
    if (codec == nullptr) {
        std::fprintf(stderr, "nexus: no image codec in this build\n");
        prCompressCodecSetDestroy(compress);
        return false;
    }
    PrPixelView view{bytes.data(), size_t(size.x) * 4, size};
    size_t encoded = 0;
    prImageCodecSetEncode(codec, {"image/png", 9}, view, nullptr, 0, &encoded);
    std::vector<uint8_t> png(encoded);
    PrStatus status = prImageCodecSetEncode(codec, {"image/png", 9}, view, png.data(), png.size(), &encoded);
    prImageCodecSetDestroy(codec);
    prCompressCodecSetDestroy(compress);
    if (status != PrStatus_Ok) {
        std::fprintf(stderr, "nexus: encoding the PNG failed (status %d)\n", int(status));
        return false;
    }
    if (!prFileWrite(PrStringView{path, std::strlen(path)}, PrByteView{png.data(), png.size()})) {
        std::fprintf(stderr, "nexus: could not write %s\n", path);
        return false;
    }
    std::printf("nexus: wrote %s (%zu bytes)\n", path, png.size());
    return true;
}

} // namespace

OffscreenFrame::OffscreenFrame(View& view, PrFontSet* fonts, PrVec2 extent, float ratio, bool seeThrough)
    : view_(view), extent_(extent), ratio_(ratio) {
    shaper_ = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTreeDesc treeDesc{.shaper = shaper_, .targetPixelRatio = ratio};
    tree_ = prLayoutTreeCreate(&treeDesc);
    // The window paints its fill once over the transparent canvas, as in the live overlay.
    PrKitTheme theme = windowTheme(fonts, seeThrough);
    ui_ = prUiCreate(tree_);
    kit_ = prKitCreate(ui_, &theme);
    display_ = prAppDisplayDefault();
    display_.pixelRatio = ratio;
    view_.extent = extent;
}

OffscreenFrame::~OffscreenFrame() {
    prKitDestroy(kit_);
    prUiDestroy(ui_);
    prLayoutTreeDestroy(tree_);
    prTextShaperDestroy(shaper_);
}

void OffscreenFrame::frame(const std::function<void(double, const PrAppDisplay&)>& before,
        const std::function<void()>& after) {
    const double time = 1000.0 + frames_++ / 60.0;
    view_.time = time;
    if (before) before(time, display_);
    PrUiFrameDesc desc{.time = time, .delta = 1 / 60.0, .extent = extent_, .maxRounds = 4,
        .display = display_, .preferences = {}};
    Build b{&view_, kit_};
    prUiFrame(ui_, &desc, buildOffscreen, &b);
    if (after) after();
}

bool OffscreenFrame::write(const char* path, bool stripes) {
    const PrLayoutScene* scene = prLayoutTreeScene(tree_);
    std::vector<PrLayoutSceneScroll> scrolls(prLayoutSceneScrolls(tree_, scene, nullptr, 0));
    prLayoutSceneScrolls(tree_, scene, scrolls.data(), uint32_t(scrolls.size()));
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = scrolls.data();
    options.scrollCount = uint32_t(scrolls.size());
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{ratio_, ratio_}));
    if (stripes) {
        // Stripes of four colours, 160 px wide, in place of the screen behind the overlay.
        const PrColor colours[] = {{0.93f, 0.45f, 0.18f, 1}, {0.16f, 0.62f, 0.56f, 1}, {0.86f, 0.86f, 0.82f, 1},
            {0.55f, 0.35f, 0.85f, 1}};
        for (int i = 0; float(i) * 160 < extent_.x; ++i) {
            PrDrawPaint paint{};
            paint.color = prColor16FromColor(colours[i % 4]);
            prDrawListRect(list, PrRect{float(i) * 160, 0, float(i + 1) * 160, extent_.y}, paint);
        }
    }
    prLayoutScenePlay(scene, list, &options);
    prDrawListPopTransform(list);
    prLayoutSceneRelease(scene);

    bool written = false;
    PrGpuDevice* device = prGpuDeviceCreate(PrGpuBackend_Memory, nullptr);
    PrDrawRenderer* renderer = nullptr;
    if (!device || prDrawRendererCreate(PrDrawRendererBackend_Cpu, device, nullptr, &renderer) != PrStatus_Ok) {
        std::fprintf(stderr, "nexus: no GPU device or renderer for the screenshot\n");
    } else {
        PrUVec2 size{uint32_t(extent_.x * ratio_), uint32_t(extent_.y * ratio_)};
        std::vector<uint8_t> bytes(size_t(size.x) * size.y * 4, 0);
        PrPixels pixels{bytes.data(), size_t(size.x) * 4, size};
        PrStatus status = prDrawRendererRenderPixels(renderer, list, pixels, PrPixelFormat_Rgba8);
        if (status != PrStatus_Ok)
            std::fprintf(stderr, "nexus: rendering the screenshot failed (status %d)\n", int(status));
        else
            written = writePng(bytes, size, path);
    }
    prDrawRendererDestroy(renderer);
    prGpuDeviceDestroy(device);
    prDrawListRelease(list);
    return written;
}

} // namespace nexus
