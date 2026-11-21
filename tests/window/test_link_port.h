// In-memory window link for driving the production frame host with synthetic server snapshots.
#pragma once

#include "frame_host.h"

namespace nexus::test {

struct FakeLink final : LinkPort {
    LinkState state;
    bool changed = false;
    std::vector<sync::Action> sent;

    bool poll(LinkState* out) override {
        if (!changed) {
            return false;
        }
        *out = state;
        changed = false;
        return true;
    }
    std::uint64_t send(sync::Action action) override { sent.push_back(std::move(action)); return sent.size(); }

    void friends(const std::vector<std::string>& names) {
        LinkState s;
        s.phase = sync::LinkPhase::Connected;
        std::vector<sync::ServerFriend> list;
        for (const std::string& n : names) {
            list.push_back(sync::ServerFriend{n});
        }
        s.sessions.setFriends(std::move(list));
        state = std::move(s);
        changed = true;
    }
};

} // namespace nexus::test
