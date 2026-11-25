#include "policy/session.h"
#include "pr/json.h"

#include <memory>

namespace nexus::policy {
bool newSession(Host& host, const std::string& path, const sync::SessionInfo& defaults, const sync::SessionInfo* source,
    const accounts::AI* who, bool hasRoster, bool canAutoAccount, sync::SessionInfo& result, std::string& why) {
    std::unique_ptr<PrJsonWriter, decltype(&prJsonWriterDestroy)> w(prJsonWriterCreate(), prJsonWriterDestroy);
    auto key = [&](std::string_view name) { prJsonWriterKey(w.get(), {name.data(), name.size()}); };
    auto text = [&](std::string_view s) { prJsonWriterString(w.get(), {s.data(), s.size()}); };
    auto field = [&](std::string_view name, std::string_view value) { key(name); text(value); };
    auto session = [&](const sync::SessionInfo& info) {
        prJsonWriterBeginObject(w.get());
        field("ai", info.aiName);
        field("account", info.account);
        key("auto_account");
        prJsonWriterBool(w.get(), info.autoAccount);
        field("instance", info.instance);
        field("model", info.model);
        field("effort", info.effort);
        prJsonWriterEndObject(w.get());
    };
    prJsonWriterBeginObject(w.get());
    field("ai", defaults.aiName);
    key("defaults");
    session(defaults);
    key("source");
    if (source) session(*source);
    else prJsonWriterNull(w.get());
    key("roster");
    if (hasRoster) {
        prJsonWriterBeginObject(w.get());
        field("preferred", who ? who->preferred : "");
        key("can_auto_account");
        prJsonWriterBool(w.get(), canAutoAccount);
        key("linked");
        prJsonWriterBeginArray(w.get());
        if (who) for (const auto& account : who->accounts) text(account);
        prJsonWriterEndArray(w.get());
        key("instances");
        prJsonWriterBeginArray(w.get());
        if (who) for (const auto& instance : who->instances) text(instance.name);
        prJsonWriterEndArray(w.get());
        prJsonWriterEndObject(w.get());
    } else {
        prJsonWriterNull(w.get());
    }
    prJsonWriterEndObject(w.get());
    const auto context = prJsonWriterText(w.get());
    const auto answer = host.decide(path, "app.new_session", std::string(context.data, context.size), why);
    if (!why.empty()) return false;
    std::unique_ptr<PrJson, decltype(&prJsonDestroy)> d(prJsonCreate(), prJsonDestroy);
    auto extract = [&](std::string_view name, std::string& value) {
        const auto s = prJsonString(d.get(), prJsonMember(d.get(), 0, {name.data(), name.size()}));
        if (!s.data) return false;
        value.assign(s.data, s.size);
        return true;
    };
    if (!prJsonParse(d.get(), {answer.data(), answer.size()}, nullptr)) {
        why = "return a session object";
        return false;
    }
    const auto mode = prJsonMember(d.get(), 0, {"auto_account", 12});
    if (mode != kPrJsonMissing && prJsonKind(d.get(), mode) != PrJsonKind_False && prJsonKind(d.get(), mode) != PrJsonKind_True) {
        why = "auto_account must be a boolean";
        return false;
    }
    result.autoAccount = mode != kPrJsonMissing && prJsonKind(d.get(), mode) == PrJsonKind_True;
    if (prJsonCount(d.get(), 0) == (mode == kPrJsonMissing ? 4u : 5u) &&
            extract("model", result.model) && extract("account", result.account) &&
            extract("instance", result.instance) && extract("effort", result.effort))
        return true;
    why = "return account, instance, model and effort strings";
    return false;
}
} // namespace nexus::policy
