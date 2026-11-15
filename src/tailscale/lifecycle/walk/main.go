// Command walk drives the lifecycle code (../lifecycle.go) with a seeded random walk and writes the trace as
// TailscaleTraceData.tla, which models/TailscaleTrace.tla checks against models/Tailscale.tla (tests/model's
// tlc_tailscale_trace_* tests, through tests/model/tlc.cmake).
//
//	walk <seed> <steps> <out.tla> [--faulty-ready | --faulty-url | --faulty-close | --faulty-table | --faulty-down]
//
// The walk plays Tailscale.tla's world: the host (starts, held in a lifecycle.Table or refused while another node
// holds the directory, each with an auth key or not and a trusted certificate or not; closes; new logins), and tsnet,
// control and the person, each step one of the model's actions, taken only where the model's guard allows it in the
// walk's own copy of the model's environment (control up or down, the directories enrolled or expired, the logins
// asked for and approved). Each step gives the code the event that action produces, with the texts tsnet gives
// (refusals, certificate and network errors, sign-in URLs), and records the action, its arguments, and the code's
// state after it. The trace check takes the action the record names in the model, so an event the model's world could
// not have produced there is rejected, as is a state the code reached that the model's action does not. Each event's
// class (a refusal's reason, a certificate or network error) is the walk's own table, so that the check binds the
// code's classification of those texts too. A --faulty-* flag sets one of lifecycle.Faults, whose trace the check
// must reject. No time, no I/O but the output file.
package main

import (
	"fmt"
	"math/rand"
	"os"
	"strconv"
	"strings"

	"github.com/robotgamedevelopers/nexus/src/tailscale/lifecycle"
)

const (
	nodes = 16
	dirs  = 3

	// The model's bounds for this trace (TraceMaxOutages, ...): the walk takes no more.
	maxOutages  = 12
	maxExpiries = 8
	maxRefusals = 12
)

// A message from control with the class the model gives it.
type message struct {
	text, class string
}

// Control's refusals, as tsnet hands them over (ipn.Notify.ErrMessage).
var refusals = []message{
	{"invalid authkey", "auth-key"},
	{"authkey already used", "auth-key"},
	{"requested tags [tag:server] are invalid or not permitted", "tags"},
	{"node not authorized for this tailnet", "refused"},
	{"tailnet requires logging to be enabled. Remove --no-logs-no-support from tailscaled command line.", "logging"},
}

const rateLimited = "node registration rate limited; will retry after 1s"

// Errors tsnet's backend logs from control ("Received error: ..."): example addresses only.
var certificateErrors = []string{
	`fetch control key: Get "https://control.example.invalid/key?v=130": tls: failed to verify certificate: ` +
		`x509: certificate signed by unknown authority`,
}

var networkErrors = []string{
	"fetch control key: dial tcp 192.0.2.1:443: connect: connection refused",
	"PollNetMap: unexpected EOF",
	"register request: Post \"https://control.example.invalid/machine/register\": context deadline exceeded",
}

// pollClosed is the text the shim gives when it sees the map poll close.
const pollClosed = "the map poll to control closed"

// world is the walk's copy of the model's environment.
type world struct {
	up                                bool
	outages, expiries, refusals       int
	enrolled, keyDead                 [dirs + 1]bool
	keyed, badCert, wantURL, approved [nodes + 1]bool
}

type walker struct {
	rng   *rand.Rand
	nodes [nodes + 1]lifecycle.Node
	dirOf [nodes + 1]int
	table lifecycle.Table
	w     world
	out   strings.Builder
	count int
}

func boolText(b bool) string {
	if b {
		return "TRUE"
	}
	return "FALSE"
}

func backendOf(r lifecycle.Node) string {
	if r.Backend == "" {
		return "NoState"
	}
	return r.Backend
}

// facts is node n's facts as the model writes them. The zero Node's empty Backend is tsnet's NoState.
func (w *walker) facts(n int) string {
	r := w.nodes[n]
	return fmt.Sprintf("[started |-> %s, closed |-> %s, backend |-> %q, pend |-> %s, peers |-> %s, kexp |-> %s, "+
		"refusal |-> %q, down |-> %s]", boolText(r.Started), boolText(r.Closed), backendOf(r),
		boolText(r.PendingURL != ""), boolText(r.Peers), boolText(r.KeyExpired), r.Refusal.String(),
		boolText(r.Down))
}

