# Customizing Nexus in your fork

The window and server each run Starlark locally. Edit the files under `policy/rules/` in your fork, preview
an event, apply the revision on the machine running that side, and commit the files. The running process uses
the new revision the next time that event occurs. Client decisions work when the server is offline.

| File | Used for |
|---|---|
| `client.star` | Local UI decisions |
| `server.star` | Decisions about server state |
| `config.json` | JSON object passed to both scripts |
| Other `*.star` files in the same directory | Shared functions, imported with `load("helpers.star", "my_function")` |

Each entry point defines `decide(event, context, config)`. The host supplies the event name and a JSON object
describing its current state. Starlark returns a JSON-compatible value. Each hook documents which values the
engine can apply. Modules can load other files from the same bundle. Scripts receive their inputs through the
function arguments and have no host filesystem, environment, network or process API. Keep credentials out of the fork.

## Edit, preview and apply

Build the window or server once with Cargo available. CMake also produces `build/nexus-policy`.
From the fork, preview the shipped choices with sample state:

```sh
echo '{"current":0,"delta":1,"min":-3,"max":4}' |
  build/nexus-policy preview client policy/rules text_zoom
echo '{"current":"one","linked":["one","two"],"eligible":["two"]}' |
  build/nexus-policy preview server policy/rules account_after_limit
```

The first command prints `1` and the second prints `"two"`. Change the source or config and repeat.
Then apply on the machine running the corresponding host:

```sh
build/nexus-policy apply client policy/rules
build/nexus-policy apply server policy/rules
git add policy/rules
git commit -m "Customize Nexus policy"
```

Each apply compiles that side's entry point and imports. It atomically replaces
`~/.nexus/client/policy.bundle.json` or `~/.nexus/server/policy.bundle.json` only on success. Pass a host
directory as the last argument for a server started with `--dir`. Windows uses the native user's client
directory and the WSL user's server directory. On separate machines, check out the same commit on each and
apply locally. To roll back, revert the policy commit and apply again. Without an applied bundle, each host
uses the rules packaged with its binary.

## Events available in the native hosts

| Host and event | Context | Result |
|---|---|---|
| Client `text_zoom` | Integer `current` text scale step, signed key `delta`, inclusive `min` and `max` supported by the renderer | An integer in range. On an invalid answer the window keeps the current scale and reports the error. |
| Server `account_after_limit` | `current` account name, `linked` names in configuration order, and `eligible` names of other signed-in linked accounts | An eligible name, or `None` to stay. An invalid answer keeps the session on its current account and adds a notice. |

The shipped client advances by the key's delta. The shipped server cycles through the linked accounts in order.
The host reads a bundle at the decision and reuses the compiled module while its contents are the same. The
server still owns account eligibility and the durable session move. The client owns rendering and saving its scale.

A new event needs a native call site that supplies the state and validates the result. A fork can then change
what that event does in Starlark, including calling shared modules and using its own config keys.

## Claude driver startup

The server's Claude driver launcher asks `driver_start(context, config)` at each driver start, including a resume.
The policy and data live together in a directory chosen by the person. The launcher only reads that directory.
A minimal profile is in [policy/example](../policy/example).

| Input | Meaning |
|---|---|
| `context["friend"]` | The profile name assigned to this driver binding |
| `context["provider"]` | `claude` for this hook |
| `context["account"]` | The account name selected by the server |
| `context["resuming"]` | Whether the server is passing a prior harness session to the driver |
| `config` | The JSON object from the profile's `config.json` |

Return a dictionary. An empty dictionary uses the driver's defaults. The optional `model` is a model identifier
of up to 64 characters. `effort` is one of `low`, `medium`, `high`, `xhigh` or `max`. An explicit driver flag
wins over a profile default. The server continues to send explicit session settings over the driver protocol.

For example, this policy uses less effort for a fresh conversation with a designated friend. The person supplies
`quick_friend` in their own JSON.

```python
def driver_start(context, config):
    if context["friend"] == config.get("quick_friend") and not context["resuming"]:
        return {"effort": "low"}
    return {}
```

The interpreter receives frozen data, with a step and allocation budget, in a separate process with a three-second
deadline. The hook has no host I/O functions or module loader. A policy error produces a driver error with a reason
and leaves the prompt unsent to the provider. Correct the profile, preview it, and retry. A change takes effect on
the next start. A driver already answering a turn continues with its current settings.

## Install and use

On the server, install the policy host once with Python 3.11 or newer. From the Nexus checkout:

```sh
runtime="$HOME/.nexus/server/policy-runtime"
python3 -m venv "$runtime"
"$runtime/bin/python" -I -m pip install ./policy
profile="$PWD/policy/example"
"$runtime/bin/python" -I -m nexus_policy check "$profile" reviewer --account work
"$runtime/bin/python" -I -m nexus_policy check "$profile" reviewer --account work --resuming
```

In the existing [drivers file](../drivers/claude/drivers.example), place the policy host before the Claude driver
command. Use absolute paths without spaces in this file. For example:

```text
driver claude /home/me/.nexus/server/policy-runtime/bin/python -I -m nexus_policy driver /home/me/nexus/policy/example reviewer /usr/bin/node /home/me/.nexus/server/current/drivers/claude/dist/src/main.js --account work
friend reviewer claude ~/work
```

The label after the profile path selects the friend's policy. Give friends with different labels separate driver
lines. Keep the account links, billing and instances in [the server's configuration](accounts.md).

Keep the driver’s `policy.star` and `config.json` together in your fork. Choose a working directory your configuring AI
already has permission to edit. Preview changes with `check` and replace files atomically when updating a live
profile. To move to another server, copy those two files, point its driver binding at the profile and connect its
own account. The policy host needs installing only once per server.

To carry the friend roster too, copy the applied `accounts.json` and `friends.json` from the directory reported by
`nexus-server config path` into your profile. These files contain the account names and links. On a new machine,
adapt the instance machine and directory, install the driver binding, then use the normal `config check` and
`config reload` workflow for the two files. An exported roster is a snapshot. Changes to those two copies take
effect when applied to the server configuration. Each machine signs in through Nexus.
