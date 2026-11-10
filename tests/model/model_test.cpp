// Unit tests of nexus::Model. Logic only: no time, no sockets, no network, no files.
//
//   model_test            runs every case
//   model_test <case>     runs one (ctest registers each case on its own)

#include "model.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

using nexus::Entry;
using nexus::EntryKind;
using nexus::FriendInfo;
using nexus::Model;
using nexus::Tab;

int failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

const std::size_t kHuge = std::numeric_limits<std::size_t>::max();

std::vector<FriendInfo> three() {
    return {{"ada", "m1"}, {"bo", "m2"}, {"cy", "m3"}};
}

bool same_entry(const Entry& a, const Entry& b) {
    return a.kind == b.kind && a.text == b.text;
}

bool same_tab(const Tab& a, const Tab& b) {
    if (a.name != b.name || a.model != b.model || a.draft != b.draft || a.transcript.size() != b.transcript.size())
        return false;
    for (std::size_t k = 0; k < a.transcript.size(); ++k) {
        if (!same_entry(a.transcript[k], b.transcript[k]))
            return false;
    }
    return true;
}

// every observable part of two models is equal
bool same(const Model& a, const Model& b) {
    if (a.tab_count() != b.tab_count() || a.active() != b.active())
        return false;
    for (std::size_t j = 0; j < a.tab_count(); ++j) {
        if (!same_tab(a.tab(j), b.tab(j)))
            return false;
    }
    return true;
}

void construct_one_tab_per_friend_in_order() {
    Model m(three());
    CHECK(m.tab_count() == 3);
    CHECK(m.active() == 0);
    CHECK(m.tab(0).name == "ada" && m.tab(0).model == "m1");
    CHECK(m.tab(1).name == "bo" && m.tab(1).model == "m2");
    CHECK(m.tab(2).name == "cy" && m.tab(2).model == "m3");
    for (std::size_t j = 0; j < 3; ++j) {
        CHECK(m.tab(j).transcript.empty());
        CHECK(m.tab(j).draft.empty());
    }
}

void empty_friend_list_refuses_everything() {
    Model m({});
    CHECK(m.tab_count() == 0);
    CHECK(m.active() == 0);
    CHECK(!m.select_tab(0));
    CHECK(!m.set_draft(0, "hi"));
    CHECK(!m.send(0));
    CHECK(!m.queue(0));
    CHECK(!m.append(0, Entry{EntryKind::Friend, "x"}));
    CHECK(m.tab_count() == 0);
    CHECK(m.active() == 0);
}

void select_in_range_changes_only_active() {
    Model m(three());
    m.set_draft(0, "keep");
    m.append(1, Entry{EntryKind::Friend, "hello"});
    Model before = m;
    CHECK(m.select_tab(2));
    CHECK(m.active() == 2);
    CHECK(same_tab(m.tab(0), before.tab(0)));
    CHECK(same_tab(m.tab(1), before.tab(1)));
    CHECK(same_tab(m.tab(2), before.tab(2)));
    CHECK(m.select_tab(2)); // selecting the active tab is in range: true, and nothing changes
    CHECK(m.active() == 2);
    CHECK(m.select_tab(0));
    CHECK(m.active() == 0);
}

void select_out_of_range_changes_nothing() {
    Model m(three());
    m.select_tab(1);
    Model before = m;
    CHECK(!m.select_tab(3));
    CHECK(!m.select_tab(4));
    CHECK(!m.select_tab(kHuge));
    CHECK(same(m, before));
}

void set_draft_changes_only_that_draft() {
    Model m(three());
    Model before = m;
    CHECK(m.set_draft(1, "hello"));
    CHECK(m.tab(1).draft == "hello");
    CHECK(m.active() == 0);
    CHECK(same_tab(m.tab(0), before.tab(0)));
    CHECK(same_tab(m.tab(2), before.tab(2)));
    CHECK(m.set_draft(1, "")); // clearing is a write like any other
    CHECK(m.tab(1).draft.empty());
}

