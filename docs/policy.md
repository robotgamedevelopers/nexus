# Personal server policy

A Nexus installation has two parts a person takes with them. Configuration describes their friends, accounts,
instances and preferences. Starlark describes how to choose among the options available to them. The same Nexus
build serves people with different policies. A person's names, model preferences and working habits belong in their
profile. Local program paths and sign-in stay on each machine.

Following the engine and policy split in Sprint, Nexus asks a named question, passes the data for that decision and
checks the answer before acting. The engine owns the durable conversation, action ordering, stopping a turn,
credential handling, billing constraints and enforcement of permissions. The person can ask an AI with access to
their profile to change the answers. The client renders the server's decisions.

## The first question

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
cp -R policy/example "$HOME/nexus-profile"   # for a new profile
"$runtime/bin/python" -I -m nexus_policy check "$HOME/nexus-profile" reviewer --account work
"$runtime/bin/python" -I -m nexus_policy check "$HOME/nexus-profile" reviewer --account work --resuming
```

In the existing [drivers file](../drivers/claude/drivers.example), place the policy host before the Claude driver
command. Use absolute paths without spaces in this file. For example:

```text
driver claude /home/me/.nexus/server/policy-runtime/bin/python -I -m nexus_policy driver /home/me/nexus-profile reviewer /usr/bin/node /home/me/.nexus/server/current/drivers/claude/dist/src/main.js --account work
friend reviewer claude ~/work
```

The label after the profile path selects the friend's policy. Give friends with different labels separate driver
lines. Keep the account links, billing and instances in [the server's configuration](accounts.md).

Keep `policy.star` and `config.json` together in your own repository. Choose a working directory your configuring AI
already has permission to edit. Preview changes with `check` and replace files atomically when updating a live
profile. To move to another server, copy those two files, point its driver binding at the profile and connect its
own account. The policy host needs installing only once per server.

To carry the friend roster too, copy the applied `accounts.json` and `friends.json` from the directory reported by
`nexus-server config path` into your profile. These files contain the account names and links. On a new machine,
adapt the instance machine and directory, install the driver binding, then use the normal `config check` and
`config reload` workflow for the two files. An exported roster is a snapshot. Changes to those two copies take
effect when applied to the server configuration. Each machine signs in through Nexus.

## Extending the split

The current hook covers model and effort at driver startup. Other candidates include ordering eligible accounts at
a usage limit, choosing which queued work to release after a timer, and deciding what a return report should show.
Each new question needs a documented context, a validated result and an engine implementation that preserves
the relevant guarantees. Once a question exists, the person can change its answer entirely in their profile.
