// Unit tests of the window's pure logic: the key bindings (src/input.h) and the friend list's parsing (src/friends.h).
// No OS, no clock, no window.

#include <cstdio>
#include <string>

#include "friends.h"
#include "input.h"

namespace {

using namespace nexus;

int failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

constexpr PrAppModifierFlags kAlt = PrAppModifierFlag_Alt;
constexpr PrAppModifierFlags kShift = PrAppModifierFlag_Shift;
constexpr PrAppModifierFlags kCmd = PrAppModifierFlag_Super;
constexpr PrAppModifierFlags kCaps = PrAppModifierFlag_CapsLock;

Filtered keyDown(InputState& s, PrAppKey key, PrAppKeyCode code, PrAppModifierFlags m) {
    return filterEvent(s, PrAppEventKind_KeyDown, key, code, m);
}

Filtered textEvent(InputState& s) {
    return filterEvent(s, PrAppEventKind_Text, PrAppKey_Unknown, PrAppKeyCode_Unidentified, 0);
}

void optionDigitsSelectTabs() {
    for (int n = 0; n < 9; ++n) {
        InputState s;
        Filtered f = keyDown(s, PrAppKey_Unknown, PrAppKeyCode(PrAppKeyCode_Digit1 + n), kAlt);
        CHECK(f.action == Action::SelectTab);
        CHECK(f.tab == std::size_t(n));
        CHECK(f.consume);
    }
    // Caps Lock does not stop it.
    InputState s;
    Filtered f = keyDown(s, PrAppKey_Unknown, PrAppKeyCode_Digit3, PrAppModifierFlags(kAlt | kCaps));
    CHECK(f.action == Action::SelectTab && f.tab == 2);
}

void otherDigitsDoNot() {
    InputState s;
    CHECK(keyDown(s, '0', PrAppKeyCode_Digit0, kAlt).action == Action::None);    // Option-0: no tab 0
    CHECK(keyDown(s, '1', PrAppKeyCode_Digit1, 0).action == Action::None);       // plain 1 types
    CHECK(!keyDown(s, '1', PrAppKeyCode_Digit1, 0).consume);
    CHECK(keyDown(s, '1', PrAppKeyCode_Digit1, PrAppModifierFlags(kAlt | kShift)).action == Action::None);
    CHECK(keyDown(s, '1', PrAppKeyCode_Digit1, kCmd).action == Action::None);
    CHECK(keyDown(s, PrAppKey_Unknown, PrAppKeyCode_Numpad1, kAlt).action == Action::None);
}

void optionDigitSwallowsItsCharacter() {
    InputState s;
    keyDown(s, PrAppKey_Unknown, PrAppKeyCode_Digit2, kAlt);
    CHECK(textEvent(s).consume);          // the character macOS types for Option-2
    CHECK(!textEvent(s).consume);         // only that one
}

void aNewKeyEndsTheSwallow() {
    InputState s;
    keyDown(s, PrAppKey_Unknown, PrAppKeyCode_Digit2, kAlt);
    keyDown(s, 'A', PrAppKeyCode_KeyA, 0);  // a chord that typed nothing, then a plain key
    CHECK(!textEvent(s).consume);
}

void capsLockIsStripped() {
    InputState s;
    Filtered f = keyDown(s, PrAppKey_Left, PrAppKeyCode_ArrowLeft, PrAppModifierFlags(kShift | kCaps));
    CHECK(!f.consume);
    CHECK(f.modifiers == kShift);
    f = filterEvent(s, PrAppEventKind_PointerDown, PrAppKey_Unknown, PrAppKeyCode_Unidentified, kCaps);
    CHECK(f.modifiers == 0);
}

void parsesEnabledFriendsInOrder() {
    FriendList list = parseFriendList(R"({"result":{"verb":"friend list","status":"ok","exit":0},"items":[
        {"kind":"friend","fields":{"name":"alex","model":"","enabled":true}},
        {"kind":"friend","fields":{"name":"off","model":"m","enabled":false}},
        {"kind":"friend","fields":{"name":"emma","model":"gemini \"2\"","enabled":true}},
        {"kind":"friend","fields":{"name":"nameless","model":"m"}}]})");
    CHECK(list.error.empty());
    CHECK(list.friends.size() == 2);
    CHECK(list.friends.size() == 2 && list.friends[0].name == "alex" && list.friends[0].model.empty());
    CHECK(list.friends.size() == 2 && list.friends[1].name == "emma" && list.friends[1].model == "gemini \"2\"");
}

void reportsWhatIsWrong() {
    CHECK(!parseFriendList("not json").error.empty());
    CHECK(parseFriendList("not json").friends.empty());
    CHECK(!parseFriendList(R"({"result":{"status":"refused"},"items":[]})").error.empty());
    CHECK(!parseFriendList(R"({"result":{"status":"ok"}})").error.empty());
    FriendList none = parseFriendList(R"({"result":{"status":"ok"},"items":[]})");
    CHECK(none.friends.empty());
    CHECK(none.error.find("no enabled friends") != std::string::npos);
}

} // namespace

int main() {
    optionDigitsSelectTabs();
    otherDigitsDoNot();
    optionDigitSwallowsItsCharacter();
    aNewKeyEndsTheSwallow();
    capsLockIsStripped();
    parsesEnabledFriendsInOrder();
    reportsWhatIsWrong();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    std::printf("window unit tests passed\n");
    return 0;
}
