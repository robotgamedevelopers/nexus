//go:build nts_testhooks

// The test build's own control server: Tailscale's in-tree test control server (tstest/integration/testcontrol, at the
// pinned tailscale.com), with a DERP relay (derp/derpserver) and a STUN server of its own, all on 127.0.0.1 in this
// process. Nodes started against it reach no other machine: its DERP map names only the relay here, the port mapper is
// off, and nothing in it names Tailscale's hosted service. Declared in tests/tailscale/nts_test_hooks.h.

package main

/*
#include <stddef.h>
*/
import "C"

import (
	"crypto/tls"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"time"

	"tailscale.com/derp/derpserver"
	"tailscale.com/envknob"
	"tailscale.com/net/netns"
	"tailscale.com/net/stun"
	"tailscale.com/tailcfg"
	"tailscale.com/tstest/integration/testcontrol"
	"tailscale.com/types/key"
)

// testTailnet is the test control server's MagicDNS suffix: peers are <hostname>.tailnet.example.
const testTailnet = "tailnet.example"

// trackedListener remembers every connection it accepted, so that taking control down closes them all: the control
// protocol takes its connections over from the HTTP server (an upgrade), which then no longer closes them itself.
type trackedListener struct {
	net.Listener
	mu    sync.Mutex
	conns map[net.Conn]bool
}

func (l *trackedListener) Accept() (net.Conn, error) {
	c, err := l.Listener.Accept()
	if err != nil {
		return nil, err
	}
	l.mu.Lock()
	l.conns[c] = true
	l.mu.Unlock()
	return c, nil
}

func (l *trackedListener) closeAll() {
	l.Listener.Close()
	l.mu.Lock()
	defer l.mu.Unlock()
	for c := range l.conns {
		c.Close()
	}
	l.conns = map[net.Conn]bool{}
}

// testControl is the one control server a test program runs at a time.
type testControl struct {
	server *testcontrol.Server
	addr   string // 127.0.0.1:<port>, kept across down and up
	http   *http.Server
	ln     *trackedListener
	derp   *derpserver.Server
	relay  *httptest.Server
	stun   net.PacketConn
}

var (
	controlMu sync.Mutex
	control   *testControl
)

var quiet = log.New(io.Discard, "", 0)

func discard(string, ...any) {}

// serveSTUN answers STUN binding requests on pc until it closes.
func serveSTUN(pc net.PacketConn) {
	buf := make([]byte, 1500)
	for {
		n, from, err := pc.ReadFrom(buf)
		if err != nil {
			return
		}
		txID, err := stun.ParseBindingRequest(buf[:n])
		if err != nil {
			continue
		}
		udp, ok := from.(*net.UDPAddr)
		if !ok {
			continue
		}
		pc.WriteTo(stun.Response(txID, udp.AddrPort()), from)
	}
}

