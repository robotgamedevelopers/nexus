// Unit tests of the window's pure logic: the event filter (src/input.h) and the friend list's parsing (src/friends.h).
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

constexpr PrAppModifierFlags kShift = PrAppModifierFlag_Shift;
constexpr PrAppModifierFlags kCmd = PrAppModifierFlag_Super;
constexpr PrAppModifierFlags kCaps = PrAppModifierFlag_CapsLock;

void capsLockIsStripped() {
    CHECK(withoutCapsLock(PrAppModifierFlags(kShift | kCaps)) == kShift);
    CHECK(withoutCapsLock(PrAppModifierFlags(kCmd | kCaps)) == kCmd);
    CHECK(withoutCapsLock(kCaps) == 0);
    CHECK(withoutCapsLock(kCmd) == kCmd);
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
