// Unit tests of the node's state code: no OS, no time (times are arguments), no network.
package lifecycle

import (
	"errors"
	"math/rand"
	"net/netip"
	"strings"
	"testing"
	"unicode/utf8"
)

func run(events ...Event) Node {
	var n Node
	for _, e := range events {
		n = Step(n, e)
	}
	return n
}

var (
	start    = Event{Kind: Start}
	closeIt  = Event{Kind: Close}
	login    = Event{Kind: Login}
	finished = Event{Kind: LoginFinished}
)

func backend(s string) Event  { return Event{Kind: Backend, Text: s} }
func urlEvent(s string) Event { return Event{Kind: URL, Text: s} }
func errMsg(s string) Event   { return Event{Kind: ErrMessage, Text: s} }
func logged(s string) Event   { return Event{Kind: Logged, Text: s} }
func poll(b bool) Event       { return Event{Kind: Poll, Flag: b} }
func self(b bool) Event       { return Event{Kind: Self, Flag: b} }

const signIn = "https://login.example.invalid/a/0123"

func expect(t *testing.T, what string, n Node, phase Phase, reason Reason) {
	t.Helper()
	if n.Phase != phase || n.Reason != reason {
		t.Fatalf("%s: got %s/%s, want %s/%s", what, n.Phase, n.Reason, phase, reason)
	}
}

func TestNamesMatchTheHeader(t *testing.T) {
	// nts.h's NTS_STATE_* and NTS_REASON_* are these, in this order.
	phases := "not-started starting needs-login running ready expired stopped failed"
	var got []string
	for p := NotStarted; p <= Failed; p++ {
		got = append(got, p.String())
	}
	if strings.Join(got, " ") != phases {
		t.Fatalf("phases %v", got)
	}
	reasons := "none auth-key tags refused certificate logging control-unreachable machine-approval closed " +
		"control-stopped"
	got = nil
	for r := ReasonNone; r <= ReasonControlStopped; r++ {
		got = append(got, r.String())
	}
	if strings.Join(got, " ") != reasons {
		t.Fatalf("reasons %v", got)
	}
	if Phase(99).String() != "unknown" || Reason(-1).String() != "unknown" || Kind(42).String() != "unknown" {
		t.Fatal("an unknown value is not named unknown")
	}
}

func TestInteractiveSignInToReady(t *testing.T) {
	n := run(start)
	expect(t, "start", n, Starting, ReasonNone)
	n = Step(n, backend("NeedsLogin"))
	expect(t, "needs login, no URL yet", n, NeedsLogin, ReasonNone)
	if n.URL != "" {
		t.Fatal("a URL before tsnet gave one")
	}
	n = Step(n, urlEvent(signIn))
	expect(t, "URL", n, NeedsLogin, ReasonNone)
	if n.URL != signIn {
		t.Fatal("the URL is not shown in needs-login")
	}
	n = Step(n, finished)
	if n.URL != "" || n.PendingURL != "" {
		t.Fatal("the URL outlived the login")
	}
	n = Step(n, backend("Starting"))
	expect(t, "starting", n, Starting, ReasonNone)
	n = Step(n, backend("Running"))
	expect(t, "running without the map", n, Running, ReasonNone)
	n = Step(n, poll(true))
	expect(t, "ready", n, Ready, ReasonNone)
}

func TestPollClosedIsControlDownWhileReady(t *testing.T) {
	n := run(start, backend("Running"), poll(true))
	gen := n.Gen
	n = Step(n, Event{Kind: Poll, Text: "the map poll to control closed"})
	expect(t, "poll closed", n, Ready, ReasonControlUnreachable)
	if !n.Down || n.Text != "the map poll to control closed" || n.Gen != gen+1 {
		t.Fatalf("poll closed: down %v, text %q, generation +%d", n.Down, n.Text, n.Gen-gen)
	}
	// A second close, or an error logged meanwhile, keeps the first reason's text.
	n = Step(n, Event{Kind: Poll, Text: "again"})
	if n.Text != "the map poll to control closed" {
		t.Fatalf("the text changed to %q", n.Text)
	}
	n = Step(n, poll(true))
	expect(t, "a new map", n, Ready, ReasonNone)
	if n.Down {
		t.Fatal("still down after a new map")
	}
	// Down is a change on its own, even where the reason does not show it (a refusal outranks it).
	n = run(start, backend("NeedsLogin"), errMsg("invalid authkey"))
	gen = n.Gen
	n = Step(n, Event{Kind: Poll, Text: "closed"})
	expect(t, "refused, then down", n, Failed, ReasonAuthKey)
	if !n.Down || n.Gen != gen+1 {
		t.Fatal("down while failed is not a change")
	}
}

