// Package lifecycle is the state of a Tailscale node as the host sees it (src/tailscale/nts.h, NTS_STATE_*), worked out
// by a pure function from what tsnet reports: Step takes the node's facts and one event and gives the facts after it,
// with the state derived from them. No time, no I/O, no tsnet: the shim (src/tailscale/shim/watch.go) turns tsnet's
// notifications and its status into events, and the walk (./walk) drives Step with scripted events for the TLA+ trace
// check (models/TailscaleTrace.tla, which binds this code to models/Tailscale.tla).
//
// The states:
//
//	not-started  no start yet
//	starting     started; tsnet has not yet said whether it needs a sign-in, or it is bringing the tunnel up
//	needs-login  a sign-in is needed: with the URL when tsnet has one (the URL is held only in this state), without
//	             it while one is being asked for, or with reason machine-approval when the control server's admin must
//	             approve the machine
//	running      tsnet says running, but no network map from control has been received and applied since this
//	             start or sign-in
//	ready        running, and a network map from control has been received and applied since this start or sign-in
//	             (tsnet's own signal: the self-node notification it sends after applying a map; a restarted node
//	             says running before it has its map, and dialing a peer then fails)
//	expired      the node's key expired; a new login (Login) asks for a URL, and the node keeps its identity
//	stopped      closed by the host, or stopped by control
//	failed       control refused the node and tsnet will not get past it alone (a refused auth key or tag, an
//	             untrusted certificate, a tailnet that requires log uploads), with the reason
//
// Control being unreachable is not a state: a ready node keeps its connections while control is down, so the state
// stays, the down flag (Node.Down, nts_status.control_down) is set and the reason says control-unreachable, from the
// moment the map poll to control closes (or tsnet logs an error reaching control) until a new map is received.
package lifecycle

import "strings"

// Phase is the host's state of a node: NTS_STATE_* in nts.h, in the same order.
type Phase int

const (
	NotStarted Phase = iota
	Starting
	NeedsLogin
	Running
	Ready
	Expired
	Stopped
	Failed
)

var phaseNames = [...]string{"not-started", "starting", "needs-login", "running", "ready", "expired", "stopped", "failed"}

func (p Phase) String() string {
	if p < 0 || int(p) >= len(phaseNames) {
		return "unknown"
	}
	return phaseNames[p]
}

// Reason qualifies a state: NTS_REASON_* in nts.h, in the same order.
type Reason int

const (
	ReasonNone Reason = iota
	// Failed: control refused the auth key (used, expired, unknown).
	ReasonAuthKey
	// Failed: control refused a tag the node asked for.
	ReasonTags
	// Failed: control refused the login for another reason, in the text.
	ReasonRefused
	// Failed: the control server's certificate is not trusted.
	ReasonCertificate
	// Failed: the tailnet requires log uploads, which nexus never turns on.
	ReasonLogging
	// Any state: control cannot be reached just now; tsnet retries by itself.
	ReasonControlUnreachable
	// Needs-login: the control server's admin must approve this machine.
	ReasonMachineApproval
	// Stopped: closed by the host.
	ReasonClosed
	// Stopped: control stopped the node.
	ReasonControlStopped
)

var reasonNames = [...]string{"none", "auth-key", "tags", "refused", "certificate", "logging", "control-unreachable",
	"machine-approval", "closed", "control-stopped"}

func (r Reason) String() string {
	if r < 0 || int(r) >= len(reasonNames) {
		return "unknown"
	}
	return reasonNames[r]
}

// Kind is the kind of an event.
type Kind int

const (
	// The host's calls.
	Start Kind = iota // the node was started (its state directory held, tsnet started)
	Close             // the host closed the node
	Login             // the host asked for a new login (after expiry, or a refusal it has fixed)

	// What tsnet reports.
	Backend       // tsnet's state (ipn.State's name) in Text: NoState, NeedsLogin, NeedsMachineAuth, Stopped, Starting, Running
	URL           // the sign-in URL tsnet asks to be opened, in Text; empty when it has none
	LoginFinished // tsnet says the login succeeded
	ErrMessage    // control's refusal (ipn.Notify.ErrMessage), in Text
	Logged        // an error tsnet's backend logged from control ("Received error: ..."), in Text
	Poll          // Flag true: a network map from control was received and applied; false: the map poll closed (Text)
	Self          // Flag: the node's own key has expired, from its self node
)