// listenControl serves the control server on addr ("127.0.0.1:0" the first time, its own port after).
func (c *testControl) listenControl(addr string) error {
	var ln net.Listener
	var err error
	// A port given back a moment ago may not be free at once on every OS: retry for up to ten seconds.
	for try := 0; try < 100; try++ {
		ln, err = net.Listen("tcp", addr)
		if err == nil {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if err != nil {
		return err
	}
	c.ln = &trackedListener{Listener: ln, conns: map[net.Conn]bool{}}
	c.addr = ln.Addr().String()
	c.http = &http.Server{Handler: c.server, ErrorLog: quiet}
	go c.http.Serve(c.ln)
	return nil
}

func (c *testControl) down() {
	if c.http != nil {
		c.http.Close()
		c.ln.closeAll()
		c.http = nil
	}
}

func (c *testControl) stop() {
	c.down()
	c.relay.CloseClientConnections()
	c.relay.Close()
	c.derp.Close()
	c.stun.Close()
}

func startControl(requireAuth bool, authKey string) (*testControl, error) {
	// No node of the test program reaches another machine: the port mapper off, and sockets made by the plain OS
	// calls (tailscale's netns would bind them to the default route's interface, which loopback is not).
	envknob.Setenv("TS_DISABLE_PORTMAPPER", "true")
	netns.SetEnabled(false)
	c := &testControl{}
	var err error
	c.stun, err = net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		return nil, fmt.Errorf("STUN: %v", err)
	}
	go serveSTUN(c.stun)
	c.derp = derpserver.New(key.NewNode(), discard)
	relayListener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		c.stun.Close()
		return nil, fmt.Errorf("DERP: %v", err)
	}
	c.relay = httptest.NewUnstartedServer(derpserver.Handler(c.derp))
	c.relay.Listener.Close()
	c.relay.Listener = relayListener
	c.relay.Config.ErrorLog = quiet
	c.relay.Config.TLSNextProto = map[string]func(*http.Server, *tls.Conn, http.Handler){}
	c.relay.StartTLS()
	derpMap := &tailcfg.DERPMap{Regions: map[tailcfg.DERPRegionID]*tailcfg.DERPRegion{
		1: {RegionID: 1, RegionCode: "test", RegionName: "test", Nodes: []*tailcfg.DERPNode{{
			Name: "t1", RegionID: 1, HostName: "127.0.0.1", IPv4: "127.0.0.1", IPv6: "none",
			STUNPort: c.stun.LocalAddr().(*net.UDPAddr).Port, DERPPort: relayListener.Addr().(*net.TCPAddr).Port,
			InsecureForTests: true, STUNTestIP: "127.0.0.1",
		}}},
	}}
	c.server = &testcontrol.Server{
		Logf: discard, DERPMap: derpMap, RequireAuth: requireAuth, RequireAuthKey: authKey,
		MagicDNSDomain: testTailnet, AllOnline: true,
	}
	if err := c.listenControl("127.0.0.1:0"); err != nil {
		c.relay.Close()
		c.derp.Close()
		c.stun.Close()
		return nil, fmt.Errorf("control: %v", err)
	}
	c.server.ExplicitBaseURL = "http://" + c.addr
	return c, nil
}

func currentControl(name string, errOut *C.char, errSize C.size_t) *testControl {
	if control == nil {
		failf(errOut, errSize, name, "no test control server runs; start one with nts_test_control_start")
	}
	return control
}

// nts_test_control_start starts the test control server, and writes its URL (http://127.0.0.1:<port>, which
// nts_start then accepts as a control URL) into url. With require_auth, a node without a key gets a sign-in URL that
// nts_test_control_approve completes; with an auth_key, a node must present that key.
//
//export nts_test_control_start
func nts_test_control_start(requireAuth C.int, authKey *C.char, url *C.char, urlSize C.size_t, errOut *C.char,
	errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_control_start", errOut, errSize, &rc, -1)
	controlMu.Lock()
	defer controlMu.Unlock()
	if control != nil {
		write(errOut, errSize, "nts_test_control_start: one runs already; stop it first")
		return -1
	}
	c, err := startControl(requireAuth != 0, goString(authKey))
	if err != nil {
		failf(errOut, errSize, "nts_test_control_start", "%v", err)
		return -1
	}
	control = c
	setPlainControl(c.server.ExplicitBaseURL)
	write(url, urlSize, c.server.ExplicitBaseURL)
	write(errOut, errSize, "")
	return 0
}

// nts_test_control_approve completes the sign-in at loginURL (as a person approving it in a browser would), first
// giving back an unexpired key to any node whose key expired (as an admin extending it would).
//
//export nts_test_control_approve
func nts_test_control_approve(loginURL *C.char, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_control_approve", errOut, errSize, &rc, -1)
	controlMu.Lock()
	defer controlMu.Unlock()
	c := currentControl("nts_test_control_approve", errOut, errSize)
	if c == nil {
		return -1
	}
	for _, n := range c.server.AllNodes() {
		if !n.KeyExpiry.IsZero() && n.KeyExpiry.Before(time.Now()) {
			n.KeyExpiry = time.Time{}
			c.server.UpdateNode(n)
		}
	}
	if !c.server.CompleteAuth(goString(loginURL)) {
		write(errOut, errSize, "nts_test_control_approve: the control server knows no such sign-in")
		return -1
	}
	write(errOut, errSize, "")
	return 0
}

