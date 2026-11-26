#include "relay/relay_ends.h"

#include <utility>

namespace nexus::relay {

namespace {

constexpr char kWindow[] = "this window";
constexpr char kRelay[] = "nexus-link-relay in WSL2";

} // namespace

std::string RelayClient::open() {
    reader_ = FrameReader();
    states_ = StateReader();
    helloRead_ = false;
    LinkState s;
    s.phase = sync::LinkPhase::Connecting;
    s.sessions = shown_.sessions;
    show(std::move(s));
    return helloFrame();
}

std::string RelayClient::send(const sync::Action& action) {
    return actionFrame(action);
}

std::string RelayClient::history(sync::SessionId id, std::uint64_t epoch, std::uint64_t before) {
    return historyFrame(id, epoch, before);
}

bool RelayClient::feed(const char* data, std::size_t size, std::string* why) {
    reader_.feed(data, size);
    Frame frame;
    bool applied = false;
    while (reader_.next(&frame, why)) {
        if (!helloRead_) {
            if (frame.kind != FrameKind::Hello) {
                *why = std::string(kRelay) + " sent a frame before its hello";
                return false;
            }
            if (!checkHello(frame.body, kWindow, kRelay, why)) return false;
            helloRead_ = true;
            continue;
        }
        if (frame.kind != FrameKind::State) {
            *why = std::string(kRelay) + " sent a frame of kind " + std::to_string(static_cast<int>(frame.kind)) +
                ", which only a window sends";
            return false;
        }
        if (!states_.apply(frame.body, why)) {
            *why = std::string(kRelay) + " sent " + *why;
            return false;
        }
        applied = true;
    }
    if (!why->empty()) {
        *why = std::string(kRelay) + " sent " + *why;
        return false;
    }
    if (applied) show(states_.state());
    return true;
}

void RelayClient::ended(const std::string& why) {
    LinkState s = shown_;
    s.phase = sync::LinkPhase::Failed;
    s.problem = why;
    s.heardMsAgo = 0;
    show(std::move(s));
}

bool RelayClient::poll(LinkState* out) {
    if (!changed_) return false;
    *out = shown_;
    changed_ = false;
    return true;
}

void RelayClient::show(LinkState state) {
    shown_ = std::move(state);
    changed_ = true;
}

bool RelayServer::feed(const char* data, std::size_t size, std::string* why) {
    reader_.feed(data, size);
    Frame frame;
    while (reader_.next(&frame, why)) {
        if (!helloRead_) {
            if (frame.kind != FrameKind::Hello) {
                *why = std::string(kWindow) + " sent a frame before its hello";
                wipe(&frame.body);
                return false;
            }
            if (!checkHello(frame.body, kRelay, kWindow, why)) return false;
            helloRead_ = true;
            continue;
        }
        Request r;
        bool ok = false;
        if (frame.kind == FrameKind::Action) {
            r.kind = Request::Kind::Action;
            ok = readAction(frame.body, &r.action, why);
        } else if (frame.kind == FrameKind::History) {
            r.kind = Request::Kind::History;
            ok = readHistory(frame.body, &r.session, &r.epoch, &r.before, why);
        } else {
            *why = "a frame of kind " + std::to_string(static_cast<int>(frame.kind)) + ", which only a relay sends";
        }
        wipe(&frame.body);
        if (!ok) {
            *why = std::string(kWindow) + " sent " + *why;
            return false;
        }
        requests_.push_back(std::move(r));
    }
    if (!why->empty()) {
        *why = std::string(kWindow) + " sent " + *why;
        return false;
    }
    return true;
}

std::vector<RelayServer::Request> RelayServer::take() {
    std::vector<Request> out;
    out.swap(requests_);
    return out;
}

} // namespace nexus::relay