void set_draft_out_of_range_changes_nothing() {
    Model m(three());
    m.set_draft(0, "a");
    Model before = m;
    CHECK(!m.set_draft(3, "x"));
    CHECK(!m.set_draft(kHuge, "x"));
    CHECK(same(m, before));
}

void send_appends_person_and_clears_that_draft() {
    Model m(three());
    m.set_draft(1, "hello there");
    m.set_draft(2, "other");
    CHECK(m.send(1));
    CHECK(m.tab(1).transcript.size() == 1);
    CHECK(same_entry(m.tab(1).transcript[0], Entry{EntryKind::Person, "hello there"}));
    CHECK(m.tab(1).draft.empty());
    CHECK(m.tab(2).draft == "other");
    CHECK(m.tab(2).transcript.empty());
    CHECK(m.tab(0).transcript.empty());
    CHECK(m.active() == 0); // the named tab, not the active one
}

void queue_appends_queued() {
    Model m(three());
    m.set_draft(0, "later");
    CHECK(m.queue(0));
    CHECK(m.tab(0).transcript.size() == 1);
    CHECK(same_entry(m.tab(0).transcript[0], Entry{EntryKind::Queued, "later"}));
    CHECK(m.tab(0).draft.empty());
}

void send_keeps_surrounding_whitespace() {
    Model m(three());
    m.set_draft(0, "  hi\n");
    CHECK(m.send(0));
    CHECK(m.tab(0).transcript[0].text == "  hi\n");
}

void whitespace_drafts_are_refused() {
    const std::vector<std::string> blanks = {"", " ", "\t", "\n", "\r", "\f", "\v", " \t\n\r\f\v  "};
    for (const std::string& b : blanks) {
        Model m(three());
        m.set_draft(1, b);
        Model before = m;
        CHECK(!m.send(1));
        CHECK(!m.queue(1));
        CHECK(same(m, before)); // the draft stays as it was
    }
}

void non_ascii_whitespace_is_text() {
    // Only ASCII whitespace counts as whitespace; a no-break space (U+00A0) is text.
    Model m(three());
    m.set_draft(0, "\xC2\xA0");
    CHECK(m.send(0));
    // a single non-whitespace byte among whitespace is text
    m.set_draft(0, " \t.\n");
    CHECK(m.queue(0));
    CHECK(m.tab(0).transcript.size() == 2);
}

void send_out_of_range_changes_nothing() {
    Model m(three());
    m.set_draft(2, "hi");
    Model before = m;
    CHECK(!m.send(3));
    CHECK(!m.queue(3));
    CHECK(!m.send(kHuge));
    CHECK(!m.queue(kHuge));
    CHECK(same(m, before));
}

void append_adds_that_entry_to_that_tab() {
    Model m(three());
    m.set_draft(2, "draft stays");
    CHECK(m.append(2, Entry{EntryKind::Friend, "reply"}));
    CHECK(m.append(2, Entry{EntryKind::Notice, ""}));
    CHECK(m.append(2, Entry{EntryKind::Person, " "})); // append takes any entry; only send and queue refuse whitespace
    CHECK(m.tab(2).transcript.size() == 3);
    CHECK(same_entry(m.tab(2).transcript[0], Entry{EntryKind::Friend, "reply"}));
    CHECK(same_entry(m.tab(2).transcript[1], Entry{EntryKind::Notice, ""}));
    CHECK(same_entry(m.tab(2).transcript[2], Entry{EntryKind::Person, " "}));
    CHECK(m.tab(2).draft == "draft stays");
    CHECK(m.tab(0).transcript.empty() && m.tab(1).transcript.empty());
}

void append_out_of_range_changes_nothing() {
    Model m(three());
    Model before = m;
    CHECK(!m.append(3, Entry{EntryKind::Friend, "x"}));
    CHECK(!m.append(kHuge, Entry{EntryKind::Friend, "x"}));
    CHECK(same(m, before));
}