// nodesNamed is the control server's nodes whose hostname is hostname (two while a re-login is in flight).
func (c *testControl) nodesNamed(hostname string) []*tailcfg.Node {
	var found []*tailcfg.Node
	for _, n := range c.server.AllNodes() {
		if n.Hostinfo.Valid() && n.Hostinfo.Hostname() == hostname {
			found = append(found, n)
		}
	}
	return found
}

// nts_test_control_expire expires the key of the node named hostname, a minute ago.
//
//export nts_test_control_expire
func nts_test_control_expire(hostname *C.char, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_control_expire", errOut, errSize, &rc, -1)
	controlMu.Lock()
	defer controlMu.Unlock()
	c := currentControl("nts_test_control_expire", errOut, errSize)
	if c == nil {
		return -1
	}
	found := c.nodesNamed(goString(hostname))
	if len(found) == 0 {
		write(errOut, errSize, "nts_test_control_expire: no node by that name")
		return -1
	}
	for _, n := range found {
		n.KeyExpiry = time.Now().Add(-time.Minute)
		c.server.UpdateNode(n)
	}
	write(errOut, errSize, "")
	return 0
}

// nts_test_control_set_tags gives the node named hostname the tags in tags (separated by spaces), as an admin would.
//
//export nts_test_control_set_tags
func nts_test_control_set_tags(hostname, tags *C.char, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_control_set_tags", errOut, errSize, &rc, -1)
	controlMu.Lock()
	defer controlMu.Unlock()
	c := currentControl("nts_test_control_set_tags", errOut, errSize)
	if c == nil {
		return -1
	}
	found := c.nodesNamed(goString(hostname))
	if len(found) == 0 {
		write(errOut, errSize, "nts_test_control_set_tags: no node by that name")
		return -1
	}
	for _, n := range found {
		n.Tags = strings.Fields(goString(tags))
		c.server.UpdateNode(n)
	}
	write(errOut, errSize, "")
	return 0
}

// nts_test_control_down takes the control server off the network (its port closed, every connection to it cut);
// the DERP relay stays.
//
//export nts_test_control_down
func nts_test_control_down(errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_control_down", errOut, errSize, &rc, -1)
	controlMu.Lock()
	defer controlMu.Unlock()
	c := currentControl("nts_test_control_down", errOut, errSize)
	if c == nil {
		return -1
	}
	c.down()
	write(errOut, errSize, "")
	return 0
}

// nts_test_control_up brings it back on the same port, with everything it knew.
//
//export nts_test_control_up
func nts_test_control_up(errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_control_up", errOut, errSize, &rc, -1)
	controlMu.Lock()
	defer controlMu.Unlock()
	c := currentControl("nts_test_control_up", errOut, errSize)
	if c == nil {
		return -1
	}
	if c.http != nil {
		write(errOut, errSize, "nts_test_control_up: it is up")
		return -1
	}
	if err := c.listenControl(c.addr); err != nil {
		failf(errOut, errSize, "nts_test_control_up", "%v", err)
		return -1
	}
	write(errOut, errSize, "")
	return 0
}

// nts_test_control_untrusted_url writes the DERP relay's https URL, whose certificate is self-signed: a control URL a
// node refuses on its certificate.
//
//export nts_test_control_untrusted_url
func nts_test_control_untrusted_url(url *C.char, urlSize C.size_t, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_control_untrusted_url", errOut, errSize, &rc, -1)
	controlMu.Lock()
	defer controlMu.Unlock()
	c := currentControl("nts_test_control_untrusted_url", errOut, errSize)
	if c == nil {
		return -1
	}
	write(url, urlSize, c.relay.URL)
	write(errOut, errSize, "")
	return 0
}

// nts_test_control_stop stops the test control server, its relay and its STUN server.
//
//export nts_test_control_stop
func nts_test_control_stop(errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_test_control_stop", errOut, errSize, &rc, -1)
	controlMu.Lock()
	defer controlMu.Unlock()
	c := currentControl("nts_test_control_stop", errOut, errSize)
	if c == nil {
		return -1
	}
	c.stop()
	control = nil
	setPlainControl("")
	write(errOut, errSize, "")
	return 0
}
