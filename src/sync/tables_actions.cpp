// Format 4 on: the actions as schema's ActionsDiff table (sync/codec.h), the half of the codec tables_codec.cpp (the
// sessions) does not hold. Its rules are that file's: nothing here encodes a byte by hand.

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
        case ActionKind::AddAI: return wire::ActionKind::AddAI;
        case ActionKind::LinkAccount: return wire::ActionKind::LinkAccount;
        case ActionKind::UnlinkAccount: return wire::ActionKind::UnlinkAccount;
        case ActionKind::SetDefault: return wire::ActionKind::SetDefault;
        case ActionKind::SecretAnswer: return wire::ActionKind::SecretAnswer;
        case ActionKind::SecretCancel: return wire::ActionKind::SecretCancel;
        case ActionKind::SignInClaim: return wire::ActionKind::SignInClaim;
        case ActionKind::SignInCancel: return wire::ActionKind::SignInCancel;
        case ActionKind::SignInOpened: return wire::ActionKind::SignInOpened;
        case ActionKind::SignOut: return wire::ActionKind::SignOut;
        case ActionKind::SignInCallback: return wire::ActionKind::SignInCallback;
        case ActionKind::ResolveLimit: return wire::ActionKind::ResolveLimit;
    }
    return wire::ActionKind::None;
}

bool isSignIn(ActionKind k) {
    return k == ActionKind::SignOut || k == ActionKind::SignInClaim ||
        k == ActionKind::SignInOpened || k == ActionKind::SignInCallback || k == ActionKind::SignInCancel;
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
        case wire::ActionKind::AddAI: return ActionKind::AddAI;
        case wire::ActionKind::LinkAccount: return ActionKind::LinkAccount;
        case wire::ActionKind::UnlinkAccount: return ActionKind::UnlinkAccount;
        case wire::ActionKind::SetDefault: return ActionKind::SetDefault;
        case wire::ActionKind::SecretAnswer: return ActionKind::SecretAnswer;
        case wire::ActionKind::SecretCancel: return ActionKind::SecretCancel;
        case wire::ActionKind::SignInClaim: return ActionKind::SignInClaim;
        case wire::ActionKind::SignInCancel: return ActionKind::SignInCancel;
        case wire::ActionKind::SignInOpened: return ActionKind::SignInOpened;
        case wire::ActionKind::SignOut: return ActionKind::SignOut;
        case wire::ActionKind::SignInCallback: return ActionKind::SignInCallback;
        case wire::ActionKind::ResolveLimit: return ActionKind::ResolveLimit;
        default: return ActionKind::Unknown;
    }
}

} // namespace

std::string writeActionsTables(std::uint64_t start, const std::vector<Action>& held, std::size_t from,
                               std::uint32_t format) {
    const bool signIn = format >= kSignInFormat;
    bool secret = false;
    for (std::size_t i = from; i < held.size(); ++i) {
        secret = secret || !held[i].secret.empty();
    }
    const wire::TableAllocator wipes = wiping();
    wire::ActionsDiffBuilder b(secret ? wipes : wire::TableDefaultAllocator());
    wire::ActionsDiff* d = b.GetRoot();
    if (d == nullptr) {
        throw std::bad_alloc();
    }
    d->start = start;
    std::string scratch;
    for (std::size_t i = from; i < held.size(); ++i) {
        const Action& a = held[i];
        wire::Action* x = added(wire::ActionsDiffActionsAdd(b.main, d->actions));
        if (isSignIn(a.kind) && !signIn) {
            // A link below 5 carries no sign-in: a stop naming session 0 and nothing else, refused by every server,
            // so the numbers stay unbroken and nothing of the sign-in leaves.
            x->kind = wire::ActionKind::Stop;
            continue;
        }
        x->kind = actionOut(a.kind);
        x->session = a.session;
        putText(b.main, x->ai_name, a.aiName, scratch);
        putText(b.main, x->text, a.text, scratch);
        putText(b.main, x->account, a.account, scratch);
        x->request = a.request;
        putBytes(b.main, x->secret, a.secret.bytes());
    }
    return saved(b, [](const wire::ActionsDiffBuilder& x) { return wire::ActionsDiffMeasure(x); },
        [](const wire::ActionsDiffBuilder& x, std::uint8_t* to, int64_t n) { return wire::ActionsDiffSave(x, to, n); },
        "actions diff");
}

std::vector<Action> readActionsTables(const std::string& diff, std::uint32_t format, int* unknown) {
    const bool signIn = format >= kSignInFormat;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(diff.data());
    const int64_t size = static_cast<int64_t>(diff.size());
    const std::size_t space = readNeed(diff, [](const std::uint8_t* b, int64_t n) {
        return wire::ActionsDiffLoadMeasure(b, n);
    }, "actions diff");
    const int64_t need = static_cast<int64_t>(space);
    Region region;
    region.bytes.resize(space);
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
        if (isSignIn(a.kind) && !signIn) {
            a.kind = ActionKind::Unknown;     // taken only from a link of 5 or later; nothing of it is kept
        }
        if (a.kind != ActionKind::Unknown) {
            a.session = x.session;
            a.aiName = textOf(x.ai_name);
            a.text = textOf(x.text);
            a.account = textOf(x.account);
            a.request = x.request;
            a.secret = bytesOf(x.secret);
        }
        out.push_back(std::move(a));
    }
    return out;
}

} // namespace nexus::sync::codec
