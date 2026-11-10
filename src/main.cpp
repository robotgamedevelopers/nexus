// nexus tracer: the first window. Composition root: reads the friends, makes the model, and either runs the window or
// renders one frame offscreen to a PNG (--screenshot).
//
// Usage: nexus [--tab N] [--demo] [--screenshot PATH.png]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pr/app.h"
#include "pr/compress.h"
#include "pr/draw_display.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/file.h"
#include "pr/gpu.h"
#include "pr/image.h"

#include "friends.h"
#include "model.h"
#include "platform_mac.h"
#include "view.h"

namespace {

using namespace nexus;

struct App {
    Model model;
    ViewState view;
    Fonts fonts;
    PrDrawList* list = nullptr;
    bool dropText = false;
    bool fullScreen = true;

    PrApp* app = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;
    PrGpuTexture rendered{};
    PrDrawDisplay* display = nullptr;

    double frameSeconds = 0;
    uint32_t frames = 0;
};

void closeDevice(App& a) {
    if (a.device != nullptr)
        prGpuTextureDestroy(a.device, a.rendered);
    prDrawDisplayDestroy(a.display);
    prDrawFrameDestroy(a.frame);
    prDrawRendererDestroy(a.renderer);
    prGpuSwapChainDestroy(a.swapChain);
    prGpuDeviceDestroy(a.device);
    a.rendered = PrGpuTexture{};
    a.display = nullptr;
    a.frame = nullptr;
    a.renderer = nullptr;
    a.swapChain = nullptr;
    a.device = nullptr;
}

bool openDevice(App& a) {
    PrAppSurface surface{};
    PrGpuSwapChainDesc desc{};
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &a.device) != PrStatus_Ok ||
            prAppSurface(a.app, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(a.device, &surface, &desc, &a.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, a.device, nullptr, &a.renderer) != PrStatus_Ok) {
        closeDevice(a);
        return false;
    }
    a.frame = prDrawFrameCreate(a.renderer);
    return true;
}

PrGpuTexture renderedFor(App& a, PrUVec2 extent) {
    PrUVec2 held = a.rendered.id != 0 ? prGpuTextureExtent(a.device, a.rendered) : PrUVec2{};
    if (a.rendered.id == 0 || extent.x > held.x || extent.y > held.y) {
        prGpuTextureDestroy(a.device, a.rendered);
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba16F, extent,
            PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        a.rendered = PrGpuTexture{};
        prGpuTextureCreate(a.device, &desc, &a.rendered);
    }
    return a.rendered;
}

// Renders the list into the swap chain and presents it (after examples/draw_gallery in Prax).
void render(App& a, PrUVec2 extent, const PrAppDisplay& display) {
    PrGpuTexture drawable{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(a.swapChain, extent, &drawable) != PrStatus_Ok ||
            !prGpuCommandsBegin(a.device, nullptr, &commands))
        return;
    PrGpuSwapChainInfo info = prGpuSwapChainInfo(a.swapChain);
    PrPixelFormat direct{};
    bool mapped = info.space != PrColorSpace_Srgb || !prGpuFormatToPixelFormat(info.format, &direct) ||
        direct == PrPixelFormat_RgbaF16;
    if (mapped && a.display == nullptr && prDrawDisplayCreate(a.device, &a.display) != PrStatus_Ok) {
        prGpuCommandsSubmit(commands);
        return;
    }
    PrGpuTexture target = mapped ? renderedFor(a, extent) : drawable;
    PrDrawTarget into{
        .extent = extent,
        .format = mapped ? PrPixelFormat(PrPixelFormat_RgbaF16) : direct,
        .commands = commands,
    };
    prDrawFramePrepare(a.frame, a.list, &into);
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, target, all, mapped ? PrGpuAccessFlag_ShaderRead : PrGpuAccessFlag_Discard,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc pass = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{0, 0, 0, 1});
    PrGpuPass* encoder = prGpuPassBegin(commands, &pass);
    prDrawFrameRender(a.frame, encoder, PrIVec2{0, 0});
    prGpuPassEnd(encoder);
    if (mapped) {
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
        prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc shown = prGpuPassDescColor(drawable, PrGpuLoad_Discard, PrColor{});
        encoder = prGpuPassBegin(commands, &shown);
        PrDrawDisplayDesc mapping{.target = info.space, .headroom = display.headroom, .gamut = display.gamut};
        prDrawDisplayDraw(a.display, encoder, target, &mapping);
        prGpuPassEnd(encoder);
    }
    prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(a.swapChain, commands);
    prGpuCommandsSubmit(commands);
}

void record(App& a, PrVec2 extent, float ratio) {
    prDrawListClear(a.list);
    prDrawListPushTransformAffine2(a.list, prAffine2Scale(PrVec2{ratio, ratio}));
    drawWindow(a.list, a.model, a.view, a.fonts, extent);
    prDrawListPopTransform(a.list);
}

