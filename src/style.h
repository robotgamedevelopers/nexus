// Every size and colour the window draws with, in one place. Sizes are in points (logical px).
//
// Measured from the owner's reference capture (the Claude desktop app, fullscreen on a 2560x1440-point display,
// captured at 2000x1125, so 1 capture px = 1.28 pt): cap heights of 15.5 capture px for body text, 13.5 for the model
// label and 14.5 for the title row, against SF Pro's cap height of 0.705 em. Spacing is written as a multiple of the
// body size so that changing kBodySize keeps the proportions. Colours are sampled from the same capture.

#pragma once

#include "pr/color.h"

namespace nexus::style {

// Type sizes.
constexpr float kBodySize = 28;        // transcript, bubbles, prompt, dim notices, status line
constexpr float kTabSize = 26;         // tab labels (the reference's title row)
constexpr float kLabelSize = 24;       // model / effort / mode label under the prompt
constexpr float kMetaSize = 21;        // "Just now", "queued" (not in the reference; one step below the label)

// Spacing, as multiples of the body size.
constexpr float kU = kBodySize;
constexpr float kLinePitch = 1.42f * kU;      // 31 capture px between body lines
constexpr float kGap = 2.4f * kU;             // between transcript entries
constexpr float kStripHeight = 3.1f * kU;     // the tab row, where the reference has its title bar
constexpr float kBubblePadX = 1.07f * kU;
constexpr float kBubblePadY = 0.75f * kU;
constexpr float kBubbleRadius = 0.9f * kU;
constexpr float kBubbleMaxFraction = 0.83f;   // of the column
constexpr float kPromptHeight = 2.9f * kU;
constexpr float kPromptRadius = 0.72f * kU;
constexpr float kPromptBottom = 2.75f * kU;   // window bottom to the prompt box's bottom
constexpr float kLabelBaseline = 0.9f * kU;   // window bottom to the label row's baseline
constexpr float kCardRow = 1.75f * kU;
constexpr float kCardRadius = 0.6f * kU;
constexpr float kIcon = 0.36f * kU;           // radius of task icons

// Widths, as fractions of the window width.
constexpr float kColumnFraction = 0.541f;     // transcript: x 462..1544 of 2000
constexpr float kPromptFraction = 0.5675f;    // prompt box: x 432..1567 of 2000

// Colours.
constexpr PrColor rgb(unsigned hex) {
    return PrColor{float((hex >> 16) & 0xFF) / 255.0f, float((hex >> 8) & 0xFF) / 255.0f, float(hex & 0xFF) / 255.0f,
        1};
}
constexpr PrColor kBackground = rgb(0x181818);
constexpr PrColor kStrip = rgb(0x1b1b1b);
constexpr PrColor kStripLine = rgb(0x373737);
constexpr PrColor kBubble = rgb(0x252525);
constexpr PrColor kInk = rgb(0xf3f3f3);
constexpr PrColor kNotice = rgb(0x9e9e9e);     // dim notices, status text, "High"
constexpr PrColor kPlaceholder = rgb(0x8c8c8c);
constexpr PrColor kMeta = rgb(0x707070);
constexpr PrColor kCardLine = rgb(0x333333);
constexpr PrColor kPromptFill = rgb(0x303030);
constexpr PrColor kPromptBorder = rgb(0x3a3a3a);
constexpr PrColor kStop = rgb(0xf1f1f1);
constexpr PrColor kOrange = rgb(0xcc7f60);
constexpr PrColor kRing = rgb(0x404c55);

} // namespace nexus::style
