// The window's frame's view of its link to nexus-server (frame_host.h LinkPort) over the link's thread
// (link/client.h): poll copies the latest snapshot under the link's lock, and send queues an action and nudges the
// thread through a non-blocking pipe. Neither waits on the network.

#pragma once

#include <memory>
#include <utility>

#include "frame_host.h"
#include "link/client.h"

namespace nexus::link {

class ClientPort final : public LinkPort {
public:
    explicit ClientPort(ClientLink::Options options) : link_(std::make_unique<ClientLink>(std::move(options))) {}

    bool poll(LinkState* out) override {
        ClientLink::Snapshot s;
        if (!link_->poll(&s))
            return false;
        out->phase = s.phase;
        out->problem = std::move(s.problem);
        out->heardMsAgo = s.heardMsAgo;
        out->sessions = std::move(s.sessions);
        return true;
    }

    void send(sync::Action action) override { link_->send(std::move(action)); }

    // Ends the link: mosh's shutdown, so the server ends this session (up to two seconds, at quit).
    void close() { link_.reset(); }

private:
    std::unique_ptr<ClientLink> link_;
};

} // namespace nexus::link
