// Every size and colour the window draws with, in one place. Sizes are in points (logical px), at a text scale of 1.
//
// Measured from the owner's reference capture (the Claude desktop app, fullscreen on a 2560x1440-point display,
// captured at 2000x1125, so 1 capture px = 1.28 pt): cap heights of 15.5 capture px for body text, 13.5 for the model
// label and 14.5 for the title row, against SF Pro's cap height of 0.705 em. Spacing is written as a multiple of the
// body size so that changing kBodySize keeps the proportions. Colours are sampled from the same capture.

#pragma once

#include "pr/app_input.h"
#include "pr/color.h"

namespace nexus::style {

// Type sizes.
constexpr float kBodySize = 28;        // transcript, bubbles, prompt, dim notices
constexpr float kTabSize = 26;         // tab labels (the reference's title row)
constexpr float kLabelSize = 24;       // the notice when there are no friends
constexpr float kMetaSize = 21;        // "queued" (not in the reference; one step below the label)
constexpr float kTerminalSize = 21;    // a terminal tab's monospaced cells (not in the reference; a guess, at 0.75 of
                                       // the body size, where Prax's terminal app sets 13 px in a smaller window)

// Spacing, as multiples of the body size.
constexpr float kU = kBodySize;
constexpr float kLinePitch = 1.42f * kU;      // 31 capture px between body lines
constexpr float kGap = 2.4f * kU;             // between transcript entries
constexpr float kStripHeight = 3.1f * kU;     // the tab row, where the reference has its title bar
constexpr float kBubblePadX = 1.07f * kU;
constexpr float kBubblePadY = 0.75f * kU;
constexpr float kBubbleRadius = 0.9f * kU;
constexpr float kBubbleMaxPercent = 83;       // of the column
constexpr float kPromptHeight = 2.9f * kU;    // the prompt box with one line of text
constexpr float kPromptRadius = 0.72f * kU;
constexpr float kPromptPadX = 0.8f * kU;      // the box's sides to the plus and to the text's end
constexpr float kPromptGap = 0.72f * kU;      // the plus to the text field
constexpr float kPlusSide = 1.3f * kU;        // the plus icon's box; the kit draws its glyph inset in it
constexpr float kPromptMaxLines = 8;          // the prompt grows to this many lines, then scrolls
constexpr float kColumnInset = 0.6f * kU;     // above the first transcript entry

// The person's text scale, set from the keyboard (view.cpp kTextKeys) for every tab at once. It multiplies every size
// above, so all text, spacing and radii scale together, as changing kBodySize would. It moves in steps of
// kTextScaleStep between kTextScaleMin and kTextScaleMax.
constexpr float kTextScaleStep = 0.1f;
constexpr float kTextScaleMin = 0.5f;
constexpr float kTextScaleMax = 2.0f;
constexpr int kTextStepsDown = int((1 - kTextScaleMin) / kTextScaleStep + 0.5f);   // steps from 1 down to the minimum
constexpr int kTextStepsUp = int((kTextScaleMax - 1) / kTextScaleStep + 0.5f);     // steps from 1 up to the maximum

// The text scale after `steps` steps from 1 (negative is smaller).
constexpr float textScale(int steps) {
    return 1 + float(steps) * kTextScaleStep;
}

// Widths, as percentages of the window width.
constexpr float kColumnPercent = 80;          // transcript (the reference's 54.1% widened at the owner's ask)
constexpr float kPromptPercent = 82;          // prompt box, a little wider than the column, as in the reference

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
constexpr PrColor kNotice = rgb(0x9e9e9e);     // dim notices
constexpr PrColor kFaint = rgb(0x8c8c8c);      // faint text: "queued", and the notice when there are no friends
constexpr PrColor kMeta = rgb(0x707070);
constexpr PrColor kPromptFill = rgb(0x303030);
constexpr PrColor kPromptBorder = rgb(0x3a3a3a);
constexpr PrColor kSelection = rgb(0x3d5a80);  // selected prompt text (not in the reference)

// The queued bubble's fill and text, as alphas of kBubble and kInk.
constexpr float kQueuedFill = 0.5f;
constexpr float kQueuedInk = 0.55f;

// The queued bubble's fill: kBubble at kQueuedFill over kBackground, mixed here so that the bubble stays opaque in the
// see-through overlay (only the window background and the tab strip let what is behind show through).
constexpr PrColor kQueuedBubble = PrColor{kBackground.r + (kBubble.r - kBackground.r) * kQueuedFill,
    kBackground.g + (kBubble.g - kBackground.g) * kQueuedFill, kBackground.b + (kBubble.b - kBackground.b) * kQueuedFill,
    1};

// The overlay window (macOS, the default; --windowed gives a normal opaque window instead): borderless, over the whole
// screen that has the pointer (menu bar included), above other apps and their fullscreen Spaces, shown and hidden by a
// global hotkey, in the manner of iTerm2's hotkey window.
constexpr float kOverlayOpacity = 0.50f;    // alpha of the window background; what is behind shows through, dimmed
constexpr float kOverlayStripFill = 0.3f;     // alpha of the tab strip's fill over that background
constexpr bool kOverlayBlur = false;          // true blurs what is behind (Prax's Blur backdrop); off: plain alpha
constexpr bool kOverlayAccessory = true;      // no Dock icon or app switcher entry while the overlay runs
// The hotkey, by key position: Command-backtick (the key left of 1, which also types the tilde; macOS's virtual key
// 0x32). Command-backtick is macOS's "move focus to next window" key, which this registration takes over while nexus
// runs.
constexpr PrAppHotkey kOverlayHotkey{.code = PrAppKeyCode_Backquote, .modifiers = PrAppModifierFlag_Super};

} // namespace nexus::style