func (w *walker) holders() string {
	var parts []string
	for d := 1; d <= dirs; d++ {
		parts = append(parts, strconv.Itoa(w.table.Holder(strconv.Itoa(d))))
	}
	return "<<" + strings.Join(parts, ", ") + ">>"
}

// action is one step the model allows now: its name and arguments as the record gives them, and what it does.
type action struct {
	op     string
	n, d   int
	k, c   bool
	cls    string
	weight int
	do     func()
}

// record writes one step: the action a, and node a.n's state after it.
func (w *walker) record(a action) {
	r := w.nodes[a.n]
	w.out.WriteString(",\n    ")
	fmt.Fprintf(&w.out, "[op |-> %q, n |-> %d, d |-> %d, k |-> %s, c |-> %s, cls |-> %q, f |-> %s, phase |-> %q, "+
		"reason |-> %q, url |-> %s, down |-> %s, holder |-> %s]", a.op, a.n, a.d, boolText(a.k), boolText(a.c),
		a.cls, w.facts(a.n), r.Phase.String(), r.Reason.String(), boolText(r.URL != ""), boolText(r.ControlDown),
		w.holders())
	w.count++
}

func (w *walker) apply(n int, e lifecycle.Event) {
	w.nodes[n] = lifecycle.Step(w.nodes[n], e)
}

func (w *walker) live(n int) bool {
	return w.nodes[n].Started && !w.nodes[n].Closed
}

func (w *walker) enrolledNode(n int) bool {
	d := w.dirOf[n]
	return w.w.enrolled[d] && !w.w.keyDead[d]
}

func (w *walker) reach(n int) bool {
	return w.w.up && !w.w.badCert[n]
}

func in(s string, list ...string) bool {
	for _, x := range list {
		if s == x {
			return true
		}
	}
	return false
}

// stuck is Tailscale.tla's Stuck: nothing to sign in with, so the host asks for a new login.
func (w *walker) stuck(n int) bool {
	r := w.nodes[n]
	switch r.Phase {
	case lifecycle.Expired, lifecycle.Failed:
		return true
	case lifecycle.NeedsLogin:
		return r.URL == "" && r.Reason != lifecycle.ReasonMachineApproval
	}
	return false
}

func (w *walker) pick(list []string) string {
	return list[w.rng.Intn(len(list))]
}

