#include "sprint.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

#include "pr/json.h"

#include "client_home.h"
#include "style.h"

namespace nexus {

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
            // Command-Up and Command-Down scroll the page to its top and its bottom, as on macOS; nexus binds neither.
            if (event.keyboard.key == PrAppKey_Up || event.keyboard.key == PrAppKey_Down)
                return BoardRoute::Page;
            if ((event.modifiers & style::kCommand) != 0 || event.keyboard.key == PrAppKey_Escape)
                return BoardRoute::Window;
            return BoardRoute::Page;
        case PrAppEventKind_Text:
            return (event.modifiers & style::kCommand) != 0 ? BoardRoute::Window : BoardRoute::Page;
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
  if (g.addEventListener) g.addEventListener("nexus-dashboard-refresh", function (e) {
    if (typeof g.poll === "function" && typeof g.renderOverall === "function") {
      g.poll();
      e.preventDefault();
    }
  });

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
    std::string style(kSprintRepoLinkStyle);
    // Use directly animatable paint properties in the embedded engine.
    style += R"CSS(
@keyframes nexus-active { 0%,17%,100% {opacity:1} 58.5% {opacity:.55} }
@keyframes nexus-transfer { from {background-color:#a8d5ff} }
@media (prefers-reduced-motion: no-preference) {
  #overall .cell { animation-name:none; }
  #overall:not(.stopped) .cell.merging, #overall:not(.stopped) .cell.review,
  #overall:not(.stopped) .cell.fix, #overall:not(.stopped) .cell.working { animation-name:nexus-active; }
  .cell.flash { animation-name:nexus-transfer; }
}
)CSS";
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

namespace {

std::string_view withoutFragment(std::string_view url) {
    return url.substr(0, url.find('#'));
}

bool startsWithFold(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const char c = text[i] >= 'A' && text[i] <= 'Z' ? char(text[i] - 'A' + 'a') : text[i];
        if (c != prefix[i])
            return false;
    }
    return true;
}

} // namespace

bool linkLeavesBoard(std::string_view href, std::string_view document, std::string_view target) {
    if (href.empty() || startsWithFold(href, "javascript:"))
        return false;
    if (target.size() == 6 && startsWithFold(target, "_blank"))
        return true;
    return withoutFragment(href) != withoutFragment(document);
}

std::string sprintPrelude(std::string_view standardZone, std::string_view daylightZone) {
    std::string text(kPrelude);
    replace(text, "@STD@", literal(standardZone));
    replace(text, "@DST@", literal(daylightZone));
    return text;
}

ZoneNames localZoneNames() {
    ZoneNames names;
#if defined(_WIN32)
    _tzset();
    char name[128];
    std::size_t size = 0;
    if (_get_tzname(&size, name, sizeof(name), 0) == 0) {
        names.standard = name;
    }
    if (_get_tzname(&size, name, sizeof(name), 1) == 0) {
        names.daylight = name;
    }
#else
    tzset();
    names.standard = tzname[0] != nullptr ? tzname[0] : "";
    names.daylight = tzname[1] != nullptr ? tzname[1] : "";
#endif
    return names;
}

} // namespace nexus
