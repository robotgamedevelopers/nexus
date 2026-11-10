// Unit tests of nexus::Model. Logic only: no time, no sockets, no network, no files.
//
//   model_test            runs every case
//   model_test <case>     runs one, by hand (ctest runs them all as the one test model_unit)

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
    if (a.id != b.id || a.kind != b.kind || a.name != b.name || a.model != b.model || a.draft != b.draft ||
            a.queue != b.queue || a.transcript.size() != b.transcript.size())
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
        CHECK(m.tab(j).queue.empty());
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
    CHECK(!m.move_tab(0, 0));
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
    for (std::size_t j = 0; j < 3; ++j)
        CHECK(m.tab(j).queue.empty()); // a send never touches a queue
}

void queue_appends_to_that_tabs_queue_not_the_transcript() {
    Model m(three());
    m.append(1, Entry{EntryKind::Friend, "earlier"});
    m.set_draft(2, "other");
    m.set_draft(1, "later");
    CHECK(m.queue(1));
    m.set_draft(1, "after that");
    CHECK(m.queue(1));
    CHECK((m.tab(1).queue == std::vector<std::string>{"later", "after that"})); // in order
    CHECK(m.tab(1).draft.empty());
    CHECK(m.tab(1).transcript.size() == 1);   // the transcript is as it was
    CHECK(same_entry(m.tab(1).transcript[0], Entry{EntryKind::Friend, "earlier"}));
    CHECK(m.tab(0).queue.empty() && m.tab(2).queue.empty());
    CHECK(m.tab(2).draft == "other");
    CHECK(m.active() == 0);
}