func TestMapForAnExpiredKeyIsNotReady(t *testing.T) {
	n := run(start, backend("Running"), poll(true), self(true))
	expect(t, "expired", n, Running, ReasonNone)
	n = Step(n, poll(true))
	expect(t, "a map for the expired key", n, Running, ReasonNone)
	if n.Peers {
		t.Fatal("a map for an expired key counted as the peers")
	}
}

func TestDialTarget(t *testing.T) {
	peers := []Peer{
		{Name: "nexus-b.tailnet.example.", Addrs: []netip.Addr{netip.MustParseAddr("100.64.0.2"),
			netip.MustParseAddr("fd7a:115c:a1e0::2")}},
		{Name: "only-six.tailnet.example.", Addrs: []netip.Addr{netip.MustParseAddr("fd7a:115c:a1e0::3")}},
		{Name: "no-address.tailnet.example.", Addrs: nil},
	}
	good := map[string]string{
		"nexus-b.tailnet.example:7000":  "100.64.0.2:7000",
		"NEXUS-B.tailnet.example.:7000": "100.64.0.2:7000",
		"nexus-b:1":                     "100.64.0.2:1",
		"100.64.0.2:65535":              "100.64.0.2:65535",
		"[fd7a:115c:a1e0::2]:80":        "[fd7a:115c:a1e0::2]:80",
		"[::ffff:100.64.0.2]:80":        "100.64.0.2:80",
		"only-six:80":                   "[fd7a:115c:a1e0::3]:80",
	}
	for address, want := range good {
		got, err := DialTarget(address, peers)
		if err != nil || got.String() != want {
			t.Fatalf("%q: got %v, %v; want %s", address, got, err, want)
		}
	}
	// Off the tailnet, or no peer's: refused, whatever the host's network or resolver would make of it.
	bad := map[string]string{
		"127.0.0.1:7000":          "no peer's tailnet address",
		"[::1]:7000":              "no peer's tailnet address",
		"localhost:7000":          "no peer is named localhost",
		"10.1.2.3:80":             "no peer's tailnet address",
		"192.168.1.1:80":          "no peer's tailnet address",
		"100.64.99.99:1":          "no peer's tailnet address",
		"203.0.113.7:443":         "no peer's tailnet address",
		"example.com:443":         "no peer is named example.com",
		"nexus-b.example.com:443": "no peer is named nexus-b.example.com",
		"[fe80::1%en0]:80":        "zone",
		"no-address:80":           "no tailnet address yet",
		"nexus-b":                 "not name:port",
		"nexus-b:":                "port",
		"nexus-b:0":               "port",
		"nexus-b:65536":           "port",
		"nexus-b:http":            "port",
		":80":                     "no host",
		"fd7a:115c:a1e0::2:80":    "not name:port",
	}
	for address, part := range bad {
		_, err := DialTarget(address, peers)
		if err == nil || !strings.Contains(err.Error(), part) {
			t.Fatalf("%q: got %v, want a refusal with %q", address, err, part)
		}
	}
	if _, err := DialTarget("100.64.0.2:1", nil); err == nil {
		t.Fatal("a dial with no peers accepted")
	}
}

