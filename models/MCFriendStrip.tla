---------------------------- MODULE MCFriendStrip ----------------------------
(***************************************************************************)
(* The instances TLC checks FriendStrip.tla on. MCFriendStrip.cfg: two     *)
(* friends and one bad name, lists of up to three, two texts besides the   *)
(* empty draft, the saved order naming the friends the other way round     *)
(* from the lists that name them first, so that NewInSavedOrder bites.     *)
(* MCFriendStrip3.cfg: three friends, the saved order naming two. In      *)
(* both the window is alone on its server. MCFriendStripShared.cfg: two    *)
(* friends, lists of up to two, one text, and another window of the       *)
(* server that keeps one draft, "o", the drafts in the file bounded by     *)
(* Small.                                                                  *)
(***************************************************************************)
EXTENDS FriendStrip

MCNames2 == {"a", "b"}
MCNames3 == {"a", "b", "c"}
MCBad == {"x y"}
MCTexts == {"", "t", "u"}
MCOrder2 == <<"b", "a">>
MCOrder3 == <<"c", "a">>
MCAlone == {}
MCOther == {"o"}
MCTextsShared == {"", "t"}

\* The file's drafts are bounded for TLC: a draft another window kept, read back at a start and kept again, is joined
\* again (by Sep) each time round.
Small == \A n \in DOMAIN file : Len(file[n]) <= 4
=============================================================================
