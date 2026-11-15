// nexus-server's sessions on disk: the sync::SessionStore the server's Hub writes to, over the store
// (src/store/store.h). Each commit is one record of the store's journal, synced before commit returns or, in a batch,
// by the batch's flush, so the server acknowledges an action, and sends the state it made, only once it is durable. The sessions are kept in memory too,
// as the shared entries the down state is made of (sync/sessions.h). A store in an older format (friend tabs) opens
// as sessions, one per tab (store.h).

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "store/fs.h"
#include "store/store.h"
#include "sync/hub.h"

namespace nexus::server {

class DurableStore : public sync::SessionStore {
public:
    // Opens (or makes) the store in `dir` on `fs`, its lock first (store::Store::open). False, with why, when the store
    // cannot be opened; busy() then says whether that was another process holding its lock.
    bool open(store::Fs& fs, const std::string& dir, std::string* why, const store::StoreOptions& options = {});
    bool busy() const { return busy_; }

    const sync::StoredSession* find(sync::SessionId id) const override;
    std::size_t size() const override { return sessions_.size(); }
    std::vector<sync::SessionId> ids() const override;
    sync::SessionId nextId() const override { return store_ == nullptr ? 1 : store_->next_session(); }
    bool commit(sync::SessionId id, const std::vector<sync::SessionChange>& changes, const std::string& client,
                std::uint64_t num, std::string* why) override;
    std::uint64_t applied(const std::string& client) const override;
    bool failed() const override { return store_ == nullptr || store_->failed(); }

    const store::OpenReport& report() const { return report_; }

    // Group commit (store::Store::begin_batch, flush): every commit until flush() is applied here and appended, and
    // all of them are made durable by flush()'s one sync. Nothing made by a step of the batch may leave the server
    // until flush() has returned true. False, with why, when the sync failed: then nothing of the batch may be told.
    void beginBatch();
    bool flush(std::string* why);

private:
    std::unique_ptr<store::Store> store_;
    store::OpenReport report_;
    bool busy_ = false;
    std::map<sync::SessionId, sync::StoredSession> sessions_;
};

} // namespace nexus::server
