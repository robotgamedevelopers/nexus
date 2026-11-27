// A Starlark window description compiled into native key captures and ordered panel draws.
#pragma once

#include <map>
#include <optional>
#include <vector>

#include "account_menu.h"
#include "model.h"
#include "policy/host.h"
#include "style.h"
#include "pr/app_input.h"

namespace nexus {

enum class WindowCommand { ScrollTranscript, ClearComposer, ReplaceDraft, SelectTab, MoveTab, Zoom, OpenTerminal, CloseTerminal, Stop, OpenTab, CloseTab, InvokeService, AccountCommand, AccountPicker, AccountAction, ResolveLimit, AccountMenu };
struct Shortcut {
    PrAppKey key;
    PrAppModifierFlags modifiers;
    WindowCommand command;
    int argument;
    std::string type{};
};
enum class Panel { Board, Terminal, Spacer, Transcript, Prompt, Status, Account, LimitChoice, Native, TextArea, Label, ServiceReply };
struct TabType {
    std::string title, group, native, service;
    bool operator==(const TabType&) const = default;
};

struct AccountItem {
    std::string text, action;
    bool selected = false;
};

struct TabStyle {
    unsigned fill = 0x1b1b1b, divider = 0x686868, selectedFill = 0x86c5ff, selectedInk = 0xffffff, ink = 0x999999;
    float opacity = 1, selectedOpacity = 0.5f, selectedRadius = 8;
    float weight = 400, selectedWeight = 700, height = style::kStripHeight;
    float tabHeight = 1.75f * style::kTabSize, gap = 8, leading = 0.8f * style::kU;
    bool reserveWidth = true;
    bool operator==(const TabStyle&) const = default;
};

using MotionCurve = std::vector<std::pair<double, double>>;

struct PromptStyle {
    MotionCurve caretCurve;
    unsigned fill = 0x303030, border = 0x3a3a3a, ink = 0xf3f3f3, selection = 0x3d5a80;
    float width = style::kPromptPercent, radius = style::kPromptRadius, paddingX = style::kPromptPadX;
    float height = style::kPromptHeight, gap = style::kPromptGap, maxLines = style::kPromptMaxLines;
    unsigned caretInk = 0xff801f;
    float textPeak = 1, caretPeak = 3, caretAspect = 0.618034f;
    float caretGap = 0, caretOffsetY = 0;
    float caretHeight = style::kCursorHeight, caretRadius = style::kCursorRadius, caretSeconds = 1.5f;
    float caretHold = 0.1f, caretFallRatio = 0.5f;
    PrVec2 segmentSize(float scale) const {
        const float h = caretHeight * scale;
        return {h * caretAspect, h};
    }
    bool bold = false, showPlus = false;
    bool operator==(const PromptStyle&) const = default;
};

PrAppHotkey overlayBinding(policy::Host& host, std::string& why);

// The client lock's idle timeout in seconds (client_lock.h), from the client policy's `lock` event: {"idle_minutes": N},
// the shipped rule's from config `lock_idle_minutes`, 10 unless set. N is a whole number of minutes from 1 to 240; the
// lock cannot be turned off. Anything else is the default, with `why` saying what was wrong.
inline constexpr double kLockIdleMinutesDefault = 10;
inline constexpr double kLockIdleMinutesMost = 240;
double lockIdleSeconds(policy::Host& host, std::string& why);
// The same from the rule's answer (JSON), for the tests.
double lockIdleSecondsFrom(const std::string& answer, std::string& why);

struct Chirp {
    float startHz = 440, endHz = 440, durationMs = 50, gain = 0;
    bool synth = false;
};

struct AccountSurface {
    Chirp copyChirp{1600, 480, 32, 0.04f};
    Chirp tabChirp, confirmChirp, openChirp, closeChirp, dialogChirp, cancelChirp, readyChirp, holdChirp, discardChirp, sendChirp, stepChirp, startupChirp;
    std::optional<std::vector<AccountItem>> items;
    std::vector<std::string> tabTitles;
    TabStyle tabs;
    PromptStyle prompt;
    AccountMenuStyle accountMenu;   // the menu under an AI's tab (account_menu.h), from the policy's account_menu
    bool showTabs = true, reorderTabs = true, promptCommand = false;
};

struct MessageCardStyle {
    unsigned fill = 0xffffff;
    unsigned ink = 0;
    unsigned emailInk = 0x477fba;
    float textSize = 34;
    float radius = 20;
    float sideExtension = 0, verticalExtension = 10, selectorWidthEm = 16;
    float rowHeight = 1.45f, rowGap = 0.15f, headingGap = 0.8f;
    float emailWeight = 700, unselectedWeight = 400, selectedRadius = 8, selectedOpacity = 0.5f;
    unsigned selectedFill = 0x86c5ff, selectedInk = 0xffffff;
    bool operator==(const MessageCardStyle&) const = default;
};

struct TranscriptStyle {
    unsigned fill = 0x252525;
    unsigned ink = 0xf3f3f3;
    unsigned personInk = ink;
    unsigned busInk = 0x86c5ff;
    std::map<std::string, unsigned, std::less<>> messageInks;
    std::optional<unsigned> panelFill, linkInk, readPathInk, commandInk, codeInk;
    unsigned codeFill = 0x303030;
    float codePadding = 6, codeRadius = 7, codeSizeOffset = 2;
    bool codeBackground = true;
    unsigned switchEmailInk = 0x86c5ff;
    unsigned noticeInk = 0xffb4b4;
    unsigned statusInk = 0xd8d8d8;
    float radius = style::kBubbleRadius;
    float pendingOutlineWidth = 3, sentOutlineWidth = 0, pendingDashSeconds = 2;
    float pendingTextPulse = 0.10f, pendingTextSeconds = 2.8f;
    unsigned sentOutlineInk = 0xffffff;
    bool showNotices = true;
    std::vector<std::string> hiddenNoticeFragments;
    bool showsNotice(std::string_view text) const {
        if (!showNotices) return false;
        for (const auto& fragment : hiddenNoticeFragments)
            if (text.find(fragment) != text.npos) return false;
        return true;
    }
    bool showTools = true;
    std::map<std::string, std::string, std::less<>> toolPrompts;
    bool personBold = false;
    float glyphPeak = 1, glyphSeconds = 0.4f, glyphStagger = 0.055f;
    float personFlashPeak = 1, personFlashSeconds = 0.45f;
    float replyWidth = 100.0f / 1.5f;
    std::optional<unsigned> scannerInk;
    float highlightEnergy = 2.5f, highlightSeconds = 4, flashSeconds = 0.22f;
    MotionCurve scannerCurve{{0, 0}, {0.08, 0}, {0.5, 1}, {0.58, 1}, {1, 0}};
    float scannerPause = 0.1f, scannerEase = 1.f / 30, scannerFade = 0.17f;
    float scannerGap = 0.15f * style::kU, scannerOffset = 0;
    float scannerSeconds = 1.5f, scannerPeak = 3, scannerPulseFloor = 0.12f;
    float listItemGapLines = 0.5f;
    float listGap = 0.4f; // em between a list marker and its text
    bool markdown = true;
    bool conversationPanel = true;
    bool operator==(const TranscriptStyle&) const = default;
};

struct SignInAccount {
    std::string name, label, email, provider{}, aiName{};
    bool operator==(const SignInAccount&) const = default;
};

struct LimitOffer {
    uint64_t session = 0, request = 0;
    std::string owner{}, account{}, model{};
    bool waiting = false, connected = false;
    bool operator==(const LimitOffer&) const = default;
};

class WindowApplication {
public:
    void update(policy::Host& host, const char* tab, const std::string& type, bool promptOpen, bool hasStatus,
                std::size_t activeTab, std::size_t tabCount, const std::string& group = {}, SignInAccount account = {},
                bool pickerOpen = false, LimitOffer offer = {}, const std::string& ui = "{}");
    std::vector<Shortcut> shortcuts;
    std::vector<Panel> panels;
    std::map<std::string, TabType> types;
    std::vector<TabGroup> groups;
    std::optional<bool> closable;
    uint64_t layoutGeneration = 0;
    std::string label;
    std::string autoAccountLabel = "auto";
    std::string signInHeading = "Please sign in";
    bool centeredAccounts = false;
    bool signInOnSelect = false;
    AccountSurface surface;
    SignInAccount signInAccount;
    LimitOffer limitOffer;
    std::string limitAccountLabel, limitModelLabel;
    MessageCardStyle messageCard;
    TranscriptStyle transcriptStyle;
    float overlayOpacity = style::kOverlayOpacity;

private:
    std::string lastContext_;
    uint64_t generation_ = 0;
    std::optional<uint64_t> appliedGeneration_;
};

} // namespace nexus
