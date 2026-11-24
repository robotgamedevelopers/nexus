#include "server/bus_inbox.h"
#include "server/home.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace nexus::server {

using namespace std::chrono_literals;
namespace {
void reportBus(const std::string& reason) {
    std::fprintf(stderr, "nexus-server: message bus: %s\n", reason.c_str());
}
}

BusInbox::BusInbox(const std::string& home, const std::string& self, std::string machine, std::string host)
    : listeners(home, self, reportBus), home_(home), machine_(std::move(machine)), host_(std::move(host)) {}

void BusInbox::refresh(const std::string& driversFile, const accounts::Registry* registry, std::int64_t now) {
    std::ifstream file(driversFile);
    std::ostringstream text;
    text << file.rdbuf();
    const auto table = parseDriverTable(text.str(), home_);
    grants_.refresh(table, now);
    if (grantError_ != grants_.problem()) {
        grantError_ = grants_.problem();
        if (!grantError_.empty()) reportBus(grantError_);
    }
    workspaces_.clear();
    std::map<std::string, std::string> addresses;
    for (const auto& [inbox, address] : grants_.addresses()) {
        const auto* owner = registry ? registry->friendNamed(inbox) : nullptr;
        if (!owner) continue;
        if (owner->instances.empty()) {
            const auto driver = resolveDriver(table, inbox, driversFile);
            if (!driver.problem.empty()) continue;
            workspaces_[inbox] = driver.cwd;
        } else {
            const auto& location = owner->instances.front();
            if (!sameMachine(location.machine, machine_, host_)) continue;
            workspaces_[inbox] = location.dir;
        }
        addresses[inbox] = address;
    }
    listeners.configure(addresses);
    std::erase_if(errors_, [&](const auto& error) { return !addresses.contains(error.first); });
    // A revoked or moved inbox cannot use a file prepared for its previous grant.
    std::erase_if(writing_, [&](const auto& w) {
        const auto workspace = workspaces_.find(w.first);
        return (workspace == workspaces_.end() || w.second.workspace != workspace->second)
            && w.second.result.wait_for(0ms) == std::future_status::ready;
    });
}

BusAccept BusInbox::refuse(const std::string& recipient, const std::string& why) {
    if (errors_[recipient] != why) {
        errors_[recipient] = why;
        reportBus(recipient + ": " + why);
    }
    return BusAccept::Retry;
}

BusAccept BusInbox::receive(sync::Hub& hub, const std::string& recipient, const BusMessage& message) {
    const auto workspace = workspaces_.find(recipient);
    if (workspace == workspaces_.end()) return BusAccept::Retry;
    if (hub.hasBusReceipt(recipient, message.id)) return BusAccept::Admit;
    Prepared prepared;
    if (message.body.size() <= kBusInlineBytes) {
        // The bus permits subjects with empty bodies. Give these messages a visible label as well.
        const auto& label = message.body.find_first_not_of(" \t\r\n") == std::string::npos
            ? message.subject : message.body;
        prepared.body = {label, message.body};
    } else {
        // A slow disk may take several frames to sync an attachment. Never wait for it in the server's poll loop.
        auto it = writing_.find(recipient);
        if (it == writing_.end()) {
            try {
                auto result = std::async(std::launch::async, [dir = workspace->second, recipient, id = message.id,
                                                              bytes = message.body] {
                    Prepared staged;
                    prepareBusBody(dir, recipient, id, bytes, staged.body, staged.problem);
                    return staged;
                });
                writing_.emplace(recipient, Writing{workspace->second, message.id, std::move(result)});
            } catch (const std::system_error&) {
                return refuse(recipient, "Cannot start an attachment writer. Check the server's available threads.");
            }
            return BusAccept::Wait;
        }
        if (it->second.result.wait_for(0ms) != std::future_status::ready) return BusAccept::Wait;
        const bool current = it->second.workspace == workspace->second && it->second.id == message.id;
        prepared = it->second.result.get();
        writing_.erase(it);
        if (!current) return BusAccept::Wait;
        if (!prepared.problem.empty()) return refuse(recipient, prepared.problem);
    }
    std::string prompt = "Message from " + message.from + " on the message bus (id " + message.id + ")";
    if (!message.replyTo.empty()) prompt += "\nIn reply to: " + message.replyTo;
    if (!message.subject.empty()) prompt += "\nSubject: " + message.subject;
    prompt += "\n\n";
    if (message.body.size() > kBusInlineBytes) prompt += "Text attachment: ";
    prompt += prepared.body.promptBody;

    std::string why;
    if (!hub.receiveBus(recipient, message.id, message.from, prepared.body.displayBody, prompt, &why))
        return refuse(recipient, why);
    errors_.erase(recipient);
    return BusAccept::Admit;
}

} // namespace nexus::server