void start(void* context, PrApp* app, const PrAppFrame*) {
    App& a = *static_cast<App*>(context);
    a.app = app;
    if (!openDevice(a)) {
        std::fprintf(stderr, "nexus: no GPU device to render with\n");
        prAppStop(app);
        return;
    }
    // Prax's lib/app has no fullscreen call; go through the window behind its NSView.
    PrAppSurface surface{};
    if (a.fullScreen && prAppSurface(app, &surface) == PrStatus_Ok && surface.kind == PrAppSurfaceKind_NsView)
        enterNativeFullScreen(surface.handle);
    prAppRequestFrame(app);
}

PrAppOutput frame(void* context, PrApp*, const PrAppFrame* f) {
    App& a = *static_cast<App*>(context);
    if (a.device != nullptr && prGpuDeviceIsLost(a.device))
        closeDevice(a);
    if (a.device == nullptr && !openDevice(a))
        return {};
    prGpuSwapChainWait(a.swapChain);
    auto begin = std::chrono::steady_clock::now();
    float ratio = f->display.pixelRatio;
    record(a, f->extent, ratio);
    PrUVec2 extent{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)};
    if (extent.x != 0 && extent.y != 0)
        render(a, extent, f->display);
    a.frameSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    ++a.frames;
    PrAppOutput out{};
    out.hasTextInput = true;
    out.textInput = PrRect{f->extent.x / 2, f->extent.y - 80, f->extent.x / 2 + 1, f->extent.y - 60};
    return out;
}

void event(void* context, PrApp* app, const PrAppEvent* e) {
    App& a = *static_cast<App*>(context);
    bool changed = true;
    switch (e->kind) {
        case PrAppEventKind_KeyDown: {
            bool alt = (e->modifiers & PrAppModifierFlag_Alt) != 0;
            bool cmd = (e->modifiers & PrAppModifierFlag_Super) != 0;
            bool ctrl = (e->modifiers & PrAppModifierFlag_Control) != 0;
            a.dropText = cmd || ctrl;
            if (alt && e->code >= PrAppKeyCode_Digit1 && e->code <= PrAppKeyCode_Digit9) {
                selectTab(a.model, size_t(e->code - PrAppKeyCode_Digit1));
                a.dropText = true;  // Option+digit also types a character; it selects a tab instead
            } else if (e->key == PrAppKey_Enter) {
                submit(a.model, cmd);
                a.view.scroll.assign(a.model.tabs.size(), 0);
                a.dropText = true;
            } else if (e->key == PrAppKey_Backspace) {
                backspace(a.model);
                a.dropText = true;
            } else if (e->key == PrAppKey_Left) {
                caretLeft(a.model);
            } else if (e->key == PrAppKey_Right) {
                caretRight(a.model);
            } else if (e->key == PrAppKey_Escape) {
                a.dropText = true;
            }
            break;
        }
        case PrAppEventKind_Text: {
            if (a.dropText) {
                a.dropText = false;
                break;
            }
            std::string text(e->text.data, e->text.size);
            bool control = false;
            for (char ch : text)
                control = control || static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F;
            if (!control && !text.empty())
                insertText(a.model, text);
            break;
        }
        case PrAppEventKind_PointerDown: {
            int tab = e->button == 0 ? tabAt(a.view, e->position) : -1;
            if (tab >= 0)
                selectTab(a.model, size_t(tab));
            break;
        }
        case PrAppEventKind_Scroll: {
            PrVec2 d = prAppEventScrollPixels(e, 23);
            scrollBy(a.model, a.view, -d.y);
            break;
        }
        default:
            changed = false;
            break;
    }
    if (changed)
        prAppRequestFrame(app);
}

void shutdown(void* context, PrApp*) {
    closeDevice(*static_cast<App*>(context));
}

