// nexus: one person and several AIs ("friends") in one window. Layer 1 is the window only: tabs, transcripts and the
// prompt, with nothing connected to any AI yet.
//
// main() is the composition root: it reads the friends, makes the model, and either runs the window in Prax's Studio
// window (pr/studio_window.h) or lays the same tree out offscreen and writes one frame to a PNG (--screenshot).
//
// The window is the overlay (platform_mac.h): see-through, over the whole screen, shown and hidden by a global hotkey
// (style.h). --windowed gives a normal opaque window. --print-window-config makes the overlay window hidden, prints its
// configuration and exits.
//
// Usage: nexus [--windowed] [--tab N] [--demo] [--screenshot PATH.png] [--bench FRAMES] [--print-window-config]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pr/compress.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/file.h"
#include "pr/font_set.h"
#include "pr/gpu.h"
#include "pr/image.h"
#include "pr/kit.h"
#include "pr/kit_theme.h"
#include "pr/layout_scene.h"
#include "pr/studio_window.h"
#include "pr/text.h"

#include "friends.h"
#include "model.h"
#include "platform_mac.h"
#include "style.h"
#include "view.h"

namespace {

using namespace nexus;

// Frame times from the Studio window's trace, in milliseconds.
struct FrameStats {
    unsigned frames = 0;
    double total = 0;       // the whole frame, swap chain wait included
    double work = 0;        // the frame less the swap chain wait: rounds, scene, play and render
    double worstTotal = 0;
    double worstWork = 0;
};

struct App {
    Model* model = nullptr;
    View view;
    Look look;
    PrFontSet* fonts = nullptr;
    bool overlay = true;            // the overlay window (platform_mac.h); --windowed gives Prax's own window
    bool overlayOpened = false;
    unsigned bench = 0;     // with --bench, frames to draw back to back before stopping
    FrameStats stats;
};

App& appOf(void* context) {
    return *static_cast<App*>(context);
}

OverlayConfig overlayConfig() {
    return OverlayConfig{
        .hotKeyCode = style::kOverlayHotKeyCode,
        .hotKeyModifiers = style::kOverlayHotKeyModifiers,
        .blur = style::kOverlayBlur,
        .accessory = style::kOverlayAccessory,
    };
}

// The window's kit theme (view.h). Its bg0 is the canvas, which the window clears to with its alpha, so the overlay's
// background is see-through and what is drawn on it is not.
bool open(void* context, PrStudioWindow* window) {
    App& a = appOf(context);
    PrKitTheme theme = windowTheme(a.fonts, a.overlay);
    prKitSetTheme(prStudioWindowKit(window), &theme);
    return true;
}

// Always the dark variant, so the window never puts Studio's own theme back over the one set in open().
PrKitThemeVariant theme(void*, PrStudioWindow*, const PrAppUserPreferences*) {
    return PrKitThemeVariant_Dark;
}

void beginFrame(void* context, PrStudioWindow* window, const PrAppFrame*) {
    App& a = appOf(context);
    beginWindowFrame(a.view, prStudioWindowKit(window), a.fonts);
    if (a.overlay && !a.overlayOpened) {
        // Prax's lib/app has no window styles; go through the window behind its NSView. Shown once at launch.
        a.overlayOpened = true;
        PrAppSurface surface{};
        if (prAppSurface(prStudioWindowApp(window), &surface) == PrStatus_Ok &&
                surface.kind == PrAppSurfaceKind_NsView && openOverlay(surface.handle, overlayConfig(), true))
            std::printf("nexus: overlay window\n%s", describeOverlay().c_str());
        else
            std::fprintf(stderr, "nexus: no NSView behind the window; the overlay is not set up\n");
    }
}

bool event(void*, PrStudioWindow* window, const PrAppEvent* e) {
    return takeEvent(prStudioWindowUi(window), *e);
}

void build(void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
    App& a = appOf(context);
    buildWindow(a.view, prStudioWindowKit(window), root);
}

bool laidOut(void* context, PrStudioWindow* window) {
    App& a = appOf(context);
    if (a.bench == 0)
        return windowWantsFrame(a.view);
    if (a.stats.frames >= a.bench) {
        prAppStop(prStudioWindowApp(window));
        return false;
    }
    return true;
}

void trace(void* context, const PrStudioWindowFrameTimes* t) {
    FrameStats& s = appOf(context).stats;
    double work = t->total - t->wait;
    ++s.frames;
    s.total += t->total;
    s.work += work;
    s.worstTotal = std::max(s.worstTotal, t->total);
    s.worstWork = std::max(s.worstWork, work);
}

void printStats(const FrameStats& s) {
    if (s.frames == 0)
        return;
    std::printf("nexus: %u frames; work (rounds, layout, scene, render) %.2f ms average, %.2f ms worst; "
        "with the swap chain wait %.2f ms average, %.2f ms worst\n", s.frames, s.work / s.frames, s.worstWork,
        s.total / s.frames, s.worstTotal);
}

// The offscreen frame: the same tree, laid out and painted without a window. ---------------------------------------

struct Offscreen {
    App* app;
    PrKit* kit;
};

void buildOffscreen(void* context, PrUi*, const PrUiRound*) {
    Offscreen& off = *static_cast<Offscreen*>(context);
    buildWindow(off.app->view, off.kit, kPrLayoutNodeNone);
}

bool writePng(std::vector<uint8_t>& bytes, PrUVec2 size, const char* path) {
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
        return false;
    }
    PrPixelView view{bytes.data(), size_t(size.x) * 4, size};
    size_t encoded = 0;
    prImageCodecEncode(codec, "image/png", view, nullptr, 0, &encoded);
    std::vector<uint8_t> png(encoded);
    PrStatus status = prImageCodecEncode(codec, "image/png", view, png.data(), png.size(), &encoded);
    prImageCodecDestroy(codec);
    prCompressCodecDestroy(compress);
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

// Lays the window out at `extent` px for a display of `ratio`, renders it on the GPU into memory, and writes a PNG.
// A workaround: Prax has no public offscreen render of a UI tree (only its test fixture, tools/harness/kit_fixture.h).
int screenshot(App& a, const char* path, PrVec2 extent, float ratio) {
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTreeDesc treeDesc{.shaper = shaper, .targetPixelRatio = ratio};
    PrLayoutTree* tree = prLayoutTreeCreate(&treeDesc);
    prLayoutTreeSetCanvasColor(tree, prColor16FromColor(style::kBackground));
    PrUi* ui = prUiCreate(tree);
    PrKitTheme theme = windowTheme(a.fonts, false);
    PrKit* kit = prKitCreate(ui, &theme);
    Offscreen off{&a, kit};
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = ratio;
    // Enough frames for every query to read a layout: the first round routes nothing, and scrolling reads the last one.
    for (int i = 0; i < 4; ++i) {
        PrUiFrameDesc frame{
            .time = 1000.0 + i / 60.0,
            .delta = 1 / 60.0,
            .extent = extent,
            .maxRounds = 4,
            .display = display,
            .preferences = {},
        };
        prUiFrame(ui, &frame, buildOffscreen, &off);
    }
    const PrLayoutScene* scene = prLayoutTreeScene(tree);
    std::vector<PrLayoutSceneScroll> scrolls(prLayoutSceneScrolls(tree, scene, nullptr, 0));
    prLayoutSceneScrolls(tree, scene, scrolls.data(), uint32_t(scrolls.size()));
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = scrolls.data();
    options.scrollCount = uint32_t(scrolls.size());
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{ratio, ratio}));
    prLayoutScenePlay(scene, list, &options);
    prDrawListPopTransform(list);
    prLayoutSceneRelease(scene);

    int code = 1;
    PrGpuDevice* device = nullptr;
    PrDrawRenderer* renderer = nullptr;
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, device, nullptr, &renderer) != PrStatus_Ok) {
        std::fprintf(stderr, "nexus: no GPU device or renderer for the screenshot\n");
    } else {
        PrUVec2 size{uint32_t(extent.x * ratio), uint32_t(extent.y * ratio)};
        std::vector<uint8_t> bytes(size_t(size.x) * size.y * 4, 0);
        PrPixels pixels{bytes.data(), size_t(size.x) * 4, size};
        PrStatus status = prDrawRendererRenderPixels(renderer, list, pixels, PrPixelFormat_Rgba8);
        if (status != PrStatus_Ok)
            std::fprintf(stderr, "nexus: rendering the screenshot failed (status %d)\n", int(status));
        else if (writePng(bytes, size, path))
            code = 0;
    }
    prDrawRendererDestroy(renderer);
    prGpuDeviceDestroy(device);
    prDrawListRelease(list);
    prKitDestroy(kit);
    prUiDestroy(ui);
    prLayoutTreeDestroy(tree);
    prTextShaperDestroy(shaper);
    return code;
}

