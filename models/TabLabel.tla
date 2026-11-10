------------------------------ MODULE TabLabel ------------------------------
(***************************************************************************)
(* The label of one terminal tab across ssh sessions: nexus::TabLabel in  *)
(* src/remote.h.                                                           *)
(*                                                                         *)
(* Once a frame the window observes the terminal: the remote label of its  *)
(* foreground process (NoRemote when that is not ssh), the title the       *)
(* program set, and whether the frame comes after the tail of a session    *)
(* that ended (the code compares the frame's time with the session's end   *)
(* plus kSessionTailSeconds; here it is the boolean `late`). The label is  *)
(* drawn from the state and the title at the time it is drawn.             *)
(*                                                                         *)
(* State, as the code's members:                                           *)
(*   remote   the session's remote label; NoRemote outside a session       *)
(*   before   the title at the last observation outside sessions and masks *)
(*   after    while masking, the title the session left                    *)
(*   masking  a session ended and the title is still one it left           *)
(*   tail     while masking, the session's tail, whose titles are its own  *)
(* `last` is a history variable: the last observation's arguments.         *)
(***************************************************************************)
EXTENDS TLC

CONSTANTS
    Titles,     \* the titles a program sets, the empty title among them
    Remotes     \* the remote labels of ssh sessions, none empty

NoRemote == ""

ASSUME NoRemote \notin Remotes /\ "" \in Titles

VARIABLES remote, before, after, masking, tail, last

vars == <<remote, before, after, masking, tail, last>>

TypeOK ==
    /\ remote \in Remotes \cup {NoRemote}
    /\ before \in Titles
    /\ after \in Titles
    /\ masking \in BOOLEAN
    /\ tail \in BOOLEAN
    /\ last \in [op : {"init", "observe"}, remote : Remotes \cup {NoRemote}, title : Titles, late : BOOLEAN]

\* TabLabel::label: the label drawn when the program's title is t
Label(t) ==
    IF remote # NoRemote THEN remote
    ELSE IF masking /\ (tail \/ t = after) THEN before
    ELSE t

Init ==
    /\ remote = NoRemote
    /\ before = ""
    /\ after = ""
    /\ masking = FALSE
    /\ tail = FALSE
    /\ last = [op |-> "init", remote |-> NoRemote, title |-> "", late |-> FALSE]

Did(r, t, late) == last' = [op |-> "observe", remote |-> r, title |-> t, late |-> late]

\* TabLabel::observe(r, t, time), with late the code's `time >= tailEnds_`
Observe(r, t, late) ==
    /\ Did(r, t, late)
    /\ IF r # NoRemote
       THEN \* in a session: it names the label
            /\ remote' = r
            /\ UNCHANGED <<before, after, masking, tail>>
       ELSE IF remote # NoRemote
       THEN \* the session ended: the title now, and any in its tail, is the session's
            /\ remote' = NoRemote
            /\ masking' = TRUE
            /\ tail' = TRUE
            /\ after' = t
            /\ UNCHANGED before
       ELSE LET inTail == masking /\ tail /\ ~late
                masked == masking /\ (inTail \/ t = after)
            IN /\ remote' = NoRemote
               /\ tail' = inTail
               /\ masking' = masked
               /\ after' = IF inTail THEN t ELSE IF masked THEN after ELSE ""
               /\ before' = IF masked THEN before ELSE t

Next == \E r \in Remotes \cup {NoRemote}, t \in Titles, late \in BOOLEAN : Observe(r, t, late)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Properties, in the owner's terms.                                       *)
(***************************************************************************)

\* The tail is part of a mask.
TailMasks == tail => masking

\* While a session is seen, the tab names where it goes, whatever title the program sets.
SessionNamed == remote # NoRemote => \A t \in Titles : Label(t) = remote

\* Outside sessions and masks, the tab shows the program's title.
TitleShows == (remote = NoRemote /\ ~masking) => \A t \in Titles : Label(t) = t

\* An observation of a session names it at once.
SessionNamedAtOnce ==
    [][last'.remote # NoRemote => \A t \in Titles : Label(t)' = last'.remote]_vars

\* The observation that sees a session end gives back the label from before it, whatever title the session left.
EndRestores ==
    [][(remote # NoRemote /\ last'.remote = NoRemote) => \A t \in Titles : Label(t)' = before]_vars

\* In the session's tail, the titles that arrive are the session's: the label stays the one from before.
TailRestores ==
    [][(remote = NoRemote /\ last'.remote = NoRemote /\ tail /\ ~last'.late) =>
        \A t \in Titles : Label(t)' = before]_vars

\* The mask never sticks: past the tail, the first title other than the one the session left is shown.
MaskEnds ==
    [][(remote = NoRemote /\ last'.remote = NoRemote /\ masking /\ (~tail \/ last'.late) /\ last'.title # after) =>
        (~masking' /\ \A t \in Titles : t = last'.title => Label(t)' = t)]_vars

\* The label to return to is only ever a title observed outside sessions and masks: never one seen as a session starts.
BeforeIsOwn ==
    [][before' # before => (remote = NoRemote /\ last'.remote = NoRemote /\ ~masking' /\ before' = last'.title)]_vars

\* And it is the latest such title.
BeforeIsLatest ==
    [][(remote = NoRemote /\ last'.remote = NoRemote /\ ~masking') => before' = last'.title]_vars

=============================================================================