void transcripts_only_grow() {
    Model m(three());
    m.set_draft(0, "one");
    m.send(0);
    m.append(0, Entry{EntryKind::Friend, "two"});
    m.set_draft(0, "three");
    m.queue(0);
    const std::vector<Entry> first(m.tab(0).transcript);
    m.set_draft(0, "four");
    m.send(0);
    m.select_tab(1);
    m.set_draft(0, " ");
    m.send(0);
    CHECK(m.tab(0).transcript.size() == 4);
    for (std::size_t k = 0; k < first.size(); ++k)
        CHECK(same_entry(m.tab(0).transcript[k], first[k]));
    CHECK(same_entry(m.tab(0).transcript[3], Entry{EntryKind::Person, "four"}));
}

void large_transcript_keeps_order() {
    Model m({{"ada", "m1"}, {"bo", "m2"}});
    const std::size_t n = 100000;
    for (std::size_t k = 0; k < n; ++k) {
        if (k % 2 == 0) {
            m.set_draft(0, std::to_string(k));
            CHECK(m.send(0));
        } else {
            CHECK(m.append(0, Entry{EntryKind::Friend, std::to_string(k)}));
        }
    }
    CHECK(m.tab(0).transcript.size() == n);
    bool ordered = true;
    for (std::size_t k = 0; k < n; ++k) {
        const Entry& e = m.tab(0).transcript[k];
        ordered = ordered && e.text == std::to_string(k) && e.kind == (k % 2 == 0 ? EntryKind::Person : EntryKind::Friend);
    }
    CHECK(ordered);
    CHECK(m.tab(1).transcript.empty());
}

void large_draft_round_trips() {
    Model m(three());
    std::string big(1 << 20, 'x');
    big[12345] = '\0'; // embedded NUL is text, not an end
    CHECK(m.set_draft(0, big));
    CHECK(m.tab(0).draft == big);
    CHECK(m.send(0));
    CHECK(m.tab(0).transcript[0].text == big);
}

struct Case {
    const char* name;
    void (*fn)();
};

const Case kCases[] = {
    {"construct_one_tab_per_friend_in_order", construct_one_tab_per_friend_in_order},
    {"empty_friend_list_refuses_everything", empty_friend_list_refuses_everything},
    {"select_in_range_changes_only_active", select_in_range_changes_only_active},
    {"select_out_of_range_changes_nothing", select_out_of_range_changes_nothing},
    {"set_draft_changes_only_that_draft", set_draft_changes_only_that_draft},
    {"set_draft_out_of_range_changes_nothing", set_draft_out_of_range_changes_nothing},
    {"send_appends_person_and_clears_that_draft", send_appends_person_and_clears_that_draft},
    {"queue_appends_queued", queue_appends_queued},
    {"send_keeps_surrounding_whitespace", send_keeps_surrounding_whitespace},
    {"whitespace_drafts_are_refused", whitespace_drafts_are_refused},
    {"non_ascii_whitespace_is_text", non_ascii_whitespace_is_text},
    {"send_out_of_range_changes_nothing", send_out_of_range_changes_nothing},
    {"append_adds_that_entry_to_that_tab", append_adds_that_entry_to_that_tab},
    {"append_out_of_range_changes_nothing", append_out_of_range_changes_nothing},
    {"transcripts_only_grow", transcripts_only_grow},
    {"large_transcript_keeps_order", large_transcript_keeps_order},
    {"large_draft_round_trips", large_draft_round_trips},
};

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::fprintf(stderr, "usage: model_test [case]\n");
        return 2;
    }
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const Case& c : kCases)
            std::printf("%s\n", c.name);
        return 0;
    }
    std::size_t ran = 0;
    for (const Case& c : kCases) {
        if (argc == 2 && std::strcmp(argv[1], c.name) != 0)
            continue;
        const int before = failures;
        c.fn();
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", c.name);
        ++ran;
    }
    if (ran == 0) {
        std::fprintf(stderr, "model_test: no case named %s (--list lists them)\n", argc == 2 ? argv[1] : "");
        return 2;
    }
    return failures == 0 ? 0 : 1;
}