var kindNames = [...]string{"start", "close", "login", "backend", "url", "login-finished", "err-message", "logged",
	"poll", "self"}

func (k Kind) String() string {
	if k < 0 || int(k) >= len(kindNames) {
		return "unknown"
	}
	return kindNames[k]
}

// Event is one input to Step.
type Event struct {
	Kind Kind
	Text string
	Flag bool
}

// Node is what the shim holds for a node: the facts the events left, and the state derived from them.
type Node struct {
	// Derived by Step after every event: what the host sees.
	Phase  Phase
	Reason Reason
	Text   string // the reason in plain words, or ""
	URL    string // the sign-in URL: non-empty only when Phase is NeedsLogin
	// ControlDown: control cannot be reached (or asks the node to wait), whatever the state; nts_status.control_down.
	ControlDown bool

	// Gen counts the changes of Phase, Reason, Text, URL or ControlDown: a host that saw generation g has seen
	// everything up to it.
	Gen uint64

	// The facts.
	Started     bool
	Closed      bool
	Backend     string // tsnet's last state
	PendingURL  string // the last sign-in URL tsnet asked for, until a login finishes or a new one is asked for
	Peers       bool   // a network map from control was received and applied since this start or sign-in
	KeyExpired  bool   // the node's own key has expired
	Refusal     Reason // control's refusal, or ReasonNone
	RefusalText string
	Down        bool // control could not be reached at the last try
	DownText    string
}

// Faults are test seams for the walk's reversed traces (./walk --faulty-*), which the trace check must reject. The
// shim never sets them.
var Faults struct {
	ReadyWithoutPeers bool // running counts as ready before the peer map is in
	URLKept           bool // the sign-in URL stays shown after the login finished
	ClosedKeepsPhase  bool // a close leaves the state as it was
	TableIgnores      bool // Table.Reserve claims a directory another node holds
	PollClosedIgnored bool // the map poll closing leaves the node shown as reaching control
}

// Step is the node after e. It never fails: an event that makes no sense in a state changes nothing it should not.
func Step(n Node, e Event) Node {
	if n.Closed {
		// A closed node is stopped for good: nothing tsnet still reports changes it.
		return n
	}
	switch e.Kind {
	case Start:
		n = Node{Started: true, Backend: "NoState", Gen: n.Gen}
	case Close:
		n.Closed = true
		n.PendingURL = ""
	case Login:
		// A new login: the old URL and refusal are gone, and the peer map must come again.
		n.PendingURL = ""
		n.Refusal, n.RefusalText = ReasonNone, ""
		n.Peers = false
	case Backend:
		n.Backend = e.Text
		switch e.Text {
		case "NeedsLogin", "NeedsMachineAuth":
			n.Peers = false
		case "Running":
			n.Refusal, n.RefusalText = ReasonNone, ""
			if !Faults.URLKept {
				n.PendingURL = ""
			}
		}
	case URL:
		n.PendingURL = e.Text
		if e.Text != "" {
			// Control answered.
			n.Down, n.DownText = false, ""
		}
	case LoginFinished:
		if !Faults.URLKept {
			n.PendingURL = ""
		}
		n.Refusal, n.RefusalText = ReasonNone, ""
		n.KeyExpired = false
		n.Down, n.DownText = false, ""
	case ErrMessage:
		if isRateLimited(e.Text) {
			// Control answered, but asks the node to wait: tsnet retries by itself.
			n.Down, n.DownText = true, e.Text
		} else {
			n.Down, n.DownText = false, ""
			n.Refusal, n.RefusalText = classifyRefusal(e.Text), e.Text
		}
	case Logged:
		if isCertificate(e.Text) {
			n.Refusal, n.RefusalText = ReasonCertificate, e.Text
		} else {
			n.Down, n.DownText = true, e.Text
		}
	case Poll:
		switch {
		case e.Flag && !n.KeyExpired:
			// A map for an expired key brings no peers: the node must sign in again first.
			n.Peers = true
			n.Down, n.DownText = false, ""
		case !e.Flag && !n.Down && !Faults.PollClosedIgnored:
			n.Down, n.DownText = true, e.Text
		}
	case Self:
		n.KeyExpired = e.Flag
		if e.Flag {
			n.Peers = false
		}
	}
	return derive(n)
}