// actions is every step the model allows now.
func (w *walker) actions() []action {
	var list []action
	add := func(a action) {
		if a.weight == 0 {
			a.weight = 8
		}
		list = append(list, a)
	}
	offered := false
	for n := 1; n <= nodes; n++ {
		n := n
		r := w.nodes[n]
		d := w.dirOf[n]
		backend := backendOf(r)
		if !r.Started {
			// The next node slot only: the host starts its nodes in turn.
			if offered {
				continue
			}
			offered = true
			k, c := w.rng.Intn(3) == 0, w.rng.Intn(5) == 0
			sd := 1 + w.rng.Intn(dirs)
			add(action{op: "HostStart", n: n, d: sd, k: k, c: c, weight: 2, do: func() {
				w.dirOf[n] = sd
				w.apply(n, lifecycle.Event{Kind: lifecycle.Start})
				w.w.keyed[n], w.w.badCert[n], w.w.wantURL[n], w.w.approved[n] = k, c, !k, false
			}})
			continue
		}
		if !w.live(n) {
			continue
		}
		add(action{op: "HostClose", n: n, d: d, weight: 1, do: func() {
			w.apply(n, lifecycle.Event{Kind: lifecycle.Close})
			w.table.Release(strconv.Itoa(d), n)
		}})
		if w.stuck(n) {
			add(action{op: "HostLogin", n: n, d: d, do: func() {
				w.apply(n, lifecycle.Event{Kind: lifecycle.Login})
				w.w.wantURL[n] = true
			}})
		}
		if !w.enrolledNode(n) && backend == "NoState" {
			add(action{op: "TsNeedsLogin", n: n, d: d, do: func() {
				w.apply(n, lifecycle.Event{Kind: lifecycle.Backend, Text: "NeedsLogin"})
			}})
		}
		if w.reach(n) && w.w.wantURL[n] && backend == "NeedsLogin" && r.PendingURL == "" {
			add(action{op: "TsURL", n: n, d: d, do: func() {
				w.apply(n, lifecycle.Event{Kind: lifecycle.URL,
					Text: fmt.Sprintf("https://login.example.invalid/a/%x", w.rng.Int63())})
				w.w.wantURL[n] = false
			}})
		}
		if r.URL != "" && !w.w.approved[n] {
			add(action{op: "Approve", n: n, d: d, do: func() { w.w.approved[n] = true }})
		}
		if w.reach(n) && w.w.approved[n] {
			add(action{op: "TsFinished", n: n, d: d, do: func() {
				w.apply(n, lifecycle.Event{Kind: lifecycle.LoginFinished})
				w.w.enrolled[d], w.w.keyDead[d], w.w.approved[n] = true, false, false
			}})
		}
		if w.reach(n) && w.w.keyed[n] && !w.w.enrolled[d] && in(backend, "NoState", "NeedsLogin") {
			add(action{op: "TsKeyAccepted", n: n, d: d, weight: 12, do: func() {
				w.apply(n, lifecycle.Event{Kind: lifecycle.LoginFinished})
				w.w.enrolled[d] = true
			}})
			add(action{op: "TsKeyAcceptedQuiet", n: n, d: d, weight: 12, do: func() { w.w.enrolled[d] = true }})
		}
		if w.enrolledNode(n) && in(backend, "NoState", "NeedsLogin") {
			add(action{op: "TsStarting", n: n, d: d, do: func() {
				w.apply(n, lifecycle.Event{Kind: lifecycle.Backend, Text: "Starting"})
			}})
		}
		if w.enrolledNode(n) && in(backend, "NoState", "Starting") {
			add(action{op: "TsRunning", n: n, d: d, do: func() {
				w.apply(n, lifecycle.Event{Kind: lifecycle.Backend, Text: "Running"})
			}})
		}
		if w.reach(n) && w.enrolledNode(n) && in(backend, "NoState", "Starting", "Running") && (!r.Peers || r.Down) {
			add(action{op: "TsNetMap", n: n, d: d, do: func() {
				w.apply(n, lifecycle.Event{Kind: lifecycle.Poll, Flag: true})
			}})
		}
		if !w.w.up && r.Peers && !r.Down {
			add(action{op: "TsPollClosed", n: n, d: d, weight: 24, do: func() {
				w.apply(n, lifecycle.Event{Kind: lifecycle.Poll, Text: pollClosed})
			}})
		}
		if w.w.expiries < maxExpiries && w.enrolledNode(n) && backend == "Running" {
			add(action{op: "TsExpire", n: n, d: d, do: func() {
				w.w.expiries++
				w.w.keyDead[d] = true
				w.apply(n, lifecycle.Event{Kind: lifecycle.Self, Flag: true})
			}})
		}
		if w.w.keyDead[d] && in(backend, "NoState", "Starting", "Running") {
			add(action{op: "TsExpiredState", n: n, d: d, do: func() {
				w.apply(n, lifecycle.Event{Kind: lifecycle.Backend, Text: "NeedsLogin"})
			}})
		}
		if w.reach(n) && !w.enrolledNode(n) && in(backend, "NoState", "NeedsLogin") {
			if w.w.refusals < maxRefusals {
				m := refusals[w.rng.Intn(len(refusals))]
				add(action{op: "TsRefuse", n: n, d: d, cls: m.class, do: func() {
					w.w.refusals++
					w.apply(n, lifecycle.Event{Kind: lifecycle.ErrMessage, Text: m.text})
				}})
			}
			if w.w.refusals < maxRefusals && !r.Down {
				add(action{op: "TsRateLimited", n: n, d: d, do: func() {
					w.w.refusals++
					w.apply(n, lifecycle.Event{Kind: lifecycle.ErrMessage, Text: rateLimited})
				}})
			}
		}
		if !w.w.up && !r.Down {
			add(action{op: "TsLogDown", n: n, d: d, do: func() {
				w.apply(n, lifecycle.Event{Kind: lifecycle.Logged, Text: w.pick(networkErrors)})
			}})
		}
		if w.w.up && w.w.badCert[n] && r.Refusal != lifecycle.ReasonCertificate {
			add(action{op: "TsCertFail", n: n, d: d, do: func() {
				w.apply(n, lifecycle.Event{Kind: lifecycle.Logged, Text: w.pick(certificateErrors)})
			}})
		}
	}
	if w.w.up && w.w.outages < maxOutages {
		add(action{op: "CtlDown", n: 1, weight: 1, do: func() {
			w.w.up = false
			w.w.outages++
		}})
	}
	if !w.w.up {
		add(action{op: "CtlUp", n: 1, weight: 2, do: func() { w.w.up = true }})
	}
	return list
}

