// Main's side of the cross-version tests (versions.h): tests/wire/main5/NexusMain.schema's generated code, state format
// 5 as main shipped it before sign-in by a pasted code, writing and reading as that nexus would. Nothing of this build's
// version is included here.

#include "versions.h"

// MSVC's C4305 off across the generated header only, as src/wire/tables.h does for this version's (it says why).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4305)
#endif
#include "NexusMainTable.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstring>

namespace wiretest {

namespace {

using namespace nexuswiremain;

void setText(TableWorker& worker, TableRef& slot, const char* text) {
    TableStringEmplace(worker, slot, text, static_cast<int64_t>(std::strlen(text)));
}

void setBytes(TableWorker& worker, TableRef& slot, const char* bytes) {
    const std::size_t n = std::strlen(bytes);
    std::uint8_t* at = TableBytesEmplace(worker, slot, static_cast<int64_t>(n));
    if (at != nullptr) {
        std::memcpy(at, bytes, n);
    }
}

std::string textOf(const TableRef& ref) {
    const TableStringView view = TableStringAt(ref);
    return view.data == nullptr ? std::string() : std::string(view.data, static_cast<std::size_t>(view.length));
}

std::string bytesOf(const TableRef& ref) {
    const TableBytesView view = TableBytesAt(ref);
    return view.data == nullptr ? std::string() :
        std::string(reinterpret_cast<const char*>(view.data), static_cast<std::size_t>(view.length));
}

template <typename Builder, typename Measure, typename Save>
Bytes saved(Builder& b, Measure measure, Save save) {
    if (!b.Lock()) {
        return Bytes();
    }
    Bytes out(static_cast<std::size_t>(measure(b)));
    const int64_t wrote = save(b, out.data(), static_cast<int64_t>(out.size()));
    out.resize(wrote < 0 ? 0 : static_cast<std::size_t>(wrote));
    return out;
}

} // namespace

Bytes mainSessions() {
    SessionsDiffBuilder b;
    SignInOffer* o = SessionsDiffSigninsAdd(b.main, b.GetRoot()->signins);
    o->id = 6;
    setText(b.main, o->account, "g1");
    setText(b.main, o->provider, "antigravity");
    setText(b.main, o->opener, "w1");
    setText(b.main, o->host, "studio");
    setBytes(b.main, o->url,
             "http://127.0.0.1:5000/nexus-signin/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    return saved(b, [](const SessionsDiffBuilder& x) { return SessionsDiffMeasure(x); },
        [](const SessionsDiffBuilder& x, std::uint8_t* out, int64_t n) { return SessionsDiffSave(x, out, n); });
}

Bytes mainClaim() {
    ActionsDiffBuilder b;
    ActionsDiff* d = b.GetRoot();
    d->start = 5;
    Action* claim = ActionsDiffActionsAdd(b.main, d->actions);
    claim->kind = ActionKind::SignInClaim;
    claim->request = 6;
    claim->session = 2;
    setText(b.main, claim->text, "w1");
    setText(b.main, claim->account, "g1");
    return saved(b, [](const ActionsDiffBuilder& x) { return ActionsDiffMeasure(x); },
        [](const ActionsDiffBuilder& x, std::uint8_t* out, int64_t n) { return ActionsDiffSave(x, out, n); });
}

Read mainReadSessions(const Bytes& bytes) {
    Read out;
    const int64_t need = SessionsDiffLoadMeasure(bytes.data(), static_cast<int64_t>(bytes.size()));
    if (need < 0) {
        out.refused = true;
        return out;
    }
    std::vector<std::uint8_t> region(static_cast<std::size_t>(need));
    TableReport report;
    const SessionsDiff* d = SessionsDiffLoad(region.data(), need, bytes.data(), static_cast<int64_t>(bytes.size()),
        &report);
    out.loaded = d != nullptr;
    out.unknown = report.unknown;
    out.damaged = report.damaged;
    out.refused = report.refused;
    if (d == nullptr) {
        return out;
    }
    out.sessions = static_cast<int>(d->sessions.size());
    for (const SignInOffer& o : d->signins) {
        out.signins.push_back(o.id);
        out.urls.push_back(bytesOf(o.url));
    }
    return out;
}

ReadActions mainReadActions(const Bytes& bytes) {
    ReadActions out;
    const int64_t need = ActionsDiffLoadMeasure(bytes.data(), static_cast<int64_t>(bytes.size()));
    if (need < 0) {
        out.refused = true;
        return out;
    }
    std::vector<std::uint8_t> region(static_cast<std::size_t>(need));
    TableReport report;
    const ActionsDiff* d = ActionsDiffLoad(region.data(), need, bytes.data(), static_cast<int64_t>(bytes.size()),
        &report);
    out.loaded = d != nullptr;
    out.unknown = report.unknown;
    out.damaged = report.damaged;
    out.refused = report.refused;
    if (d == nullptr) {
        return out;
    }
    out.start = d->start;
    for (const Action& a : d->actions) {
        out.kinds.push_back(EnumName(a.kind));
        out.sessions.push_back(a.session);
        out.accounts.push_back(textOf(a.account));
        out.texts.push_back(textOf(a.text));
        out.requests.push_back(a.request);
        out.secrets.push_back(bytesOf(a.secret));
    }
    return out;
}

} // namespace wiretest
