package main

/*
#include <stddef.h>
#define NTS_TYPES_ONLY
#include "../nts.h"
*/
import "C"

import (
	"errors"
	"fmt"
	"io"
	"net"
	"net/netip"
	"sort"
	"strconv"
	"strings"
	"time"
	"unsafe"

	"tailscale.com/client/local"
	"tailscale.com/ipn/ipnstate"
)

// Peers, who-is, and TCP and UDP through a node's embedded stack (nts.h). Every name is resolved through the
// tailnet (tsnet's own MagicDNS, or the node's peer list), never the host's resolver, and nothing here opens a socket
// on the host's network.

// peersOf is the node's peers, sorted by DNS name.
func peersOf(n *node, timeoutMs C.int) ([]*ipnstate.PeerStatus, error) {
	client, err := n.server.LocalClient()
	if err != nil {
		return nil, err
	}
	ctx, cancel := within(n, timeoutMs)
	defer cancel()
	status, err := client.Status(ctx)
	if err != nil {
		return nil, err
	}
	list := make([]*ipnstate.PeerStatus, 0, len(status.Peer))
	for _, p := range status.Peer {
		list = append(list, p)
	}
	sort.Slice(list, func(i, j int) bool { return list[i].DNSName < list[j].DNSName })
	return list, nil
}

// sameName is whether a peer's DNS name is name: letter case and a trailing dot ignored.
func sameName(dnsName, name string) bool {
	return strings.EqualFold(strings.TrimSuffix(dnsName, "."), strings.TrimSuffix(name, "."))
}

func putPeer(out *C.nts_peer, p *ipnstate.PeerStatus) {
	dns, host, addresses, tags, online := peerOf(p)
	write(&out.dns_name[0], C.size_t(len(out.dns_name)), dns)
	write(&out.host_name[0], C.size_t(len(out.host_name)), host)
	write(&out.addresses[0], C.size_t(len(out.addresses)), addresses)
	write(&out.tags[0], C.size_t(len(out.tags)), tags)
	out.online = 0
	if online {
		out.online = 1
	}
}

//export nts_peers
func nts_peers(id C.int, out *C.nts_peer, max C.int, timeoutMs C.int, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_peers", errOut, errSize, &rc, -1)
	testPanic("nts_peers")
	if max < 0 || (max > 0 && out == nil) {
		write(errOut, errSize, "nts_peers: max is negative, or out is null with max above 0")
		return -1
	}
	n := nodeOf("nts_peers", id, errOut, errSize)
	if n == nil {
		return -1
	}
	list, err := peersOf(n, timeoutMs)
	if err != nil {
		failf(errOut, errSize, "nts_peers", "tsnet's status: %v", err)
		return -1
	}
	if max > 0 {
		slots := unsafe.Slice(out, int(max))
		for i := 0; i < len(list) && i < int(max); i++ {
			putPeer(&slots[i], list[i])
		}
	}
	write(errOut, errSize, "")
	return C.int(len(list))
}

//export nts_peer_find
func nts_peer_find(id C.int, name *C.char, out *C.nts_peer, timeoutMs C.int, errOut *C.char,
	errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_peer_find", errOut, errSize, &rc, -1)
	testPanic("nts_peer_find")
	if name == nil || out == nil {
		write(errOut, errSize, "nts_peer_find: a DNS name and an nts_peer to fill are both needed")
		return -1
	}
	n := nodeOf("nts_peer_find", id, errOut, errSize)
	if n == nil {
		return -1
	}
	list, err := peersOf(n, timeoutMs)
	if err != nil {
		failf(errOut, errSize, "nts_peer_find", "tsnet's status: %v", err)
		return -1
	}
	want := C.GoString(name)
	write(errOut, errSize, "")
	for _, p := range list {
		if sameName(p.DNSName, want) {
			putPeer(out, p)
			return 1
		}
	}
	return 0
}

