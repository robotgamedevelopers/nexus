package main

import (
	"fmt"
	"net"
	"sync"
	"sync/atomic"
)

// kind is what a handle names.
type kind int

const (
	kindNode kind = iota + 1
	kindListener
	kindConn
	kindUDP
)

func (k kind) String() string {
	switch k {
	case kindNode:
		return "node"
	case kindListener:
		return "TCP listener"
	case kindConn:
		return "connection"
	case kindUDP:
		return "UDP socket"
	}
	return "unknown"
}

// handle is one thing the host holds by number (nts.h, Handles). Every handle comes from one counter and is never
// reused, so a number that was issued and is no longer in the table is a closed handle, and is told apart from one
// never issued.
type handle struct {
	id   int
	kind kind
	node *node // the node it belongs to (for a node, itself)

	// A listener: the tsnet listener, its accept loop's hand-off (unbuffered: a connection waits in the loop until
	// nts_accept takes it), and done, closed when the handle closes.
	ln       net.Listener
	accepted chan net.Conn
	// A connection.
	conn net.Conn
	// A UDP socket.
	pc net.PacketConn

	done chan struct{}
	// One read-side and one write-side call at a time (nts.h, Threads).
	reading, writing atomic.Bool
	closeOnce        sync.Once
}

var (
	handlesMu sync.Mutex
	handles   = map[int]*handle{}
	issued    int // the last number issued
)

// issue gives the next handle number. A number may be issued and never enter the table (a failed start, a prepare):
// it then reads as closed.
func issue() int {
	handlesMu.Lock()
	defer handlesMu.Unlock()
	issued++
	return issued
}

// lookup finds an open handle of kind want, or says why id is not one.
func lookup(id int, want kind) (*handle, error) {
	handlesMu.Lock()
	defer handlesMu.Unlock()
	return lookupLocked(id, want)
}

func lookupLocked(id int, want kind) (*handle, error) {
	h := handles[id]
	switch {
	case h != nil && (want == 0 || h.kind == want):
		return h, nil
	case h != nil:
		return nil, fmt.Errorf("%d is a %s's handle, not a %s's", id, h.kind, want)
	case id > 0 && id <= issued:
		return nil, fmt.Errorf("%d is closed (or its node is); a handle is not valid once closed", id)
	}
	return nil, fmt.Errorf("%d is not a handle this process was given", id)
}

// add puts h in the table under a new number, as one of n's handles, unless n is closed (then it reports false and h
// is the caller's to close).
func add(n *node, h *handle) bool {
	handlesMu.Lock()
	defer handlesMu.Unlock()
	if handles[n.id] == nil {
		return false
	}
	issued++
	h.id = issued
	h.node = n
	h.done = make(chan struct{})
	handles[h.id] = h
	n.children[h.id] = h
	return true
}

// remove takes h out of the table (and its node's list); false if it was out already.
func remove(h *handle) bool {
	handlesMu.Lock()
	defer handlesMu.Unlock()
	if handles[h.id] != h {
		return false
	}
	delete(handles, h.id)
	if h.node != nil && h.kind != kindNode {
		delete(h.node.children, h.id)
	}
	return true
}

// shut closes what h holds, once: anything blocked on it ends with an error.
func (h *handle) shut() {
	h.closeOnce.Do(func() {
		if h.done != nil {
			close(h.done)
		}
		switch {
		case h.ln != nil:
			h.ln.Close()
		case h.conn != nil:
			h.conn.Close()
		case h.pc != nil:
			h.pc.Close()
		}
	})
}

// acceptLoop hands the listener's connections to nts_accept until it closes; a connection nobody takes before then
// is closed.
func (h *handle) acceptLoop() {
	for {
		c, err := h.ln.Accept()
		if err != nil {
			return
		}
		select {
		case h.accepted <- c:
		case <-h.done:
			c.Close()
			return
		}
	}
}