func TestScrub(t *testing.T) {
	key := "tskey-auth-kXYZ-0123456789abcdef"
	text := "register: invalid authkey " + key + "; visit https://login.example.invalid/a/0123 or " +
		"http://127.0.0.1:41641/a/x?y=1 and tskey-other-key"
	got := Scrub(text, []string{key, ""}, 0)
	for _, leaked := range []string{key, "https://", "http://", "login.example.invalid", "tskey-"} {
		if strings.Contains(got, leaked) {
			t.Fatalf("%q kept %q", got, leaked)
		}
	}
	if !strings.Contains(got, URLWithheld) || !strings.Contains(got, SecretWithheld) ||
		!strings.Contains(got, "invalid authkey") {
		t.Fatalf("scrubbed: %q", got)
	}
	// A secret that is no tskey (a key a control server of another make issues) is withheld too.
	if got := Scrub("refused: hunter2-key", []string{"hunter2-key"}, 0); strings.Contains(got, "hunter2") {
		t.Fatalf("a secret kept: %q", got)
	}
	// Cut to the limit on a character boundary.
	long := strings.Repeat("é", 200)
	cut := Scrub(long, nil, 101)
	if len(cut) > 101 || !strings.HasSuffix(cut, "...") || !utf8.ValidString(cut) {
		t.Fatalf("cut: %d bytes, %q", len(cut), cut[len(cut)-8:])
	}
	if Scrub("short", nil, 101) != "short" {
		t.Fatal("a short text changed")
	}
}

func TestRunningIsNotReadyUntilTheMapComesAgain(t *testing.T) {
	// A restarted node says running before control sends its peers.
	n := run(start, backend("Running"))
	expect(t, "restart", n, Running, ReasonNone)
	n = run(start, backend("Running"), poll(true), backend("NeedsLogin"), backend("Running"))
	expect(t, "after a login is needed again", n, Running, ReasonNone)
}

func TestCloseIsFinal(t *testing.T) {
	n := run(start, backend("Running"), poll(true), closeIt)
	expect(t, "closed", n, Stopped, ReasonClosed)
	gen := n.Gen
	for _, e := range []Event{start, backend("Running"), poll(true), urlEvent(signIn), errMsg("invalid authkey"),
		finished, login, self(true)} {
		n = Step(n, e)
		expect(t, "after close, "+e.Kind.String(), n, Stopped, ReasonClosed)
	}
	if n.Gen != gen || n.URL != "" {
		t.Fatal("a closed node changed")
	}
	n = run(start, backend("NeedsLogin"), urlEvent(signIn), closeIt)
	if n.URL != "" {
		t.Fatal("a closed node keeps its URL")
	}
}

func TestRefusals(t *testing.T) {
	cases := []struct {
		text   string
		reason Reason
	}{
		{"invalid authkey", ReasonAuthKey},
		{"authkey already used", ReasonAuthKey},
		{"auth key expired", ReasonAuthKey},
		{"pre-auth key not found", ReasonAuthKey},
		{"requested tags [tag:x] are invalid or not permitted", ReasonTags},
		{"tailnet requires logging to be enabled", ReasonLogging},
		{"node not authorized", ReasonRefused},
	}
	for _, c := range cases {
		n := run(start, backend("NeedsLogin"), errMsg(c.text))
		expect(t, c.text, n, Failed, c.reason)
		if n.Text != c.text {
			t.Fatalf("%s: the text is %q", c.text, n.Text)
		}
		// A new login clears the refusal, and so does running.
		expect(t, c.text+", login", Step(n, login), NeedsLogin, ReasonNone)
		expect(t, c.text+", running", Step(n, backend("Running")), Running, ReasonNone)
	}
}

func TestRateLimitIsUnreachableNotARefusal(t *testing.T) {
	n := run(start, backend("NeedsLogin"), errMsg("node registration rate limited; will retry after 1s"))
	expect(t, "rate limited", n, NeedsLogin, ReasonControlUnreachable)
	n = Step(n, urlEvent(signIn))
	expect(t, "control answered", n, NeedsLogin, ReasonNone)
}

