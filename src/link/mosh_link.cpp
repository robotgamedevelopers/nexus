#include "link/mosh_link.h"

#include <exception>
#include <stdexcept>

// mosh's headers, as its own programs include them (src/frontend/mosh-server.cc). They bring `using namespace`
// declarations of their own, so they stay in this file. networktransport-impl.h uses fatal_assert without including
// its header, as mosh's own files include it first.
#include "fatal_assert.h"
#include "networktransport-impl.h"
#include "timestamp.h"

#include "sync/wire.h"

namespace nexus::link {

namespace {

using ServerTransport = Network::Transport<sync::Tabs, sync::Actions>;
using ClientTransport = Network::Transport<sync::Actions, sync::Tabs>;

// Runs one call into mosh: false, with what it threw, on an exception. mosh throws NetworkException for the socket,
// CryptoException for a packet that does not authenticate, and our states throw BadDiff or std::logic_error.
template <class F>
bool guarded(F&& f, std::string* why) {
    try {
        f();
        return true;
    } catch (const Network::NetworkException& e) {
        if (why != nullptr)
            *why = std::string("network: ") + e.what();
    } catch (const Crypto::CryptoException& e) {
        if (why != nullptr)
            *why = std::string("crypto: ") + e.what();
    } catch (const std::exception& e) {
        if (why != nullptr)
            *why = e.what();
    }
    return false;
}

} // namespace

void freezeClock() {
    freeze_timestamp();
}

// Both ends start from the empty states (mosh's state 0, which each end assumes the other holds); the server's tabs
// are its first change.
struct ServerSession::Impl {
    sync::Tabs initialMine;
    sync::Actions initialRemote;
    ServerTransport transport;
    sync::Applier applier;
    explicit Impl(const sync::Tabs& state) : transport(initialMine, initialRemote, nullptr, nullptr) {
        transport.set_current_state(state);
    }
};

ServerSession::ServerSession(const sync::Tabs& state) {
    std::string why;
    if (!guarded([&] { impl_ = std::make_unique<Impl>(state); }, &why))
        throw std::runtime_error("no UDP port for the session (mosh binds one of 60001 to 60999): " + why);
}

ServerSession::~ServerSession() = default;

std::string ServerSession::port() const {
    return impl_->transport.port();
}

std::string ServerSession::key() const {
    return impl_->transport.get_key();
}

std::vector<int> ServerSession::fds() const {
    return impl_->transport.fds();
}

bool ServerSession::receive(std::string* why) {
    return guarded([&] { impl_->transport.recv(); }, why);
}

std::vector<sync::Action> ServerSession::take(std::string* problem) {
    std::vector<sync::Action> fresh;
    std::string diff;
    if (!guarded([&] { diff = impl_->transport.get_remote_diff(); }, problem))
        return fresh;
    if (diff.empty())
        return fresh;
    std::vector<sync::Action> actions;
    if (!guarded([&] { actions = sync::Actions::parse(diff); }, problem))
        return fresh;
    return impl_->applier.apply(actions, problem);
}

void ServerSession::setState(const sync::Tabs& state) {
    impl_->transport.set_current_state(state);
}

bool ServerSession::tick(std::string* why) {
    return guarded([&] { impl_->transport.tick(); }, why);
}

int ServerSession::waitMs() {
    return impl_->transport.wait_time();
}

bool ServerSession::ended() const {
    return impl_->transport.counterparty_shutdown_ack_sent();
}

std::uint64_t ServerSession::applied() const {
    return impl_->applier.applied();
}

struct ClientSession::Impl {
    sync::Actions initialMine;
    sync::Tabs initialRemote;
    ClientTransport transport;
    Impl(const std::string& key, const std::string& ip, const std::string& port)
        : transport(initialMine, initialRemote, key.c_str(), ip.c_str(), port.c_str()) {}
};

ClientSession::ClientSession(const std::string& key, const std::string& ip, const std::string& port) {
    std::string why;
    if (!guarded([&] { impl_ = std::make_unique<Impl>(key, ip, port); }, &why))
        throw std::runtime_error("cannot start the session with " + ip + " port " + port + ": " + why);
}

ClientSession::~ClientSession() = default;

std::vector<int> ClientSession::fds() const {
    return impl_->transport.fds();
}

bool ClientSession::receive(std::string* why) {
    return guarded([&] { impl_->transport.recv(); }, why);
}

std::uint64_t ClientSession::add(sync::ActionKind kind, std::string tab, std::string text) {
    return impl_->transport.get_current_state().add(kind, std::move(tab), std::move(text));
}

const sync::Tabs& ClientSession::state() const {
    return impl_->transport.get_latest_remote_state().state;
}

std::uint64_t ClientSession::stateNum() const {
    return impl_->transport.get_remote_state_num();
}

std::uint64_t ClientSession::heardMsAgo() const {
    const std::uint64_t at = impl_->transport.get_latest_remote_state().timestamp;
    const std::uint64_t now = frozen_timestamp();
    return now > at ? now - at : 0;
}

bool ClientSession::allAcknowledged() const {
    // mosh subtracts the acknowledged state from the current one on every tick (rationalize_states): what is left
    // is what the server has not acknowledged.
    return impl_->transport.get_current_state().held().empty();
}

bool ClientSession::tick(std::string* why) {
    return guarded([&] { impl_->transport.tick(); }, why);
}

int ClientSession::waitMs() {
    return impl_->transport.wait_time();
}

void ClientSession::shutdown() {
    impl_->transport.start_shutdown();
}

bool ClientSession::shutdownDone() const {
    return impl_->transport.shutdown_acknowledged() || impl_->transport.shutdown_ack_timed_out();
}

} // namespace nexus::link
