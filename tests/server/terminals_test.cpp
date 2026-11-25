// The server's terminals (docs/terminals.md), logic only: no processes, files, sockets or clock.
//
//   book    server/terminal_book.h: every action and every refusal, the epoch in each id, the owner's gate
//   modes   server/terminal_screen.h TerminalModes: set and reset, split sequences, strings skipped, the resets
//   rows    server/terminal_screen.h paintRow over a real lib/vterm fed bytes: colours, attributes, wide clusters,
//           trailing blanks, controls never written
//   state   sync/terminal_state.h in the sessions state: a diff carries only the rows that changed, a window's copy
//           comes out equal, every malformed diff is refused, a state file holds no screen, and each fate
//   paint   sync/terminal_paint.h: what the window writes into its own emulator
//
//   terminals_test <group>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "pr/vterm.h"
#include "server/terminal_book.h"
#include "server/terminal_screen.h"
#include "sync/sessions.h"
#include "sync/terminal_paint.h"
#include "sync/terminal_state.h"
#include "sync/wire.h"

namespace {

using namespace nexus;
using server::TerminalBook;
using server::TerminalEffect;
using sync::Action;
using sync::ActionKind;

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                            \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

Action create(std::string opener = "w1", std::uint32_t columns = 100, std::uint32_t rows = 30) {
    Action a;
    a.kind = ActionKind::TerminalCreate;
    a.text = std::move(opener);
    a.columns = columns;
    a.rows = rows;
    return a;
}

Action on(ActionKind kind, sync::TerminalId id, std::string input = {}) {
    Action a;
    a.kind = kind;
    a.terminal = id;
    a.secret = sync::SecretValue(input);
    return a;
}

void testBook() {
    TerminalBook book(7);
    // Only the owner's link gets a shell.
    auto o = book.apply(create(), false);
    CHECK(contains(o.refused, "owner"));
    CHECK(o.effect.kind == TerminalEffect::Kind::None);
    CHECK(book.terminals().empty());

    o = book.apply(create("w1", 100, 30), true);
    CHECK(o.refused.empty());
    CHECK(o.effect.kind == TerminalEffect::Kind::Spawn);
    const sync::TerminalId id = o.effect.id;
    CHECK((id >> 32) == 7);         // the epoch is in the id
    CHECK((id & 0xffffffffu) == 1);
    CHECK(o.effect.columns == 100 && o.effect.rows == 30);
    CHECK(book.find(id) && book.find(id)->opener == "w1" && book.find(id)->state == TerminalBook::State::Running);

    // A create that names no screen gets 80 by 24; one past the bounds is held to them.
    auto d = book.apply(create("w2", 0, 0), true);
    CHECK(d.effect.columns == server::kDefaultTerminalColumns && d.effect.rows == server::kDefaultTerminalRows);
    auto big = book.apply(create("w3", 100000, 100000), true);
    CHECK(big.effect.columns == sync::kMaxTerminalColumns && big.effect.rows == sync::kMaxTerminalRows);

    // Input to a running terminal is written; empty input does nothing; too much is refused whole.
    o = book.apply(on(ActionKind::TerminalInput, id, "ls\r"), true);
    CHECK(o.refused.empty() && o.effect.kind == TerminalEffect::Kind::Write && o.effect.id == id);
    o = book.apply(on(ActionKind::TerminalInput, id, ""), true);
    CHECK(o.refused.empty() && o.effect.kind == TerminalEffect::Kind::None);
    o = book.apply(on(ActionKind::TerminalInput, id, std::string(sync::kMaxTerminalInput + 1, 'x')), true);
    CHECK(!o.refused.empty() && o.effect.kind == TerminalEffect::Kind::None);
    // A refusal never holds the input.
    o = book.apply(on(ActionKind::TerminalInput, 12345, "hunter2"), true);
    CHECK(!o.refused.empty() && !contains(o.refused, "hunter2"));

    // Resize, bounded.
    Action r = on(ActionKind::TerminalResize, id);
    r.columns = 120;
    r.rows = 0;     // 0 keeps what it was
    o = book.apply(r, true);
    CHECK(o.effect.kind == TerminalEffect::Kind::Resize && o.effect.columns == 120 && o.effect.rows == 30);

    // Its shell ends: shown, never silent; input is refused; a resize still taken; the end reported once.
    book.ended(id, server::endedLine(false, 3));
    CHECK(book.find(id)->state == TerminalBook::State::Ended && book.find(id)->ended == "exited with code 3");
    book.ended(id, server::endedLine(true, 9));
    CHECK(book.find(id)->ended == "exited with code 3");
    o = book.apply(on(ActionKind::TerminalInput, id, "x"), true);
    CHECK(contains(o.refused, "ended") && o.effect.kind == TerminalEffect::Kind::None);
    CHECK(server::endedLine(true, 9) == "ended by signal 9");
    book.ended(d.effect.id, "");
    CHECK(book.find(d.effect.id)->ended == "ended");

    // Close: an ended one is removed with nothing to kill; a running one is killed; a second close is refused.
    o = book.apply(on(ActionKind::TerminalClose, id), true);
    CHECK(o.refused.empty() && o.effect.kind == TerminalEffect::Kind::None && !book.find(id));
    o = book.apply(on(ActionKind::TerminalClose, big.effect.id), true);
    CHECK(o.refused.empty() && o.effect.kind == TerminalEffect::Kind::Kill && !book.find(big.effect.id));
    o = book.apply(on(ActionKind::TerminalClose, big.effect.id), true);
    CHECK(contains(o.refused, "no terminal"));
    // A closed terminal never receives input.
    o = book.apply(on(ActionKind::TerminalInput, big.effect.id, "x"), true);
    CHECK(!o.refused.empty() && o.effect.kind == TerminalEffect::Kind::None);

    // An id of another run (another epoch) is never this run's: its input is refused, saying the server restarted.
    TerminalBook next(8);
    o = next.apply(on(ActionKind::TerminalInput, id, "x"), true);
    CHECK(contains(o.refused, "restarted") && o.effect.kind == TerminalEffect::Kind::None);
    auto fresh = next.apply(create(), true);
    CHECK(fresh.effect.id != id && (fresh.effect.id >> 32) == 8);

    // At most kMaxTerminals at once.
    TerminalBook full(9);
    for (std::size_t i = 0; i < sync::kMaxTerminals; ++i) {
        CHECK(full.apply(create(), true).refused.empty());
    }
    o = full.apply(create(), true);
    CHECK(contains(o.refused, "at most"));

    // Not a terminal's action.
    Action send;
    send.kind = ActionKind::Send;
    CHECK(!full.apply(send, true).refused.empty());
    bool threw = false;
    try {
        TerminalBook zero(0);
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
}

void testModes() {
    server::TerminalModes m;
    m.feed("\x1b[?1h\x1b[?2004h");
    CHECK(m.text() == "1,2004");
    m.feed("\x1b[?1l");
    CHECK(m.text() == "2004");
    // Split across reads.
    m.feed("\x1b[?10");
    m.feed("00;1006h");
    CHECK(m.text() == "1000,1006,2004");
    // A mode that is not an input mode is not kept (the alternate screen).
    m.feed("\x1b[?1049h\x1b[?25l");
    CHECK(m.text() == "1000,1006,2004");
    // The keypad.
    m.feed("\x1b=");
    CHECK(m.text() == "66,1000,1006,2004");
    m.feed("\x1b>");
    CHECK(m.text() == "1000,1006,2004");
    // A string's bytes never read as a mode, whether it ends with BEL or ST.
    m.feed("\x1b]0;\x1b[?1h title\x07");
    m.feed("\x1bP\x1b[?66h\x1b\\");
    CHECK(m.text() == "1000,1006,2004");
    // CAN cancels a sequence.
    m.feed("\x1b[?1\x18h");
    CHECK(m.text() == "1000,1006,2004");
    // DECSTR and RIS reset all.
    m.feed("\x1b[!p");
    CHECK(m.text().empty());
    m.feed("\x1b[?1;2004h");
    CHECK(m.text() == "1,2004");
    m.feed("\x1b" "c");
    CHECK(m.text().empty());
    // An intermediate byte makes it another sequence; a parameter run past the bound is dropped.
    m.feed("\x1b[?1$h");
    CHECK(m.text().empty());
    m.feed("\x1b[?" + std::string(200, '1') + "h");
    CHECK(m.text().empty());
}

struct Vterm {
    PrVterm* v;
    explicit Vterm(std::uint32_t columns = 20, std::uint32_t rows = 4) {
        PrVtermDesc desc{};
        desc.extent = PrUVec2{columns, rows};
        v = prVtermCreate(&desc);
    }
    ~Vterm() { prVtermDestroy(v); }
    void write(const std::string& s) {
        prVtermWrite(v, PrByteView{reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
    }
};

void testRows() {
    Vterm t;
    t.write("plain   ");
    CHECK(server::paintRow(t.v, 0) == "plain");     // trailing blanks with the default pen left out
    CHECK(server::paintRow(t.v, 1).empty());
    CHECK(server::paintRow(t.v, 9).empty());        // past the screen
    t.write("\r\n\x1b[1;31mred\x1b[0m \x1b[38;2;1;2;3;48;5;200mrgb\x1b[0m");
    const std::string row = server::paintRow(t.v, 1);
    CHECK(row == "\x1b[0;1;31mred\x1b[0m \x1b[0;38;2;1;2;3;48;5;200mrgb\x1b[0m");
    CHECK(sync::cleanRow(row));
    // A background colour past the text is kept (the cells are not blank).
    t.write("\r\n\x1b[44m  \x1b[0m");
    CHECK(server::paintRow(t.v, 2) == "\x1b[0;44m  \x1b[0m");
    // A wide cluster once, its tail nothing; a combining mark with its base.
    Vterm w;
    w.write("\xe4\xb8\xad" "e\xcc\x81");
    CHECK(server::paintRow(w.v, 0) == "\xe4\xb8\xad" "e\xcc\x81");
    // Controls never reach a row: lib/vterm acts on them, and paintRow writes what is on the screen.
    Vterm c;
    c.write("a\x07" "b\x1b]0;t\x07" "c");
    CHECK(server::paintRow(c.v, 0) == "abc");
    CHECK(sync::cleanRow(server::paintRow(c.v, 0)));
    // Underline styles and other attributes.
    Vterm u;
    u.write("\x1b[4:3;3;9;53mx");
    CHECK(server::paintRow(u.v, 0) == "\x1b[0;3;4:3;9;53mx\x1b[0m");
}

sync::TerminalRef view(sync::TerminalId id, std::vector<std::string> rows, std::string ended = {}) {
    auto v = std::make_shared<sync::TerminalView>();
    v->id = id;
    v->opener = "w1";
    v->title = "zsh";
    v->columns = 20;
    v->rows = static_cast<std::uint32_t>(rows.size());
    v->cursorColumn = 2;
    v->cursorRow = 1;
    v->modes = "1";
    v->ended = std::move(ended);
    for (auto& r : rows) {
        v->lines.push_back(std::make_shared<const std::string>(std::move(r)));
    }
    return v;
}

void testState() {
    const sync::TerminalId id = (sync::TerminalId(5) << 32) | 1;
    sync::Sessions server;
    server.setTerminals(true, 5, {view(id, {"one", "two", "three"})});
    sync::Sessions window;
    CHECK(!window.terminalsServed());
    window.apply_string(server.init_diff());
    CHECK(window == server);
    CHECK(window.terminalsServed() && window.terminalEpoch() == 5);
    CHECK(window.findTerminal(id) && window.findTerminal(id)->title == "zsh");

    // One row changes: the diff carries it and not the others.
    sync::Sessions later = server;
    later.setTerminals(true, 5, {view(id, {"one", "TWO-CHANGED", "three"})});
    const std::string diff = later.diff_from(server);
    CHECK(contains(diff, "TWO-CHANGED"));
    CHECK(!contains(diff, "three"));
    window.apply_string(diff);
    CHECK(window == later);
    CHECK(*window.findTerminal(id)->lines[2] == "three");
    // Equal states: no diff at all.
    CHECK(later.diff_from(later).empty());

    // A window of format 3 is sent no terminals, and holds none.
    sync::Sessions old;
    old.setFormat(3);
    sync::Sessions oldServer = later;
    oldServer.setFormat(3);
    old.apply_string(oldServer.init_diff());
    CHECK(!old.terminalsServed() && old.terminals().empty());

    // Refused: a diff that keeps a terminal or a row the state lacks, a screen out of bounds, rows out of order.
    auto refused = [](const sync::Sessions& from, const sync::Sessions& to, const sync::Sessions& at) {
        sync::Sessions s = at;
        try {
            s.apply_string(to.diff_from(from));
        } catch (const sync::BadDiff&) {
            return s == at;
        }
        return false;
    };
    CHECK(refused(server, later, sync::Sessions()));            // keeps rows of a terminal the reader lacks
    sync::Sessions zero;
    auto bad = std::make_shared<sync::TerminalView>(*view(id, {"x"}));
    bad->columns = 0;
    zero.setTerminals(true, 5, {bad});
    CHECK(refused(sync::Sessions(), zero, sync::Sessions()));
    auto wide = std::make_shared<sync::TerminalView>(*view(id, {"x"}));
    wide->columns = sync::kMaxTerminalColumns + 1;
    zero.setTerminals(true, 5, {wide});
    CHECK(refused(sync::Sessions(), zero, sync::Sessions()));
    bool threw = false;
    try {
        sync::Sessions s;
        s.setTerminals(true, 5, {view(2, {"a"}), view(1, {"b"})});
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);

    // A state file never holds a screen.
    const std::string file = sync::stateFile(later);
    CHECK(!contains(file, "TWO-CHANGED") && !contains(file, "three"));
    sync::Sessions read;
    std::string why;
    CHECK(sync::readStateFile(file, &read, &why) && read.terminals().empty() && read.terminalsServed());

    // Fates: running, ended, closed in this run, lost with a restart.
    std::vector<sync::TerminalRef> list{view(id, {"a"}), view(id + 1, {"b"}, "exited with code 0")};
    CHECK(sync::terminalFate(id, 5, 5, list) == sync::TerminalFate::Running);
    CHECK(sync::terminalFate(id + 1, 5, 5, list) == sync::TerminalFate::Ended);
    CHECK(sync::terminalFate(id + 2, 5, 5, list) == sync::TerminalFate::Closed);
    CHECK(sync::terminalFate(id + 2, 4, 5, list) == sync::TerminalFate::Restarted);

    // Modes.
    std::vector<std::uint32_t> modes;
    CHECK(sync::parseModes("1,66,2004", &modes) && modes.size() == 3);
    CHECK(sync::parseModes("", &modes) && modes.empty());
    CHECK(!sync::parseModes("2004,1", &modes));
    CHECK(!sync::parseModes("1049", &modes));
    CHECK(!sync::parseModes("1,,2", &modes));
    CHECK(!sync::parseModes("x", &modes));
}

void testPaint() {
    const sync::TerminalRef a = view(1, {"one", "two", "three"});
    // First paint: autowrap off, erased, every row, the modes, the cursor.
    std::string p = sync::paintTerminal(nullptr, *a, 20, 3);
    CHECK(p.rfind("\x1b[?7l", 0) == 0);
    CHECK(contains(p, "\x1b[2J") && contains(p, "one") && contains(p, "two") && contains(p, "three"));
    CHECK(contains(p, "\x1b[?1h") && contains(p, "\x1b[?2004l"));
    CHECK(contains(p, "\x1b[2;3H\x1b[?25h"));
    // Only the rows that changed, and only the modes that changed.
    auto b = std::make_shared<sync::TerminalView>(*a);
    b->lines[1] = std::make_shared<const std::string>("TWO");
    b->modes = "1,2004";
    p = sync::paintTerminal(a.get(), *b, 20, 3);
    CHECK(contains(p, "TWO") && !contains(p, "one") && !contains(p, "three") && !contains(p, "\x1b[2J"));
    CHECK(contains(p, "\x1b[?2004h") && !contains(p, "\x1b[?1h"));
    // Rows past the window's screen are left out; the cursor is kept on it.
    p = sync::paintTerminal(nullptr, *a, 20, 2);
    CHECK(!contains(p, "three") && contains(p, "\x1b[2;3H"));
    p = sync::paintTerminal(nullptr, *a, 1, 1);
    CHECK(contains(p, "\x1b[1;1H"));
    // A row that is not as paintRow writes it is never written.
    auto c = std::make_shared<sync::TerminalView>(*a);
    c->lines[0] = std::make_shared<const std::string>("evil\x1b]52;c;Zm9v\x07");
    p = sync::paintTerminal(nullptr, *c, 20, 3);
    CHECK(!contains(p, "evil") && !contains(p, "]52"));
    CHECK(!sync::cleanRow("a\nb") && !sync::cleanRow("\x1b[?1h") && !sync::cleanRow("\xc2\x9b" "1m"));
    CHECK(!sync::cleanRow("\x1b[1") && !sync::cleanRow("\x1b") && sync::cleanRow("\x1b[0;38:2:1:2:3mx"));
    // An ended terminal hides its cursor.
    auto e = std::make_shared<sync::TerminalView>(*a);
    e->ended = "exited with code 0";
    CHECK(contains(sync::paintTerminal(nullptr, *e, 20, 3), "\x1b[?25l"));
}

} // namespace

int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "";
    if (group == "book") testBook();
    else if (group == "modes") testModes();
    else if (group == "rows") testRows();
    else if (group == "state") testState();
    else if (group == "paint") testPaint();
    else {
        std::fprintf(stderr, "usage: terminals_test book|modes|rows|state|paint\n");
        return 2;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("terminals %s: ok\n", group.c_str());
    return 0;
}
