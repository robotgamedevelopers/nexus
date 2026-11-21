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
