package lifecycle

import (
	"errors"
	"net/url"
	"strings"
	"time"
)

// Hosted is what a state directory records (<dir>/nexus-tailscale.control) when its node uses Tailscale's hosted
// service, whose control URL a start leaves empty.
const Hosted = "hosted"

// ControlURL checks a start's control server and gives the form the state directory records. Empty is Tailscale's
// hosted service (and gives ""). Anything else must be https://host or https://host:port, with no user, query,
// fragment or path but "/", and not a host of Tailscale's own (leave it empty for that). plain is the one plain http
// URL a test build allows for its own loopback control server, or "" (always, in nexus's build). The URL is never
// repeated in an error: one with a user in it may hold a password.
func ControlURL(raw, plain string) (string, error) {
	if raw == "" {
		return "", nil
	}
	if plain != "" && raw == plain {
		return raw, nil
	}
	u, err := url.Parse(raw)
	if err != nil {
		return "", errors.New("the control URL is not a URL; give https://<your control server>, or leave it empty " +
			"for Tailscale's hosted service")
	}
	switch {
	case !strings.EqualFold(u.Scheme, "https"):
		return "", errors.New("the control URL is not https; a control server is reached only over https " +
			"(give https://<your control server>)")
	case u.Opaque != "" || u.Host == "":
		return "", errors.New("the control URL has no host; give https://<your control server>")
	case u.User != nil:
		return "", errors.New("the control URL holds a user name or password; give https://<host> alone, and " +
			"never put a password in it")
	case u.RawQuery != "" || u.ForceQuery || u.Fragment != "" || (u.Path != "" && u.Path != "/"):
		return "", errors.New("the control URL has more than a host (a path, query or fragment); give " +
			"https://<host> or https://<host>:<port> alone")
	}
	host := strings.ToLower(u.Hostname())
	if host == "tailscale.com" || strings.HasSuffix(host, ".tailscale.com") {
		return "", errors.New("the control URL names Tailscale's hosted service; leave it empty for that")
	}
	return "https://" + strings.ToLower(u.Host), nil
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
// none) with the one a start asks for (ControlURL's form). A directory with no record but a node's saved state
// (hasState: layer 0 wrote no record, and used Tailscale's hosted service) counts as Hosted. It reports what to write
// when there is no record yet ("" when the record stands), or refuses a directory of another control server.
func CheckRecord(recorded string, hasState bool, control string) (write string, err error) {
	want := Recorded(control)
	if recorded == "" {
		if !hasState {
			return want, nil
		}
		recorded = Hosted
		write = Hosted
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

// SettleAfter is how long the map poll must have been open, and the node running, at two looks for its peer map to
// count as received (Settle). Tailscale marks the poll open just before it handles the map that opened it; 200 ms is a
// margin over that hand-off, a guess, not a measurement.
const SettleAfter = 200 * time.Millisecond

// Settle tells when the map poll from control counts as open: the node running and the poll open at two looks at
// least SettleAfter apart, with neither seen false between them. The times are arguments: no clock is read here.
type Settle struct {
	open  bool
	since time.Time
}

// Look records one look at time at, and reports whether the poll has settled open.
func (s *Settle) Look(at time.Time, running, inPoll bool) bool {
	if !running || !inPoll {
		s.open = false
		return false
	}
	if !s.open {
		s.open, s.since = true, at
		return false
	}
	return at.Sub(s.since) >= SettleAfter
}

// Reset forgets the poll seen so far: after a new login the next map must be a new one.
func (s *Settle) Reset() {
	s.open = false
}
