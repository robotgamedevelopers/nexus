// `nexus --screenshot --state` on iOS: draws a server's state file through the window's offscreen frame (offscreen.h),
// the same render the desktop's screenshot uses, and writes a PNG. Run on a simulator device by tests/ios (its ctest
// emulator), with nothing from the app's home: the state file and the frame's size are its only inputs.
//
// With --compare, the shot is then compared with a golden image (matches below), and the exit says whether it matched.
//
//   nexus_ios_shot STATE OUT.png WIDTH HEIGHT RATIO [--lost] [--tab N] [--status TEXT] [--text-steps N]
//                  [--ask-server TEXT] [--compare GOLDEN.png]
//
// STATE `-` is none: the window before the server has sent anything, with no tab. --status sets the link's line (what
// FrameHost::pollLink writes, sync::linkStatus). The text scale is the one an iOS window starts at
// (style.h kDefaultTextSteps), or --text-steps. --ask-server shows the server dialog (server_choice.h) as at first
// launch, with TEXT typed in its field and, when TEXT is not an address, the reason under it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pr/compress_select.h"
#include "pr/font_set.h"
#include "pr/png.h"
#include "pr/text_font.h"

#include "frame_host.h"
#include "model.h"
#include "offscreen.h"
#include "server_choice.h"
#include "state_window.h"
#include "style.h"
#include "view.h"

namespace {

// The RGBA8 pixels of the PNG at `path`, as the shot writes them (4 channels of 8 bits). False, with why, if not.
bool readPng(const char* path, std::vector<uint8_t>* pixels, PrUVec2* extent, std::string* why) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *why = std::string("could not read ") + path;
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrPngDecoder* decoder = nullptr;
    PrStatus status = prPngDecoderCreate(PrByteView{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()},
        compress, nullptr, 0, &decoder);
    bool ok = false;
    if (status != PrStatus_Ok) {
        *why = std::string(path) + " is not a PNG (status " + std::to_string(int(status)) + ")";
    } else if (const PrPngInfo* info = prPngDecoderInfo(decoder); info->channels != 4 || info->sampleBytes != 1) {
        *why = std::string(path) + " is not 8-bit RGBA";
    } else {
        *extent = info->extent;
        pixels->assign(size_t(extent->x) * extent->y * 4, 0);
        status = prPngDecoderRead(decoder, 0, PrPixels{pixels->data(), size_t(extent->x) * 4, *extent});
        ok = status == PrStatus_Ok;
        if (!ok) *why = std::string(path) + " did not decode (status " + std::to_string(int(status)) + ")";
    }
    prPngDecoderDestroy(decoder);
    prCompressCodecSetDestroy(compress);
    return ok;
}

// The shot against the golden image: the same size, and at most 1 pixel in 2000 differing by more than 24 in a channel
// (antialiasing and text rasterization may move a little between simulator runtimes; a changed layout, a missing tab
// or a different state moves far more: a missing line of small text on an iPad is about 1 in 700). Prints what it found.
bool matches(const char* shot, const char* golden) {
    std::vector<uint8_t> a, b;
    PrUVec2 ea{}, eb{};
    std::string why;
    if (!readPng(shot, &a, &ea, &why) || !readPng(golden, &b, &eb, &why)) {
        std::fprintf(stderr, "nexus_ios_shot: %s\n", why.c_str());
        return false;
    }
    if (ea.x != eb.x || ea.y != eb.y) {
        std::fprintf(stderr, "nexus_ios_shot: the shot is %ux%u and the golden %ux%u\n", ea.x, ea.y, eb.x, eb.y);
        return false;
    }
    std::size_t differing = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
        for (std::size_t c = 0; c < 4; ++c) {
            if (std::abs(int(a[i + c]) - int(b[i + c])) > 24) {
                ++differing;
                break;
            }
        }
    }
    const std::size_t total = a.size() / 4;
    const std::size_t allowed = total / 2000;
    std::printf("nexus_ios_shot: %zu of %zu pixels differ from %s (at most %zu may)\n", differing, total, golden,
        allowed);
    return differing <= allowed;
}

} // namespace

