#include "sprint.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "pr/json.h"

namespace nexus {

namespace {

constexpr const char* kShown = "nova-config service show sprint-run --json";
constexpr const char* kSetUrl = "set NEXUS_SPRINT_URL to the board's address (http://<host>:<port>/)";

std::string_view view(PrStringView s) {
    return s.data != nullptr ? std::string_view(s.data, s.size) : std::string_view();
}

PrStringView pr(std::string_view s) {
    return PrStringView{s.data(), s.size()};
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

SprintBoardAddress failed(std::string error) {
    return SprintBoardAddress{{}, std::move(error)};
}

// The address a client reaches a server listening on `listen` (host:port) at.
std::string reachable(std::string_view listen) {
    std::size_t colon = listen.rfind(':');
    std::string_view host = colon == std::string_view::npos ? std::string_view() : listen.substr(0, colon);
    std::string_view port = colon == std::string_view::npos ? listen : listen.substr(colon + 1);
    if (host.empty() || host == "0.0.0.0" || host == "[::]" || host == "::")
        host = "127.0.0.1";
    return "http://" + std::string(host) + ":" + std::string(port) + "/";
}

} // namespace

Command sprintRunCommand(const char* override, const char* shell) {
    return novaConfigCommand(override, shell, {"service", "show", "sprint-run", "--json"});
}

SprintBoardAddress parseSprintRun(std::string_view json) {
    PrJson* doc = prJsonCreate();
    size_t at = 0;
    if (!prJsonParse(doc, pr(json), &at)) {
        prJsonDestroy(doc);
        return failed(std::string(kShown) + " printed something that is not JSON (stopped at byte " +
            std::to_string(at) + "); run it in a terminal to see what it prints, or " + kSetUrl);
    }
    PrJsonValueIndex status = prJsonMember(doc, prJsonMember(doc, 0, pr("result")), pr("status"));
    if (prJsonKind(doc, status) == PrJsonKind_String && view(prJsonString(doc, status)) != "ok") {
        std::string s(view(prJsonString(doc, status)));
        prJsonDestroy(doc);
        return failed(std::string(kShown) + " answered status " + s + "; run it in a terminal to see why, or " +
            kSetUrl);
    }
    PrJsonValueIndex items = prJsonMember(doc, 0, pr("items"));
    PrJsonValueIndex item = prJsonKind(doc, items) == PrJsonKind_Array ? prJsonFirst(doc, items) : kPrJsonMissing;
    PrJsonValueIndex argv = item != kPrJsonMissing ? prJsonMember(doc, prJsonMember(doc, item, pr("fields")),
        pr("argv")) : kPrJsonMissing;
    if (prJsonKind(doc, argv) != PrJsonKind_Array) {
        prJsonDestroy(doc);
        return failed(std::string(kShown) + " printed no service with an argv; set the sprint up with `nova-sprint up`, "
            "or " + kSetUrl);
    }
    std::vector<std::string> words;
    for (PrJsonValueIndex i = prJsonFirst(doc, argv); i != kPrJsonMissing; i = prJsonAfter(doc, argv, i))
        words.emplace_back(prJsonKind(doc, i) == PrJsonKind_String ? view(prJsonString(doc, i)) : std::string_view());
    prJsonDestroy(doc);
    for (std::size_t k = 0; k < words.size(); ++k) {
        std::string_view w = words[k];
        if (w == "--listen" && k + 1 < words.size() && !words[k + 1].empty())
            return SprintBoardAddress{reachable(words[k + 1]), {}};
        if (startsWith(w, "--listen=") && w.size() > 9)
            return SprintBoardAddress{reachable(w.substr(9)), {}};
    }
    return failed("the sprint's run service (" + std::string(kShown) + ") has no --listen address in its argv, so it "
        "serves no board; set it up again with `nova-sprint up`, or " + kSetUrl);
}

SprintBoardAddress findSprintBoard(const char* override, const Command& command, const CommandRunner& run) {
    if (override != nullptr && *override != 0) {
        std::string_view url(override);
        if (!startsWith(url, "http://") && !startsWith(url, "https://"))
            return failed("NEXUS_SPRINT_URL is \"" + std::string(url) + "\", which is not an http:// or https:// "
                "address; " + kSetUrl + ", or unset it to read the sprint's record");
        return SprintBoardAddress{std::string(url), {}};
    }
    CommandResult r = run(command, kSprintRunTimeoutSeconds);
    const std::string cmd = "`" + command.shown + "`";
    const std::string fix = std::string("set NEXUS_NOVA_CONFIG to the full path of nova-config, or install nova-tools "
        "so that nova-config is on your login shell's PATH, or ") + kSetUrl;
    switch (r.end) {
        case CommandResult::End::NotFound:
            return failed("cannot find the sprint board: " + command.program + " was not found to run " + cmd + "; " +
                fix);
        case CommandResult::End::NotStarted:
            return failed("cannot find the sprint board: " + cmd + " could not start; " + fix);
        case CommandResult::End::TimedOut:
            return failed("cannot find the sprint board: " + cmd + " did not finish within " +
                std::to_string(kSprintRunTimeoutSeconds) + " seconds and was stopped; run it in a terminal to see "
                "where it waits (the store may be unreachable), or " + kSetUrl);
        case CommandResult::End::Signaled:
            return failed("cannot find the sprint board: " + cmd + " was ended by signal " + std::to_string(r.code) +
                "; run it in a terminal to see why, or " + kSetUrl);
        case CommandResult::End::Exited:
            break;
    }
    if (r.code != 0) {
        std::string why = trimmed(r.err);
        std::string error = "cannot find the sprint board: " + cmd + " exited with status " + std::to_string(r.code);
        if (r.code == 127)
            error += " (command not found)";
        error += why.empty() ? " and printed nothing on standard error" : ": " + why;
        error += r.code == 127 ? "; " + fix : std::string("; if the sprint is not set up, run `nova-sprint up`; or ") +
            kSetUrl;
        return failed(error);
    }
    return parseSprintRun(r.out);
}

SprintBoardAddress findSprintBoard() {
    return findSprintBoard(std::getenv("NEXUS_SPRINT_URL"),
        sprintRunCommand(std::getenv("NEXUS_NOVA_CONFIG"), std::getenv("SHELL")), runCommand);
}

BoardRoute routeBoardEvent(const PrAppEvent& event, bool inPage, bool pressInPage) {
    switch (event.kind) {
        case PrAppEventKind_PointerMove:
            return BoardRoute::Both;
        case PrAppEventKind_PointerDown:
        case PrAppEventKind_Scroll:
            return inPage ? BoardRoute::Page : BoardRoute::Window;
        case PrAppEventKind_PointerUp:
            return pressInPage ? BoardRoute::Page : BoardRoute::Window;
        case PrAppEventKind_KeyDown:
        case PrAppEventKind_KeyUp:
            if ((event.modifiers & PrAppModifierFlag_Super) != 0 || event.keyboard.key == PrAppKey_Escape)
                return BoardRoute::Window;
            return BoardRoute::Page;
        case PrAppEventKind_Text:
            return (event.modifiers & PrAppModifierFlag_Super) != 0 ? BoardRoute::Window : BoardRoute::Page;
        case PrAppEventKind_CompositionUpdate:
        case PrAppEventKind_CompositionEnd:
            return BoardRoute::Page;
        default:
            return BoardRoute::Window;
    }
}

namespace {

// The prelude, with @STD@ and @DST@ for the zone names, each a JavaScript string literal.
constexpr std::string_view kPrelude = R"JS((function () {
  "use strict";
  var g = globalThis;
  var STANDARD = @STD@, DAYLIGHT = @DST@;