//export nts_whois_addr
func nts_whois_addr(id C.int, remote *C.char, out *C.nts_whois, timeoutMs C.int, errOut *C.char,
	errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_whois_addr", errOut, errSize, &rc, -1)
	testPanic("nts_whois_addr")
	if remote == nil || out == nil {
		write(errOut, errSize, "nts_whois_addr: an address and an nts_whois to fill are both needed")
		return -1
	}
	addr, err := netip.ParseAddrPort(C.GoString(remote))
	if err != nil {
		failf(errOut, errSize, "nts_whois_addr", "the address is not ip:port: %v", err)
		return -1
	}
	n := nodeOf("nts_whois_addr", id, errOut, errSize)
	if n == nil {
		return -1
	}
	client, err := n.server.LocalClient()
	if err != nil {
		failf(errOut, errSize, "nts_whois_addr", "no local client: %v", err)
		return -1
	}
	ctx, cancel := within(n, timeoutMs)
	defer cancel()
	who, err := client.WhoIs(ctx, addr.String())
	if errors.Is(err, local.ErrPeerNotFound) {
		write(errOut, errSize, "")
		return 0
	}
	if err != nil {
		failf(errOut, errSize, "nts_whois_addr", "tsnet's who-is: %v", err)
		return -1
	}
	login, display, nodeName, tags := "", "", "", ""
	if who.UserProfile != nil {
		login, display = who.UserProfile.LoginName, who.UserProfile.DisplayName
	}
	if who.Node != nil {
		nodeName = strings.TrimSuffix(who.Node.Name, ".")
		tags = strings.Join(who.Node.Tags, " ")
	}
	write(&out.login[0], C.size_t(len(out.login)), login)
	write(&out.display_name[0], C.size_t(len(out.display_name)), display)
	write(&out.node_name[0], C.size_t(len(out.node_name)), nodeName)
	write(&out.tags[0], C.size_t(len(out.tags)), tags)
	write(errOut, errSize, "")
	return 1
}

// validPort reports whether port is 1..65535.
func validPort(port C.int) bool {
	return port >= 1 && port <= 65535
}

//export nts_listen_tcp
func nts_listen_tcp(id C.int, port C.int, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_listen_tcp", errOut, errSize, &rc, 0)
	testPanic("nts_listen_tcp")
	if !validPort(port) {
		failf(errOut, errSize, "nts_listen_tcp", "port %d is not 1..65535", int(port))
		return 0
	}
	n := nodeOf("nts_listen_tcp", id, errOut, errSize)
	if n == nil {
		return 0
	}
	ln, err := n.server.Listen("tcp", ":"+strconv.Itoa(int(port)))
	if err != nil {
		failf(errOut, errSize, "nts_listen_tcp", "%v", err)
		return 0
	}
	h := &handle{kind: kindListener, ln: ln, accepted: make(chan net.Conn)}
	if !add(n, h) {
		ln.Close()
		failf(errOut, errSize, "nts_listen_tcp", "node %d closed while listening", int(id))
		return 0
	}
	go h.acceptLoop()
	write(errOut, errSize, "")
	return C.int(h.id)
}

//export nts_accept
func nts_accept(id C.int, timeoutMs C.int, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_accept", errOut, errSize, &rc, C.NTS_ERROR)
	testPanic("nts_accept")
	h, err := lookup(int(id), kindListener)
	if err != nil {
		failf(errOut, errSize, "nts_accept", "%v", err)
		return C.NTS_ERROR
	}
	if !h.reading.CompareAndSwap(false, true) {
		failf(errOut, errSize, "nts_accept", "busy: another nts_accept on listener %d is waiting", int(id))
		return C.NTS_ERROR
	}
	defer h.reading.Store(false)
	timeout, stop := deadline(timeoutMs)
	defer stop()
	select {
	case c := <-h.accepted:
		child := &handle{kind: kindConn, conn: c}
		if !add(h.node, child) {
			c.Close()
			failf(errOut, errSize, "nts_accept", "the listener's node closed")
			return C.NTS_ERROR
		}
		write(errOut, errSize, "")
		return C.int(child.id)
	case <-h.done:
		failf(errOut, errSize, "nts_accept", "listener %d was closed while waiting", int(id))
		return C.NTS_ERROR
	case <-timeout:
		write(errOut, errSize, "nts_accept: timed out")
		return C.NTS_TIMEOUT
	}
}

