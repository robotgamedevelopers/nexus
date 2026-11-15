// Command walk drives the lifecycle code (../lifecycle.go) with a seeded random walk and writes the trace as
// TailscaleTraceData.tla, which models/TailscaleTrace.tla checks against models/Tailscale.tla (tests/model's
// tlc_tailscale_trace_* tests, through tests/model/tlc.cmake).
//
//	walk <seed> <steps> <out.tla> [--faulty-ready | --faulty-url | --faulty-close | --faulty-table]
//
// Nodes 1..4 on state directories 1..2: starts (held in a lifecycle.Table, or refused while another node holds the
// directory), closes, and every kind of event in any order, with the texts tsnet gives. Each event's class (a refusal's
// reason, a certificate or network error) is the walk's own table, so that the trace check binds the code's
// classification of those texts too. A --faulty-* flag sets one of lifecycle.Faults, whose trace the check must
// reject. No time, no I/O but the output file.
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
	nodes = 4
	dirs  = 2
)

var backends = []string{"NoState", "NeedsLogin", "NeedsMachineAuth", "Stopped", "Starting", "Running"}

// A message from control with the class the model gives it.
type message struct {
	text, class string
}

// Control's refusals, as tsnet hands them over (ipn.Notify.ErrMessage), and its rate limit.
var refusals = []message{
	{"invalid authkey", "auth-key"},
	{"authkey already used", "auth-key"},
	{"requested tags [tag:server] are invalid or not permitted", "tags"},
	{"node not authorized for this tailnet", "refused"},
	{"tailnet requires logging to be enabled. Remove --no-logs-no-support from tailscaled command line.", "logging"},
	{"node registration rate limited; will retry after 1s", "ratelimit"},
}

// Errors tsnet's backend logs from control ("Received error: ..."): example addresses only.
var logged = []message{
	{`fetch control key: Get "https://control.example.invalid/key?v=130": tls: failed to verify certificate: ` +
		`x509: certificate signed by unknown authority`, "certificate"},
	{"fetch control key: dial tcp 192.0.2.1:443: connect: connection refused", "network"},
	{"PollNetMap: unexpected EOF", "network"},
	{"register request: Post \"https://control.example.invalid/machine/register\": context deadline exceeded",
		"network"},
}

type walker struct {
	rng   *rand.Rand
	nodes [nodes + 1]lifecycle.Node
	dirOf [nodes + 1]int
	table lifecycle.Table
	out   strings.Builder
	count int
}

func boolText(b bool) string {
	if b {
		return "TRUE"
	}
	return "FALSE"
}