  function group(digits) { return digits.replace(/\B(?=(\d{3})+(?!\d))/g, ","); }
  // en-US grouping of a number's integer part, with at most three fraction digits, as en-US formats by default.
  function formatNumber(n) {
    if (typeof n === "bigint") { var s = n.toString(); return s[0] === "-" ? "-" + group(s.slice(1)) : group(s); }
    n = Number(n);
    if (!isFinite(n)) return String(n);
    var neg = n < 0 || Object.is(n, -0), text = String(Math.abs(Math.round(Math.abs(n) * 1000) / 1000));
    var dot = text.indexOf(".");
    var whole = dot < 0 ? text : text.slice(0, dot), fraction = dot < 0 ? "" : text.slice(dot);
    return (neg && n !== 0 ? "-" : "") + group(whole) + fraction;
  }
  if ((1234).toLocaleString("en-US") !== "1,234") {
    Number.prototype.toLocaleString = function () { return formatNumber(Number(this)); };
    if (typeof BigInt === "function") BigInt.prototype.toLocaleString = function () { return formatNumber(BigInt(this)); };
  }

  if (typeof g.Intl === "object" && g.Intl && typeof g.Intl.DateTimeFormat === "function") return;

  var WEEKDAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
  var LONG_WEEKDAYS = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
  var MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
  function pad(n) { return (n < 10 ? "0" : "") + n; }
  // The zone's short name on date d: daylight time when its offset is the smaller of January's and July's.
  function zoneName(d) {
    var jan = new Date(d.getFullYear(), 0, 1).getTimezoneOffset(), jul = new Date(d.getFullYear(), 6, 1).getTimezoneOffset();
    return jan !== jul && d.getTimezoneOffset() === Math.min(jan, jul) ? DAYLIGHT : STANDARD;
  }
  // en-US's parts for the options Intl.DateTimeFormat takes that the board uses: weekday, year, month, day, hour,
  // minute, second, hour12 and timeZoneName. With no date or time field, the date is given, as en-US does.
  function toParts(d, o) {
    var out = [];
    function add(type, value) { out.push({ type: type, value: String(value) }); }
    var time = !!(o.hour || o.minute || o.second);
    var date = !!(o.weekday || o.year || o.month || o.day);
    if (!time && !date) { o = Object.assign({ year: "numeric", month: "numeric", day: "numeric" }, o); date = true; }
    if (o.weekday) add("weekday", o.weekday === "long" ? LONG_WEEKDAYS[d.getDay()] : WEEKDAYS[d.getDay()]);
    if (o.month || o.day || o.year) {
      if (out.length) add("literal", ", ");
      if (o.month === "short" || o.month === "long") {
        add("month", MONTHS[d.getMonth()]);
        if (o.day) { add("literal", " "); add("day", o.day === "2-digit" ? pad(d.getDate()) : d.getDate()); }
        if (o.year) { add("literal", o.day ? ", " : " "); add("year", d.getFullYear()); }
      } else {
        var fields = [];
        if (o.month) fields.push(["month", o.month === "2-digit" ? pad(d.getMonth() + 1) : d.getMonth() + 1]);
        if (o.day) fields.push(["day", o.day === "2-digit" ? pad(d.getDate()) : d.getDate()]);
        if (o.year) fields.push(["year", o.year === "2-digit" ? pad(d.getFullYear() % 100) : d.getFullYear()]);
        fields.forEach(function (f, i) { if (i) add("literal", "/"); add(f[0], f[1]); });
      }
    }
    if (time) {
      if (out.length) add("literal", ", ");
      var h = d.getHours(), twelve = o.hour12 !== false && o.hourCycle !== "h23" && o.hourCycle !== "h24";
      if (o.hour) { var hv = twelve ? (h % 12 || 12) : h; add("hour", o.hour === "2-digit" ? pad(hv) : hv); }
      if (o.minute) { if (o.hour) add("literal", ":"); add("minute", o.hour || o.minute === "2-digit" ? pad(d.getMinutes()) : d.getMinutes()); }
      if (o.second) { if (o.hour || o.minute) add("literal", ":"); add("second", o.hour || o.minute || o.second === "2-digit" ? pad(d.getSeconds()) : d.getSeconds()); }
      if (o.hour && twelve) { add("literal", " "); add("dayPeriod", h < 12 ? "AM" : "PM"); }
    }
    if (o.timeZoneName) { if (out.length) add("literal", " "); add("timeZoneName", zoneName(d)); }
    return out;
  }
  function dateOf(v) { return v === undefined ? new Date() : new Date(+v); }
  function DateTimeFormat(locale, options) {
    if (!(this instanceof DateTimeFormat)) return new DateTimeFormat(locale, options);
    this.options = Object.assign({}, options || {});
  }
  DateTimeFormat.prototype.formatToParts = function (d) { return toParts(dateOf(d), this.options); };
  DateTimeFormat.prototype.format = function (d) {
    return toParts(dateOf(d), this.options).map(function (p) { return p.value; }).join("");
  };
  DateTimeFormat.prototype.resolvedOptions = function () {
    return Object.assign({ locale: "en-US", calendar: "gregory", numberingSystem: "latn" }, this.options);
  };
  function NumberFormat(locale, options) {
    if (!(this instanceof NumberFormat)) return new NumberFormat(locale, options);
  }
  NumberFormat.prototype.format = formatNumber;
  g.Intl = { DateTimeFormat: DateTimeFormat, NumberFormat: NumberFormat };