// dial is nts_dial_tcp and nts_dial_udp: network "tcp" or "udp".
func dial(name string, id C.int, network string, address *C.char, timeoutMs C.int, errOut *C.char,
	errSize C.size_t) C.int {
	testPanic(name)
	if address == nil {
		failf(errOut, errSize, name, "no address given (a null pointer); pass name:port or ip:port")
		return 0
	}
	n := nodeOf(name, id, errOut, errSize)
	if n == nil {
		return 0
	}
	ctx, cancel := within(n, timeoutMs)
	defer cancel()
	c, err := n.server.Dial(ctx, network, C.GoString(address))
	if err != nil {
		if ctx.Err() != nil && n.ctx.Err() == nil {
			failf(errOut, errSize, name, "timed out: %v", err)
		} else {
			failf(errOut, errSize, name, "%v", err)
		}
		return 0
	}
	h := &handle{kind: kindConn, conn: c}
	if !add(n, h) {
		c.Close()
		failf(errOut, errSize, name, "node %d closed while dialing", int(id))
		return 0
	}
	write(errOut, errSize, "")
	return C.int(h.id)
}

//export nts_dial_tcp
func nts_dial_tcp(id C.int, address *C.char, timeoutMs C.int, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_dial_tcp", errOut, errSize, &rc, 0)
	return dial("nts_dial_tcp", id, "tcp", address, timeoutMs, errOut, errSize)
}

//export nts_dial_udp
func nts_dial_udp(id C.int, address *C.char, timeoutMs C.int, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_dial_udp", errOut, errSize, &rc, 0)
	return dial("nts_dial_udp", id, "udp", address, timeoutMs, errOut, errSize)
}

//export nts_listen_udp
func nts_listen_udp(id C.int, port C.int, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_listen_udp", errOut, errSize, &rc, 0)
	testPanic("nts_listen_udp")
	if !validPort(port) {
		failf(errOut, errSize, "nts_listen_udp", "port %d is not 1..65535", int(port))
		return 0
	}
	n := nodeOf("nts_listen_udp", id, errOut, errSize)
	if n == nil {
		return 0
	}
	ip4, _ := n.server.TailscaleIPs()
	if !ip4.IsValid() {
		failf(errOut, errSize, "nts_listen_udp", "the node has no tailnet address yet; wait until it is ready")
		return 0
	}
	pc, err := n.server.ListenPacket("udp4", netip.AddrPortFrom(ip4, uint16(port)).String())
	if err != nil {
		failf(errOut, errSize, "nts_listen_udp", "%v", err)
		return 0
	}
	h := &handle{kind: kindUDP, pc: pc}
	if !add(n, h) {
		pc.Close()
		failf(errOut, errSize, "nts_listen_udp", "node %d closed while listening", int(id))
		return 0
	}
	write(errOut, errSize, "")
	return C.int(h.id)
}

// ioDeadline is the time a read or write of timeoutMs gives up: none for a negative one, and for 0 a millisecond on
// (the embedded stack fails a read whose deadline has passed before it looks for data).
func ioDeadline(timeoutMs C.int) time.Time {
	if timeoutMs < 0 {
		return time.Time{}
	}
	if timeoutMs == 0 {
		timeoutMs = 1
	}
	return time.Now().Add(time.Duration(timeoutMs) * time.Millisecond)
}

func isTimeout(err error) bool {
	var ne net.Error
	return errors.As(err, &ne) && ne.Timeout()
}

// ioResult is a read's or write's count and error as the API's answer.
func ioResult(name string, h *handle, count int, err error, eofIsZero bool, errOut *C.char,
	errSize C.size_t) C.longlong {
	closed := false
	select {
	case <-h.done:
		closed = true
	default:
	}
	switch {
	case err == nil || (count > 0 && !isTimeout(err)):
		write(errOut, errSize, "")
		return C.longlong(count)
	case closed:
		// Closed under the call (by another thread, or with its node): an error, whatever the stack said.
		failf(errOut, errSize, name, "handle %d was closed during the call", h.id)
		return C.NTS_ERROR
	case isTimeout(err):
		write(errOut, errSize, name+": timed out")
		return C.NTS_TIMEOUT
	case eofIsZero && errors.Is(err, io.EOF):
		write(errOut, errSize, "")
		return 0
	}
	failf(errOut, errSize, name, "%v", err)
	return C.NTS_ERROR
}