// The server dialog as at first launch: no server kept, TEXT typed, and Enter pressed once if TEXT is not empty.
struct FirstLaunch : nexus::ServerChoice {
    std::string field;
    std::string why;
    bool asking() override { return true; }
    std::string_view current() override { return {}; }
    std::string_view typed() override { return field; }
    std::string_view problem() override { return why; }
    void type(std::string_view text) override { field = std::string(text); }
    void submit() override {}
    void change() override {}
    void cancel() override {}
};

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: nexus_ios_shot STATE|- OUT.png WIDTH HEIGHT RATIO [--lost] [--tab N] [--status TEXT] "
            "[--text-steps N] [--ask-server TEXT] [--compare GOLDEN.png]\n");
        return 2;
    }
    const std::string state = argv[1];
    const char* out = argv[2];
    const float width = std::strtof(argv[3], nullptr);
    constexpr float kRegularFromPoints = 700;
    const float height = std::strtof(argv[4], nullptr);
    const float ratio = std::strtof(argv[5], nullptr);
    bool lost = false;
    std::size_t tab = 0;
    const char* golden = nullptr;
    const char* status = nullptr;
    const char* askServer = nullptr;
    int textSteps = nexus::style::kDefaultTextSteps;
    for (int i = 6; i < argc; ++i) {
        if (std::strcmp(argv[i], "--lost") == 0) {
            lost = true;
        } else if (std::strcmp(argv[i], "--status") == 0 && i + 1 < argc) {
            status = argv[++i];
        } else if (std::strcmp(argv[i], "--tab") == 0 && i + 1 < argc) {
            tab = std::size_t(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--text-steps") == 0 && i + 1 < argc) {
            textSteps = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--ask-server") == 0 && i + 1 < argc) {
            askServer = argv[++i];
        } else if (std::strcmp(argv[i], "--compare") == 0 && i + 1 < argc) {
            golden = argv[++i];
        } else {
            std::fprintf(stderr, "nexus_ios_shot: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!(width > 0 && height > 0 && ratio > 0)) {
        std::fprintf(stderr, "nexus_ios_shot: the width, height and ratio must be positive\n");
        return 2;
    }
    nexus::sync::Sessions held;
    std::string why;
    if (state != "-" && !nexus::readStateAt(state, &held, &why)) {
        std::fprintf(stderr, "nexus_ios_shot: %s\n", why.c_str());
        return 2;
    }
    // No tab yet: the state's AIs and dashboards come with it (state_window.h showHeldState, frame_host.h takeDashboards).
    nexus::Model model({});
    nexus::Look look;
    nexus::View view;
    view.model = &model;
    view.look = &look;
    nexus::FrameHost host;
    host.model = &model;
    host.view = &view;
    if (state != "-" && !nexus::showHeldState(held, lost, tab, model, view, host, &why)) {
        std::fprintf(stderr, "nexus_ios_shot: %s\n", why.c_str());
        return 2;
    }
    if (status != nullptr) view.status = status;
    view.askedTextSteps = textSteps;
    view.dialogsAtTop = true;   // as the app places a typed dialog (main_ios.cpp, View::dialogsAtTop)
    // A frame as wide as an iPad's (a regular horizontal size class starts well below 1032 points) takes the narrower
    // prompt and column the app gives a regular width (main_ios.cpp, style.h kRegularWidthColumn).
    if (width >= kRegularFromPoints) view.columnScale = nexus::style::kRegularWidthColumn;
    // Narrower, an iPhone's compact width: the replies take the column's whole width (View::fullWidthReplies).
    else view.fullWidthReplies = true;
    FirstLaunch asked;
    if (askServer != nullptr) {
        asked.field = askServer;
        std::string address;
        if (!asked.field.empty()) nexus::parseServerAddress(asked.field, true, &address, &asked.why);
        view.serverChoice = &asked;
    }
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    prFontSetSetSystem(fonts, system);
    prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));
    int code = 1;
    {
        nexus::OffscreenFrame off(view, fonts, PrVec2{width, height}, ratio, false);
        // Enough frames for every query to read a layout, as the desktop's screenshot draws.
        for (int i = 0; i < 4; ++i)
            off.frame();
        code = off.write(out, false) ? 0 : 1;
    }
    prFontSetDestroy(fonts);
    prFontSystemDestroy(system);
    if (code == 0 && golden != nullptr && !matches(out, golden))
        code = 1;
    return code;
}