func TestLoggedErrors(t *testing.T) {
	cert := `fetch control key: tls: failed to verify certificate: x509: certificate signed by unknown authority`
	n := run(start, logged(cert))
	expect(t, "certificate", n, Failed, ReasonCertificate)
	n = run(start, backend("Running"), poll(true), logged("PollNetMap: unexpected EOF"))
	expect(t, "control down while ready", n, Ready, ReasonControlUnreachable)
	if n.Text != "PollNetMap: unexpected EOF" {
		t.Fatalf("the text is %q", n.Text)
	}
	expect(t, "control back", Step(n, poll(true)), Ready, ReasonNone)
	n = run(start, logged("fetch control key: connection refused"))
	expect(t, "unreachable at start", n, Starting, ReasonControlUnreachable)
	expect(t, "stopped by control wins", Step(n, backend("Stopped")), Stopped, ReasonControlStopped)
}

func TestExpiryAndNewLogin(t *testing.T) {
	n := run(start, backend("Running"), poll(true))
	n = Step(n, self(true))
	expect(t, "key expired, still running", n, Running, ReasonNone)
	n = Step(n, backend("NeedsLogin"))
	expect(t, "expired", n, Expired, ReasonNone)
	n = Step(n, login)
	expect(t, "login asked", n, Expired, ReasonNone)
	n = Step(n, urlEvent(signIn))
	expect(t, "new URL", n, NeedsLogin, ReasonNone)
	n = Step(n, finished)
	if n.KeyExpired {
		t.Fatal("the login did not clear the expiry")
	}
	n = Step(Step(n, backend("Running")), poll(true))
	expect(t, "ready again", n, Ready, ReasonNone)
}

func TestMachineApproval(t *testing.T) {
	n := run(start, backend("NeedsMachineAuth"))
	expect(t, "machine approval", n, NeedsLogin, ReasonMachineApproval)
}

func TestGenerationCountsChangesOnly(t *testing.T) {
	n := run(start)
	gen := n.Gen
	n = Step(n, backend("Starting"))
	if n.Gen != gen {
		t.Fatal("no change counted as one")
	}
	n = Step(n, backend("NeedsLogin"))
	if n.Gen != gen+1 {
		t.Fatalf("one change counted as %d", n.Gen-gen)
	}
	n = Step(n, urlEvent(signIn))
	if n.Gen != gen+2 {
		t.Fatal("the URL's arrival is not a change")
	}
}

// The model's invariants, on long random runs of the code itself (the trace check binds the same code to
// models/Tailscale.tla; this is the quick check of the same rules).
func TestInvariantsOnRandomRuns(t *testing.T) {
	texts := []string{"invalid authkey", "node registration rate limited", "requested tags bad", "x509: bad",
		"connection refused", "PollNetMap: EOF"}
	backends := []string{"NoState", "NeedsLogin", "NeedsMachineAuth", "Stopped", "Starting", "Running"}
	rng := rand.New(rand.NewSource(1))
	for walk := 0; walk < 200; walk++ {
		var n Node
		for step := 0; step < 300; step++ {
			var e Event
			switch rng.Intn(11) {
			case 0:
				e = start
			case 1:
				if rng.Intn(10) == 0 {
					e = closeIt
				} else {
					e = login
				}
			case 2, 3:
				e = backend(backends[rng.Intn(len(backends))])
			case 4:
				e = urlEvent([]string{"", signIn}[rng.Intn(2)])
			case 5:
				e = finished
			case 6:
				e = errMsg(texts[rng.Intn(len(texts))])
			case 7:
				e = logged(texts[rng.Intn(len(texts))])
			case 8, 9:
				e = poll(rng.Intn(2) == 0)
			default:
				e = self(rng.Intn(3) == 0)
			}
			was := n
			n = Step(n, e)
			if n.Phase == Ready && !n.Peers {
				t.Fatalf("walk %d step %d: ready without peers", walk, step)
			}
			if n.URL != "" && n.Phase != NeedsLogin {
				t.Fatalf("walk %d step %d: a URL in %s", walk, step, n.Phase)
			}
			if n.Closed && (n.Phase != Stopped || n.Reason != ReasonClosed) {
				t.Fatalf("walk %d step %d: closed but %s", walk, step, n.Phase)
			}
			if was.Closed && n != was {
				t.Fatalf("walk %d step %d: a closed node changed", walk, step)
			}
		}
	}
}

