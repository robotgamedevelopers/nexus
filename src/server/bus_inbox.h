#pragma once

#include "server/bus_attachment.h"
#include "server/bus_grants.h"
#include "server/bus_listener.h"
#include "sync/hub.h"

namespace nexus::server {

// Receives only on this machine's assigned inboxes. The window sees the durable transcript entry.
class BusInbox {
public:
    BusInbox(const std::string& home, const std::string& self, std::string machine, std::string host);
    void refresh(const std::string& driversFile, const accounts::Registry* registry, std::int64_t now);
    BusAccept receive(sync::Hub& hub, const std::string& recipient, const BusMessage& message);
    BusListeners listeners;

private:
    struct Prepared {
        BusBody body;
        std::string problem;
    };
    struct Writing {
        std::string workspace, id;
        std::future<Prepared> result;
    };
    BusGrants grants_;
    std::string home_, machine_, host_, grantError_;
    std::map<std::string, std::string> workspaces_, errors_;
    std::map<std::string, Writing> writing_;
    BusAccept refuse(const std::string& recipient, const std::string& why);
};

} // namespace nexus::server