// ioHandle finds the open handle id names, of one of kinds, for the export name.
func ioHandle(name string, id C.int, errOut *C.char, errSize C.size_t, kinds ...kind) *handle {
	h, err := lookup(int(id), 0)
	if err == nil {
		for _, k := range kinds {
			if h.kind == k {
				return h
			}
		}
		err = fmt.Errorf("%d is a %s's handle, not a %s's", int(id), h.kind, kinds[0])
	}
	failf(errOut, errSize, name, "%v", err)
	return nil
}

//export nts_read
func nts_read(id C.int, buf unsafe.Pointer, size C.size_t, timeoutMs C.int, errOut *C.char,
	errSize C.size_t) (rc C.longlong) {
	defer recoverLong("nts_read", errOut, errSize, &rc)
	testPanic("nts_read")
	h := ioHandle("nts_read", id, errOut, errSize, kindConn)
	if h == nil {
		return C.NTS_ERROR
	}
	if buf == nil && size > 0 {
		write(errOut, errSize, "nts_read: a null buffer")
		return C.NTS_ERROR
	}
	if !h.reading.CompareAndSwap(false, true) {
		failf(errOut, errSize, "nts_read", "busy: another nts_read on %d is running", int(id))
		return C.NTS_ERROR
	}
	defer h.reading.Store(false)
	h.conn.SetReadDeadline(ioDeadline(timeoutMs))
	count, err := h.conn.Read(unsafe.Slice((*byte)(buf), int(size)))
	return ioResult("nts_read", h, count, err, true, errOut, errSize)
}

//export nts_write
func nts_write(id C.int, buf unsafe.Pointer, size C.size_t, timeoutMs C.int, errOut *C.char,
	errSize C.size_t) (rc C.longlong) {
	defer recoverLong("nts_write", errOut, errSize, &rc)
	testPanic("nts_write")
	h := ioHandle("nts_write", id, errOut, errSize, kindConn)
	if h == nil {
		return C.NTS_ERROR
	}
	if buf == nil && size > 0 {
		write(errOut, errSize, "nts_write: a null buffer")
		return C.NTS_ERROR
	}
	if !h.writing.CompareAndSwap(false, true) {
		failf(errOut, errSize, "nts_write", "busy: another nts_write on %d is running", int(id))
		return C.NTS_ERROR
	}
	defer h.writing.Store(false)
	h.conn.SetWriteDeadline(ioDeadline(timeoutMs))
	count, err := h.conn.Write(unsafe.Slice((*byte)(buf), int(size)))
	return ioResult("nts_write", h, count, err, false, errOut, errSize)
}

//export nts_recv_from
func nts_recv_from(id C.int, buf unsafe.Pointer, size C.size_t, timeoutMs C.int, from *C.char, fromSize C.size_t,
	errOut *C.char, errSize C.size_t) (rc C.longlong) {
	defer recoverLong("nts_recv_from", errOut, errSize, &rc)
	testPanic("nts_recv_from")
	h := ioHandle("nts_recv_from", id, errOut, errSize, kindUDP)
	if h == nil {
		return C.NTS_ERROR
	}
	if buf == nil && size > 0 {
		write(errOut, errSize, "nts_recv_from: a null buffer")
		return C.NTS_ERROR
	}
	if !h.reading.CompareAndSwap(false, true) {
		failf(errOut, errSize, "nts_recv_from", "busy: another nts_recv_from on %d is running", int(id))
		return C.NTS_ERROR
	}
	defer h.reading.Store(false)
	h.pc.SetReadDeadline(ioDeadline(timeoutMs))
	count, addr, err := h.pc.ReadFrom(unsafe.Slice((*byte)(buf), int(size)))
	if err == nil && addr != nil {
		write(from, fromSize, addr.String())
	}
	return ioResult("nts_recv_from", h, count, err, false, errOut, errSize)
}