func TestTable(t *testing.T) {
	var table Table
	if ok, _ := table.Reserve("a", 1); !ok {
		t.Fatal("a free directory refused")
	}
	if ok, holder := table.Reserve("a", 2); ok || holder != 1 {
		t.Fatalf("a held directory given away (holder %d)", holder)
	}
	table.Release("a", 2)
	if table.Holder("a") != 1 {
		t.Fatal("another node's release freed it")
	}
	table.Release("a", 1)
	if ok, _ := table.Reserve("a", 2); !ok {
		t.Fatal("a released directory refused")
	}
	if ok, _ := table.Reserve("b", 2); !ok {
		t.Fatal("a second directory refused")
	}
}

// sameIDNA is an IDNA lookup that changes nothing; badIDNA one that calls every name invalid.
func sameIDNA(s string) (string, error) { return s, nil }

func badIDNA(s string) (string, error) { return "", errors.New("invalid") }

func TestControlURL(t *testing.T) {
	good := map[string]string{
		"":                               "",
		"https://control.example":        "https://control.example",
		"https://Control.Example:8443/":  "https://control.example:8443",
		"HTTPS://control.example":        "https://control.example",
		"http://127.0.0.1:41641":         "http://127.0.0.1:41641", // the test build's own, given as plain
		"https://example.com.tailscale2": "https://example.com.tailscale2",
		// One server, one record: a trailing dot, the default port and letter case are spellings of the same host.
		"https://control.example.":      "https://control.example",
		"https://control.example:443":   "https://control.example",
		"https://control.example.:443/": "https://control.example",
		"https://CONTROL.example:00443": "https://control.example",
		"https://control.example:1":     "https://control.example:1",
		"https://control.example:65535": "https://control.example:65535",
		"https://192.0.2.10":            "https://192.0.2.10",
		"https://192.0.2.10:8443":       "https://192.0.2.10:8443",
		"https://[2001:DB8::0:1]":       "https://[2001:db8::1]",
		"https://[2001:db8::1]:443":     "https://[2001:db8::1]",
		"https://[2001:db8::1]:8443":    "https://[2001:db8::1]:8443",
		"https://xn--bcher-kva.example": "https://xn--bcher-kva.example",
		"https://tailscale.com.example": "https://tailscale.com.example",
		"https://nottailscale.com":      "https://nottailscale.com",
	}
	for raw, want := range good {
		got, err := ControlURL(raw, "http://127.0.0.1:41641", sameIDNA)
		if err != nil || got != want {
			t.Fatalf("%q: got %q, %v; want %q", raw, got, err, want)
		}
	}
	bad := []string{"http://control.example", "http://127.0.0.1:41641", "ftp://control.example", "control.example",
		"https://", "https://user:secret@control.example", "https://control.example/path",
		"https://control.example?x=1", "https://control.example#f", "https://controlplane.tailscale.com",
		"https://login.tailscale.com", "https://tailscale.com", "https://%zz",
		// The hosted service in other spellings: a trailing dot, case, the default port, fullwidth and other
		// non-ASCII forms that IDNA maps to it, percent escapes.
		"https://controlplane.tailscale.com.", "https://CONTROLPLANE.TAILSCALE.COM", "https://tailscale.com.",
		"https://controlplane.tailscale.com:443", "https://controlplane.tailscale.com.:443",
		"https://ｃｏｎｔｒｏｌｐｌａｎｅ.ｔａｉｌｓｃａｌｅ.ｃｏｍ", "https://controlplane.tailscale。com",
		"https://controlplane.tailscale．com", "https://controlplane.tailſcale.com", "https://tailscale.com​",
		"https://controlplane%2etailscale.com", "https://login.tailscale.io", "https://tailscale.io.",
		// Ports: none after the colon, zero, out of range, not a number.
		"https://control.example:", "https://control.example:0", "https://control.example:99999",
		"https://control.example:65536", "https://control.example:-1", "https://control.example:+443",
		"https://control.example:44 3", "https://control.example:0x1bb", "https://[2001:db8::1]:",
		"https://[2001:db8::1]:0",
		// IPv6 zones and malformed brackets; IPv4 in forms other than dotted decimal.
		"https://[fe80::1%25en0]", "https://[fe80::1%en0]:8443", "https://[2001:db8::1", "https://[192.0.2.1]",
		"https://[::ffff:192.0.2.1]", "https://2001:db8::1", "https://127.1", "https://0x7f.0.0.1",
		"https://192.0.2.010.example.1",
		// Names that are not host names.
		"https://.", "https://..", "https://a..example", "https://-control.example", "https://control-.example",
		"https://con_trol.example", "https://" + strings.Repeat("a", 64) + ".example",
		"https://" + strings.Repeat("a.", 127) + "aa",
	}
	for _, raw := range bad {
		_, err := ControlURL(raw, "", sameIDNA)
		if err == nil {
			t.Fatalf("%q accepted", raw)
		}
		if strings.Contains(err.Error(), "secret") {
			t.Fatalf("%q: the error repeats the URL: %v", raw, err)
		}
	}
	// A name IDNA calls invalid (or would change) is refused; an address is not given to IDNA.
	if _, err := ControlURL("https://xn--a.example", "", badIDNA); err == nil {
		t.Fatal("a name IDNA refuses accepted")
	}
	changes := func(s string) (string, error) { return s + "x", nil }
	if _, err := ControlURL("https://control.example", "", changes); err == nil {
		t.Fatal("a name IDNA would change accepted")
	}
	if got, err := ControlURL("https://192.0.2.10", "", badIDNA); err != nil || got != "https://192.0.2.10" {
		t.Fatalf("an IPv4 address given to IDNA: %q, %v", got, err)
	}
	if got, err := ControlURL("https://[2001:db8::1]", "", badIDNA); err != nil || got != "https://[2001:db8::1]" {
		t.Fatalf("an IPv6 address given to IDNA: %q, %v", got, err)
	}
}