// derive sets the host's state from the facts, and counts a change.
func derive(n Node) Node {
	before := [5]string{n.Phase.String(), n.Reason.String(), n.Text, n.URL, boolName(n.ControlDown)}
	phase, reason, text, url := NotStarted, ReasonNone, "", ""
	switch {
	case n.Closed && !Faults.ClosedKeepsPhase:
		phase, reason = Stopped, ReasonClosed
	case n.Closed:
		phase, reason, text, url = n.Phase, n.Reason, n.Text, n.URL
	case !n.Started:
		phase = NotStarted
	case n.Refusal != ReasonNone:
		phase, reason, text = Failed, n.Refusal, n.RefusalText
	default:
		switch n.Backend {
		case "NeedsLogin":
			switch {
			case n.PendingURL != "":
				phase, url = NeedsLogin, n.PendingURL
			case n.KeyExpired:
				phase = Expired
			default:
				phase = NeedsLogin
			}
		case "NeedsMachineAuth":
			phase, reason = NeedsLogin, ReasonMachineApproval
		case "Running":
			if n.Peers || Faults.ReadyWithoutPeers {
				phase = Ready
			} else {
				phase = Running
			}
			if Faults.URLKept {
				url = n.PendingURL
			}
		case "Stopped":
			phase, reason = Stopped, ReasonControlStopped
		default:
			phase = Starting
		}
		if n.Down && reason == ReasonNone && phase != Stopped {
			reason, text = ReasonControlUnreachable, n.DownText
		}
	}
	n.Phase, n.Reason, n.Text, n.URL = phase, reason, text, url
	n.ControlDown = n.Started && !n.Closed && n.Down
	if [5]string{n.Phase.String(), n.Reason.String(), n.Text, n.URL, boolName(n.ControlDown)} != before {
		n.Gen++
	}
	return n
}

func boolName(b bool) string {
	if b {
		return "true"
	}
	return "false"
}

// classifyRefusal names control's refusal (ipn.Notify.ErrMessage, which tsnet sends for a register response's error).
func classifyRefusal(text string) Reason {
	t := strings.ToLower(text)
	switch {
	case strings.Contains(t, "requires logging"):
		return ReasonLogging
	case strings.Contains(t, "authkey"), strings.Contains(t, "auth key"), strings.Contains(t, "invalid key"),
		strings.Contains(t, "pre-auth"), strings.Contains(t, "preauth"):
		return ReasonAuthKey
	case strings.Contains(t, "tag"):
		return ReasonTags
	case isCertificate(t):
		return ReasonCertificate
	}
	return ReasonRefused
}

// isRateLimited is whether control's message asks the node to wait (tsnet's "node registration rate limited; will
// retry after ...").
func isRateLimited(text string) bool {
	return strings.Contains(strings.ToLower(text), "rate limited")
}

// isCertificate is whether an error from control is TLS refusing its certificate, which tsnet retries for ever.
func isCertificate(text string) bool {
	t := strings.ToLower(text)
	return strings.Contains(t, "x509:") || strings.Contains(t, "certificate")
}

// Table is the state directories held by this process's nodes, by key (the cleaned path, lower-cased on Windows):
// at most one node per state directory. Not safe for concurrent use; the shim holds its lock around it.
type Table struct {
	held map[string]int // key -> the node holding it
}

// Reserve claims key for node, or reports false (and the holder) when another node holds it.
func (t *Table) Reserve(key string, node int) (bool, int) {
	if t.held == nil {
		t.held = map[string]int{}
	}
	if holder, ok := t.held[key]; ok && !Faults.TableIgnores {
		return false, holder
	}
	t.held[key] = node
	return true, 0
}

// Release frees key if node holds it.
func (t *Table) Release(key string, node int) {
	if t.held[key] == node {
		delete(t.held, key)
	}
}

// Holder is the node holding key, or 0.
func (t *Table) Holder(key string) int {
	return t.held[key]
}
