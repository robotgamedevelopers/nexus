#include "policy/host.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <memory>
#include <string_view>

#include "pr/json.h"

extern "C" {
NexusPolicyHost* nexus_star_open_v1(const char* side, char** error);
char* nexus_star_call_v1(NexusPolicyHost*, const char* path, const char* event, const char* context, char** error);
char* nexus_star_watch_v1(NexusPolicyHost*, const char* path);
uint64_t nexus_star_refresh_v1(NexusPolicyHost*);
void nexus_star_close_v1(NexusPolicyHost*);
void nexus_star_free_v1(char*);
}

namespace nexus::policy {
namespace {
std::string take(char* text) {
    std::unique_ptr<char, decltype(&nexus_star_free_v1)> owned(text, nexus_star_free_v1);
    return text ? std::string(text) : std::string();
}
PrStringView pr(std::string_view s) { return {s.data(), s.size()}; }
void strings(PrJsonWriter* w, const char* key, const std::vector<std::string>& values) {
    prJsonWriterKey(w, pr(key));
    prJsonWriterBeginArray(w);
    for (const auto& value : values)
        prJsonWriterString(w, pr(value));
    prJsonWriterEndArray(w);
}
} // namespace

Host::Host(const char* side) {
    char* error = nullptr;
    host_ = nexus_star_open_v1(side, &error);
    initError_ = take(error);
}

Host::~Host() { nexus_star_close_v1(host_); }

void Host::watch(const std::string& path) {
    if (!host_) return;
    const auto error = take(nexus_star_watch_v1(host_, path.c_str()));
    if (!error.empty())
        std::fprintf(stderr, "nexus: client policy watcher: %s\n", error.c_str());
}

uint64_t Host::refresh() { return host_ ? nexus_star_refresh_v1(host_) : 0; }

std::string Host::decide(const std::string& path, const char* event, const std::string& context, std::string& why) {
    if (!host_) {
        why = initError_;
        return {};
    }
    char* error = nullptr;
    auto answer = take(nexus_star_call_v1(host_, path.c_str(), event, context.c_str(), &error));
    why = take(error);
    return answer;
}

bool messagePlan(Host& host, const std::string& path, bool client, const char* intent,
    const MessageState& state, MessagePlan& out, std::string& why) {
    std::unique_ptr<PrJsonWriter, decltype(&prJsonWriterDestroy)> w(prJsonWriterCreate(), prJsonWriterDestroy);
    prJsonWriterBeginObject(w.get());
    auto field = [&](const char* name, std::string_view value) {
        prJsonWriterKey(w.get(), pr(name));
        prJsonWriterString(w.get(), pr(value));
    };
    field(client ? "key" : "intent", intent);
    field("text", state.text);
    field("friend", state.friendName);
    field("model", state.model);
    prJsonWriterKey(w.get(), pr("queued"));
    prJsonWriterNumber(w.get(), static_cast<double>(state.queued));
    prJsonWriterKey(w.get(), pr("busy"));
    prJsonWriterBool(w.get(), state.busy);
    prJsonWriterEndObject(w.get());
    const auto ctx = prJsonWriterText(w.get());
    const auto answer = host.decide(path, client ? "app.prompt_key" : "app.message", std::string(ctx.data, ctx.size), why);
    if (!why.empty())
        return false;

    std::unique_ptr<PrJson, decltype(&prJsonDestroy)> doc(prJsonCreate(), prJsonDestroy);
    auto* d = doc.get();
    if (!prJsonParse(d, pr(answer), nullptr) || prJsonKind(d, 0) != PrJsonKind_Array || prJsonCount(d, 0) > 16) {
        why = "return a list of up to 16 commands";
        return false;
    }
    MessagePlan plan;
    for (uint32_t i = 0; i < prJsonCount(d, 0); ++i) {
        const auto item = prJsonElement(d, 0, i);
        const auto op = prJsonString(d, prJsonMember(d, item, pr("op")));
        const auto text = prJsonString(d, prJsonMember(d, item, pr("text")));
        const std::string_view name = op.data ? std::string_view(op.data, op.size) : std::string_view();
        if (plan.delivery != Delivery::None) {
            why = "a delivery or native service call must be the last command for a message";
            return false;
        }
        if (!client && name == "native") {
            const auto service = prJsonString(d, prJsonMember(d, item, pr("name")));
            if (!service.data || service.size == 0 || service.size > 128 ||
                    prJsonCount(d, item) != (text.data ? 3u : 2u)) {
                why = "a native call takes a service name and optional text";
                return false;
            }
            plan.delivery = Delivery::Native;
            plan.service.assign(service.data, service.size);
            if (text.data) plan.text.emplace(text.data, text.size);
            continue;
        }
        if (prJsonKind(d, item) != PrJsonKind_Object || prJsonCount(d, item) != (text.data ? 2u : 1u)) {
            why = "a command takes an op string and an optional text string";
            return false;
        }
        if (name == "notice" && text.data && text.size > 0 && text.size <= 1024) {
            plan.notices.emplace_back(text.data, text.size);
        } else if (name == "draft" && client && text.data && text.size <= 32768) {
            plan.text.emplace(text.data, text.size);
        } else if (name == "send" || name == "queue") {
            plan.delivery = name == "send" ? Delivery::Send : Delivery::Queue;
            if (text.data)
                plan.text.emplace(text.data, text.size);
        } else {
            why = "unsupported command or text length; see docs/policy.md";
            return false;
        }
    }
    out = std::move(plan);
    return true;
}

int textZoom(Host& host, const std::string& path, int current, int delta, int low, int high) {
    const auto context = "{\"current\":" + std::to_string(current) + ",\"delta\":" + std::to_string(delta) +
        ",\"min\":" + std::to_string(low) + ",\"max\":" + std::to_string(high) + "}";
    std::string why;
    const auto answer = host.decide(path, "text_zoom", context, why);
    int next = 0;
    const auto end = answer.data() + answer.size();
    const auto parsed = std::from_chars(answer.data(), end, next);
    if (parsed.ec == std::errc{} && parsed.ptr == end && next >= low && next <= high)
        return next;
    std::fprintf(stderr, "nexus: text_zoom: %s. Check client.star and apply it again.\n",
        why.empty() ? "return an integer in the supplied range" : why.c_str());
    return current;
}

std::string accountAfterLimit(Host& host, const std::string& path, const std::string& current,
    const std::vector<std::string>& linked, const std::vector<std::string>& eligible, std::string& why) {
    std::unique_ptr<PrJsonWriter, decltype(&prJsonWriterDestroy)> w(prJsonWriterCreate(), prJsonWriterDestroy);
    prJsonWriterBeginObject(w.get());
    prJsonWriterKey(w.get(), pr("current"));
    prJsonWriterString(w.get(), pr(current));
    strings(w.get(), "linked", linked);
    strings(w.get(), "eligible", eligible);
    prJsonWriterEndObject(w.get());
    const auto ctx = prJsonWriterText(w.get());
    const auto answer = host.decide(path, "account_after_limit", std::string(ctx.data, ctx.size), why);
    if (!why.empty() || answer == "null")
        return {};
    std::unique_ptr<PrJson, decltype(&prJsonDestroy)> doc(prJsonCreate(), prJsonDestroy);
    if (prJsonParse(doc.get(), pr(answer), nullptr) && prJsonKind(doc.get(), 0) == PrJsonKind_String) {
        const auto value = prJsonString(doc.get(), 0);
        const std::string selected(value.data, value.size);
        if (std::find(eligible.begin(), eligible.end(), selected) != eligible.end())
            return selected;
    }
    why = "account_after_limit must return one of the eligible accounts or None";
    return {};
}

} // namespace nexus::policy
