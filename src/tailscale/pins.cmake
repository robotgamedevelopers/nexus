# The Tailscale shim's pins (docs/tailscale.md), apart from the module versions go.mod and go.sum hold. CI keys the
# shim's persistent caches by this file, go.mod and go.sum together (.github/workflows/tailscale.yml), so only a change
# to one of them is a cold build: nothing else belongs in this file.

set(NEXUS_TAILSCALE_GO 1.27.4)
# llvm-mingw, UCRT, x86_64 (github.com/mstorsjo/llvm-mingw, release 20261020, LLVM 23.1.4).
set(NEXUS_LLVM_MINGW_RELEASE 20261020)
set(NEXUS_LLVM_MINGW_NAME llvm-mingw-${NEXUS_LLVM_MINGW_RELEASE}-ucrt-x86_64)
set(NEXUS_LLVM_MINGW_SHA256 fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd)

# The tailscale.com features the shim leaves out (tailscale.com/feature/featuretags: each ts_omit_<name> build tag drops
# one). None is used by a tsnet node that signs in interactively and talks over the tailnet; each one dropped is code,
# and for webclient an embedded web UI with its font, that nexus would otherwise ship. Every Go command of the build,
# vet and the licence notices uses the same list. Kept: netstack, portmapper, DNS, logtail (its uploads are off; it
# writes the state directory's own log buffer), health, c2n and the rest tsnet needs. cachenetmap is left out for
# correctness as well: a network map loaded from disk at a start would count as received from control, and ready means
# a map from control since this start (docs/tailscale.md).
set(NEXUS_TAILSCALE_OMIT
    ts_omit_webclient ts_omit_aws ts_omit_kube ts_omit_bird ts_omit_drive ts_omit_taildrop ts_omit_ssh ts_omit_tap
    ts_omit_tpm ts_omit_synology ts_omit_systray ts_omit_wakeonlan ts_omit_capture ts_omit_cli ts_omit_completion
    ts_omit_clientupdate ts_omit_doctor ts_omit_flashappliance ts_omit_qrcodes ts_omit_networkmanager
    ts_omit_resolved ts_omit_iptables ts_omit_dbus ts_omit_syslog ts_omit_sdnotify ts_omit_relayserver
    ts_omit_appconnectors ts_omit_conn25 ts_omit_linuxdnsfight ts_omit_webbrowser ts_omit_colorable
    ts_omit_cliconndiag ts_omit_androiddns ts_omit_androidbin ts_omit_identityfederation ts_omit_oauthkey
    ts_omit_debugeventbus ts_omit_tundevstats ts_omit_portlist ts_omit_netlog ts_omit_favorites
    ts_omit_serviceclientprefs ts_omit_cachenetmap)