// step takes one action the model allows, and records it; it reports false when none is left.
func (w *walker) step() bool {
	list := w.actions()
	total := 0
	for _, a := range list {
		total += a.weight
	}
	if total == 0 {
		return false
	}
	roll := w.rng.Intn(total)
	var a action
	for _, a = range list {
		if roll < a.weight {
			break
		}
		roll -= a.weight
	}
	if a.op == "HostStart" {
		if ok, _ := w.table.Reserve(strconv.Itoa(a.d), a.n); !ok {
			a.op = "refused"
			a.k, a.c = false, false
			w.record(a)
			return true
		}
	}
	a.do()
	w.record(a)
	return true
}

func main() {
	if len(os.Args) < 4 || len(os.Args) > 5 {
		fmt.Fprintln(os.Stderr, "usage: walk <seed> <steps> <out.tla> "+
			"[--faulty-ready|--faulty-url|--faulty-close|--faulty-table|--faulty-down]")
		os.Exit(2)
	}
	seed, err1 := strconv.ParseInt(os.Args[1], 10, 64)
	steps, err2 := strconv.Atoi(os.Args[2])
	if err1 != nil || err2 != nil || steps < 1 {
		fmt.Fprintln(os.Stderr, "walk: the seed and the step count must be integers, the steps at least 1")
		os.Exit(2)
	}
	if len(os.Args) == 5 {
		switch os.Args[4] {
		case "--faulty-ready":
			lifecycle.Faults.ReadyWithoutPeers = true
		case "--faulty-url":
			lifecycle.Faults.URLKept = true
		case "--faulty-close":
			lifecycle.Faults.ClosedKeepsPhase = true
		case "--faulty-table":
			lifecycle.Faults.TableIgnores = true
		case "--faulty-down":
			lifecycle.Faults.PollClosedIgnored = true
		default:
			fmt.Fprintf(os.Stderr, "walk: unknown flag %s\n", os.Args[4])
			os.Exit(2)
		}
	}
	w := &walker{rng: rand.New(rand.NewSource(seed))}
	w.w.up = true
	for i := 0; i < steps && w.step(); i++ {
	}
	var b strings.Builder
	b.WriteString("--------------------------- MODULE TailscaleTraceData ---------------------------\n")
	fmt.Fprintf(&b, "\\* Written by src/tailscale/lifecycle/walk, seed %d, %d steps.\n", seed, steps)
	fmt.Fprintf(&b, "TraceN == %d\nTraceD == %d\n", nodes, dirs)
	fmt.Fprintf(&b, "TraceMaxOutages == %d\nTraceMaxExpiries == %d\nTraceMaxRefusals == %d\n", maxOutages,
		maxExpiries, maxRefusals)
	b.WriteString("Trace == <<\n    [op |-> \"init\"]")
	b.WriteString(w.out.String())
	b.WriteString("\n>>\n=============================================================================\n")
	if err := os.WriteFile(os.Args[3], []byte(b.String()), 0o644); err != nil {
		fmt.Fprintln(os.Stderr, "walk:", err)
		os.Exit(1)
	}
	fmt.Printf("walk: %d steps written to %s\n", w.count, os.Args[3])
}
