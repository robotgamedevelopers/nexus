------------------------------ MODULE MCWindow ------------------------------
(***************************************************************************)
(* The instance TLC checks Window.tla on: the constants as values, and     *)
(* bounds on transcript and queue length and on the terminals ever added.  *)
(* MCWindow.cfg is two friends, MCWindow3.cfg three, MCWindowEmpty.cfg     *)
(* none, each after the sprint tab; MCWindowNoSprint.cfg is two friends    *)
(* without it. The witnesses run without it, as they were written.         *)
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

\* The empty draft and one with text: for the instance with the sprint tab, three friends and a terminal, where every
\* order of the selection history multiplies the states (the other instances keep an all-whitespace draft).
MCDrafts2 == {"", "hi"}
MCBlank2 == {""}

\* the longest transcript, and the longest queue, TLC explores (set in each .cfg)
CONSTANT MaxLen

\* the most terminals ever added (a .cfg may override it: MaxTerms <- ...)
MaxTerms == 1
MCMaxTerms0 == 0
MCMaxTerms2 == 2

\* positions up to one past the last tab with every terminal open
MCIdxT0 == 0 .. 2           \* no friends, up to two terminals
MCIdxT2 == 0 .. 4           \* two friends, up to two terminals
MCIdxT3 == 0 .. 4           \* three friends, up to one terminal

\* and with the sprint tab in front (Sprint = TRUE), one more
MCIdxS2T2 == 0 .. 5         \* the sprint tab, two friends, up to two terminals
MCIdxS3T1 == 0 .. 5         \* the sprint tab, three friends, up to one terminal
MCIdxS2 == 0 .. 3           \* the sprint tab, two friends, no terminals
MCIdxST0 == 0 .. 3          \* the sprint tab, no friends, up to two terminals

\* The friends coming and going (SpecFriendsWith): one friend at the start, and add_friend tried with it (so it comes
\* back after a remove, and is refused while it stands), a second friend, and an empty name, which is refused.
MCFriends1 == <<[name |-> "ada", model |-> "m1"]>>
MCArrivals == {[name |-> "ada", model |-> "m1"], [name |-> "bo", model |-> ""], [name |-> "", model |-> ""]}
MCNextFriends == NextFriendsWith(MCArrivals)
MCSpecFriends == Init /\ [][MCNextFriends]_vars
MCIdxF == 0 .. 4            \* the sprint tab, one friend, and up to two tabs more (friends or terminals)
MCIdxF0 == 0 .. 2           \* no sprint tab, no friend at the start, up to two tabs (friends or terminals)

\* the state constraint that keeps the instance finite (each friend added takes an id, as a terminal does: MaxTerms
\* bounds both)
Bounded == /\ \A f \in Ids : Len(transcript[f]) <= MaxLen /\ Len(queue[f]) <= MaxLen
           /\ nextId <= N + Pinned + MaxTerms

\* TLC tells states apart by the model variables alone. Sound because no
\* action reads `last` and no property reads it unprimed: two states that
\* differ only in `last` have the same successors and the same checks.
ModelView == model

=============================================================================
