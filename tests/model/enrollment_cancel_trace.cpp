// Exercise the production desk with every provider, ownership combination, and cancellable/terminal phase.
#include "sync/signin.h"
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string fault(argv[1]);
    std::ofstream out(argv[2]);
    if (!out) return 2;
    const auto bit = [](bool b) { return b ? "TRUE" : "FALSE"; };
    using namespace nexus::sync;
    out << "---- MODULE EnrollmentCancelTraceData ----\nTrace == <<\n";
    bool first = true;
    for (const auto* provider : {"claude", "codex", "antigravity", "grok", "muse"}) {
        for (unsigned phase = 0; phase < 9; ++phase) for (unsigned flags = 0; flags < 8; ++flags) {
            if (phase == 4 && std::string(provider) != "claude") continue; // local helper receipts have no redirect callback
            SignInDesk desk;
            desk.need(1, "account", provider);
            if (phase > 0) desk.claim(1, "window", 0);
            if (phase >= 2) {
                const auto url = std::string(provider) == "claude" ?
                    "https://claude.ai/a?redirect_uri=http://localhost:3333/callback&state=fixture" :
                    "http://127.0.0.1:3333/nexus-signin/" + std::string(64, 'a');
                desk.url("account", 1, SecretValue(url));
            }
            if (phase >= 3) desk.opened(1, "window");
            if (phase == 4) desk.callback(1, "window", SecretValue("code=fixture&state=fixture"));
            if (phase == 5) { SignInEffects effects; desk.printed("account", &effects); }
            if (phase == 6) desk.exited("account", 1, "fixture");
            if (phase >= 7) desk.cancel(1, "window");
            if (phase == 8) desk.exited("account", 1, "stopped");
            const auto before = *desk.stateOf(1);
            const bool matching = flags & 1, owner = flags & 2, nonempty = flags & 4;
            const auto id = (matching || fault == "id") ? 1 : 2;
            const std::string token = !nonempty ? "" : (owner || fault == "owner") ? "window" : "other";
            const auto effects = desk.cancel(id, token);
            const auto after = *desk.stateOf(1);
            if (after == SignInState::Cancelling) {
                if (desk.deadline() || !desk.expire(kSignInDeadlineMs * 2).needAgain.empty()) return 4;
                if (desk.stateOf(1) != SignInState::Cancelling || desk.offers().front().opener != "window" ||
                        !desk.offers().front().url.empty()) return 4;
                if (fault != "drain") desk.exited("account", 1, "stopped");
            }
            out << (first ? "" : ",\n") << "[before |-> \"" << signInStateName(before) << "\", after |-> \""
                << signInStateName(after) << "\", drained |-> \"" << signInStateName(*desk.stateOf(1)) << "\", matching |-> " << bit(matching) << ", owner |-> "
                << bit(owner) << ", nonempty |-> " << bit(nonempty) << ", stopped |-> " << bit(!effects.stop.empty()) << "]";
            first = false;
            // Cancellation is idempotent, never prints credentials and never ends a different account.
            if (effects.stop.size() > 1 || (!effects.stop.empty() && effects.stop.front() != "account") ||
                    !effects.replays.empty() || !desk.cancel(id, token).stop.empty()) return 3;
        }
    }
    out << "\n>>\n====\n";
    return out ? 0 : 2;
}