  var TIME = { hour: "numeric", minute: "2-digit", second: "2-digit" }, DATE = { year: "numeric", month: "numeric", day: "numeric" };
  function withDefaults(options, defaults) {
    var o = Object.assign({}, options || {});
    var any = o.weekday || o.year || o.month || o.day || o.hour || o.minute || o.second;
    return any ? o : Object.assign(o, defaults);
  }
  Date.prototype.toLocaleTimeString = function (locale, options) {
    return new DateTimeFormat(locale, withDefaults(options, TIME)).format(this);
  };
  Date.prototype.toLocaleDateString = function (locale, options) {
    return new DateTimeFormat(locale, withDefaults(options, DATE)).format(this);
  };
  Date.prototype.toLocaleString = function (locale, options) {
    return new DateTimeFormat(locale, withDefaults(options, Object.assign({}, DATE, TIME))).format(this);
  };
})();
)JS";

// `s` as a JavaScript string literal.
std::string literal(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '<': out += "\\u003c"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xf];
                    out += kHex[c & 0xf];
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

void replace(std::string& text, std::string_view mark, const std::string& with) {
    std::size_t at = text.find(mark);
    if (at != std::string::npos)
        text.replace(at, mark.size(), with);
}

} // namespace

std::string sprintPageStyle(const SpendFit& fit) {
    std::string style;
    if (fit.top < 0)
        return style;
    // The open panel a column whose body takes the height to spare: the body's grid row stretches, so the pie's column
    // and its divider reach the bottom while the pie stays at the top.
    char above[64];
    char margin[64];
    std::snprintf(above, sizeof above, "%.2f", fit.top + std::max(fit.gap, 0.0));
    std::snprintf(margin, sizeof margin, "%.2f", std::max({fit.gap, fit.below, 0.0}));
    const std::string open = std::string(kSpendSelector) + ":not(.folded)";
    style += open + " { display: flex; flex-direction: column; box-sizing: border-box; min-height: calc(100vh - " +
        above + "px); margin-bottom: " + margin + "px; }\n";
    style += open + " > .spend-body { flex: 1 1 auto; }\n";
    return style;
}

std::string sprintPrelude(std::string_view standardZone, std::string_view daylightZone) {
    std::string text(kPrelude);
    replace(text, "@STD@", literal(standardZone));
    replace(text, "@DST@", literal(daylightZone));
    return text;
}

} // namespace nexus
