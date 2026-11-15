// Format 4 on: the actions as schema's ActionsDiff table (sync/codec.h), the half of the codec tables_codec.cpp (the
// sessions) does not hold. Its rules are that file's: nothing here encodes a byte by hand.

#include <cstdlib>
#include <cstring>

#include "sync/tables_shared.h"

namespace nexus::sync::codec {

namespace {

using namespace shared;

wire::ActionKind actionOut(ActionKind k) {
    switch (k) {
        case ActionKind::Unknown: return wire::ActionKind::None;
        case ActionKind::Send: return wire::ActionKind::Send;
        case ActionKind::Queue: return wire::ActionKind::Queue;
        case ActionKind::Stop: return wire::ActionKind::Stop;
        case ActionKind::Create: return wire::ActionKind::Create;
        case ActionKind::Ensure: return wire::ActionKind::Ensure;
        case ActionKind::SetAccount: return wire::ActionKind::SetAccount;
        case ActionKind::SetModel: return wire::ActionKind::SetModel;
        case ActionKind::SetEffort: return wire::ActionKind::SetEffort;
        case ActionKind::AddAccount: return wire::ActionKind::AddAccount;
        case ActionKind::RemoveAccount: return wire::ActionKind::RemoveAccount;
        case ActionKind::AddFriend: return wire::ActionKind::AddFriend;
        case ActionKind::LinkAccount: return wire::ActionKind::LinkAccount;
        case ActionKind::UnlinkAccount: return wire::ActionKind::UnlinkAccount;
        case ActionKind::SetDefault: return wire::ActionKind::SetDefault;
        case ActionKind::SecretAnswer: return wire::ActionKind::SecretAnswer;
        case ActionKind::SecretCancel: return wire::ActionKind::SecretCancel;
    }
    return wire::ActionKind::None;
}

// A kind this build does not know (None: a later window's) is Unknown, which the server counts and never applies.
ActionKind actionIn(wire::ActionKind k) {
    switch (k) {
        case wire::ActionKind::Send: return ActionKind::Send;
        case wire::ActionKind::Queue: return ActionKind::Queue;
        case wire::ActionKind::Stop: return ActionKind::Stop;
        case wire::ActionKind::Create: return ActionKind::Create;
        case wire::ActionKind::Ensure: return ActionKind::Ensure;
        case wire::ActionKind::SetAccount: return ActionKind::SetAccount;
        case wire::ActionKind::SetModel: return ActionKind::SetModel;
        case wire::ActionKind::SetEffort: return ActionKind::SetEffort;
        case wire::ActionKind::AddAccount: return ActionKind::AddAccount;
        case wire::ActionKind::RemoveAccount: return ActionKind::RemoveAccount;
        case wire::ActionKind::AddFriend: return ActionKind::AddFriend;
        case wire::ActionKind::LinkAccount: return ActionKind::LinkAccount;
        case wire::ActionKind::UnlinkAccount: return ActionKind::UnlinkAccount;
        case wire::ActionKind::SetDefault: return ActionKind::SetDefault;
        case wire::ActionKind::SecretAnswer: return ActionKind::SecretAnswer;
        case wire::ActionKind::SecretCancel: return ActionKind::SecretCancel;
        default: return ActionKind::Unknown;
    }
}

// A builder's memory, every byte overwritten when it is freed: for an actions diff that holds a secret's value, which
// the builder copied into its arena (sync/secrets.h: where a value may be). Each block keeps its size in front of it.
constexpr std::size_t kWipeHeader = 16;

void* wipingAlloc(void*, int64_t bytes) {
    if (bytes <= 0) {
        return nullptr;
    }
    const std::size_t size = static_cast<std::size_t>(bytes);
    auto* block = static_cast<unsigned char*>(std::calloc(1, size + kWipeHeader));
    if (block == nullptr) {
        return nullptr;
    }
    std::memcpy(block, &size, sizeof size);
    return block + kWipeHeader;
}

void wipingFree(void*, void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    unsigned char* block = static_cast<unsigned char*>(pointer) - kWipeHeader;
    std::size_t size = 0;
    std::memcpy(&size, block, sizeof size);
    volatile unsigned char* bytes = block;
    for (std::size_t i = 0; i < size + kWipeHeader; ++i) {
        bytes[i] = 0;
    }
    std::free(block);
}

} // namespace

std::string writeActionsTables(std::uint64_t start, const std::vector<Action>& held, std::size_t from) {
    bool secret = false;
    for (std::size_t i = from; i < held.size(); ++i) {
        secret = secret || !held[i].secret.empty();
    }
    const wire::TableAllocator wiping{wipingAlloc, wipingFree, nullptr};
    wire::ActionsDiffBuilder b(secret ? wiping : wire::TableDefaultAllocator());
    wire::ActionsDiff* d = b.GetRoot();
    if (d == nullptr) {
        throw std::bad_alloc();
    }
    d->start = start;
    std::string scratch;
    for (std::size_t i = from; i < held.size(); ++i) {
        const Action& a = held[i];
        wire::Action* x = added(wire::ActionsDiffActionsAdd(b.main, d->actions));
        x->kind = actionOut(a.kind);
        x->session = a.session;
        putText(b.main, x->friend_name, a.friendName, scratch);
        putText(b.main, x->text, a.text, scratch);
        putText(b.main, x->account, a.account, scratch);
        x->request = a.request;
        if (!a.secret.empty()) {
            const std::string& value = a.secret.bytes();
            std::uint8_t* to = wire::TableBytesEmplace(b.main, x->secret, static_cast<int64_t>(value.size()));
            if (to == nullptr) {
                throw std::bad_alloc();
            }
            std::memcpy(to, value.data(), value.size());
        }
    }
    return saved(b, [](const wire::ActionsDiffBuilder& x) { return wire::ActionsDiffMeasure(x); },
        [](const wire::ActionsDiffBuilder& x, std::uint8_t* to, int64_t n) { return wire::ActionsDiffSave(x, to, n); },
        "actions diff");
}

std::vector<Action> readActionsTables(const std::string& diff, int* unknown) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(diff.data());
    const int64_t size = static_cast<int64_t>(diff.size());
    const int64_t need = wire::ActionsDiffLoadMeasure(bytes, size);
    if (need < 0) {
        throw BadDiff("actions diff of " + std::to_string(diff.size()) + " bytes does not load as schema's "
            "ActionsDiff (damaged, cut short, or a form this build does not read)");
    }
    Region region;
    region.bytes.resize(static_cast<std::size_t>(need));
    wire::TableReport report;
    const wire::ActionsDiff* d = wire::ActionsDiffLoad(region.bytes.data(), need, bytes, size, &report);
    if (d == nullptr || report.damaged || report.refused) {
        throw BadDiff("actions diff of " + std::to_string(diff.size()) + " bytes does not load as schema's "
            "ActionsDiff (damaged, cut short, or a form this build does not read)");
    }
    *unknown = report.unknown;
    const std::uint64_t count = static_cast<std::uint64_t>(d->actions.size());
    if (d->start == 0) {
        throw BadDiff("actions diff starts at number 0; numbers start at 1");
    }
    if (count == 0) {
        throw BadDiff("actions diff holds no actions");
    }
    if (d->start + count < d->start) {
        throw BadDiff("actions diff numbers run past 64 bits");
    }
    std::vector<Action> out;
    out.reserve(static_cast<std::size_t>(count));
    std::uint64_t num = d->start;
    for (const wire::Action& x : d->actions) {
        Action a;
        a.num = num++;
        a.kind = actionIn(x.kind);
        if (a.kind != ActionKind::Unknown) {
            a.session = x.session;
            a.friendName = textOf(x.friend_name);
            a.text = textOf(x.text);
            a.account = textOf(x.account);
            a.request = x.request;
            const wire::TableBytesView value = wire::TableBytesAt(x.secret);
            if (value.data != nullptr && value.length > 0) {
                a.secret = SecretValue(std::string_view(reinterpret_cast<const char*>(value.data),
                    static_cast<std::size_t>(value.length)));
            }
        }
        out.push_back(std::move(a));
    }
    return out;
}

} // namespace nexus::sync::codec
