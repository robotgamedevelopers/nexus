# Wiring a plugin into Nexus

A fork can register a new tab type, group, shortcut and server handler together. The window can call a native Prax
renderer by name or draw a panel list composed in Starlark. The server can dispatch to a native service, handle
a request in Starlark, or pass a message to its normal AI driver.

The [plugin test fixtures](../tests/policy/fixtures/plugins) exercise both registration paths in an isolated configuration.
Each open tab has its own buffer. Submitting to a service uses the existing conversation transport and requires
the target friend in the server roster. Tabs targeting the same conversation share its latest result.

## Registering another type

Each record in `config.json`'s `tab_types` has a unique `id`, a `title`, and a `group`. Opening the first
instance creates that group's strip. Tabs can be dragged within their group. `open` and `close` bind window keys.
A type backed by compiled code names its renderer in `native`. A script-defined type can choose its `panels`
from `label`, `text_area` and `service_reply`, with `label` providing the caption. The `service` and `invoke`
fields choose a conversation and submission key. Each open tab has its own buffer and identity.

The schema above belongs to the shipped [window application](../policy/rules/client/window.star). A fork can
change how its config is interpreted or implement a registration in code inside `handle("window", state, config)`.
Return the registrations as `tab_types`, plus `panels` and `shortcuts`. An `open_tab` shortcut takes a key chord, the operation name and the registered type ID. The other operations are `select_tab` (zero-based index), `zoom`
(signed delta), `open_terminal`, `close_terminal`, `close_tab`, `stop` and `invoke_service`.

A native UI developer registers a C++ draw function in the [view](../src/view.h)'s `nativeTabs` registry.
The draw function gets Prax's kit and parent node, the model and permission to take keyboard focus. A native
server developer registers a bounded, side-effect-free text handler through `Hub::registerService` in the server
composition root. A script developer imports a module from [the conversation application](../policy/rules/server/conversation.star)
and returns ordinary Nexus commands. Installing new compiled implementations currently requires building their
host. Rebinding installed implementations or changing script-only plugins takes a policy apply.
