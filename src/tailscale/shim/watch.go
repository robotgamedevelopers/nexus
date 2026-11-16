package main

/*
#define NTS_TYPES_ONLY
#include "../nts.h"
*/
import "C"

import (
	"context"
	"fmt"
	"io"
	"log"
	"runtime"
	"strings"
	"sync"
	"time"
	"unsafe"

	"github.com/robotgamedevelopers/nexus/src/tailscale/lifecycle"
	"tailscale.com/ipn"
	"tailscale.com/ipn/ipnstate"
	"tailscale.com/tsnet"
)

// look is how often a node's watcher reads tsnet's status (its state, sign-in URL and key expiry, and whether the map
// poll is open), besides what tsnet's notifications say at once. A guess, not a measurement: four times a second
// costs little, and a host waits at most this long past the event.
const look = 250 * time.Millisecond

// node is one started tsnet server and what the shim keeps for it.
type node struct {
	id     int
	server *tsnet.Server
	held   *dirLock
	log    *cappedLog
	ctx    context.Context // cancelled when the node closes
	cancel context.CancelFunc

	// children are the node's open listeners, connections and UDP sockets (under handlesMu).
	children map[int]*handle

	mu        sync.Mutex
	state     lifecycle.Node
	changed   chan struct{} // closed, and replaced, at each change of state.Gen
	keyExpiry int64         // seconds since 1970 UTC, 0 for none or not known
	watchFn   C.nts_watch_fn
	watchUser unsafe.Pointer
	poked     bool // nts_watch set a callback: call it once at once

	poke         chan struct{} // wakes the notifier (capacity 1)
	stop         chan struct{} // closed when the node closes: the notifier returns
	notifierDone chan struct{}
	watchersDone sync.WaitGroup
	relogged     bool // under mu: nts_login was called since the start (a map tsnet holds is then an old one)
	wasInPoll    bool // the status watcher's own: the map poll was open at its last look
	stopped      bool // under mu: the node is closing, and no callback starts again
}

func newNode(id int) *node {
	ctx, cancel := context.WithCancel(context.Background())
	return &node{
		id:           id,
		ctx:          ctx,
		cancel:       cancel,
		children:     map[int]*handle{},
		changed:      make(chan struct{}),
		poke:         make(chan struct{}, 1),
		stop:         make(chan struct{}),
		notifierDone: make(chan struct{}),
	}
}

// apply steps the node's state (lifecycle.Step, the code the TLA+ trace check binds) and wakes whoever waits on a
// change. It never calls into tsnet, so it may be called from inside tsnet's logger.
func (n *node) apply(e lifecycle.Event) {
	n.mu.Lock()
	defer n.mu.Unlock()
	before := n.state.Gen
	n.state = lifecycle.Step(n.state, e)
	if n.state.Gen != before {
		close(n.changed)
		n.changed = make(chan struct{})
	}
}

// snapshot is the node's state now, with the channel that closes at its next change.
func (n *node) snapshot() (lifecycle.Node, int64, chan struct{}) {
	n.mu.Lock()
	defer n.mu.Unlock()
	return n.state, n.keyExpiry, n.changed
}

// notify runs on a goroutine of its own, locked to its OS thread so that nts_close can tell it is called from inside
// the callback: it calls the host's callback after each change, outside every lock, until the node closes.
func (n *node) notify() {
	defer close(n.notifierDone)
	// Locked for the goroutine's life: Go ends the thread with it.
	runtime.LockOSThread()
	self := threadID()
	notifiers.Store(self, true)
	defer notifiers.Delete(self)
	var last uint64
	called := false
	for {
		n.mu.Lock()
		if n.stopped {
			n.mu.Unlock()
			return
		}
		gen, fn, user, poked, changed := n.state.Gen, n.watchFn, n.watchUser, n.poked, n.changed
		n.poked = false
		call := fn != nil && (poked || !called || gen != last)
		n.mu.Unlock()
		if call {
			called, last = true, gen
			callWatch(fn, user, n.id, gen)
		}
		select {
		case <-changed:
		case <-n.poke:
		case <-n.stop:
			return
		}
	}
}

// notifiers is the OS threads that run a node's notifier (threadID -> true): a call of nts_close from one of them is
// from inside a callback.
var notifiers sync.Map

// insideCallback is whether the calling thread is running a node's callback.
func insideCallback() bool {
	_, ok := notifiers.Load(threadID())
	return ok
}

