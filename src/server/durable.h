// nexus-server's tabs on disk: the sync::TabStore the server's Hub writes to, over the store (src/store/store.h). Each
// commit is one record of the store's journal, synced before commit returns, so the server acknowledges an action, and
// sends the state it made, only once it is durable. The tabs are kept in memory too, as the shared entries the down
// state is made of (sync/tabs.h).

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "store/fs.h"
#include "store/store.h"
#include "sync/hub.h"

namespace nexus::server {

class DurableStore : public sync::TabStore {
public:
    // Opens (or makes) the store in `dir` on `fs`. False, with why, when the store cannot be opened.
    bool open(store::Fs& fs, const std::string& dir, std::string* why, const store::StoreOptions& options = {});

    const sync::StoredTab* find(const std::string& name) const override;
    std::size_t size() const override { return tabs_.size(); }
    std::vector<std::string> names() const override;
    bool commit(const std::string& name, const std::vector<sync::TabChange>& changes, const std::string& client,
                std::uint64_t num, std::string* why) override;
    std::uint64_t applied(const std::string& client) const override;
    bool failed() const override { return store_ == nullptr || store_->failed(); }

    const store::OpenReport& report() const { return report_; }

private:
    std::unique_ptr<store::Store> store_;
    store::OpenReport report_;
    std::map<std::string, sync::StoredTab> tabs_;
};

} // namespace nexus::server