void only_queue_touches_a_queue() {
    Model m(three());
    m.set_draft(0, "held");
    m.queue(0);
    const std::vector<std::string> held = m.tab(0).queue;
    m.set_draft(0, "now");
    m.send(0);
    m.append(0, Entry{EntryKind::Friend, "reply"});
    m.select_tab(2);
    m.set_draft(0, "draft");
    CHECK(m.tab(0).queue == held);
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
    CHECK(m.tab(0).transcript.size() == 1);
    CHECK(m.tab(0).queue.size() == 1);
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
    m.queue(0);
    CHECK(m.tab(0).transcript.size() == 3);
    for (std::size_t k = 0; k < first.size(); ++k)
        CHECK(same_entry(m.tab(0).transcript[k], first[k]));
    CHECK(same_entry(m.tab(0).transcript[2], Entry{EntryKind::Person, "four"}));
    CHECK((m.tab(0).queue == std::vector<std::string>{"three"}));
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

// the friends' names in tab order, joined by spaces
std::string names(const Model& m) {
    std::string out;
    for (std::size_t j = 0; j < m.tab_count(); ++j)
        out += (j ? " " : "") + m.tab(j).name;
    return out;
}

std::vector<FriendInfo> friendsOne() {
    return {{"ada", "m1"}};
}

std::vector<FriendInfo> five() {
    return {{"a", "m"}, {"b", "m"}, {"c", "m"}, {"d", "m"}, {"e", "m"}};
}

void construct_gives_each_tab_its_friends_id() {
    Model m(three());
    for (std::size_t j = 0; j < 3; ++j)
        CHECK(m.tab(j).id == j);
}

void move_first_to_last_and_last_to_first() {
    Model m(five());
    CHECK(m.move_tab(0, 4));
    CHECK(names(m) == "b c d e a");
    CHECK(m.move_tab(4, 0));
    CHECK(names(m) == "a b c d e");
    CHECK(m.move_tab(4, 0));
    CHECK(names(m) == "e a b c d");
}

void move_shifts_the_tabs_between() {
    Model m(five());
    CHECK(m.move_tab(1, 3));    // b to position 3: c and d shift left
    CHECK(names(m) == "a c d b e");
    CHECK(m.move_tab(3, 1));    // and back
    CHECK(names(m) == "a b c d e");
    CHECK(m.move_tab(3, 0));    // d to the front: a, b, c shift right
    CHECK(names(m) == "d a b c e");
}

void move_adjacent_swaps() {
    Model m(three());
    CHECK(m.move_tab(0, 1));
    CHECK(names(m) == "bo ada cy");
    CHECK(m.move_tab(2, 1));
    CHECK(names(m) == "bo cy ada");
    CHECK(m.move_tab(1, 2));
    CHECK(names(m) == "bo ada cy");
}

void move_to_itself_is_true_and_changes_nothing() {
    Model m(three());
    m.set_draft(1, "x");
    m.select_tab(1);
    Model before = m;
    for (std::size_t j = 0; j < 3; ++j)
        CHECK(m.move_tab(j, j));
    CHECK(same(m, before));
}

// Everything a tab holds goes with it: id, name, model, transcript, draft and queue.
void move_carries_everything_the_tab_holds() {
    Model m(three());
    for (std::size_t j = 0; j < 3; ++j) {
        m.append(j, Entry{EntryKind::Friend, "reply " + std::to_string(j)});
        m.set_draft(j, "held " + std::to_string(j));
        m.queue(j);
        m.set_draft(j, "draft " + std::to_string(j));
    }
    const Model before = m;
    CHECK(m.move_tab(2, 0));
    CHECK(same_tab(m.tab(0), before.tab(2)));
    CHECK(same_tab(m.tab(1), before.tab(0)));
    CHECK(same_tab(m.tab(2), before.tab(1)));
    // and operations by position now address the friend standing there
    CHECK(m.send(0));
    CHECK(m.tab(0).name == "cy" && m.tab(0).transcript.back().text == "draft 2");
    CHECK(before.tab(0).transcript.size() == m.tab(1).transcript.size());
}

// The active tab stays the same friend, wherever it moves and whatever moves around it.
void move_keeps_the_active_friend() {
    for (std::size_t from = 0; from < 5; ++from) {
        for (std::size_t to = 0; to < 5; ++to) {
            for (std::size_t a = 0; a < 5; ++a) {
                Model m(five());
                m.select_tab(a);
                const std::size_t id = m.tab(a).id;
                CHECK(m.move_tab(from, to));
                CHECK(m.tab(m.active()).id == id);
            }
        }
    }
}

void repeated_moves_are_a_permutation() {
    Model m(five());
    m.select_tab(2);
    const std::size_t moves[][2] = {{0, 4}, {3, 1}, {4, 0}, {2, 2}, {1, 3}, {0, 1}, {4, 2}, {2, 0}, {1, 4}};
    for (const auto& mv : moves)
        CHECK(m.move_tab(mv[0], mv[1]));
    std::vector<bool> seen(5, false);
    for (std::size_t j = 0; j < 5; ++j) {
        CHECK(m.tab(j).id < 5 && !seen[m.tab(j).id]);
        seen[m.tab(j).id] = true;
        CHECK(m.tab(j).name == std::string(1, char('a' + m.tab(j).id)));
    }
    CHECK(m.tab(m.active()).name == "c");
}

void move_out_of_range_changes_nothing() {
    Model m(three());
    m.set_draft(0, "a");
    m.select_tab(2);
    Model before = m;
    CHECK(!m.move_tab(3, 0));
    CHECK(!m.move_tab(0, 3));
    CHECK(!m.move_tab(3, 3));
    CHECK(!m.move_tab(kHuge, 0));
    CHECK(!m.move_tab(0, kHuge));
    CHECK(!m.move_tab(kHuge, kHuge));
    CHECK(same(m, before));
}

void move_with_one_tab_or_none() {
    Model one({{"ada", "m1"}});
    one.set_draft(0, "x");
    Model before = one;
    CHECK(one.move_tab(0, 0));
    CHECK(!one.move_tab(0, 1));
    CHECK(!one.move_tab(1, 0));
    CHECK(same(one, before));
    Model none({});
    CHECK(!none.move_tab(0, 0));
    CHECK(!none.move_tab(kHuge, kHuge));
    CHECK(none.tab_count() == 0 && none.active() == 0);
}

// ---- Terminal tabs ----

using nexus::TabKind;

// the tabs' ids in order, joined by spaces
std::string ids(const Model& m) {
    std::string out;
    for (std::size_t j = 0; j < m.tab_count(); ++j)
        out += (j ? " " : "") + std::to_string(m.tab(j).id);
    return out;
}

void add_terminal_appends_selects_and_returns_a_new_id() {
    Model m(three());
    m.set_draft(1, "kept");
    const Model before = m;
    CHECK(m.add_terminal() == 3);
    CHECK(m.tab_count() == 4);
    CHECK(m.active() == 3);
    CHECK(m.tab(3).id == 3 && m.tab(3).kind == TabKind::Terminal);
    CHECK(m.tab(3).name.empty() && m.tab(3).transcript.empty() && m.tab(3).draft.empty() && m.tab(3).queue.empty());
    for (std::size_t j = 0; j < 3; ++j)
        CHECK(same_tab(m.tab(j), before.tab(j)));
    CHECK(m.add_terminal() == 4);
    CHECK(ids(m) == "0 1 2 3 4" && m.active() == 4);
}

void ids_are_never_reused() {
    Model m(three());
    CHECK(m.add_terminal() == 3);
    CHECK(m.add_terminal() == 4);
    CHECK(m.close_tab(4));
    CHECK(m.close_tab(3));
    CHECK(m.add_terminal() == 5);
    CHECK(ids(m) == "0 1 2 5");
    Model none({});
    CHECK(none.add_terminal() == 0);
    CHECK(none.close_tab(0));
    CHECK(none.add_terminal() == 1);
}

void close_refuses_a_friend_and_out_of_range() {
    Model m(three());
    m.add_terminal();
    m.set_draft(0, "x");
    m.select_tab(1);
    const Model before = m;
    CHECK(!m.close_tab(0));
    CHECK(!m.close_tab(2));
    CHECK(!m.close_tab(4));
    CHECK(!m.close_tab(kHuge));
    CHECK(same(m, before));
}

void close_removes_exactly_that_tab() {
    Model m(friendsOne());
    m.add_terminal();   // id 1
    m.add_terminal();   // id 2
    m.add_terminal();   // id 3
    m.select_tab(0);
    CHECK(m.close_tab(2));
    CHECK(ids(m) == "0 1 3");
    CHECK(m.active() == 0);   // another tab was active: it stays active
    m.select_tab(2);          // id 3
    CHECK(m.close_tab(1));
    CHECK(ids(m) == "0 3");
    CHECK(m.active() == 1 && m.tab(1).id == 3);   // the active tab, shifted left with it
}

void close_active_takes_the_next_or_the_last() {
    Model m(friendsOne());
    m.add_terminal();   // 1
    m.add_terminal();   // 2
    m.add_terminal();   // 3
    m.select_tab(2);    // id 2, in the middle
    CHECK(m.close_tab(2));
    CHECK(ids(m) == "0 1 3" && m.active() == 2 && m.tab(2).id == 3);   // the tab now at its position
    CHECK(m.close_tab(2));   // the active tab is the last
    CHECK(ids(m) == "0 1" && m.active() == 1);
    CHECK(m.close_tab(1));
    CHECK(ids(m) == "0" && m.active() == 0);
    Model none({});
    none.add_terminal();
    CHECK(none.close_tab(0));
    CHECK(none.tab_count() == 0 && none.active() == 0);   // none left
}

void a_terminal_takes_no_text() {
    Model m(three());
    m.set_draft(0, "friend's");
    m.add_terminal();
    const Model before = m;
    CHECK(!m.set_draft(3, "hi"));
    CHECK(!m.send(3));
    CHECK(!m.queue(3));
    CHECK(!m.append(3, Entry{EntryKind::Friend, "x"}));
    CHECK(same(m, before));
    CHECK(m.select_tab(3) && m.select_tab(0));   // select treats every kind alike
}

void moves_stay_in_their_zone() {
    Model m(three());
    m.add_terminal();   // 3
    m.add_terminal();   // 4
    m.select_tab(1);
    const Model before = m;
    CHECK(!m.move_tab(3, 0));   // a terminal before a friend
    CHECK(!m.move_tab(4, 2));   // a terminal between friends
    CHECK(!m.move_tab(0, 3));   // a friend behind a terminal
    CHECK(!m.move_tab(2, 4));
    CHECK(same(m, before));
    CHECK(m.move_tab(4, 3));    // terminals among themselves
    CHECK(ids(m) == "0 1 2 4 3");
    CHECK(m.move_tab(0, 2));    // friends among themselves
    CHECK(ids(m) == "1 2 0 4 3");
    CHECK(m.tab(m.active()).id == 1);
}

// ---- The sprint tab ----

void sprint_tab_is_first_and_active_with_the_friends_after() {
    Model m(three(), true);
    CHECK(m.tab_count() == 4);
    CHECK(m.active() == 0);
    CHECK(m.tab(0).kind == TabKind::Sprint && m.tab(0).id == 3);
    CHECK(m.tab(0).name.empty() && m.tab(0).model.empty() && m.tab(0).transcript.empty() && m.tab(0).queue.empty());
    CHECK(ids(m) == "3 0 1 2");
    CHECK(m.tab(1).name == "ada" && m.tab(2).name == "bo" && m.tab(3).name == "cy");
    CHECK(m.tab(1).kind == TabKind::Friend);
}

void sprint_tab_takes_no_text() {
    Model m(three(), true);
    m.set_draft(1, "ada's");
    const Model before = m;
    CHECK(!m.set_draft(0, "hi"));
    CHECK(!m.send(0));
    CHECK(!m.queue(0));
    CHECK(!m.append(0, Entry{EntryKind::Friend, "x"}));
    CHECK(same(m, before));
    CHECK(m.send(1));   // the friend after it still sends
}

void sprint_tab_never_moves() {
    Model m(three(), true);
    m.add_terminal();   // 4
    m.select_tab(2);
    const Model before = m;
    CHECK(!m.move_tab(0, 1));   // dragged onto a friend
    CHECK(!m.move_tab(0, 3));
    CHECK(!m.move_tab(0, 4));   // onto a terminal
    CHECK(!m.move_tab(1, 0));   // a friend dropped in front of it
    CHECK(!m.move_tab(4, 0));   // a terminal dropped in front of it
    CHECK(same(m, before));
    CHECK(m.move_tab(0, 0));    // onto itself: in its zone, and nothing changes
    CHECK(same(m, before));
    CHECK(m.move_tab(1, 3));    // the friends still move among themselves behind it
    CHECK(ids(m) == "3 1 2 0 4" && m.tab(m.active()).id == 1);
}

void sprint_tab_never_closes() {
    Model m(three(), true);
    const Model before = m;
    CHECK(!m.close_tab(0));
    CHECK(same(m, before));
    CHECK(m.add_terminal() == 4);   // the first id after the friends' and the sprint tab's
    CHECK(m.add_terminal() == 5);
    CHECK(m.close_tab(5) && m.close_tab(4));
    CHECK(ids(m) == "3 0 1 2" && m.active() == 3);
    CHECK(!m.close_tab(0));
}

void sprint_tab_alone_without_friends() {
    Model m({}, true);
    CHECK(m.tab_count() == 1 && m.tab(0).kind == TabKind::Sprint && m.tab(0).id == 0 && m.active() == 0);
    CHECK(m.add_terminal() == 1);
    CHECK(ids(m) == "0 1" && m.active() == 1);
    CHECK(m.close_tab(1));
    CHECK(ids(m) == "0" && m.active() == 0);   // closing the last terminal leaves the sprint tab active
    CHECK(!m.close_tab(0) && !m.set_draft(0, "x") && m.select_tab(0));
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
    {"queue_appends_to_that_tabs_queue_not_the_transcript", queue_appends_to_that_tabs_queue_not_the_transcript},
    {"only_queue_touches_a_queue", only_queue_touches_a_queue},
    {"send_keeps_surrounding_whitespace", send_keeps_surrounding_whitespace},
    {"whitespace_drafts_are_refused", whitespace_drafts_are_refused},
    {"non_ascii_whitespace_is_text", non_ascii_whitespace_is_text},
    {"send_out_of_range_changes_nothing", send_out_of_range_changes_nothing},
    {"append_adds_that_entry_to_that_tab", append_adds_that_entry_to_that_tab},
    {"append_out_of_range_changes_nothing", append_out_of_range_changes_nothing},
    {"transcripts_only_grow", transcripts_only_grow},
    {"large_transcript_keeps_order", large_transcript_keeps_order},
    {"large_draft_round_trips", large_draft_round_trips},
    {"construct_gives_each_tab_its_friends_id", construct_gives_each_tab_its_friends_id},
    {"move_first_to_last_and_last_to_first", move_first_to_last_and_last_to_first},
    {"move_shifts_the_tabs_between", move_shifts_the_tabs_between},
    {"move_adjacent_swaps", move_adjacent_swaps},
    {"move_to_itself_is_true_and_changes_nothing", move_to_itself_is_true_and_changes_nothing},
    {"move_carries_everything_the_tab_holds", move_carries_everything_the_tab_holds},
    {"move_keeps_the_active_friend", move_keeps_the_active_friend},
    {"repeated_moves_are_a_permutation", repeated_moves_are_a_permutation},
    {"move_out_of_range_changes_nothing", move_out_of_range_changes_nothing},
    {"move_with_one_tab_or_none", move_with_one_tab_or_none},
    {"add_terminal_appends_selects_and_returns_a_new_id", add_terminal_appends_selects_and_returns_a_new_id},
    {"ids_are_never_reused", ids_are_never_reused},
    {"close_refuses_a_friend_and_out_of_range", close_refuses_a_friend_and_out_of_range},
    {"close_removes_exactly_that_tab", close_removes_exactly_that_tab},
    {"close_active_takes_the_next_or_the_last", close_active_takes_the_next_or_the_last},
    {"a_terminal_takes_no_text", a_terminal_takes_no_text},
    {"moves_stay_in_their_zone", moves_stay_in_their_zone},
    {"sprint_tab_is_first_and_active_with_the_friends_after", sprint_tab_is_first_and_active_with_the_friends_after},
    {"sprint_tab_takes_no_text", sprint_tab_takes_no_text},
    {"sprint_tab_never_moves", sprint_tab_never_moves},
    {"sprint_tab_never_closes", sprint_tab_never_closes},
    {"sprint_tab_alone_without_friends", sprint_tab_alone_without_friends},
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
