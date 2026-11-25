# Wiring a plugin into Nexus

A fork can register a new tab type, group, shortcut and server handler together. The window can call a native Prax
renderer by name or draw a panel list composed in Starlark. The server can dispatch to a native service, handle
a request in Starlark, or pass a message to its normal AI driver.

The [plugin test fixtures](../tests/policy/fixtures/plugins) exercise both registration paths in an isolated configuration.
Each open tab has its own buffer. Submitting to a service uses the existing conversation transport and requires
the target AI in the server roster. Tabs targeting the same conversation share its latest result.

## Registering another type

Each record in `config.json`'s `tab_types` has a unique `id`, a `title`, and a `group`. Opening the first
instance places it with that group's tabs. `open` and `close` bind window keys.
A type backed by compiled code names its renderer in `native`. A script-defined type can choose its `panels`
from `label`, `text_area` and `service_reply`, with `label` providing the caption. The `service` and `invoke`
fields choose a conversation and submission key. Each open tab has its own buffer and identity.

The schema above belongs to the shipped [window application](../policy/rules/client/window.star). A fork can
change how its config is interpreted or implement a registration in code inside `handle("window", state, config)`.
Return `tab_types`, ordered `tab_groups`, the active tab’s boolean `closable`, `panels` and `shortcuts`. An `open_tab` shortcut takes a key chord, the operation name and the registered type ID. The other operations are `select_tab` (zero-based index), `move_tab` (−1 or +1 position), `zoom`
(signed delta), `open_terminal`, `close_tab`, `account_command`, `stop` and `invoke_service`. Older bundles can still use `close_terminal`.

A native UI developer registers a C++ draw function in the [view](../src/view.h)'s `nativeTabs` registry.
The draw function gets Prax's kit and parent node, the model and permission to take keyboard focus. A native
server developer registers a bounded, side-effect-free text handler through `Hub::registerService` in the server
composition root. A script developer imports a module from [the conversation application](../policy/rules/server/conversation.star)
and returns ordinary Nexus commands. Installing new compiled implementations currently requires building their
host. Rebinding installed implementations or changing script-only plugins takes a policy apply.

## Group order and closing tabs

The stock client reads the ordered `tab_groups` array from the fork's `config.json`. The default configuration looks like this.

```json
{
  "tab_groups": [
    {"id":"dashboards", "types":["dashboard"], "closable":false},
    {"id":"ais", "types":["ai"], "closable":false},
    {"id":"terminals", "types":["terminal"], "closable":true}
  ]
}
```

Each type belongs to one group. The array sets the strip order. A plugin can name an existing group or
introduce a group through its registration. Command-W checks the active group's `closable` flag. A plugin
type can set its own `closable` to `false` to keep its tabs open even in a closable group. A terminal closed
by the person ends its shell and returns focus to the previously selected tab. A shell that exits independently
still closes its tab. A fork that enables closing AI tabs dismisses the local tab until the next launch.
Its server session continues.

Command-Shift-Left and Command-Shift-Right move the selected tab one position, using the same rules as dragging. The fork can rebind these through its `tab_reorder` map. Dragging within a group keeps its members together. A group can opt into receiving tabs from another group
with `"accepts_from":["source-group-id"]`. Clicking a tab or selecting it with the arrow keys only changes focus.
To change the rules for a running window, edit the fork's client Starlark or JSON and apply the client bundle.

## Saved tab layout

The client writes the selected tab, tab order, groups and terminal working directories together in
`~/.nexus/client/tab-layout.json`. Each open terminal and extension instance has its own saved identity.
On the first launch without a layout file, the client reads the earlier `tab-order.txt`, `selected-tab.txt`
and `terminals.txt` preferences.

On relaunch, AIs return as the server sends its roster. Extension tabs reopen when their type is registered.
If the selected tab is still loading, it regains focus unless the person chooses another tab first. The fork’s
current group order and `accepts_from` rules govern where each restored tab can appear.
