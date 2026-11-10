#include "view.h"

#include <algorithm>
#include <cmath>

#include "pr/draw.h"

#include "style.h"

namespace nexus {

namespace {

using namespace style;

constexpr float kPi = 3.14159265358979f;

PrColor faded(PrColor c, float alpha) {
    c.a = alpha;
    return c;
}

// ---------------------------------------------------------------------------------------------------------------------
// Text

uint32_t decode(const std::string& s, size_t at, size_t* length) {
    unsigned char c = static_cast<unsigned char>(s[at]);
    uint32_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (at + n > s.size())
        n = 1;
    uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (uint32_t i = 1; i < n; ++i)
        cp = (cp << 6) | (static_cast<unsigned char>(s[at + i]) & 0x3F);
    *length = n;
    return cp;
}

struct Run {
    const PrFont* font;
    std::string text;
};

// Splits text into runs of the base font and of the symbol font, for code points the base font has no glyph for.
// (Prax's font set does this fallback; this tracer shapes against single fonts, so it does its own.)
std::vector<Run> splitRuns(const Fonts& fonts, const PrFont* base, const std::string& text) {
    std::vector<Run> runs;
    for (size_t at = 0; at < text.size();) {
        size_t length = 1;
        uint32_t cp = decode(text, at, &length);
        const PrFont* font = base;
        if (cp > 0x7F && prFontCodePointGlyph(base, cp) == 0 && fonts.symbols != nullptr &&
                prFontCodePointGlyph(fonts.symbols, cp) != 0)
            font = fonts.symbols;
        if (runs.empty() || runs.back().font != font)
            runs.push_back(Run{font, {}});
        runs.back().text.append(text, at, length);
        at += length;
    }
    return runs;
}

struct Shaped {
    std::vector<PrFontGlyph> glyphs;
    std::vector<PrAffine2> transforms;
};

float shapeRun(const Fonts& fonts, const PrFont* font, float size, const std::string& text, PrVec2 origin,
        Shaped& out) {
    out.glyphs.clear();
    out.transforms.clear();
    PrTextRun run{};
    run.font = font;
    run.text = PrStringView{text.data(), text.size()};
    run.pixelsPerEm = size;
    if (prTextShaperShape(fonts.shaper, &run, fonts.buffer) != PrStatus_Ok)
        return 0;
    uint32_t count = prTextGlyphBufferGlyphCount(fonts.buffer);
    out.glyphs.resize(count);
    out.transforms.resize(count);
    PrVec2 pen = prTextGlyphBufferPlace(fonts.buffer, size, origin, out.glyphs.data(), out.transforms.data());
    bool scaled = false;
    for (const PrAffine2& t : out.transforms)
        scaled = scaled || t.a != 1;
    if (!scaled)
        out.transforms.clear();
    return pen.x - origin.x;
}

float measure(const Fonts& fonts, const PrFont* base, float size, const std::string& text) {
    float width = 0;
    Shaped shaped;
    for (const Run& run : splitRuns(fonts, base, text))
        width += shapeRun(fonts, run.font, size, run.text, PrVec2{width, 0}, shaped);
    return width;
}

// Draws text with its baseline at `origin`. Returns the advance.
float drawText(PrDrawList* list, const Fonts& fonts, const PrFont* base, float size, const std::string& text,
        PrVec2 origin, PrColor color) {
    float x = origin.x;
    Shaped shaped;
    for (const Run& run : splitRuns(fonts, base, text)) {
        x += shapeRun(fonts, run.font, size, run.text, PrVec2{x, origin.y}, shaped);
        if (shaped.glyphs.empty())
            continue;
        PrFontGlyphRun glyphs{};
        glyphs.font = run.font;
        glyphs.size = size;
        glyphs.glyphs = shaped.glyphs.data();
        glyphs.glyphCount = uint32_t(shaped.glyphs.size());
        glyphs.transforms = shaped.transforms.empty() ? nullptr : shaped.transforms.data();
        prDrawListGlyphs(list, &glyphs, PrVec2{}, PrDrawPaint{.color = color});
    }
    return x - origin.x;
}

// Greedy word wrap at spaces. (Prax's lib/text has line breaking; the tracer did not wire it in.)
std::vector<std::string> wrap(const Fonts& fonts, const PrFont* font, float size, const std::string& text, float width) {
    std::vector<std::string> lines;
    std::string line;
    size_t at = 0;
    while (at <= text.size()) {
        size_t space = text.find(' ', at);
        if (space == std::string::npos)
            space = text.size();
        std::string word = text.substr(at, space - at);
        std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && measure(fonts, font, size, candidate) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        at = space + 1;
    }
    lines.push_back(line);
    return lines;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shapes

PrCornerRect rounded(PrRect r, float radius) {
    PrCornerRect shape{};
    shape.rect = r;
    for (PrCorner& corner : shape.corners)
        corner = PrCorner{PrVec2{radius, radius}, 1};
    return shape;
}

void fillRounded(PrDrawList* list, PrRect r, float radius, PrColor color) {
    prDrawListCornerRect(list, rounded(r, radius), PrDrawPaint{.color = color});
}

void strokeRounded(PrDrawList* list, PrRect r, float radius, PrColor color, float width) {
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = width;
    stroke.align = PrDrawStrokeAlign_Inside;
    prDrawListCornerRect(list, rounded(r, radius), PrDrawPaint{.color = color, .stroke = &stroke});
}

void line(PrDrawList* list, PrVec2 a, PrVec2 b, PrColor color, float width) {
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = width;
    stroke.shape.startCap = PrPathCap_Round;
    stroke.shape.endCap = PrPathCap_Round;
    prDrawListLine(list, a, b, PrDrawPaint{.color = color, .stroke = &stroke});
}

void check(PrDrawList* list, PrVec2 at, float s, PrColor color, float width) {
    line(list, PrVec2{at.x, at.y}, PrVec2{at.x + s * 0.35f, at.y + s * 0.35f}, color, width);
    line(list, PrVec2{at.x + s * 0.35f, at.y + s * 0.35f}, PrVec2{at.x + s, at.y - s * 0.45f}, color, width);
}

void circleStroke(PrDrawList* list, PrVec2 center, float radius, PrColor color, float width,
        const float* dashes = nullptr, uint32_t dashCount = 0) {
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = width;
    stroke.shape.dashes = dashes;
    stroke.shape.dashCount = dashCount;
    prDrawListEllipse(list, center, PrVec2{radius, radius}, 0, PrDrawPaint{.color = color, .stroke = &stroke});
}

// ---------------------------------------------------------------------------------------------------------------------
// Transcript

struct Column {
    float left;
    float right;
};

// Draws a paragraph wrapped to `width`, with an optional bold run-in `lead`. Returns its height.
float paragraph(PrDrawList* list, const Fonts& fonts, const std::string& lead, const std::string& text, float left,
        float width, float top, PrColor ink) {
    std::string whole = lead.empty() ? text : lead + " " + text;
    std::vector<std::string> lines = wrap(fonts, fonts.regular, kBodySize, whole, width);
    if (list != nullptr) {
        for (size_t i = 0; i < lines.size(); ++i) {
            PrVec2 at{left, top + kLinePitch * float(i) + kLinePitch * 0.72f};
            if (i == 0 && !lead.empty() && lines[0].compare(0, lead.size(), lead) == 0) {
                at.x += drawText(list, fonts, fonts.semibold, kBodySize, lead, at, ink);
                drawText(list, fonts, fonts.regular, kBodySize, lines[0].substr(lead.size()), at, ink);
            } else {
                drawText(list, fonts, fonts.regular, kBodySize, lines[i], at, ink);
            }
        }
    }
    return float(lines.size()) * kLinePitch;
}

// Lays out one entry at `top`, drawing it if `list` is not null. Returns its height including the gap after it.
float entry(PrDrawList* list, const Fonts& fonts, const Entry& e, bool newestPerson, Column c, float top) {
    float width = c.right - c.left;
    switch (e.kind) {
        case EntryKind::Person: {
            float maxText = width * kBubbleMaxFraction - kBubblePadX * 2;
            std::vector<std::string> lines = wrap(fonts, fonts.regular, kBodySize, e.text, maxText);
            float textWidth = 0;
            for (const std::string& l : lines)
                textWidth = std::max(textWidth, measure(fonts, fonts.regular, kBodySize, l));
            float h = float(lines.size()) * kLinePitch + kBubblePadY * 2;
            PrRect bubble{c.right - textWidth - kBubblePadX * 2, top, c.right, top + h};
            float meta = (newestPerson || e.queued) ? kMetaSize * 1.6f : 0;
            if (list != nullptr) {
                fillRounded(list, bubble, kBubbleRadius, faded(kBubble, e.queued ? 0.5f : 1));
                for (size_t i = 0; i < lines.size(); ++i)
                    drawText(list, fonts, fonts.regular, kBodySize, lines[i],
                        PrVec2{bubble.left + kBubblePadX, top + kBubblePadY + kLinePitch * float(i) + kLinePitch * 0.72f},
                        faded(kInk, e.queued ? 0.55f : 1));
                float baseline = bubble.bottom + kMetaSize * 1.3f;
                if (e.queued) {
                    float w = measure(fonts, fonts.regular, kMetaSize, "queued");
                    drawText(list, fonts, fonts.regular, kMetaSize, "queued", PrVec2{c.right - w - 6, baseline}, kMeta);
                } else if (newestPerson) {
                    float w = measure(fonts, fonts.regular, kMetaSize, "Just now");
                    float x = c.right - w - 6;
                    drawText(list, fonts, fonts.regular, kMetaSize, "Just now", PrVec2{x, baseline}, kMeta);
                    float s = kMetaSize * 0.55f;
                    check(list, PrVec2{x - s * 3.1f, baseline - s * 0.45f}, s, kMeta, 1.6f);
                    check(list, PrVec2{x - s * 2.3f, baseline - s * 0.45f}, s, kMeta, 1.6f);
                }
            }
            return h + meta + kGap;
        }
        case EntryKind::Ai:
            return paragraph(list, fonts, e.lead, e.text, c.left, width, top, kInk) + kGap;
        case EntryKind::ToolSummary:
            return paragraph(list, fonts, {}, e.text + "  \xE2\x80\xBA", c.left, width, top, kNotice) + kGap;
        case EntryKind::TaskList: {
            float h = kCardRow * float(e.tasks.size()) + kCardRow * 0.35f;
            if (list != nullptr) {
                strokeRounded(list, PrRect{c.left, top, c.right, top + h}, kCardRadius, kCardLine, 1);
                for (size_t i = 0; i < e.tasks.size(); ++i) {
                    const Task& t = e.tasks[i];
                    float cy = top + kCardRow * 0.175f + kCardRow * float(i) + kCardRow / 2;
                    PrVec2 center{c.left + kU * 1.0f, cy};
                    PrColor ink = kInk;
                    if (t.state == TaskState::Done) {
                        prDrawListEllipse(list, center, PrVec2{kIcon, kIcon}, 0, PrDrawPaint{.color = kNotice});
                        check(list, PrVec2{center.x - kIcon * 0.5f, center.y + kIcon * 0.05f}, kIcon, kBackground, 2.2f);
                        ink = kNotice;
                    } else if (t.state == TaskState::InProgress) {
                        circleStroke(list, center, kIcon * 0.92f, kCardLine, 2.4f);
                        prDrawListSector(list, center, kIcon * 0.8f, kIcon * 1.05f, -kPi / 2, kPi * 1.2f,
                            PrDrawPaint{.color = kInk});
                    } else {
                        static const float dashes[] = {2.8f, 2.8f};
                        circleStroke(list, center, kIcon * 0.92f, kNotice, 1.6f, dashes, 2);
                        ink = faded(kInk, 0.85f);
                    }
                    drawText(list, fonts, fonts.regular, kBodySize, t.text,
                        PrVec2{c.left + kU * 1.9f, cy + kBodySize * 0.35f}, ink);
                }
            }
            return h + kGap;
        }
        case EntryKind::Status: {
            if (list != nullptr) {
                // Two small orange dots, then dim text, as in the reference.
                float cy = top + kLinePitch * 0.72f - kBodySize * 0.33f;
                float r = kBodySize * 0.12f;
                prDrawListEllipse(list, PrVec2{c.left + r * 1.3f, cy}, PrVec2{r, r}, 0, PrDrawPaint{.color = kOrange});
                prDrawListEllipse(list, PrVec2{c.left + r * 7.0f, cy}, PrVec2{r * 1.1f, r}, 0,
                    PrDrawPaint{.color = kOrange});
                drawText(list, fonts, fonts.regular, kBodySize, e.text,
                    PrVec2{c.left + kBodySize * 1.7f, top + kLinePitch * 0.72f}, kNotice);
            }
            return kLinePitch + kGap;
        }
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

bool openFonts(Fonts& fonts, std::string& error) {
    if (prFontCreateFromFile("/System/Library/Fonts/SFNS.ttf", 0, nullptr, &fonts.regular) != PrStatus_Ok) {
        error = "could not load /System/Library/Fonts/SFNS.ttf";
        return false;
    }
    PrFontVariation weight{PR_FONT_TAG('w', 'g', 'h', 't'), 600};
    PrFontDerivation derivation = prFontDerivationDefault();
    derivation.variations = &weight;
    derivation.variationCount = 1;
    fonts.semibold = prFontCreateDerived(fonts.regular, &derivation);
    if (prFontCreateFromFile("/System/Library/Fonts/Apple Symbols.ttf", 0, nullptr, &fonts.symbols) != PrStatus_Ok)
        fonts.symbols = nullptr;
    fonts.shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    fonts.buffer = prTextGlyphBufferCreate();
    if (fonts.shaper == nullptr) {
        error = "no text shaper in this build";
        return false;
    }
    return true;
}

void closeFonts(Fonts& fonts) {
    prTextGlyphBufferDestroy(fonts.buffer);
    prTextShaperDestroy(fonts.shaper);
    prFontRelease(fonts.symbols);
    prFontRelease(fonts.semibold);
    prFontRelease(fonts.regular);
    fonts = Fonts{};
}

void drawWindow(PrDrawList* list, const Model& model, ViewState& view, const Fonts& fonts, PrVec2 extent) {
    view.scroll.resize(model.tabs.size(), 0);
    prDrawListRect(list, PrRect{0, 0, extent.x, extent.y}, PrDrawPaint{.color = kBackground});

    // Tab strip, in the place and quiet style of the reference's title row.
    prDrawListRect(list, PrRect{0, 0, extent.x, kStripHeight}, PrDrawPaint{.color = kStrip});
    view.tabRects.assign(model.tabs.size(), PrRect{});
    float x = kU * 0.8f;
    float pillH = kTabSize * 1.75f;
    float pillTop = (kStripHeight - pillH) / 2;
    float tabBaseline = kStripHeight / 2 + kTabSize * 0.36f;
    for (size_t i = 0; i < model.tabs.size(); ++i) {
        bool active = i == model.active;
        std::string number = std::to_string(i + 1);
        const std::string& name = model.tabs[i].who.name;
        float numberW = measure(fonts, fonts.regular, kTabSize, number);
        float nameW = measure(fonts, fonts.regular, kTabSize, name);
        float pad = kTabSize * 0.6f;
        PrRect r{x, pillTop, x + pad + numberW + kTabSize * 0.45f + nameW + pad, pillTop + pillH};
        view.tabRects[i] = r;
        if (active)
            fillRounded(list, r, pillH * 0.3f, kBubble);
        drawText(list, fonts, fonts.regular, kTabSize, number, PrVec2{r.left + pad, tabBaseline}, kPlaceholder);
        drawText(list, fonts, fonts.regular, kTabSize, name,
            PrVec2{r.left + pad + numberW + kTabSize * 0.45f, tabBaseline}, active ? kInk : kNotice);
        x = r.right + kTabSize * 0.3f;
    }
    prDrawListRect(list, PrRect{0, kStripHeight, extent.x, kStripHeight + 1}, PrDrawPaint{.color = kStripLine});

    // The transcript column and the slightly wider prompt box, both centred.
    float columnW = extent.x * kColumnFraction;
    Column c{(extent.x - columnW) / 2, (extent.x + columnW) / 2};
    float promptW = extent.x * kPromptFraction;

    // Prompt box and the label row beneath it, pinned to the bottom.
    const Tab& tab = activeTab(model);
    PrRect box{(extent.x - promptW) / 2, extent.y - kPromptBottom - kPromptHeight, (extent.x + promptW) / 2,
        extent.y - kPromptBottom};
    fillRounded(list, box, kPromptRadius, kPromptFill);
    strokeRounded(list, box, kPromptRadius, kPromptBorder, 1);
    float mid = (box.top + box.bottom) / 2;
    float plusX = box.left + kU * 1.15f;
    float plus = kU * 0.32f;
    line(list, PrVec2{plusX - plus, mid}, PrVec2{plusX + plus, mid}, kInk, 2);
    line(list, PrVec2{plusX, mid - plus}, PrVec2{plusX, mid + plus}, kInk, 2);
    float textX = box.left + kU * 2.3f;
    float baseline = mid + kBodySize * 0.36f;
    float caretH = kBodySize * 0.62f;
    if (tab.prompt.empty()) {
        drawText(list, fonts, fonts.regular, kBodySize, "\xE2\x8F\x8E to send, \xE2\x8C\x98\xE2\x8F\x8E to queue for later",
            PrVec2{textX, baseline}, kPlaceholder);
        if (view.caretOn)
            prDrawListRect(list, PrRect{textX - 3, mid - caretH, textX - 1, mid + caretH}, PrDrawPaint{.color = kInk});
    } else {
        drawText(list, fonts, fonts.regular, kBodySize, tab.prompt, PrVec2{textX, baseline}, kInk);
        float caretX = textX + measure(fonts, fonts.regular, kBodySize, tab.prompt.substr(0, tab.caret));
        if (view.caretOn)
            prDrawListRect(list, PrRect{caretX, mid - caretH, caretX + 2, mid + caretH}, PrDrawPaint{.color = kInk});
    }
    // Stop square, then the microphone, at the right (the reference's order).
    float micX = box.right - kU * 1.15f;
    float stopX = micX - kU * 1.9f;
    float stop = kU * 0.36f;
    fillRounded(list, PrRect{stopX - stop, mid - stop, stopX + stop, mid + stop}, stop * 0.3f, kStop);
    PrDrawStroke micStroke = prDrawStrokeDefault();
    micStroke.shape.width = 2;
    float m = kU * 0.2f;
    prDrawListCapsule(list, PrVec2{micX, mid - m * 2}, PrVec2{micX, mid + m * 0.2f}, m * 1.1f,
        PrDrawPaint{.color = kInk, .stroke = &micStroke});
    prDrawListSector(list, PrVec2{micX, mid + m * 0.2f}, m * 1.8f, m * 1.8f + 2, 0, kPi, PrDrawPaint{.color = kInk});
    line(list, PrVec2{micX, mid + m * 2.0f}, PrVec2{micX, mid + m * 2.8f}, kInk, 2);

    // Model, effort, mode and a ring: right-aligned under the box.
    float rowBase = extent.y - kLabelBaseline;
    std::string modelName = tab.who.model.empty() ? std::string("model") : tab.who.model;
    float space = kLabelSize * 0.5f;
    float ringR = kLabelSize * 0.38f;
    float wModel = measure(fonts, fonts.regular, kLabelSize, modelName);
    float wHigh = measure(fonts, fonts.regular, kLabelSize, "High");
    float wAuto = measure(fonts, fonts.regular, kLabelSize, "Auto");
    float rx = box.right - kU * 0.7f - ringR * 2 - space * 2 - wAuto - space * 2 - wHigh - space - wModel;
    rx += drawText(list, fonts, fonts.regular, kLabelSize, modelName, PrVec2{rx, rowBase}, kInk) + space;
    rx += drawText(list, fonts, fonts.regular, kLabelSize, "High", PrVec2{rx, rowBase}, kNotice) + space * 2;
    rx += drawText(list, fonts, fonts.regular, kLabelSize, "Auto", PrVec2{rx, rowBase}, kInk) + space * 2;
    circleStroke(list, PrVec2{rx + ringR, rowBase - kLabelSize * 0.35f}, ringR, kRing, 2.2f);

    // Transcript: everything between the strip and the box, bottom-anchored, scrolled, clipped.
    PrRect area{0, kStripHeight + 1, extent.x, box.top - kU};
    bool newestFound = false;
    size_t newestPerson = tab.transcript.size();
    for (size_t i = tab.transcript.size(); i-- > 0 && !newestFound;) {
        if (tab.transcript[i].kind == EntryKind::Person && !tab.transcript[i].queued) {
            newestPerson = i;
            newestFound = true;
        }
    }
    float inset = kU * 0.6f;
    float total = inset;
    for (size_t i = 0; i < tab.transcript.size(); ++i)
        total += entry(nullptr, fonts, tab.transcript[i], i == newestPerson, c, 0);
    float areaH = area.bottom - area.top;
    view.maxScroll = std::max(0.0f, total - areaH);
    float& scroll = view.scroll[model.active];
    scroll = std::clamp(scroll, 0.0f, view.maxScroll);
    float y = total > areaH ? area.bottom - total + scroll + inset : area.top + inset;
    prDrawListPushClipRect(list, area, PrDrawEdges_Antialiased);
    if (tab.transcript.empty()) {
        std::string hint = "Say something to " + tab.who.name;
        float w = measure(fonts, fonts.regular, kBodySize, hint);
        drawText(list, fonts, fonts.regular, kBodySize, hint,
            PrVec2{(extent.x - w) / 2, (area.top + area.bottom) / 2}, kPlaceholder);
    }
    for (size_t i = 0; i < tab.transcript.size(); ++i)
        y += entry(list, fonts, tab.transcript[i], i == newestPerson, c, y);
    prDrawListPopClip(list);
}

int tabAt(const ViewState& view, PrVec2 p) {
    for (size_t i = 0; i < view.tabRects.size(); ++i) {
        const PrRect& r = view.tabRects[i];
        if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom)
            return int(i);
    }
    return -1;
}

void scrollBy(const Model& model, ViewState& view, float dy) {
    view.scroll.resize(model.tabs.size(), 0);
    float& s = view.scroll[model.active];
    s = std::clamp(s + dy, 0.0f, view.maxScroll);
}

} // namespace nexus