func TestAuthKeyOnlyWithAControlServer(t *testing.T) {
	err := CheckAuthKey("", "tskey-a-value-that-must-not-be-shown")
	if err == nil || strings.Contains(err.Error(), "must-not-be-shown") {
		t.Fatalf("a key with the hosted service: %v", err)
	}
	if CheckAuthKey("https://control.example", "tskey-x") != nil || CheckAuthKey("", "") != nil {
		t.Fatal("a valid start refused")
	}
}

func TestRecord(t *testing.T) {
	write, err := CheckRecord("", false, "https://control.example")
	if err != nil || write != "https://control.example" {
		t.Fatalf("a new directory: %q, %v", write, err)
	}
	write, err = CheckRecord("", false, "")
	if err != nil || write != Hosted {
		t.Fatalf("a new directory for the hosted service: %q, %v", write, err)
	}
	// Node state with no record: the record was removed outside nexus, and no start guesses whose state it is.
	for _, control := range []string{"", "https://control.example"} {
		if _, err := CheckRecord("", true, control); err == nil {
			t.Fatalf("state with no record started for %q", control)
		}
	}
	// The record is ControlURL's canonical form, so every spelling of one server matches it.
	for _, raw := range []string{"https://Control.Example.", "https://control.example:443", "https://control.example"} {
		control, err := ControlURL(raw, "", sameIDNA)
		if err != nil {
			t.Fatalf("%q: %v", raw, err)
		}
		if _, err := CheckRecord("https://control.example", true, control); err != nil {
			t.Fatalf("%q refused by its own server's record: %v", raw, err)
		}
	}
	write, err = CheckRecord("https://control.example", true, "https://control.example")
	if err != nil || write != "" {
		t.Fatalf("the same control server: %q, %v", write, err)
	}
	if _, err := CheckRecord(Hosted, true, "https://control.example"); err == nil {
		t.Fatal("the hosted service's directory given to another control server")
	}
	if _, err := CheckRecord("https://a.example", false, "https://b.example"); err == nil {
		t.Fatal("one control server's directory given to another")
	}
}
