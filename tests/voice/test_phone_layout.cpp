// The iPhone's AI page as laid out (src/view.cpp phoneBack, buildWindow), through the window's offscreen frame
// (src/offscreen.h): the AI's title centred on the window's width whatever the back chevron's, a long one shortened
// with an ellipsis to the room both sides of the chevron leave, and the prompt's rounded box inside the room the
// window may use (the screen less its safe area and the keyboard, usableHeightOf). Pure: made-up time, no window.

#include <cmath>
#include <cstdio>
#include <string>

#include "pr/font_set.h"
#include "pr/layout_query.h"
#include "pr/text_font.h"

#include "model.h"
#include "offscreen.h"
#include "view.h"

namespace {

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

constexpr float kWidth = 402, kHeight = 874;  // an iPhone 17 Pro, in points

PrRect boundsOf(nexus::OffscreenFrame& off, PrLayoutNode node) {
    return prLayoutRectToPx(prLayoutNodeBounds(off.kit()->tree, node, PrLayoutCoordinates_Viewport));
}

struct Page {
    nexus::Model model;
    nexus::Look look;
    nexus::View view;
    explicit Page(const std::string& name) : model({{name, ""}}, false) {
        view.model = &model;
        view.look = &look;
        view.idiom = "phone";
        view.phoneMenu = false;
        view.windowApp.panels = {nexus::Panel::Back, nexus::Panel::Transcript, nexus::Panel::Prompt};
        view.windowApp.surface.showTabs = false;
        view.windowApp.surface.tabTitles = {name};
        model.select_tab(0);
    }
};

void theTitleIsCentredOnTheWindow(PrFontSet* fonts, const std::string& name, bool shortened) {
    Page page(name);
    nexus::OffscreenFrame off(page.view, fonts, PrVec2{kWidth, kHeight}, 3, false);
    for (int i = 0; i < 6; ++i)
        off.frame();
    CHECK(page.view.phoneTitle != kPrLayoutNodeNone);
    CHECK(page.view.phoneBackButton != kPrLayoutNodeNone);
    const PrRect title = boundsOf(off, page.view.phoneTitle);
    const PrRect back = boundsOf(off, page.view.phoneBackButton);
    const float centre = (title.left + title.right) / 2;
    const PrRect room = boundsOf(off, page.view.phoneTitleRoom);
    if (std::fabs((title.left + title.right) / 2 - kWidth / 2) > 1 || title.left < back.right)
        std::fprintf(stderr, "  %s -> '%s': title %.1f..%.1f, back %.1f..%.1f, room %.1f..%.1f\n", name.c_str(),
                 page.view.phoneTitleShown.c_str(), title.left, title.right, back.left, back.right, room.left, room.right);
    if (std::fabs(centre - kWidth / 2) > 1)
        if (std::fabs((title.left + title.right) / 2 - kWidth / 2) > 1 || title.left < back.right)
        std::fprintf(stderr, "  %s: the title's centre is %.1f, the window's %.1f\n", name.c_str(), centre, kWidth / 2);
    CHECK(std::fabs(centre - kWidth / 2) <= 1);
    CHECK(title.left >= back.right);  // the chevron never covers the title
    CHECK(title.right <= kWidth - (back.right));  // the same room on the other side
    const bool ellipsis = page.view.phoneTitleShown.size() >= 3 &&
        page.view.phoneTitleShown.compare(page.view.phoneTitleShown.size() - 3, 3, "…") == 0;
    CHECK(ellipsis == shortened);
    if (!shortened) CHECK(page.view.phoneTitleShown == name);
}

void thePromptIsInTheRoomLeft(PrFontSet* fonts, float keyboardTop) {
    Page page("Johnny Grok");
    // The room on the screen: Studio's panel starts below the status bar; offscreen, the window starts at 0.
    const float safeTop = 62, safeBottom = 34;
    page.view.usableHeight = nexus::usableHeightOf(kHeight, safeTop, safeBottom, keyboardTop);
    nexus::OffscreenFrame off(page.view, fonts, PrVec2{kWidth, kHeight}, 3, false);
    for (int i = 0; i < 6; ++i)
        off.frame();
    CHECK(page.view.promptBox != kPrLayoutNodeNone);
    const PrRect box = boundsOf(off, page.view.promptBox);
    const float bottom = page.view.usableHeight;
    if (box.bottom > bottom + 0.5f || box.bottom < bottom - 60)
        std::fprintf(stderr, "  the prompt's box ends at %.1f, the room at %.1f\n", box.bottom, bottom);
    CHECK(box.bottom <= bottom + 0.5f);  // never under the home indicator or the keyboard
    CHECK(box.bottom >= bottom - 60);    // and at the foot of the room, not floating up the page
}

void theRoom() {
    CHECK(nexus::usableHeightOf(874, 62, 34, 0) == 778);
    CHECK(nexus::usableHeightOf(874, 62, 34, 538) == 476);  // the keyboard is higher than the home indicator
    CHECK(nexus::usableHeightOf(874, 62, 34, 860) == 778);  // a keyboard below the safe area changes nothing
    CHECK(nexus::usableHeightOf(100, 62, 34, 0) == 4);
    CHECK(nexus::usableHeightOf(50, 62, 34, 0) == 0);
}

} // namespace

int main() {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    prFontSetSetSystem(fonts, system);
    prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(PrFontGeneric_SystemUi));
    theRoom();
    theTitleIsCentredOnTheWindow(fonts, "Johnny Grok", false);
    theTitleIsCentredOnTheWindow(fonts, "Jo", false);
    theTitleIsCentredOnTheWindow(fonts, "Maximiliana Wolkenstein-Hohenberg of the Northern Reach", true);
    thePromptIsInTheRoomLeft(fonts, 0);
    thePromptIsInTheRoomLeft(fonts, 538);
    prFontSetDestroy(fonts);
    prFontSystemDestroy(system);
    if (failures) {
        std::fprintf(stderr, "%d phone-layout check(s) failed\n", failures);
        return 1;
    }
    std::printf("phone_layout: all checks passed\n");
    return 0;
}
