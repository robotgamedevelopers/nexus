// The node's lifecycle as the host sees it (lifecycle.go): pure Go with no dependency, a module of its own so that the
// TLA+ trace check (models/TailscaleTrace.tla, its walk in ./walk) builds with any Go from 1.21 on, offline, without the
// pinned toolchain or tailscale.com. The shim (../shim) requires it through a directory replace in ../go.mod.
module github.com/robotgamedevelopers/nexus/src/tailscale/lifecycle

go 1.21
