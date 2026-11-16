package lifecycle

import (
	"errors"
	"net/netip"
	"regexp"
	"strconv"
	"strings"
	"unicode/utf8"
)

// Peer is what a dial's decision knows of one of the node's peers: its tailnet DNS name and its tailnet addresses.
type Peer struct {
	Name  string
	Addrs []netip.Addr
}

// DialTarget decides where a dial or a datagram to address (host:port) goes, before any socket or resolver is touched:
// only to a peer of the tailnet. host is a peer's tailnet address, or a peer's DNS name (letter case and a trailing
// dot ignored) or the first label of it, which gives the peer's IPv4 address (its first address when it has none). Any
// other host (loopback, a private or public address that is no peer's, a name no peer has, an IPv6 zone) is refused
// with the reason, and nothing is sent: a node's dials never leave the tailnet, and a name is never given to the
// host's resolver.
func DialTarget(address string, peers []Peer) (netip.AddrPort, error) {
	host, portText, ok := splitHostPort(address)
	if !ok {
		return netip.AddrPort{}, errors.New("the address is not name:port or ip:port (an IPv6 address in brackets)")
	}
	if portText == "" || len(portText) > 5 || strings.Trim(portText, "0123456789") != "" {
		return netip.AddrPort{}, errors.New("the port is not a number from 1 to 65535")
	}
	port, _ := strconv.Atoi(portText)
	if port < 1 || port > 65535 {
		return netip.AddrPort{}, errors.New("the port is not a number from 1 to 65535")
	}
	if addr, err := netip.ParseAddr(host); err == nil {
		if addr.Zone() != "" {
			return netip.AddrPort{}, errors.New("the address has an IPv6 zone; a tailnet address has none")
		}
		for _, p := range peers {
			for _, a := range p.Addrs {
				if a == addr.Unmap() || a == addr {
					return netip.AddrPortFrom(a, uint16(port)), nil
				}
			}
		}
		return netip.AddrPort{}, errors.New(addr.String() + " is no peer's tailnet address; a node dials and sends " +
			"only to its tailnet's peers, never to an address off the tailnet")
	}
	want := strings.TrimSuffix(host, ".")
	if want == "" {
		return netip.AddrPort{}, errors.New("the address has no host")
	}
	for _, p := range peers {
		name := strings.TrimSuffix(p.Name, ".")
		short, _, _ := strings.Cut(name, ".")
		if !strings.EqualFold(name, want) && !strings.EqualFold(short, want) {
			continue
		}
		var chosen netip.Addr
		for _, a := range p.Addrs {
			if a.Is4() {
				chosen = a
				break
			}
		}
		if !chosen.IsValid() && len(p.Addrs) > 0 {
			chosen = p.Addrs[0]
		}
		if !chosen.IsValid() {
			return netip.AddrPort{}, errors.New("the peer " + name + " has no tailnet address yet")
		}
		return netip.AddrPortFrom(chosen, uint16(port)), nil
	}
	return netip.AddrPort{}, errors.New("no peer is named " + want + " on the tailnet; a name is resolved only " +
		"through the tailnet's peers, never by the host's resolver")
}

// splitHostPort is net.SplitHostPort's split, without its error texts: host:port, or [ipv6]:port.
func splitHostPort(address string) (host, port string, ok bool) {
	if strings.HasPrefix(address, "[") {
		end := strings.Index(address, "]")
		if end < 0 || end+1 >= len(address) || address[end+1] != ':' {
			return "", "", false
		}
		return address[1:end], address[end+2:], true
	}
	i := strings.LastIndex(address, ":")
	if i < 0 || strings.Contains(address[:i], ":") {
		return "", "", false
	}
	return address[:i], address[i+1:], true
}

// URLWithheld and SecretWithheld are what Scrub puts in place of a URL and of a secret.
const (
	URLWithheld    = "<a URL, withheld>"
	SecretWithheld = "<a secret, withheld>"
)

// urlPattern finds URLs (scheme://...); keyPattern finds Tailscale's auth keys and API tokens (tskey-...).
var (
	urlPattern = regexp.MustCompile(`[A-Za-z][A-Za-z0-9+.-]*://[^\s"'<>]+`)
	keyPattern = regexp.MustCompile(`tskey-[A-Za-z0-9_-]+`)
)

// Scrub is text with every secret in secrets (the auth keys a node was started with), every Tailscale key (tskey-...)
// and every URL (a sign-in URL is a secret) withheld, and cut to at most max bytes (on a character boundary, ending in
// "..." when cut; no cut when max is 0 or less). It is what the shim does to every text that comes from tsnet or
// control before it is shown to the host (a state's reason) or written to a node's log.
func Scrub(text string, secrets []string, max int) string {
	for _, s := range secrets {
		if s != "" {
			text = strings.ReplaceAll(text, s, SecretWithheld)
		}
	}
	text = keyPattern.ReplaceAllString(text, SecretWithheld)
	text = urlPattern.ReplaceAllString(text, URLWithheld)
	if max > 0 && len(text) > max {
		cut := max - 3
		if cut < 0 {
			cut = 0
		}
		for cut > 0 && !utf8.RuneStart(text[cut]) {
			cut--
		}
		text = text[:cut] + "..."
	}
	return text
}
