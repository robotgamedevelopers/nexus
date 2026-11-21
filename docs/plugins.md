# Wiring a plugin into Nexus

A fork can register a new tab type, group, shortcut and server handler together. The window can call a native Prax
renderer by name or draw a panel list composed in Starlark. The server can dispatch to a native service, handle
a request in Starlark, or pass a message to its normal AI driver.

[The editor example](../policy/examples/editor.json) defines two independent registrations. To try both in a
fresh fork, copy the example over the empty `policy/rules/config.json`. In a customized fork merge its
`tab_types` and `services` entries with yours. Apply the client and server revisions on their respective machines.

```sh
build/nexus-policy apply client policy/rules
build/nexus-policy apply server policy/rules
git add policy/rules
git commit -m "Wire editor tabs and text services"
```

* **Command-E** opens a native Prax text editor in the **Writing** group. Its buffer is local to that tab until
  it closes. **Command-Enter** submits the buffer to the configured `text-tools` conversation. Server Starlark
  dispatches to the native `text.sha256` service and the tab displays the resulting digest.
* **Command-Shift-W** opens the **Word count** tab in **Tools**. Its UI uses the label, text area and service reply
  panels. Its server handler counts words in Starlark. Both examples bind **Command-W** to close their own tabs.
  The shortcut modifier named `Command` maps to the platform's Nexus command modifier.

Opening and typing in either tab works offline. To use the example services, add friends named `text-tools`
and `word-count` to the [server roster](accounts.md), then check and reload the roster. A submission uses the
existing conversation transport and its acknowledgement. The result is an entry in that friend's conversation.
Tabs targeting the same conversation share its latest result. These example handlers need no provider login.

## Registering another type

Each record in `config.json`'s `tab_types` has a unique `id`, a `title`, and a `group`. Opening the first
instance creates that group's strip. Tabs can be dragged within their group. `open` and `close` bind window keys.
A type backed by compiled code names its renderer in `native`. A script-defined type can choose its `panels`
from `label`, `text_area` and `service_reply`, with `label` providing the caption. The `service` and `invoke`
fields choose a conversation and submission key. Each open tab has its own buffer and identity.

The schema above belongs to the shipped [window application](../policy/rules/client/window.star). A fork can
change how its config is interpreted or implement a registration in code inside `handle("window", state, config)`.
Return the registrations as `tab_types`, plus `panels` and `shortcuts`. A shortcut is a list such as
`["Command+E", "open_tab", "editor"]`. The other operations are `select_tab` (zero-based index), `zoom`
(signed delta), `open_terminal`, `close_terminal`, `close_tab`, `stop` and `invoke_service`.

A native UI developer registers a C++ draw function in [the native tab registry](../src/native_tabs.cpp).
The draw function gets Prax's kit and parent node, the model and permission to take keyboard focus. A native
server developer registers a bounded, side-effect-free text handler through `Hub::registerService` in the server
composition root. A script developer imports a module from [the conversation application](../policy/rules/server/conversation.star)
and returns ordinary Nexus commands. Installing new compiled implementations currently requires building their
host. Rebinding installed implementations or changing script-only plugins takes a policy apply.
