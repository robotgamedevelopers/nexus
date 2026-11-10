------------------------------ MODULE MCWindow ------------------------------
(***************************************************************************)
(* The instance TLC checks Window.tla on: the constants as values, and a   *)
(* bound on transcript and queue length. MCWindow.cfg is two tabs,         *)
(* MCWindow3.cfg three, MCWindowEmpty.cfg none.                            *)
(***************************************************************************)
EXTENDS Window

MCFriends0 == <<>>
MCFriends2 == <<[name |-> "ada", model |-> "m1"], [name |-> "bo", model |-> "m2"]>>
MCFriends3 == <<[name |-> "ada", model |-> "m1"], [name |-> "bo", model |-> "m2"],
                [name |-> "cy", model |-> "m3"]>>

\* every index up to one past the last tab
MCIdx0 == 0 .. 1
MCIdx2 == 0 .. 2
MCIdx3 == 0 .. 3

\* the empty draft, two all-whitespace drafts and one with text
MCDrafts == {"", " ", " \t\n", "hi"}
\* and a second text, for a witness that needs an order of queued prompts to show
MCDraftsTwoTexts == MCDrafts \cup {"yo"}
MCBlank == {"", " ", " \t\n"}

\* a friend's reply, an empty notice, and an all-whitespace Person entry
\* (append takes any entry; only send and queue refuse whitespace)
MCAppendEntries == {[kind |-> "Friend", text |-> "ok"],
                    [kind |-> "Notice", text |-> ""],
                    [kind |-> "Person", text |-> " "]}

\* Three tabs have six orders: there the drafts and entries are fewer, to keep the instance to seconds.
\* The empty draft, one all-whitespace draft and one with text; one reply.
MCDrafts3 == {"", " ", "hi"}
MCBlank3 == {"", " "}
MCAppendEntries3 == {[kind |-> "Friend", text |-> "ok"]}

\* the longest transcript, and the longest queue, TLC explores (set in each .cfg)
CONSTANT MaxLen

\* the state constraint that keeps the instance finite
Bounded == \A f \in Ids : Len(transcript[f]) <= MaxLen /\ Len(queue[f]) <= MaxLen

\* TLC tells states apart by the model variables alone. Sound because no
\* action reads `last` and no property reads it unprimed: two states that
\* differ only in `last` have the same successors and the same checks.
ModelView == model

=============================================================================