// resolveUDP is address as a UDP address on the tailnet: ip:port, or name:port with name a peer's DNS name (or its
// first label), resolved through the node's peer list.
func resolveUDP(n *node, address string, timeoutMs C.int) (*net.UDPAddr, error) {
	if ap, err := netip.ParseAddrPort(address); err == nil {
		return net.UDPAddrFromAddrPort(ap), nil
	}
	host, portText, err := net.SplitHostPort(address)
	if err != nil {
		return nil, fmt.Errorf("the address is not name:port or ip:port: %v", err)
	}
	port, err := strconv.Atoi(portText)
	if err != nil || port < 1 || port > 65535 {
		return nil, fmt.Errorf("the port %q is not 1..65535", portText)
	}
	list, err := peersOf(n, timeoutMs)
	if err != nil {
		return nil, fmt.Errorf("tsnet's status: %v", err)
	}
	for _, p := range list {
		short, _, _ := strings.Cut(p.DNSName, ".")
		if !sameName(p.DNSName, host) && !strings.EqualFold(short, host) {
			continue
		}
		for _, a := range p.TailscaleIPs {
			if a.Is4() {
				return net.UDPAddrFromAddrPort(netip.AddrPortFrom(a, uint16(port))), nil
			}
		}
	}
	return nil, fmt.Errorf("no peer is named %s on the tailnet", host)
}

//export nts_send_to
func nts_send_to(id C.int, buf unsafe.Pointer, size C.size_t, address *C.char, timeoutMs C.int, errOut *C.char,
	errSize C.size_t) (rc C.longlong) {
	defer recoverLong("nts_send_to", errOut, errSize, &rc)
	testPanic("nts_send_to")
	h := ioHandle("nts_send_to", id, errOut, errSize, kindUDP)
	if h == nil {
		return C.NTS_ERROR
	}
	if address == nil || (buf == nil && size > 0) {
		write(errOut, errSize, "nts_send_to: a null buffer or address")
		return C.NTS_ERROR
	}
	if !h.writing.CompareAndSwap(false, true) {
		failf(errOut, errSize, "nts_send_to", "busy: another nts_send_to on %d is running", int(id))
		return C.NTS_ERROR
	}
	defer h.writing.Store(false)
	to, err := resolveUDP(h.node, C.GoString(address), timeoutMs)
	if err != nil {
		failf(errOut, errSize, "nts_send_to", "%v", err)
		return C.NTS_ERROR
	}
	h.pc.SetWriteDeadline(ioDeadline(timeoutMs))
	count, err := h.pc.WriteTo(unsafe.Slice((*byte)(buf), int(size)), to)
	return ioResult("nts_send_to", h, count, err, false, errOut, errSize)
}

//export nts_local_addr
func nts_local_addr(id C.int, out *C.char, outSize C.size_t, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_local_addr", errOut, errSize, &rc, -1)
	testPanic("nts_local_addr")
	h := ioHandle("nts_local_addr", id, errOut, errSize, kindListener, kindConn, kindUDP)
	if h == nil {
		return -1
	}
	var addr net.Addr
	switch h.kind {
	case kindListener:
		addr = h.ln.Addr()
	case kindConn:
		addr = h.conn.LocalAddr()
	default:
		addr = h.pc.LocalAddr()
	}
	write(out, outSize, addr.String())
	write(errOut, errSize, "")
	return 0
}

//export nts_remote_addr
func nts_remote_addr(id C.int, out *C.char, outSize C.size_t, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_remote_addr", errOut, errSize, &rc, -1)
	testPanic("nts_remote_addr")
	h := ioHandle("nts_remote_addr", id, errOut, errSize, kindConn)
	if h == nil {
		return -1
	}
	write(out, outSize, h.conn.RemoteAddr().String())
	write(errOut, errSize, "")
	return 0
}

//export nts_handle_close
func nts_handle_close(id C.int, errOut *C.char, errSize C.size_t) (rc C.int) {
	defer recoverTo("nts_handle_close", errOut, errSize, &rc, -1)
	testPanic("nts_handle_close")
	h := ioHandle("nts_handle_close", id, errOut, errSize, kindListener, kindConn, kindUDP)
	if h == nil {
		if lh, err := lookup(int(id), kindNode); err == nil && lh != nil {
			write(errOut, errSize, fmt.Sprintf("nts_handle_close: %d is a node; close it with nts_close", int(id)))
		}
		return -1
	}
	if !remove(h) {
		failf(errOut, errSize, "nts_handle_close", "%d is closed already", int(id))
		return -1
	}
	h.shut()
	write(errOut, errSize, "")
	return 0
}
