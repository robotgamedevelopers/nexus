// The test build's authenticator (CMake option NEXUS_TEST_AUTHENTICATOR, off by default and never in a release): it
// stands in for the system's so that the window can be run and screenshotted locked and unlocked on a machine where no
// one can touch the sensor. It answers each request with the word in the client home's `lock-test-answer` file
// (client_home.h): grant, fail, cancel or unavailable; with the file absent, empty or any other word it never answers,
// and the window stays at "waiting for you". The lock screen names it, so a test build is never mistaken for the app.

#include "client_lock_platform.h"

#include <cstdio>
#include <fstream>
#include <string>

#include "client_home.h"

namespace nexus {

namespace {

class TestAuthenticator final : public LockAuthenticator {
public:
    std::string method() const override { return "the TEST BUILD's authenticator (no real check)"; }

    void request(std::uint64_t attempt, const PrSurface&, LockPost post) override {
        std::string home, why, word;
        if (clientHome(&home, &why)) {
            std::ifstream file(home + "/lock-test-answer");
            file >> word;
        }
        std::fprintf(stderr, "nexus: TEST BUILD: attempt %llu answered '%s' from lock-test-answer\n",
            static_cast<unsigned long long>(attempt), word.c_str());
        LockAnswer a;
        if (word == "grant") a = LockAnswer::Granted;
        else if (word == "fail") a = LockAnswer::Failed;
        else if (word == "cancel") a = LockAnswer::Cancelled;
        else if (word == "unavailable") a = LockAnswer::Unavailable;
        else return;
        post({LockNotice::Kind::Answer, attempt, a, LockReason::None, a == LockAnswer::Granted ? "" : "test answer"});
    }

    void cancel() override {}
};

} // namespace

std::unique_ptr<LockAuthenticator> systemLockAuthenticator() {
    std::fprintf(stderr, "nexus: TEST BUILD: the lock's authenticator is a stand-in that checks nothing; never ship "
        "this build\n");
    return std::make_unique<TestAuthenticator>();
}

} // namespace nexus
