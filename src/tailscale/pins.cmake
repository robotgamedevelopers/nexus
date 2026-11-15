# The Tailscale shim's pins (docs/tailscale.md), apart from the module versions go.mod and go.sum hold. CI keys the
# shim's persistent caches by this file, go.mod and go.sum together (.github/workflows/tailscale.yml), so only a change
# to one of them is a cold build: nothing else belongs in this file.

set(NEXUS_TAILSCALE_GO 1.27.1)
set(NEXUS_TAILSCALE_MODULE_VERSION v1.104.1)
# llvm-mingw, UCRT, x86_64 (github.com/mstorsjo/llvm-mingw, release 20261020, LLVM 23.1.4).
set(NEXUS_LLVM_MINGW_RELEASE 20261020)
set(NEXUS_LLVM_MINGW_NAME llvm-mingw-${NEXUS_LLVM_MINGW_RELEASE}-ucrt-x86_64)
set(NEXUS_LLVM_MINGW_SHA256 fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd)
