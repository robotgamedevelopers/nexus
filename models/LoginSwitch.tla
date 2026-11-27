---------------------------- MODULE LoginSwitch ----------------------------
\* A conversation's login switch (src/sync/hub.cpp: the SignInClaim that names a session and another of its AI's
\* accounts, switchTo, switchSignedIn, dropSwitches; docs/accounts.md "Switching accounts"). Glenn's rules, 2026-11-25:
\*   - /login of another account: the conversation stays on its account until the new sign-in succeeds; on success it
\*     moves at once, and its driver (which reads the login of the account it started on) is ended, so the next one
\*     starts on the new account's login alone;
\*   - a sign-in that fails, is declined or expires changes nothing;
\*   - the old account's own saved login stays: switching back needs no sign-in;
\*   - /logout deletes an account's saved login; no driver runs on it after.
\* Every account here has a login of its own (accounts/<name>/); a driver holds the login of the account it started on.
\*
\* Reversed witnesses (models/witnesses): WSwitchBeforeSignIn, the behaviour before the fix (the account picker moved the
\* conversation first and signed in after, so a declined sign-in left it on an account with no login), is rejected by
\* SwitchLandsSignedIn; WLogoutKeepsDriver (a sign-out that leaves the driver running on the login it deleted) is
\* rejected by DriverOnSignedIn.
EXTENDS FiniteSets

CONSTANTS Accounts, Sessions, None

VARIABLES
    on,         \* each session's account
    by,         \* how it came to be there: "start", "switch" or "logout" (the account it is on was signed out)
    signedIn,   \* the accounts with a saved login
    flow,       \* each account's sign-in: "none" or "running"
    pending,    \* each account: the sessions that move to it when its sign-in succeeds
    driver      \* each session's driver: the account whose login it read at its start, or None

vars == <<on, by, signedIn, flow, pending, driver>>

Init ==
    /\ on \in [Sessions -> Accounts]
    /\ by = [s \in Sessions |-> "start"]
    /\ signedIn \in SUBSET Accounts
    /\ flow = [a \in Accounts |-> "none"]
    /\ pending = [a \in Accounts |-> {}]
    /\ driver = [s \in Sessions |-> None]

\* /login of b from session s, on another account: an account signed in already takes it at once; otherwise its
\* sign-in runs and s waits on it, where it is.
Claim(s, b) ==
    /\ b # on[s]
    /\ IF b \in signedIn
       THEN /\ on' = [on EXCEPT ![s] = b]
            /\ by' = [by EXCEPT ![s] = "switch"]
            /\ UNCHANGED <<flow, pending>>
       ELSE /\ flow' = [flow EXCEPT ![b] = "running"]
            /\ pending' = [pending EXCEPT ![b] = @ \cup {s}]
            /\ UNCHANGED <<on, by>>
    /\ UNCHANGED <<signedIn, driver>>

\* b's sign-in succeeds: its login is saved, and every session waiting on it moves to it.
SignInOk(b) ==
    /\ flow[b] = "running"
    /\ signedIn' = signedIn \cup {b}
    /\ flow' = [flow EXCEPT ![b] = "none"]
    /\ on' = [s \in Sessions |-> IF s \in pending[b] THEN b ELSE on[s]]
    /\ by' = [s \in Sessions |-> IF s \in pending[b] THEN "switch" ELSE by[s]]
    /\ pending' = [pending EXCEPT ![b] = {}]
    /\ UNCHANGED driver

\* b's sign-in fails, is declined or expires: no login, and the sessions waiting on it stay where they are.
SignInFail(b) ==
    /\ flow[b] = "running"
    /\ flow' = [flow EXCEPT ![b] = "none"]
    /\ pending' = [pending EXCEPT ![b] = {}]
    /\ UNCHANGED <<on, by, signedIn, driver>>

\* A prompt starts the session's driver on its account's login, which must be there.
Start(s) ==
    /\ driver[s] = None
    /\ on[s] \in signedIn
    /\ driver' = [driver EXCEPT ![s] = on[s]]
    /\ UNCHANGED <<on, by, signedIn, flow, pending>>

