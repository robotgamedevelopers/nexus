#include "font_prefetch.h"

#include <cstddef>
#include <iterator>
#include <string_view>

#include "pr/kit.h"
#include "pr/kit_panel.h"
#include "pr/kit_text.h"
#include "pr/layout_tree.h"
#include "pr/text.h"
#include "pr/ui.h"

#include "view.h"

namespace nexus {

namespace {

// A line of each script and symbol set the transcript and terminals are likely to meet.
constexpr std::string_view kSample =
    "Aa \xF0\x9F\x8E\x89\xF0\x9F\x9A\x80\xE2\x9C\x85\xE2\x9D\x8C\xF0\x9F\x91\x8D "             // emoji
    "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C "                                     // Chinese
    "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF\xE3\x82\xAB\xE3\x82\xBF "   // Japanese
    "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4 "                                                 // Korean
    "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D "             // Arabic, Hebrew
    "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xCE\x93\xCE\xB5\xCE\xB9\xCE\xAC "     // Cyrillic, Greek
    "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5 "             // Thai
    "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87 "             // Devanagari
    "\xE2\x86\x92\xE2\x86\x90\xE2\x94\x80\xE2\x94\x82\xE2\x94\x8C\xE2\x88\x91\xE2\x88\x9E"
    "\xE2\x9C\x93\xE2\x80\xA2\xE2\x80\xA6";                                                 // arrows, boxes, math

// A line of the window's own text: Latin letters, digits and punctuation.
constexpr std::string_view kLatin = "Aa Bb Cc 0123456789 .,:;!?'\"()[]{}<>/\\|-_=+*&%$#@~`^";

struct Samples {
    PrKit* kit;
    std::string_view text;
    const PrKitTextRole* roles;
    std::size_t roleCount;
};

void buildSamples(void* context, PrUi*, const PrUiRound*) {
    const Samples& s = *static_cast<Samples*>(context);
    PrKitPanelStyle surface{
        .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
        .background = PrKitBackground_App,
    };
    PrLayoutNode root = prKitPanel(s.kit, kPrLayoutNodeNone, {}, surface);
    for (std::size_t i = 0; i < s.roleCount; ++i) {
        PrKitTextStyle style{.role = s.roles[i], .tone = PrKitTone_Primary, .wrap = PrKitWrap_Auto,
            .align = PrKitTextAlign_Start, .preserve = true};
        prKitLabel(s.kit, root, PrKitLabel{.key = 0, .text = PrStringView{s.text.data(), s.text.size()}}, style);
    }
}

// Sets `text` in each of `roles`, in a layout of its own over `fonts` and `shaper`, in the window's theme.
void setSamples(PrTextShaper* shaper, PrFontSet* fonts, bool overlay, std::string_view text, const PrKitTextRole* roles,
        std::size_t roleCount) {
    PrLayoutTreeDesc desc{.shaper = shaper, .targetPixelRatio = 2};
    PrLayoutTree* tree = prLayoutTreeCreate(&desc);
    PrUi* ui = prUiCreate(tree);
    PrKitTheme theme = windowTheme(fonts, overlay);
    PrKit* kit = prKitCreate(ui, &theme);
    Samples s{kit, text, roles, roleCount};
    PrUiFrameDesc frame{.time = 0, .delta = 0, .extent = PrVec2{1200, 800}, .maxRounds = 1,
        .display = prAppDisplayDefault(), .preferences = {}};
    prUiFrame(ui, &frame, buildSamples, &s);
    prKitDestroy(kit);
    prUiDestroy(ui);
    prLayoutTreeDestroy(tree);
}

void prefetch(PrFontSet* fonts) {
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    const PrKitTextRole roles[] = {PrKitTextRole_Body, PrKitTextRole_BodySmall, PrKitTextRole_Meta,
        PrKitTextRole_Mono};
    setSamples(shaper, fonts, false, kSample, roles, std::size(roles));
    prTextShaperDestroy(shaper);
}

} // namespace

void warmShaper(PrTextShaper* shaper, PrFontSet* fonts, bool overlay) {
    PrKitTextRole roles[PrKitTextRole_Count];
    for (std::size_t i = 0; i < std::size(roles); ++i) {
        roles[i] = PrKitTextRole(i);
    }
    setSamples(shaper, fonts, overlay, kLatin, roles, std::size(roles));
}

FontPrefetch::FontPrefetch(PrFontSet* fonts) : thread_([fonts] { prefetch(fonts); }) {}

FontPrefetch::~FontPrefetch() {
    wait();
}

void FontPrefetch::wait() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

} // namespace nexus