// Renders one frame on the GPU into memory and writes it as a PNG. Returns a process exit code.
int screenshot(App& a, const char* path, PrVec2 extent, float ratio) {
    PrGpuDevice* device = nullptr;
    PrDrawRenderer* renderer = nullptr;
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, device, nullptr, &renderer) != PrStatus_Ok) {
        std::fprintf(stderr, "nexus: no GPU device or renderer for the screenshot\n");
        prDrawRendererDestroy(renderer);
        prGpuDeviceDestroy(device);
        return 1;
    }
    PrUVec2 size{uint32_t(extent.x * ratio), uint32_t(extent.y * ratio)};
    std::vector<uint8_t> bytes(size_t(size.x) * size.y * 4, 0);
    PrPixels pixels{bytes.data(), size_t(size.x) * 4, size};
    auto begin = std::chrono::steady_clock::now();
    record(a, extent, ratio);
    auto recorded = std::chrono::steady_clock::now();
    PrStatus status = prDrawRendererRenderPixels(renderer, a.list, pixels, PrPixelFormat_Rgba8);
    auto done = std::chrono::steady_clock::now();
    prDrawRendererDestroy(renderer);
    prGpuDeviceDestroy(device);
    if (status != PrStatus_Ok) {
        std::fprintf(stderr, "nexus: rendering the screenshot failed (status %d)\n", int(status));
        return 1;
    }
    std::printf("nexus: recorded in %.2f ms, rendered and read back in %.2f ms (%ux%u px)\n",
        std::chrono::duration<double, std::milli>(recorded - begin).count(),
        std::chrono::duration<double, std::milli>(done - recorded).count(), size.x, size.y);

    // The render is premultiplied; the encoder takes straight alpha. Everything is opaque, so only alpha 0 needs care.
    for (size_t i = 0; i < bytes.size(); i += 4) {
        uint8_t alpha = bytes[i + 3];
        if (alpha != 0 && alpha != 255) {
            for (int c = 0; c < 3; ++c)
                bytes[i + c] = uint8_t(std::min(255, bytes[i + c] * 255 / alpha));
        }
    }
    PrCompressCodec* compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
    PrImageCodec* codec = compress != nullptr ? prImageCodecCreate(PrImageCodecBackend_Default, compress) : nullptr;
    if (codec == nullptr) {
        std::fprintf(stderr, "nexus: no image codec in this build\n");
        prCompressCodecDestroy(compress);
        return 1;
    }
    PrPixelView view{bytes.data(), size_t(size.x) * 4, size};
    size_t encoded = 0;
    prImageCodecEncode(codec, "image/png", view, nullptr, 0, &encoded);
    std::vector<uint8_t> png(encoded);
    status = prImageCodecEncode(codec, "image/png", view, png.data(), png.size(), &encoded);
    prImageCodecDestroy(codec);
    prCompressCodecDestroy(compress);
    if (status != PrStatus_Ok) {
        std::fprintf(stderr, "nexus: encoding the PNG failed (status %d)\n", int(status));
        return 1;
    }
    if (!prFileWrite(PrStringView{path, std::strlen(path)}, PrByteView{png.data(), png.size()})) {
        std::fprintf(stderr, "nexus: could not write %s\n", path);
        return 1;
    }
    std::printf("nexus: wrote %s (%zu bytes)\n", path, png.size());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* shot = nullptr;
    size_t tab = 1;
    bool demo = false;
    bool windowed = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--windowed") == 0) {
            windowed = true;
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            shot = argv[++i];
        } else if (std::strcmp(argv[i], "--tab") == 0 && i + 1 < argc) {
            tab = size_t(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--demo") == 0) {
            demo = true;
        } else {
            std::fprintf(stderr, "usage: nexus [--windowed] [--tab N] [--demo] [--screenshot PATH.png]\n");
            return 2;
        }
    }

    static App a;
    std::string note;
    a.model = makeModel(loadFriends(note));
    std::printf("nexus: %s\n", note.c_str());
    seedSample(a.model.tabs[0]);
    if (tab >= 1 && !selectTab(a.model, tab - 1) && tab - 1 != a.model.active) {
        std::fprintf(stderr, "nexus: no tab %zu (there are %zu)\n", tab, a.model.tabs.size());
        return 2;
    }
    if (demo) {
        insertText(a.model, "Can you look at why the nightly build went red?");
        submit(a.model, false);
        insertText(a.model, "After that, review the open PR");
        submit(a.model, true);
    }
    std::string error;
    if (!openFonts(a.fonts, error)) {
        std::fprintf(stderr, "nexus: %s\n", error.c_str());
        return 1;
    }
    a.list = prDrawListCreate();

    int code = 0;
    if (shot != nullptr) {
        // A fullscreen-sized frame of the main display (a 1000x780 window with --windowed).
        float w = 1000, h = 780, scale = 2;
        if (!windowed && !mainDisplay(&w, &h, &scale)) {
            std::fprintf(stderr, "nexus: no main display to size the screenshot by\n");
            return 1;
        }
        code = screenshot(a, shot, PrVec2{w, h}, scale);
    } else {
        a.fullScreen = !windowed;
        PrAppDesc desc{.title = "nexus", .extent = {1000, 780}};
        PrApp* app = prAppCreate(PrAppBackend_Default, &desc);
        if (app == nullptr) {
            std::fprintf(stderr, "nexus: no application backend in this build\n");
            code = 1;
        } else {
            PrAppHandler handler{&a, start, frame, event, shutdown};
            prAppRun(app, &handler);
            prAppDestroy(app);
            if (a.frames != 0)
                std::printf("nexus: %u frames, %.2f ms average to record, render and present\n", a.frames,
                    a.frameSeconds * 1000 / a.frames);
        }
    }
    prDrawListRelease(a.list);
    closeFonts(a.fonts);
    return code;
}