\* A driver on an account that is no longer its session's is ended (Supervisor::retire, once no turn is open).
Retire(s) ==
    /\ driver[s] # None
    /\ driver[s] # on[s]
    /\ driver' = [driver EXCEPT ![s] = None]
    /\ UNCHANGED <<on, by, signedIn, flow, pending>>

\* /logout of a from a session on it, with no sign-in of a running: a's login is deleted, and every driver on it ended
\* first (Drivers::retireForSignOut). The sessions on a stay there and ask for its sign-in.
EndDrivers(a) == [s \in Sessions |-> IF driver[s] = a THEN None ELSE driver[s]]
Logout(s, a) ==
    /\ on[s] = a
    /\ a \in signedIn
    /\ flow[a] = "none"
    /\ signedIn' = signedIn \ {a}
    /\ driver' = EndDrivers(a)
    /\ by' = [t \in Sessions |-> IF on[t] = a THEN "logout" ELSE by[t]]
    /\ UNCHANGED <<on, flow, pending>>

Next ==
    \/ \E s \in Sessions, b \in Accounts : Claim(s, b)
    \/ \E b \in Accounts : SignInOk(b) \/ SignInFail(b)
    \/ \E s \in Sessions : Start(s) \/ Retire(s)
    \/ \E s \in Sessions, a \in Accounts : Logout(s, a)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ on \in [Sessions -> Accounts]
    /\ by \in [Sessions -> {"start", "switch", "logout"}]
    /\ signedIn \subseteq Accounts
    /\ flow \in [Accounts -> {"none", "running"}]
    /\ pending \in [Accounts -> SUBSET Sessions]
    /\ driver \in [Sessions -> Accounts \cup {None}]

\* A switch only ever lands a conversation on an account with a saved login: never one whose sign-in failed.
SwitchLandsSignedIn == \A s \in Sessions : by[s] = "switch" => on[s] \in signedIn

\* A session waiting on a sign-in is not on that account yet, and waits only while the sign-in runs.
WaitsWhereItIs == \A a \in Accounts : \A s \in pending[a] : on[s] # a /\ flow[a] = "running"

\* No driver runs on a login that was deleted: what a driver reads is its account's own saved login.
DriverOnSignedIn == \A s \in Sessions : driver[s] # None => driver[s] \in signedIn

\* ---- Reversed witnesses ----

\* Before the fix: the picker moved the conversation to b first (SetAccount), and b's sign-in came after.
ClaimMovesFirst(s, b) ==
    /\ b # on[s]
    /\ on' = [on EXCEPT ![s] = b]
    /\ by' = [by EXCEPT ![s] = "switch"]
    /\ IF b \in signedIn THEN UNCHANGED flow ELSE flow' = [flow EXCEPT ![b] = "running"]
    /\ UNCHANGED <<signedIn, pending, driver>>
SwitchBeforeSignInNext ==
    \/ \E s \in Sessions, b \in Accounts : ClaimMovesFirst(s, b)
    \/ \E b \in Accounts : SignInOk(b) \/ SignInFail(b)
    \/ \E s \in Sessions : Start(s) \/ Retire(s)
    \/ \E s \in Sessions, a \in Accounts : Logout(s, a)
SwitchBeforeSignIn == Init /\ [][SwitchBeforeSignInNext]_vars

\* A sign-out that deletes the login but leaves the driver that read it running.
LogoutKeepsDriver(s, a) ==
    /\ on[s] = a
    /\ a \in signedIn
    /\ flow[a] = "none"
    /\ signedIn' = signedIn \ {a}
    /\ by' = [t \in Sessions |-> IF on[t] = a THEN "logout" ELSE by[t]]
    /\ UNCHANGED <<on, flow, pending, driver>>
LogoutKeepsDriverNext ==
    \/ \E s \in Sessions, b \in Accounts : Claim(s, b)
    \/ \E b \in Accounts : SignInOk(b) \/ SignInFail(b)
    \/ \E s \in Sessions : Start(s) \/ Retire(s)
    \/ \E s \in Sessions, a \in Accounts : LogoutKeepsDriver(s, a)
LogoutKeepsDriverSpec == Init /\ [][LogoutKeepsDriverNext]_vars

=============================================================================
