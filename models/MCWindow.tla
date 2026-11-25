------------------------------ MODULE MCWindow ------------------------------
(***************************************************************************)
(* The instance TLC checks Window.tla on: the constants as values, and     *)
(* bounds on transcript and queue length and on the tabs ever added.      *)
(* Each .cfg names its dashboards (BoardList: MCBoards0, MCBoards1 or      *)
(* MCBoards2) and its AIs; the README's table lists them.                  *)
(***************************************************************************)
EXTENDS Window

\* the constructor's dashboards: none (as the window makes its model), one, and two
MCBoards0 == <<>>
MCBoards1 == <<"d1">>
MCBoards2 == <<"d1", "d2">>

MCAIs0 == <<>>
MCAIs2 == <<[name |-> "ada", model |-> "m1"], [name |-> "bo", model |-> "m2"]>>
MCAIs3 == <<[name |-> "ada", model |-> "m1"], [name |-> "bo", model |-> "m2"],
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

\* an AI's reply, an empty notice, and an all-whitespace Person entry
\* (append takes any entry; only send and queue refuse whitespace)
MCAppendEntries == {[kind |-> "AI", text |-> "ok"],
                    [kind |-> "Notice", text |-> ""],
                    [kind |-> "Person", text |-> " "]}

\* Three tabs have six orders: there the drafts and entries are fewer, to keep the instance to seconds.
\* The empty draft, one all-whitespace draft and one with text; one reply.
MCDrafts3 == {"", " ", "hi"}
MCBlank3 == {"", " "}
MCAppendEntries3 == {[kind |-> "AI", text |-> "ok"]}

\* The empty draft and one with text: for the instance with the sprint tab, three AIs and a terminal, where every
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
MCIdxT0 == 0 .. 2           \* no AIs, up to two terminals
MCIdxT2 == 0 .. 4           \* two AIs, up to two terminals
MCIdxT3 == 0 .. 4           \* three AIs, up to one terminal

\* and with one dashboard in front (MCBoards1), one more
MCIdxS2T2 == 0 .. 5         \* one dashboard, two AIs, up to two terminals
MCIdxS3T1 == 0 .. 5         \* one dashboard, three AIs, up to one terminal
MCIdxS2 == 0 .. 3           \* one dashboard, two AIs, no terminals
MCIdxST0 == 0 .. 3          \* one dashboard, no AIs, up to two terminals
\* and with two (MCBoards2)
MCIdxB2T2 == 0 .. 4         \* two dashboards, no AIs, up to two terminals

\* The AIs coming and going (SpecAIsWith): one AI at the start, and add_ai tried with it (so it comes
\* back after a remove, and is refused while it stands), a second AI, and an empty name, which is refused.
MCAIs1 == <<[name |-> "ada", model |-> "m1"]>>
MCArrivals == {[name |-> "ada", model |-> "m1"], [name |-> "bo", model |-> ""], [name |-> "", model |-> ""]}
MCNextAIs == NextAIsWith(MCArrivals)
MCSpecAIs == Init /\ [][MCNextAIs]_vars
MCIdxF == 0 .. 4            \* one dashboard, one AI, and up to two tabs more (AIs, dashboards or terminals)
MCIdxF0 == 0 .. 2           \* no dashboard, no AI at the start, up to two tabs (AIs, dashboards or terminals)
MCIdxF2 == 0 .. 4           \* two dashboards, no AI at the start, up to two tabs more

\* The dashboards coming and going (SpecBoardsWith, SpecAllWith): add_board tried with d1 (standing at the start in
\* MCBoards1 and MCBoards2, so refused while it stands and back as a new tab after a remove), d3 (new) and the empty
\* name (refused).
MCBoardArrivals == {"d1", "d3", ""}
\* (Window.tla's SpecBoardsWith and SpecAllWith, written out: TLC reads a specification's Init and next-state relation only
\* through definitions without arguments.)
MCNextBoards == NextAllWith({}, MCBoardArrivals)
MCSpecBoards == Init /\ [][MCNextBoards]_vars
MCNextAll == NextAllWith(MCArrivals, MCBoardArrivals)
MCSpecAll == Init /\ [][MCNextAll]_vars

\* the state constraint that keeps the instance finite (each AI or dashboard added takes an id, as a terminal does:
\* MaxTerms bounds all three)
Bounded == /\ \A f \in Ids : Len(transcript[f]) <= MaxLen /\ Len(queue[f]) <= MaxLen
           /\ nextId <= N + B + MaxTerms

\* TLC tells states apart by the model variables alone. Sound because no
\* action reads `last` and no property reads it unprimed: two states that
\* differ only in `last` have the same successors and the same checks.
ModelView == model

=============================================================================
