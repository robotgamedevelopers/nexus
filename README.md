# ![nexus](docs/nexus-banner.png)

**Talk to your AIs in one keyboard-driven window.** On macOS, press ⌘ and the backtick key from any app. Your conversations are right there.

## What you get

- **One tab per friend.** Work with Claude on a change, then switch to Codex for a second opinion. Each keeps its own conversation and workspace.
- **The same controls across harnesses.** Use `/model` and `/effort` to choose how a friend works. Type `/info` to see the account, model and working directories.
- **Room to finish a thought.** Stack messages with Enter and send them together. Close the overlay while your friends work. The server keeps going.
- **A setup your AI can change with you.** Configure friends in nova-config. Tune shortcuts, tabs, colors and sounds in Starlark and JSON.
- **Terminal windows in tabs, and a dashboard you can customize.** Run a test or inspect a diff in a shell, then switch to the dashboard to see what your friends are working on.

## Platform support

- **macOS 26 or newer on Apple silicon** supports the full desktop chat experience. The same Mac can run the client and server.
- **Windows x64** has a native window and tested WSL2 terminal tabs. Connecting that window to the Nexus server for chat is still in development.
- **Linux** has CI coverage for core libraries, drivers and services. The Wayland desktop client is still in development.

Bring a supported subscription to use a connected friend. [Build and platform details](docs/README.md)

**To install, give this repo to the AI you already use** and say “Set up nexus on this machine so I can talk with you there.” Your AI follows the [setup instructions](docs/setup.md) and checks the running system with you.

[Using nexus](docs/README.md) · [Customize your client](docs/policy.md) · [Supported harnesses](drivers/rpc/README.md)