// Sample entries for --demo: every kind, and a person's message long enough to wrap.
void seedDemo(Model& model, std::size_t tab) {
    model.append(0, Entry{EntryKind::Person,
        "I'd like the transcript to keep up with a long conversation, so here is a message long enough to wrap onto a "
        "second line and show how a bubble grows when it runs past most of the column."});
    model.append(0, Entry{EntryKind::Friend,
        "Sure. The bubble is laid out by Prax, so it wraps at the column's width and keeps its corners however many "
        "lines it takes."});
    model.append(0, Entry{EntryKind::Notice, "read 3 files"});
    model.append(0, Entry{EntryKind::Friend, "Anything else before I start?"});
    model.set_draft(tab, "Can you look at why the nightly build went red?");
    model.send(tab);
    model.set_draft(tab, "After that, review the open PR");
    model.queue(tab);
}

} // namespace

int main(int argc, char** argv) {
    const char* shot = nullptr;
    std::size_t tab = 1;
    bool demo = false;
    bool windowed = false;
    bool printConfig = false;
    unsigned bench = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--windowed") == 0) {
            windowed = true;
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            shot = argv[++i];
        } else if (std::strcmp(argv[i], "--tab") == 0 && i + 1 < argc) {
            tab = std::size_t(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
            bench = unsigned(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--demo") == 0) {
            demo = true;
        } else if (std::strcmp(argv[i], "--print-window-config") == 0) {
            printConfig = true;
        } else {
            std::fprintf(stderr, "usage: nexus [--windowed] [--tab N] [--demo] [--screenshot PATH.png] [--bench FRAMES] "
                "[--print-window-config]\n");
            return 2;
        }
    }

    if (printConfig) {
        std::string text = describeHiddenOverlay(overlayConfig());
        if (text.empty()) {
            std::fprintf(stderr, "nexus: no screen to make the overlay window on\n");
            return 1;
        }
        std::printf("nexus: overlay window (made hidden, as Prax's backend makes its window)\n%s"
            "background alpha: %.2f; tab strip fill alpha: %.2f; blur: %s\n", text.c_str(),
            double(style::kOverlayOpacity), double(style::kOverlayStripFill), style::kOverlayBlur ? "on" : "off");
        return 0;
    }

    FriendList friends = loadFriends();
    if (friends.error.empty())
        std::printf("nexus: %zu friends from nova-config\n", friends.friends.size());
    else
        std::fprintf(stderr, "nexus: %s\n", friends.error.c_str());
    static Model model(friends.friends);
    if (model.tab_count() != 0 && (tab < 1 || !model.select_tab(tab - 1))) {
        std::fprintf(stderr, "nexus: no tab %zu (there are %zu)\n", tab, model.tab_count());
        return 2;
    }
    if (demo && model.tab_count() != 0)
        seedDemo(model, model.active());

    static App a;
    a.model = &model;
    a.view.model = &model;
    a.view.look = &a.look;
    a.view.notice = "No friends to show.\n" + friends.error;
    // The screenshot stays opaque: an offscreen frame has nothing behind it to show through.
    a.overlay = !windowed && shot == nullptr;
    a.look.overlay = a.overlay;
    a.bench = bench;
    a.fonts = prFontSetCreate(nullptr);
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    prFontSetSetSystem(a.fonts, system);
    prFontSetSetDefaultFamily(a.fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));

    int code = 0;
    if (shot != nullptr) {
        // A fullscreen-sized frame of the main display (a 1000x780 window with --windowed).
        float w = 1000, h = 780, scale = 2;
        if (!windowed && !mainDisplay(&w, &h, &scale)) {
            std::fprintf(stderr, "nexus: no main display to size the screenshot by\n");
            code = 1;
        } else {
            code = screenshot(a, shot, PrVec2{w, h}, scale);
        }
    } else {
        PrStudioWindowDesc desc{
            .title = "nexus",
            .extent = {1000, 780},
            .fonts = a.fonts,
            .open = open,
            .theme = theme,
            .beginFrame = beginFrame,
            .event = event,
            .build = build,
            .laidOut = laidOut,
            .trace = trace,
            .context = &a,
        };
        PrStatus status = prStudioWindowRun(&desc);
        if (status == PrStatus_NotSupported) {
            std::fprintf(stderr, "nexus: no GPU device to render with\n");
            code = 1;
        }
        closeOverlay();
        printStats(a.stats);
    }
    prFontSetDestroy(a.fonts);
    prFontSystemDestroy(system);
    return code;
}
