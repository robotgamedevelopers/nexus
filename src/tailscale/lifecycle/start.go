package lifecycle

import (
	"errors"
	"net/netip"
	"net/url"
	"strconv"
	"strings"
)

// Hosted is what a state directory records (<dir>/nexus-tailscale.control) when its node uses Tailscale's hosted
// service, whose control URL a start leaves empty.
const Hosted = "hosted"

// hostedDomains are the domains of Tailscale's own service: a control URL naming one of them, or a host under one, is
// the hosted service, which a start reaches by leaving the control URL empty (and never with an auth key).
var hostedDomains = []string{"tailscale.com", "tailscale.io"}

// ControlURL checks a start's control server and gives its canonical form, which the state directory records and
// tsnet is given. Empty is Tailscale's hosted service (and gives ""). Anything else must be https://host or
// https://host:port, with no user, query, fragment or path but "/". The host is canonicalised before it is judged:
// letters lower-cased, one trailing dot dropped, and it must then be a valid ASCII host name (letters, digits and
// hyphens, labels of 1 to 63, at most 253 in all, the last label not all digits), an IPv4 address in dotted decimal, or
// a bracketed IPv6 address with no zone. A host in any other spelling (non-ASCII, percent-escaped, a zone id) is
// refused rather than mapped, and so is one that idna (when not nil: the IDNA lookup mapping to ASCII) would change or
// call invalid: its ASCII (xn--) form is what to give. The port, when given, is 1 to 65535, and 443 is dropped. A host
// in one of Tailscale's own domains is refused (leave the URL empty for that). plain is the one plain http URL a test
// build allows for its own loopback control server, or "" (always, in nexus's build). The URL is never repeated in an
// error: one with a user in it may hold a password.
func ControlURL(raw, plain string, idna func(string) (string, error)) (string, error) {
	if raw == "" {
		return "", nil
	}
	if plain != "" && raw == plain {
		return raw, nil
	}
	const fix = "; give https://<host> or https://<host>:<port> alone, or leave it empty for Tailscale's hosted service"
	scheme, rest, ok := strings.Cut(raw, "://")
	if !ok || !strings.EqualFold(scheme, "https") {
		return "", errors.New("the control URL is not https; a control server is reached only over https" + fix)
	}
	authority := rest
	if i := strings.IndexAny(rest, "/?#"); i >= 0 {
		authority = rest[:i]
	}
	if strings.ContainsAny(authority, "@") {
		return "", errors.New("the control URL holds a user name or password; never put a password in it" + fix)
	}
	if strings.ContainsAny(authority, "%") {
		return "", errors.New("the control URL's host is percent-escaped or carries an IPv6 zone" + fix)
	}
	u, err := url.Parse(raw)
	if err != nil {
		return "", errors.New("the control URL is not a URL" + fix)
	}
	switch {
	case u.Opaque != "" || u.Host == "":
		return "", errors.New("the control URL has no host" + fix)
	case u.User != nil:
		return "", errors.New("the control URL holds a user name or password; never put a password in it" + fix)
	case u.RawQuery != "" || u.ForceQuery || u.Fragment != "" || (u.Path != "" && u.Path != "/"):
		return "", errors.New("the control URL has more than a host (a path, query or fragment)" + fix)
	}
	host, port, err := splitAuthority(authority)
	if err != nil {
		return "", errors.New("the control URL's " + err.Error() + fix)
	}
	if !strings.HasPrefix(host, "[") {
		if _, err := netip.ParseAddr(host); err != nil && idna != nil {
			mapped, err := idna(host)
			if err != nil || mapped != host {
				return "", errors.New("the control URL's host is not a valid host name as IDNA reads it" + fix)
			}
		}
	}
	for _, d := range hostedDomains {
		if host == d || strings.HasSuffix(host, "."+d) {
			return "", errors.New("the control URL names Tailscale's hosted service; leave it empty for that")
		}
	}
	if port != "" {
		return "https://" + host + ":" + port, nil
	}
	return "https://" + host, nil
}

