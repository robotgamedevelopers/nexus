// Record the actual FrameHost picker callbacks, local preview and SetAccount link effects for AccountPickerTrace.tla.
#include "test_link_port.h"

#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace nexus;

template<class Choice>
void options(std::ostream& out, const std::vector<Choice>& choices) {
    out << "<<";
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (i) out << ", ";
        out << std::quoted(choices[i].name);
    }
    out << ">>";
}

struct Trace {
    std::ostream& out;
    bool faulty;
    Model model{{}, true};
    View view;
    test::FakeLink link;
    FrameHost host;
    std::vector<sync::AccountChoice> roster;
    std::string current, account;
    bool offersAutomatic = false;
    std::size_t tab = 1;
    sync::SessionId session = 1;
    std::size_t seen = 0;

    Trace(std::ostream& stream, bool fault) : out(stream), faulty(fault) {
        host.model = view.model = &model;
        host.view = &view;
        host.strip = sync::FriendStrip({}, "one", {});
        host.connect(&link, "fixture");
        out << "---- MODULE AccountPickerTraceData ----\nEXTENDS Integers\nTrace == <<\n";
        record("init");
    }

    void record(const std::string& op, int direction = 0, const std::string& id = "", bool ready = true) {
        if (op != "init") out << ",\n";
        out << "[op |-> " << std::quoted(op) << ", tab |-> " << tab << ", session |-> " << session
            << ", current |-> " << std::quoted(current) << ", account |-> " << std::quoted(account)
            << ", direction |-> " << direction
            << ", id |-> " << std::quoted(id) << ", ready |-> " << (ready ? "TRUE" : "FALSE") << ", options |-> ";
        auto observed = roster;
        if (offersAutomatic) observed.insert(observed.begin(), sync::AccountChoice{sync::kAutoAccountTarget});
        options(out, observed);
        const auto& picker = view.accountPicker;
        out << ", picker |-> [tab |-> " << (picker ? picker->tab : 0)
            << ", session |-> " << (picker ? picker->session : 0)
            << ", current |-> " << std::quoted(picker ? picker->current : "")
            << ", account |-> " << std::quoted(picker ? picker->account : "")
            << ", highlight |-> " << std::quoted(picker ? picker->options.at(picker->highlighted).name : "")
            << ", options |-> ";
        options(out, picker ? picker->options : std::vector<PickerOption>{});
        sync::SessionId target = 0;
        std::string account;
        for (; seen < link.sent.size(); ++seen) {
            const auto& action = link.sent[seen];
            if (action.kind == sync::ActionKind::SetAccount) {
                if (target) throw std::runtime_error("one picker event sent two account switches");
                target = action.session;
                account = action.text;
            }
        }
        out << "], request |-> [session |-> " << target << ", account |-> " << std::quoted(account) << "]]";
    }

    // Supply server observations, independently of the state that the picker kept from the previous snapshot.
    void publish(std::string names, const std::string& active, std::size_t activeTab = 1,
                 sync::SessionId firstSession = 1, bool modal = false, bool capable = false, bool automatic = false) {
        tab = activeTab;
        session = tab == 1 ? firstSession : 2;
        account = active;
        offersAutomatic = capable && names.size() > 1;
        current = offersAutomatic && automatic ? sync::kAutoAccountTarget : active;
        roster.clear();
        for (char name : names) roster.push_back({std::string(1, name)});
        if (model.tab_count() > tab) model.select_tab(tab);
        std::vector<sync::SessionRef> sessions;
        for (std::size_t i = 1; i <= 2; ++i) {
            auto entry = std::make_shared<sync::SessionView>();
            entry->id = i == 1 ? firstSession : 2;
            entry->info = {i == 1 ? "one" : "two", "codex", active, "", ""};
            entry->info.autoAccount = automatic;
            sessions.push_back(entry);
        }
        if (sessions.front()->id > sessions.back()->id) std::swap(sessions.front(), sessions.back());
        link.state.phase = sync::LinkPhase::Connected;
        link.state.sessions = sync::Sessions(std::move(sessions));
        sync::ServerFriend one{"one", roster}, two{"two", roster};
        one.canAutoAccount = two.canAutoAccount = capable;
        link.state.sessions.setFriends({one, two});
        link.state.sessions.setNextAction(link.sent.size() + 1);
        if (modal) {
            sync::SecretRequest ask;
            ask.id = 1;
            ask.account = "a";
            ask.what = "oauth";
            link.state.sessions.setRequests({ask});
        }
        link.changed = true;
        host.pollLink();
        if (model.tab(model.active()).id != tab) throw std::runtime_error("fixture tab was not selected");
        record(modal ? "modal" : "refresh");
    }

    void command(const std::string& key, bool ready = true) {
        const bool wasOpen = view.accountPicker.has_value();
        if (faulty && key == "cancel" && wasOpen) {
            const auto& p = *view.accountPicker;
            sync::Action action;
            action.kind = sync::ActionKind::SetAccount;
            action.session = p.session;
            action.text = p.options.at(p.highlighted).name;
            link.send(std::move(action));
        }
        view.pickerCommand(key);
        if (key == "cancel" || key == "confirm") record(key);
        else record(wasOpen ? "step" : "open", key == "next" ? 1 : key == "previous" ? -1 : 0, "", ready);
    }

    void row(const std::string& id) {
        view.chooseAccount(id);
        record("row", 0, id);
    }

    void run() {
        publish("ab", "a");
        command("open"); command("next"); command("cancel"); command("confirm");
        command("previous"); publish("ba", "a"); command("confirm");
        command("next", false); // the account switch awaits the server's acknowledgment
        publish("ba", "b");
        command("open"); row("a"); publish("ab", "a");
        command("open"); command("confirm"); // already using this account
        command("next"); publish("a", "a"); row("b"); // rendered choice was removed
        publish("ab", ""); command("previous"); command("next"); command("previous"); row("");
        command("next"); publish("ab", "b"); command("confirm"); // current account changed elsewhere
        command("next"); publish("ba", "b", 2); row("a");        // switched tabs while previewing
        command("next"); publish("ab", "b");
        publish("ab", "a", 1, 1, false, true);
        command("previous"); command("confirm"); // enter automatic mode
        publish("ab", "a", 1, 1, false, true, true);
        command("open"); publish("ab", "b", 1, 1, false, true, true); // automatic payer rotated
        command("open"); row("b"); // explicitly pin the current payer
        publish("ab", "b");
        command("next"); publish("ab", "b", 1, 3); row("a");    // the shown session disappeared
        command("next", false);                                  // mirror still binds the old session
        publish("ba", "b", 2, 3);
        command("open"); publish("ba", "b", 2, 3, true); command("open", false);
        publish("ba", "b", 2, 3); command("previous");
        link.state.phase = sync::LinkPhase::Failed;
        link.changed = true;
        host.pollLink();
        record("lost");
        command("confirm"); command("next", false);
        publish("", "", 2, 3); command("open");
        out << "\n>>\n====\n";
    }
};
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && (argc != 3 || std::string(argv[2]) != "--faulty-cancel")) return 2;
    std::ofstream out(argv[1]);
    Trace trace(out, argc == 3);
    trace.run();
    return out ? 0 : 1;
}
