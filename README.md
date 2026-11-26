# ![nexus](docs/nexus-banner.png)

**Talk to your AIs in one keyboard-driven window.** On macOS, press ⌘ and the backtick key from any app. Your AIs are right there. Type `/help` in any AI’s prompt for keyboard shortcuts and slash commands.

## What you get

- **One tab per AI.** Give an AI a name if you like. Work with Claude on a change, then switch to Codex for a second opinion. Each keeps its own conversation and workspace.
- **The same controls across harnesses.** Use `/model` and `/effort` for each AI. Type `/info` to see the account, model and working directories.
- **Room to finish a thought.** Stack messages with Enter and send them together. Close the overlay while your AIs work. Close your laptop. The server keeps going.
- **A setup your AI can change with you.** Configure AIs in nova-config. Tune shortcuts, tabs, colors and sounds in Starlark and JSON.
- **Terminal windows in tabs, and a dashboard you can customize.** Run a test or inspect a diff in a shell, then switch to the dashboard to see what your AIs are working on.

## Platform support

- **macOS 26 or newer on Apple silicon** supports the full desktop chat experience. The same Mac can run the client and server.
- **Windows x64** has native window code and tested WSL2 terminal support. Connecting that window to the Nexus server for chat is still in development.
- **Linux** (and WSL2 on Windows) runs `nexus-server` and the drivers, built and tested in CI. The desktop client waits on a Wayland backend in Prax, the UI library it is built on.
- **iPhone and iPad (iOS 26 or newer)** have one app that reaches a Nexus server on your Mac over ssh. It is still in
  development: no terminal tabs and no attachments yet. [Building and installing it](docs/ios.md)
- **Apple Watch** (watchOS 26 or newer): talk to your AIs, and they talk back, through the nexus iPhone app. The watch app runs in the simulator; the iPhone's side of its relay is still in development. [The watch](docs/watch.md)

Bring a supported subscription to use a connected AI. [Build and platform details](docs/README.md)

**To install, give this repo to the AI you already use** and say “Set up nexus on this machine so I can talk with you there.” Your AI follows the [setup instructions](docs/setup.md) and checks the running system with you.

[Using nexus](docs/README.md) · [Customize your client](docs/policy.md) · [Supported harnesses](drivers/rpc/README.md)
