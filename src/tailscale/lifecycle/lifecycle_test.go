// Unit tests of the node's state code: no OS, no time (times are arguments), no network.
package lifecycle

import (
	"math/rand"
	"strings"
	"testing"
	"time"
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
	n = Step(n, poll(false))
	expect(t, "a poll not yet open", n, Running, ReasonNone)
	n = Step(n, poll(true))
	expect(t, "ready", n, Ready, ReasonNone)
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

func TestControlURL(t *testing.T) {
	good := map[string]string{
		"":                               "",
		"https://control.example":        "https://control.example",
		"https://Control.Example:8443/":  "https://control.example:8443",
		"HTTPS://control.example":        "https://control.example",
		"http://127.0.0.1:41641":         "http://127.0.0.1:41641", // the test build's own, given as plain
		"https://example.com.tailscale2": "https://example.com.tailscale2",
	}
	for raw, want := range good {
		got, err := ControlURL(raw, "http://127.0.0.1:41641")
		if err != nil || got != want {
			t.Fatalf("%q: got %q, %v; want %q", raw, got, err, want)
		}
	}
	bad := []string{"http://control.example", "http://127.0.0.1:41641", "ftp://control.example", "control.example",
		"https://", "https://user:secret@control.example", "https://control.example/path",
		"https://control.example?x=1", "https://control.example#f", "https://controlplane.tailscale.com",
		"https://login.tailscale.com", "https://tailscale.com", "https://%zz"}
	for _, raw := range bad {
		_, err := ControlURL(raw, "")
		if err == nil {
			t.Fatalf("%q accepted", raw)
		}
		if strings.Contains(err.Error(), "secret") {
			t.Fatalf("%q: the error repeats the URL: %v", raw, err)
		}
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
	write, err = CheckRecord("", true, "")
	if err != nil || write != Hosted {
		t.Fatalf("a layer 0 directory with the hosted service: %q, %v", write, err)
	}
	if _, err := CheckRecord("", true, "https://control.example"); err == nil {
		t.Fatal("a layer 0 directory (the hosted service's) given to another control server")
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

func TestSettle(t *testing.T) {
	var s Settle
	at := time.Unix(1000, 0)
	if s.Look(at, true, true) {
		t.Fatal("settled at the first look")
	}
	if s.Look(at.Add(SettleAfter-time.Millisecond), true, true) {
		t.Fatal("settled too soon")
	}
	if !s.Look(at.Add(SettleAfter), true, true) {
		t.Fatal("not settled after SettleAfter")
	}
	if s.Look(at.Add(time.Second), true, false) {
		t.Fatal("settled with the poll closed")
	}
	if s.Look(at.Add(2*time.Second), true, true) {
		t.Fatal("settled at once after the poll reopened")
	}
	s.Reset()
	if s.Look(at.Add(3*time.Second), true, true) {
		t.Fatal("settled at once after a reset")
	}
	if s.Look(at.Add(4*time.Second), false, true) {
		t.Fatal("settled while not running")
	}
}