// facts is node n's facts as the model writes them. The zero Node's empty Backend is tsnet's NoState.
func (w *walker) facts(n int) string {
	r := w.nodes[n]
	backend := r.Backend
	if backend == "" {
		backend = "NoState"
	}
	return fmt.Sprintf("[started |-> %s, closed |-> %s, backend |-> %q, pend |-> %s, peers |-> %s, kexp |-> %s, "+
		"refusal |-> %q, down |-> %s]", boolText(r.Started), boolText(r.Closed), backend,
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

// record writes one step: op on node n (directory d), the model's event e, and the state after it.
func (w *walker) record(op string, n, d int, e string) {
	r := w.nodes[n]
	w.out.WriteString(",\n    ")
	fmt.Fprintf(&w.out, "[op |-> %q, n |-> %d, d |-> %d, e |-> %s, f |-> %s, phase |-> %q, reason |-> %q, "+
		"url |-> %s, holder |-> %s]", op, n, d, e, w.facts(n), r.Phase.String(), r.Reason.String(),
		boolText(r.URL != ""), w.holders())
	w.count++
}

func ev(k, s string, has bool, class string, flag bool) string {
	return fmt.Sprintf("[k |-> %q, s |-> %q, has |-> %s, cls |-> %q, flag |-> %s]", k, s, boolText(has), class,
		boolText(flag))
}

const noEvent = `[k |-> "none", s |-> "", has |-> FALSE, cls |-> "", flag |-> FALSE]`

// started is the nodes started so far, open or closed.
func (w *walker) started() []int {
	var list []int
	for n := 1; n <= nodes; n++ {
		if w.nodes[n].Started {
			list = append(list, n)
		}
	}
	return list
}

func (w *walker) step() {
	var fresh []int
	for n := 1; n <= nodes; n++ {
		if !w.nodes[n].Started {
			fresh = append(fresh, n)
		}
	}
	started := w.started()
	var live []int
	for _, n := range started {
		if !w.nodes[n].Closed {
			live = append(live, n)
		}
	}
	roll := w.rng.Intn(100)
	switch {
	case len(fresh) > 0 && (roll < 6 || len(started) == 0):
		n := fresh[w.rng.Intn(len(fresh))]
		d := 1 + w.rng.Intn(dirs)
		if ok, _ := w.table.Reserve(strconv.Itoa(d), n); !ok {
			w.record("refused", n, d, noEvent)
			return
		}
		w.dirOf[n] = d
		w.nodes[n] = lifecycle.Step(w.nodes[n], lifecycle.Event{Kind: lifecycle.Start})
		w.record("start", n, d, noEvent)
	case len(live) > 0 && roll < 9:
		n := live[w.rng.Intn(len(live))]
		w.nodes[n] = lifecycle.Step(w.nodes[n], lifecycle.Event{Kind: lifecycle.Close})
		w.table.Release(strconv.Itoa(w.dirOf[n]), n)
		w.record("close", n, w.dirOf[n], noEvent)
	default:
		// An event to a started node; now and then to a closed one, which changes nothing.
		pool := live
		if len(pool) == 0 || w.rng.Intn(20) == 0 {
			pool = started
		}
		n := pool[w.rng.Intn(len(pool))]
		e, model := w.event()
		w.nodes[n] = lifecycle.Step(w.nodes[n], e)
		w.record("event", n, w.dirOf[n], model)
	}
}

// event is a random event and the model's form of it.
func (w *walker) event() (lifecycle.Event, string) {
	switch w.rng.Intn(10) {
	case 0:
		return lifecycle.Event{Kind: lifecycle.Login}, ev("login", "", false, "", false)
	case 1, 2:
		s := backends[w.rng.Intn(len(backends))]
		return lifecycle.Event{Kind: lifecycle.Backend, Text: s}, ev("backend", s, false, "", false)
	case 3:
		url := ""
		if w.rng.Intn(4) != 0 {
			url = fmt.Sprintf("https://login.example.invalid/a/%x", w.rng.Int63())
		}
		return lifecycle.Event{Kind: lifecycle.URL, Text: url}, ev("url", "", url != "", "", false)
	case 4:
		return lifecycle.Event{Kind: lifecycle.LoginFinished}, ev("finished", "", false, "", false)
	case 5:
		m := refusals[w.rng.Intn(len(refusals))]
		return lifecycle.Event{Kind: lifecycle.ErrMessage, Text: m.text}, ev("err", "", false, m.class, false)
	case 6:
		m := logged[w.rng.Intn(len(logged))]
		return lifecycle.Event{Kind: lifecycle.Logged, Text: m.text}, ev("logged", "", false, m.class, false)
	case 7, 8:
		flag := w.rng.Intn(3) != 0
		return lifecycle.Event{Kind: lifecycle.Poll, Flag: flag}, ev("poll", "", false, "", flag)
	default:
		flag := w.rng.Intn(3) == 0
		return lifecycle.Event{Kind: lifecycle.Self, Flag: flag}, ev("self", "", false, "", flag)
	}
}

func main() {
	if len(os.Args) < 4 || len(os.Args) > 5 {
		fmt.Fprintln(os.Stderr, "usage: walk <seed> <steps> <out.tla> [--faulty-ready|--faulty-url|--faulty-close|--faulty-table]")
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
		default:
			fmt.Fprintf(os.Stderr, "walk: unknown flag %s\n", os.Args[4])
			os.Exit(2)
		}
	}
	w := &walker{rng: rand.New(rand.NewSource(seed))}
	for i := 0; i < steps; i++ {
		w.step()
	}
	var b strings.Builder
	b.WriteString("--------------------------- MODULE TailscaleTraceData ---------------------------\n")
	fmt.Fprintf(&b, "\\* Written by src/tailscale/lifecycle/walk, seed %d, %d steps.\n", seed, steps)
	fmt.Fprintf(&b, "TraceN == %d\nTraceD == %d\n", nodes, dirs)
	b.WriteString("Trace == <<\n    [op |-> \"init\"]")
	b.WriteString(w.out.String())
	b.WriteString("\n>>\n=============================================================================\n")
	if err := os.WriteFile(os.Args[3], []byte(b.String()), 0o644); err != nil {
		fmt.Fprintln(os.Stderr, "walk:", err)
		os.Exit(1)
	}
	fmt.Printf("walk: %d steps written to %s\n", w.count, os.Args[3])
}