// splitAuthority splits a control URL's authority into its canonical host (a lower-case ASCII name with no trailing
// dot, an IPv4 address, or a bracketed IPv6 address) and its port ("" for none or 443). The error names what is wrong,
// never the text.
func splitAuthority(authority string) (host, port string, err error) {
	raw := authority
	portText, hasPort := "", false
	if strings.HasPrefix(raw, "[") {
		end := strings.Index(raw, "]")
		if end < 0 {
			return "", "", errors.New("IPv6 address has no closing bracket")
		}
		inside, after := raw[1:end], raw[end+1:]
		if after != "" {
			if !strings.HasPrefix(after, ":") {
				return "", "", errors.New("IPv6 address is followed by something but a port")
			}
			portText, hasPort = after[1:], true
		}
		addr, perr := netip.ParseAddr(inside)
		if perr != nil || !addr.Is6() || addr.Zone() != "" || addr.Is4In6() {
			return "", "", errors.New("bracketed host is not an IPv6 address (with no zone)")
		}
		host = "[" + addr.String() + "]"
	} else {
		name := raw
		if i := strings.LastIndex(raw, ":"); i >= 0 {
			name, portText, hasPort = raw[:i], raw[i+1:], true
		}
		if strings.Contains(name, ":") {
			return "", "", errors.New("host holds a colon; an IPv6 address goes in brackets")
		}
		host, err = canonicalName(name)
		if err != nil {
			return "", "", err
		}
	}
	if hasPort {
		if portText == "" || len(portText) > 5 || strings.Trim(portText, "0123456789") != "" {
			return "", "", errors.New("port is not a number from 1 to 65535")
		}
		n, _ := strconv.Atoi(portText)
		if n < 1 || n > 65535 {
			return "", "", errors.New("port is not a number from 1 to 65535")
		}
		if n != 443 {
			port = strconv.Itoa(n)
		}
	}
	return host, port, nil
}

// canonicalName is a host name or IPv4 address in its canonical form: lower case, one trailing dot dropped.
func canonicalName(name string) (string, error) {
	for i := 0; i < len(name); i++ {
		if name[i] >= 0x80 {
			return "", errors.New("host is not ASCII; give its ASCII (xn--) form")
		}
	}
	name = strings.ToLower(strings.TrimSuffix(name, "."))
	if addr, err := netip.ParseAddr(name); err == nil {
		if !addr.Is4() {
			return "", errors.New("host is not a host name or an IPv4 address")
		}
		return addr.String(), nil
	}
	if name == "" || len(name) > 253 {
		return "", errors.New("host is empty or longer than 253 characters")
	}
	labels := strings.Split(name, ".")
	for _, label := range labels {
		if label == "" || len(label) > 63 || label[0] == '-' || label[len(label)-1] == '-' ||
			strings.Trim(label, "abcdefghijklmnopqrstuvwxyz0123456789-") != "" {
			return "", errors.New("host is not a valid host name (letters, digits and hyphens in labels of 1 to 63)")
		}
	}
	if strings.Trim(labels[len(labels)-1], "0123456789") == "" {
		return "", errors.New("host's last label is all digits but it is not an IPv4 address")
	}
	return name, nil
}

// CheckAuthKey refuses an auth key with Tailscale's hosted service (control ""), where nexus signs in interactively
// only. The key itself is never in the error.
func CheckAuthKey(control, authKey string) error {
	if authKey != "" && control == "" {
		return errors.New("an auth key is given without a control URL; with Tailscale's hosted service nexus signs " +
			"in interactively only (an auth key is for your own control server, named in the control URL)")
	}
	return nil
}

// Recorded is what a state directory records for control (ControlURL's form): Hosted for "".
func Recorded(control string) string {
	if control == "" {
		return Hosted
	}
	return control
}

// CheckRecord compares the control server a state directory recorded (its record's text, trimmed; "" when it has
// none) with the one a start asks for (ControlURL's form). A directory with a node's saved state (hasState) and no
// record was changed outside nexus (the record is written before a node first starts there, and never removed), and is
// refused: guessing its control server could hand a node's keys to another. It reports what to write when there is no
// record yet ("" when the record stands), or refuses a directory of another control server.
func CheckRecord(recorded string, hasState bool, control string) (write string, err error) {
	want := Recorded(control)
	if recorded == "" {
		if hasState {
			return "", errors.New("the state directory holds a node's state but no record of its control server " +
				"(nexus-tailscale.control is missing or empty), so it was changed outside nexus; restore the record, " +
				"or give a new state directory")
		}
		return want, nil
	}
	if recorded != want {
		return "", errors.New("the state directory is " + describe(recorded) + "'s, and this start asks for " +
			describe(want) + "; a node's keys belong to one control server, so give a state directory of its own " +
			"to each")
	}
	return write, nil
}

func describe(recorded string) string {
	if recorded == Hosted {
		return "Tailscale's hosted service"
	}
	return "the control server " + recorded
}