// userLog is tsnet's user log for a node: to the node's log file, which withholds every URL (tsnet's user log names
// the sign-in URL, a secret, every few seconds while one is wanted) and every key (cappedLog.Write).
func userLog(w io.Writer) func(string, ...any) {
	logger := log.New(w, "", log.LstdFlags|log.LUTC|log.Lmicroseconds)
	return func(format string, args ...any) {
		logger.Print(fmt.Sprintf(format, args...))
	}
}

// reasonMax is the most of a text from tsnet or control a state's reason holds (nts_status.reason_text holds 512).
const reasonMax = 256

// controlText is a text that came from tsnet or control, as the node's state may hold it: URLs and keys withheld. A
// control server's refusal is its own text: one that echoed the auth key or named a URL would otherwise reach the
// host. It is not cut here, so the reason code is classified from all its words; fill cuts it to reasonMax.
func controlText(text string) string {
	return scrub(text)
}

// receivedError is how tsnet's backend logs an error from control that it does not send as a notification
// (ipnlocal.LocalBackend.SetControlClientStatus): a certificate refused, control unreachable.
const receivedError = "Received error: "

// backendLog is tsnet's backend log for a node (tsnet.Server.Logf): written nowhere (tsnet keeps its own buffer
// files), but control's errors in it become the node's state. It runs inside tsnet's locks, so it only applies an
// event.
func (n *node) backendLog(format string, args ...any) {
	if !strings.HasPrefix(format, receivedError) {
		return
	}
	text := strings.TrimPrefix(fmt.Sprintf(format, args...), receivedError)
	n.apply(lifecycle.Event{Kind: lifecycle.Logged, Text: controlText(text)})
}

// pollClosed is the reason's text when the map poll to control closes.
const pollClosed = "the map poll to control closed; the tailnet keeps working while tsnet reconnects"

// watch turns what tsnet says into the node's events, until the node closes: its notifications as they come
// (watchBus), and its status every look (watchStatus).
func (n *node) watch() {
	n.watchersDone.Add(2)
	go n.watchBus()
	go n.watchStatus()
}

func (n *node) watchBus() {
	defer n.watchersDone.Done()
	for n.ctx.Err() == nil {
		client, err := n.server.LocalClient()
		if err == nil {
			var watcher interface {
				Next() (ipn.Notify, error)
				Close() error
			}
			watcher, err = client.WatchIPNBus(n.ctx, ipn.NotifyInitialState|ipn.NotifyInitialStatus)
			if err == nil {
				for {
					note, err := watcher.Next()
					if err != nil {
						break
					}
					n.fromNotify(note)
				}
				watcher.Close()
			}
		}
		select {
		case <-n.ctx.Done():
		case <-time.After(look):
		}
	}
}

// fromNotify is one of tsnet's notifications as events.
//
// A network map: tsnet sends the self node (SelfChange) after it has received a map from control and applied it
// (ipnlocal.LocalBackend.SetControlClientStatus), which is when the node has its peers; that is the map event, never a
// time. A watcher that subscribes after a map came sees it in its first notification's status (InitialStatus,
// Self.InNetworkMap); since the shim builds tsnet without its map cache (pins.cmake), a map tsnet holds came from
// control since this start, but after nts_login only a new one counts.
func (n *node) fromNotify(note ipn.Notify) {
	if note.ErrMessage != nil {
		n.apply(lifecycle.Event{Kind: lifecycle.ErrMessage, Text: controlText(*note.ErrMessage)})
	}
	if note.State != nil {
		n.apply(lifecycle.Event{Kind: lifecycle.Backend, Text: note.State.String()})
	}
	if note.BrowseToURL != nil {
		n.apply(lifecycle.Event{Kind: lifecycle.URL, Text: *note.BrowseToURL})
	}
	if note.LoginFinished != nil {
		n.apply(lifecycle.Event{Kind: lifecycle.LoginFinished})
	}
	if st := note.InitialStatus; st != nil && st.Self != nil && st.Self.InNetworkMap {
		n.mu.Lock()
		relogged := n.relogged
		n.mu.Unlock()
		if !relogged {
			if st.Self.KeyExpiry != nil {
				n.selfExpiry(true, *st.Self.KeyExpiry)
			}
			n.apply(lifecycle.Event{Kind: lifecycle.Poll, Flag: true})
		}
	}
	if note.SelfChange != nil {
		expiry := note.SelfChange.KeyExpiry
		n.selfExpiry(!expiry.IsZero(), expiry)
		// A map for an expired key brings no peers; lifecycle.Step ignores it then.
		n.apply(lifecycle.Event{Kind: lifecycle.Poll, Flag: true})
	}
}

