// One supervised nova-bus hook per configured inbox. Only the hook's own process group
// can deliver on its private Unix socket. A hook acknowledges after flushed() replies.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <poll.h>
#include <string>
#include <vector>

namespace nexus::server {

struct BusMessage {
    std::string id, from, replyTo, subject, body;
};

enum class BusAccept { Retry, Wait, Admit };

class BusListeners {
public:
    using Accept = std::function<BusAccept(const std::string&, const BusMessage&)>;
    BusListeners(std::string home, std::string self, std::function<void(const std::string&)> report);
    ~BusListeners();
    BusListeners(const BusListeners&) = delete;
    BusListeners& operator=(const BusListeners&) = delete;

    // The registry grants each enabled inbox access to one loopback Redis address.
    void configure(const std::map<std::string, std::string>& inboxAddresses);
    std::vector<pollfd> fds() const;
    int waitMs() const;
    // Called in Apply. Wait keeps the parsed message for the next pass. Admit requires
    // a durable receipt keyed by (inbox, message id), written before flushed().
    bool serve(const Accept& accept);
    // Called after the batch's durable flush. A refused or disconnected helper retries through nova-bus.
    void flushed();

private:
    struct Hook;
    std::string home_, self_;
    std::function<void(const std::string&)> report_;
    std::map<std::string, std::unique_ptr<Hook>> hooks_;
    std::vector<std::unique_ptr<Hook>> stopping_;
    void reapStopped();
};

// Mode of the server executable invoked by nova-bus hook --exec, with the escaped message line on stdin.
int busDeliver(const std::string& socketPath);

} // namespace nexus::server
