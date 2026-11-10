#include "view.h"

#include <algorithm>
#include <cmath>

#include "pr/draw.h"

namespace nexus {

namespace {

constexpr float kPi = 3.14159265358979f;

PrColor rgb(uint32_t hex, float alpha = 1) {
    return PrColor{float((hex >> 16) & 0xFF) / 255.0f, float((hex >> 8) & 0xFF) / 255.0f, float(hex & 0xFF) / 255.0f,
        alpha};
}

const PrColor kBackground = rgb(0x1a1a1a);
const PrColor kBubble = rgb(0x2a2a2a);
const PrColor kInk = rgb(0xececec);
const PrColor kBright = rgb(0xffffff);
const PrColor kMid = rgb(0x8f8f8f);
const PrColor kDim = rgb(0x6c6c6c);
const PrColor kLine = rgb(0x333333);
const PrColor kPromptFill = rgb(0x242424);
const PrColor kPromptBorder = rgb(0x3a3a3a);
const PrColor kOrange = rgb(0xd97757);

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

constexpr float kBodySize = 15;
constexpr float kBodyLine = 23;
constexpr float kGap = 18;

struct Column {
    float left;
    float right;
};

// Lays out one entry at `top`, drawing it if `list` is not null. Returns its height including the gap after it.
float entry(PrDrawList* list, const Fonts& fonts, const Entry& e, bool newestPerson, Column c, float top) {
    float width = c.right - c.left;
    switch (e.kind) {
        case EntryKind::Person: {
            float padX = 16;
            float padY = 11;
            float maxText = width * 0.72f - padX * 2;
            std::vector<std::string> lines = wrap(fonts, fonts.regular, kBodySize, e.text, maxText);
            float textWidth = 0;
            for (const std::string& l : lines)
                textWidth = std::max(textWidth, measure(fonts, fonts.regular, kBodySize, l));
            float h = float(lines.size()) * kBodyLine + padY * 2;
            PrRect bubble{c.right - textWidth - padX * 2, top, c.right, top + h};
            float meta = (newestPerson || e.queued) ? 22 : 0;
            if (list != nullptr) {
                float alpha = e.queued ? 0.5f : 1;
                PrColor fill = kBubble;
                fill.a = alpha;
                fillRounded(list, bubble, 18, fill);
                PrColor ink = kBright;
                ink.a = e.queued ? 0.6f : 1;
                for (size_t i = 0; i < lines.size(); ++i)
                    drawText(list, fonts, fonts.regular, kBodySize, lines[i],
                        PrVec2{bubble.left + padX, top + padY + kBodyLine * float(i) + 16.5f}, ink);
                float baseline = bubble.bottom + 16;
                if (e.queued) {
                    float w = measure(fonts, fonts.regular, 12, "queued");
                    drawText(list, fonts, fonts.regular, 12, "queued", PrVec2{c.right - w - 4, baseline}, kDim);
                } else if (newestPerson) {
                    float w = measure(fonts, fonts.regular, 12, "Just now");
                    float x = c.right - w - 4;
                    drawText(list, fonts, fonts.regular, 12, "Just now", PrVec2{x, baseline}, kDim);
                    check(list, PrVec2{x - 26, baseline - 4}, 8, kDim, 1.3f);
                    check(list, PrVec2{x - 20, baseline - 4}, 8, kDim, 1.3f);
                }
            }
            return h + meta + kGap;
        }
        case EntryKind::Ai: {
            std::vector<std::string> lines = wrap(fonts, fonts.regular, kBodySize, e.text, width);
            if (list != nullptr) {
                for (size_t i = 0; i < lines.size(); ++i)
                    drawText(list, fonts, fonts.regular, kBodySize, lines[i],
                        PrVec2{c.left, top + kBodyLine * float(i) + 16.5f}, kInk);
            }
            return float(lines.size()) * kBodyLine + kGap;
        }
        case EntryKind::ToolSummary: {
            if (list != nullptr) {
                float w = drawText(list, fonts, fonts.regular, 14, e.text, PrVec2{c.left, top + 15}, kMid);
                drawText(list, fonts, fonts.regular, 14, " \xE2\x80\xBA", PrVec2{c.left + w, top + 15}, kMid);
            }
            return 20 + kGap;
        }
        case EntryKind::TaskList: {
            float row = 38;
            float h = row * float(e.tasks.size()) + 12;
            if (list != nullptr) {
                strokeRounded(list, PrRect{c.left, top, c.right, top + h}, 12, kLine, 1);
                for (size_t i = 0; i < e.tasks.size(); ++i) {
                    const Task& t = e.tasks[i];
                    float cy = top + 6 + row * float(i) + row / 2;
                    PrVec2 center{c.left + 24, cy};
                    PrColor ink = kInk;
                    if (t.state == TaskState::Done) {
                        prDrawListEllipse(list, center, PrVec2{8, 8}, 0, PrDrawPaint{.color = kMid});
                        check(list, PrVec2{center.x - 4, center.y + 0.5f}, 8, kBackground, 1.8f);
                        ink = kMid;
                    } else if (t.state == TaskState::InProgress) {
                        circleStroke(list, center, 7.5f, kLine, 2);
                        prDrawListSector(list, center, 6.5f, 8.5f, -kPi / 2, kPi * 1.2f,
                            PrDrawPaint{.color = kBright});
                        ink = kBright;
                    } else {
                        static const float dashes[] = {2.2f, 2.2f};
                        circleStroke(list, center, 7.5f, kMid, 1.4f, dashes, 2);
                        ink = rgb(0xc8c8c8);
                    }
                    drawText(list, fonts, fonts.regular, 14, t.text, PrVec2{c.left + 44, cy + 5}, ink);
                }
            }
            return h + 12;
        }
        case EntryKind::Status: {
            if (list != nullptr) {
                // A small cluster of orange dots.
                const PrVec2 dots[] = {{0, 0}, {5, -3}, {5, 3}, {10, 0}};
                for (PrVec2 d : dots)
                    prDrawListEllipse(list, PrVec2{c.left + 3 + d.x, top + 10 + d.y}, PrVec2{1.8f, 1.8f}, 0,
                        PrDrawPaint{.color = kOrange});
                drawText(list, fonts, fonts.regular, 13, e.text, PrVec2{c.left + 22, top + 14.5f}, kDim);
            }
            return 20 + kGap;
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

    // Tab strip.
    const float stripH = 44;
    view.tabRects.assign(model.tabs.size(), PrRect{});
    float x = 12;
    for (size_t i = 0; i < model.tabs.size(); ++i) {
        bool active = i == model.active;
        std::string number = std::to_string(i + 1);
        const std::string& name = model.tabs[i].who.name;
        float numberW = measure(fonts, fonts.regular, 12, number);
        float nameW = measure(fonts, active ? fonts.semibold : fonts.regular, 13, name);
        PrRect r{x, 8, x + 12 + numberW + 8 + nameW + 14, stripH - 6};
        view.tabRects[i] = r;
        if (active)
            fillRounded(list, r, 8, kBubble);
        drawText(list, fonts, fonts.regular, 12, number, PrVec2{r.left + 12, r.top + 19.5f}, active ? kMid : kDim);
        drawText(list, fonts, active ? fonts.semibold : fonts.regular, 13, name,
            PrVec2{r.left + 12 + numberW + 8, r.top + 19.5f}, active ? kBright : kMid);
        x = r.right + 6;
    }
    prDrawListRect(list, PrRect{0, stripH, extent.x, stripH + 1}, PrDrawPaint{.color = kLine});

    // The column that the transcript and prompt share.
    float columnW = std::min(760.0f, extent.x - 48);
    Column c{(extent.x - columnW) / 2, (extent.x + columnW) / 2};

    // Prompt box and the row beneath it, pinned to the bottom.
    const Tab& tab = activeTab(model);
    float belowH = 30;
    float boxH = 58;
    PrRect box{c.left, extent.y - belowH - boxH - 8, c.right, extent.y - belowH - 8};
    fillRounded(list, box, 18, kPromptFill);
    strokeRounded(list, box, 18, kPromptBorder, 1);
    float mid = (box.top + box.bottom) / 2;
    line(list, PrVec2{box.left + 18, mid}, PrVec2{box.left + 30, mid}, kMid, 1.6f);
    line(list, PrVec2{box.left + 24, mid - 6}, PrVec2{box.left + 24, mid + 6}, kMid, 1.6f);
    float textX = box.left + 46;
    float baseline = mid + 5.5f;
    if (tab.prompt.empty()) {
        drawText(list, fonts, fonts.regular, kBodySize, "\xE2\x8F\x8E to send, \xE2\x8C\x98\xE2\x8F\x8E to queue for later",
            PrVec2{textX, baseline}, kDim);
        if (view.caretOn)
            prDrawListRect(list, PrRect{textX - 1, mid - 10, textX + 0.5f, mid + 10}, PrDrawPaint{.color = kInk});
    } else {
        drawText(list, fonts, fonts.regular, kBodySize, tab.prompt, PrVec2{textX, baseline}, kInk);
        float caretX = textX + measure(fonts, fonts.regular, kBodySize, tab.prompt.substr(0, tab.caret));
        if (view.caretOn)
            prDrawListRect(list, PrRect{caretX, mid - 10, caretX + 1.5f, mid + 10}, PrDrawPaint{.color = kInk});
    }
    // Stop button and microphone on the right.
    PrRect stop{box.right - 46, mid - 14, box.right - 18, mid + 14};
    fillRounded(list, stop, 8, kBright);
    fillRounded(list, PrRect{stop.left + 9, stop.top + 9, stop.right - 9, stop.bottom - 9}, 2, kPromptFill);
    float micX = box.right - 70;
    PrDrawStroke micStroke = prDrawStrokeDefault();
    micStroke.shape.width = 1.6f;
    prDrawListCapsule(list, PrVec2{micX, mid - 5}, PrVec2{micX, mid + 1}, 3.5f,
        PrDrawPaint{.color = kMid, .stroke = &micStroke});
    prDrawListSector(list, PrVec2{micX, mid + 1}, 6, 7.6f, 0, kPi, PrDrawPaint{.color = kMid});
    line(list, PrVec2{micX, mid + 7.6f}, PrVec2{micX, mid + 10}, kMid, 1.6f);

    // Model, effort, mode, ring: right-aligned under the box.
    float rowBase = extent.y - belowH / 2 + 2;
    std::string model_ = tab.who.model.empty() ? std::string("model") : tab.who.model;
    float wModel = measure(fonts, fonts.regular, 12, model_);
    float wHigh = measure(fonts, fonts.regular, 12, "High");
    float wAuto = measure(fonts, fonts.regular, 12, "Auto");
    float rx = c.right - 14 - 8 - wAuto - 12 - wHigh - 6 - wModel;
    rx += drawText(list, fonts, fonts.regular, 12, model_, PrVec2{rx, rowBase}, kInk) + 6;
    rx += drawText(list, fonts, fonts.regular, 12, "High", PrVec2{rx, rowBase}, kDim) + 12;
    rx += drawText(list, fonts, fonts.regular, 12, "Auto", PrVec2{rx, rowBase}, kMid) + 8;
    circleStroke(list, PrVec2{rx + 6, rowBase - 4}, 5, kMid, 1.4f);

    // Transcript: everything between the strip and the box, bottom-anchored, scrolled, clipped.
    PrRect area{0, stripH + 1, extent.x, box.top - 10};
    bool newestFound = false;
    size_t newestPerson = tab.transcript.size();
    for (size_t i = tab.transcript.size(); i-- > 0 && !newestFound;) {
        if (tab.transcript[i].kind == EntryKind::Person && !tab.transcript[i].queued) {
            newestPerson = i;
            newestFound = true;
        }
    }
    float total = 16;
    for (size_t i = 0; i < tab.transcript.size(); ++i)
        total += entry(nullptr, fonts, tab.transcript[i], i == newestPerson, c, 0);
    float areaH = area.bottom - area.top;
    view.maxScroll = std::max(0.0f, total - areaH);
    float& scroll = view.scroll[model.active];
    scroll = std::clamp(scroll, 0.0f, view.maxScroll);
    float y = total > areaH ? area.bottom - total + scroll + 16 : area.top + 16;
    prDrawListPushClipRect(list, area, PrDrawEdges_Antialiased);
    if (tab.transcript.empty()) {
        std::string hint = "Say something to " + tab.who.name;
        float w = measure(fonts, fonts.regular, 14, hint);
        drawText(list, fonts, fonts.regular, 14, hint, PrVec2{(extent.x - w) / 2, (area.top + area.bottom) / 2}, kDim);
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