// selfExpiry records the node's key expiry and, when it changed whether the key has expired, says so.
func (n *node) selfExpiry(known bool, expiry time.Time) {
	expired := known && expiry.Before(time.Now())
	n.mu.Lock()
	if known {
		n.keyExpiry = expiry.Unix()
	} else {
		n.keyExpiry = 0
	}
	same := n.state.KeyExpired == expired
	n.mu.Unlock()
	if !same {
		n.apply(lifecycle.Event{Kind: lifecycle.Self, Flag: expired})
	}
}

func (n *node) watchStatus() {
	defer n.watchersDone.Done()
	ticker := time.NewTicker(look)
	defer ticker.Stop()
	for {
		select {
		case <-n.ctx.Done():
			return
		case <-ticker.C:
		}
		n.lookOnce()
	}
}

// lookOnce reads tsnet's status once: its state and URL when they differ from what the node has, its key expiry, and
// whether the map poll to control has closed since the last look (tsnet's health tracker: the poll is the node's one
// connection to control, and tsnet marks it closed the moment it ends), which is the down flag at once while tsnet
// reconnects. A poll that stays open but silent (control's host gone without closing it) is seen when tsnet's own
// keep-alive watchdog closes it.
func (n *node) lookOnce() {
	client, err := n.server.LocalClient()
	if err != nil {
		return
	}
	ctx, cancel := context.WithTimeout(n.ctx, 5*time.Second)
	defer cancel()
	status, err := client.StatusWithoutPeers(ctx)
	if err != nil {
		return
	}
	inPoll := n.server.Sys().HealthTracker.Get().GetInPollNetMap()
	n.mu.Lock()
	facts := n.state
	n.mu.Unlock()
	if status.BackendState != "" && status.BackendState != facts.Backend {
		n.apply(lifecycle.Event{Kind: lifecycle.Backend, Text: status.BackendState})
	}
	if status.AuthURL != facts.PendingURL && status.AuthURL != "" {
		n.apply(lifecycle.Event{Kind: lifecycle.URL, Text: status.AuthURL})
	}
	if self := status.Self; self != nil && status.CurrentTailnet != nil {
		if self.KeyExpiry != nil {
			n.selfExpiry(true, *self.KeyExpiry)
		} else {
			n.selfExpiry(false, time.Time{})
		}
	}
	closed := n.wasInPoll && !inPoll
	n.wasInPoll = inPoll
	if closed {
		n.apply(lifecycle.Event{Kind: lifecycle.Poll, Text: pollClosed})
	}
}

// login asks tsnet for a new login: the node's peers must come again, from a new map.
func (n *node) login(ctx context.Context) error {
	n.mu.Lock()
	n.relogged = true
	n.mu.Unlock()
	n.apply(lifecycle.Event{Kind: lifecycle.Login})
	client, err := n.server.LocalClient()
	if err != nil {
		return err
	}
	return client.StartLoginInteractive(ctx)
}

// fill writes the node's state into out.
func fill(out *C.nts_status, state lifecycle.Node, keyExpiry int64) {
	out.state = C.int(state.Phase)
	out.reason = C.int(state.Reason)
	out.generation = C.ulonglong(state.Gen)
	out.key_expiry = C.longlong(keyExpiry)
	out.control_down = 0
	if state.ControlDown {
		out.control_down = 1
	}
	// The sign-in URL is given by nts_login_url only: never in the reason.
	write(&out.reason_text[0], C.size_t(len(out.reason_text)), scrubCut(state.Text, reasonMax))
}

// peerOf is a peer's status as nts_peer.
func peerOf(p *ipnstate.PeerStatus) (dns, host, addresses, tags string, online bool) {
	var four, six []string
	for _, a := range p.TailscaleIPs {
		if a.Is4() {
			four = append(four, a.String())
		} else {
			six = append(six, a.String())
		}
	}
	var tagList []string
	if p.Tags != nil {
		tagList = p.Tags.AsSlice()
	}
	return strings.TrimSuffix(p.DNSName, "."), p.HostName, strings.Join(append(four, six...), " "),
		strings.Join(tagList, " "), p.Online
}
