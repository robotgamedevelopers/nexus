// The window's frame's view of its link to nexus-server (link_port.h LinkPort) over the link's thread
// (link/client.h): poll copies the latest snapshot under the link's lock, and send queues an action and nudges the
// thread through a non-blocking pipe. Neither waits on the network.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <utility>

#include "link_port.h"
#include "link/client.h"

namespace nexus::link {

class ClientPort final : public LinkPort {
public:
    explicit ClientPort(ClientLink::Options options) : bootstrap_(options.bootstrap), wake_(options.wake),
        run_(linkRunner(options.run)), link_(std::make_unique<ClientLink>(std::move(options))) {}

    bool poll(LinkState* out) override {
        const bool changed = link_->poll(&latest_);
        sync::Sessions sessions = latest_.sessions;
        bool restored = false;
        for (auto it = pages_.begin(); it != pages_.end();) {
            auto& job = it->second;
            if (!job.result.valid() || job.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            auto entries = job.result.get();
            const auto* live = sessions.find(it->first);
            if (live && live->info.contextEpoch == job.epoch && !entries.empty()) {
                job.prefix.insert(job.prefix.end(), entries.begin(), entries.end());
                if (job.prefix.size() >= live->first) {
                    job.prefix.resize(static_cast<std::size_t>(live->first));
                    job.prefix.insert(job.prefix.end(), live->entries.begin(), live->entries.end());
                    auto full = std::make_shared<sync::SessionView>(*live);
                    full->first = 0;
                    full->entries = std::move(job.prefix);
                    auto all = sessions.sessions();
                    for (auto& view : all) if (view->id == full->id) view = full;
                    sessions.replaceSessions(std::move(all));
                    restored = true;
                    it = pages_.erase(it);
                    continue;
                }
            } else {
                job.retry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            ++it;
        }
        for (auto& [id, job] : pages_) {
            const auto* live = sessions.find(id);
            if (live && live->first && latest_.phase == sync::LinkPhase::Connected)
                requestHistory(id, live->info.contextEpoch, live->first);
        }
        if (!changed && !restored) return false;
        out->phase = latest_.phase;
        out->problem = latest_.problem;
        out->heardMsAgo = latest_.heardMsAgo;
        out->loopback = latest_.loopback;
        out->sessions = std::move(sessions);
        return true;
    }

    void requestHistory(sync::SessionId id, std::uint64_t epoch, std::uint64_t before) override {
        auto& job = pages_[id];
        if (job.result.valid()) return;
        if (job.epoch != epoch) job = {};
        if (std::chrono::steady_clock::now() < job.retry) return;
        job.epoch = epoch;
        std::packaged_task fetch([b = bootstrap_, run = run_, stop = stopping_, id, epoch, before, start = job.prefix.size()]() mutable {
            std::vector<sync::EntryRef> entries;
            auto verb = std::find(b.command.begin(), b.command.end(), "new");
            if (verb == b.command.end()) return entries;
            *verb = "history-page";
            b.command.insert(b.command.end(), {std::to_string(id), std::to_string(epoch), ""});
            b.client.clear();
            while (!stop->load() && start + entries.size() < before) {
                const auto offset = start + entries.size();
                b.command.back() = std::to_string(offset);
                const auto result = run(bootstrapCommand(b), b.timeoutSeconds);
                if (result.end != CommandResult::End::Exited || result.code || result.out.empty()) break;
                sync::Sessions decoded;
                try { decoded.apply_string(result.out); }
                catch (const std::exception&) { break; }
                const auto* page = decoded.find(id);
                if (!page || page->first != offset || page->info.contextEpoch != epoch || page->entries.empty()) break;
                entries.insert(entries.end(), page->entries.begin(), page->entries.end());
            }
            return entries;
        });
        job.result = fetch.get_future();
        job.worker = std::async(std::launch::async, [fetch = std::move(fetch), wake = wake_]() mutable {
            fetch();
            if (wake) wake();
        });
    }

    std::uint64_t send(sync::Action action) override { return link_->send(std::move(action)); }

    // Ends the link: mosh's shutdown, so the server ends this session (up to two seconds, at quit).
    ~ClientPort() override { stopping_->store(true); }
    void close() { stopping_->store(true); pages_.clear(); link_.reset(); }

private:
    struct HistoryJob {
        std::uint64_t epoch = 0;
        std::vector<sync::EntryRef> prefix;
        std::future<std::vector<sync::EntryRef>> result;
        std::future<void> worker;
        std::chrono::steady_clock::time_point retry;
    };
    std::shared_ptr<std::atomic<bool>> stopping_ = std::make_shared<std::atomic<bool>>(false);
    Bootstrap bootstrap_;
    std::function<void()> wake_;
    CommandRunner run_;   // the link's (ClientLink::Options run)
    std::map<sync::SessionId, HistoryJob> pages_;
    ClientLink::Snapshot latest_;
    std::unique_ptr<ClientLink> link_;
};

} // namespace nexus::link
