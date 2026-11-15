---------------------------- MODULE AccountsTrace ----------------------------
(***************************************************************************)
(* Trace validation: TLC checks that one trace of the real hub's accounts  *)
(* and friends (src/sync/hub.h, src/accounts/registry.h) is a behaviour of *)
(* Accounts.tla.                                                           *)
(*                                                                         *)
(* tests/accounts/accounts_walk.cpp drives the code with a seeded random   *)
(* walk and writes AccountsTraceData.tla beside this module. It defines    *)
(* the instance (TraceAccts, TraceProvider, TraceProviders, TraceFriends,  *)
(* TraceInsts, TraceMaxSes, TraceStartHarness, TraceBills, TraceLimits)  *)
(* and Trace: one record per step. Trace[1] is the start (op "init");     *)
(* each later record is the operation the code was given (op, with its    *)
(* friend f, account or instance a, session s, provider p, limit v and    *)
(* billing b), whether the code took it (ok), and after it the code's     *)
(* whole state: the accounts, providers, friends, links, defaults,        *)
(* instances, sessions and next id, the friends' limits and the accounts' *)
(* billings, the drivers the walk started (each limited or open as        *)
(* accounts::limitedFor decided at its start), and the sessions the step  *)
(* gave a notice (told).                                                  *)
(*                                                                         *)
(* Each step of this spec takes the Accounts action the record names.      *)
(* TraceMatches requires the spec's state to equal the code's, variable by *)
(* variable, with the notices and whether the step was taken. A step the   *)
(* spec cannot take is a deadlock. The model's properties are checked on   *)
(* the trace.                                                              *)
(***************************************************************************)
EXTENDS Integers, Sequences, TLC, AccountsTraceData

VARIABLES acct, prov, known, links, dflt, insts, ses, nextSes, lim, bill, run, told, last, l

L == INSTANCE Accounts WITH Accts <- TraceAccts, Provider <- TraceProvider, Providers <- TraceProviders,
                            Friends <- TraceFriends, Insts <- TraceInsts, MaxSes <- TraceMaxSes,
                            StartHarness <- TraceStartHarness, Bills <- TraceBills, Limits <- TraceLimits,
                            Runs <- TRUE

ASSUME Len(Trace) >= 1 /\ Trace[1].op = "init"

Matches(k) ==
    LET r == Trace[k] IN
    /\ acct = r.acct
    /\ prov = r.prov
    /\ known = r.known
    /\ links = r.links
    /\ dflt = r.dflt
    /\ insts = r.insts
    /\ ses = r.ses
    /\ nextSes = r.nextSes
    /\ lim = r.lim
    /\ bill = r.bill
    /\ run = r.run
    /\ told = r.told
    /\ last.ok = r.ok

TraceMatches == Matches(l)

Step(r) ==
    CASE r.op = "add"         -> L!AddAccount(r.a)
      [] r.op = "remove"      -> L!RemoveAccount(r.a)
      [] r.op = "in"          -> L!Report(r.a, "in")
      [] r.op = "out"         -> L!Report(r.a, "out")
      [] r.op = "friend"      -> L!AddFriend(r.f)
      [] r.op = "link"        -> L!LinkAccount(r.f, r.a)
      [] r.op = "unlink"      -> L!Unlink(r.f, r.a)
      [] r.op = "default"     -> L!SetDefault(r.f, r.a)
      [] r.op = "dropfriend"  -> L!ReloadDropFriend(r.f)
      [] r.op = "droplink"    -> L!ReloadDropLink(r.f, r.a)
      [] r.op = "dropaccount" -> L!ReloadDropAccount(r.a)
      [] r.op = "provider"    -> L!ReloadProvider(r.a, r.p)
      [] r.op = "addinst"     -> L!ReloadAddInstance(r.f, r.a)
      [] r.op = "dropinst"    -> L!ReloadDropInstance(r.f, r.a)
      [] r.op = "moveinst"    -> L!ReloadMoveInstance(r.f, r.a)
      [] r.op = "set"         -> L!SetSession(r.s, r.a)
      [] r.op = "rotate"      -> L!Rotate(r.s)
      [] r.op = "create"      -> L!Create(r.f, r.s)
      [] r.op = "limit"       -> L!ReloadLimit(r.f, r.v)
      [] r.op = "billing"     -> L!ReloadBilling(r.a, r.b)
      [] r.op = "start"       -> L!Start(r.s)
      [] r.op = "end"         -> L!End(r.s)
      [] OTHER                -> FALSE

modelVars == <<acct, prov, known, links, dflt, insts, ses, nextSes, lim, bill, run, told, last>>
traceVars == <<acct, prov, known, links, dflt, insts, ses, nextSes, lim, bill, run, told, last, l>>

TraceInit == l = 1 /\ L!Init

TraceNext ==
    /\ l < Len(Trace)
    /\ l' = l + 1
    /\ Step(Trace[l + 1])

Done == l = Len(Trace) /\ UNCHANGED traceVars

TraceSpec == TraceInit /\ [][TraceNext \/ Done]_traceVars

\* Accounts' properties, checked on the trace.
TypeOK == L!TypeOK
LinksSound == L!LinksSound
DefaultLinked == L!DefaultLinked
SessionLinked == L!SessionLinked
InstanceHeld == L!InstanceHeld
HarnessMatches == L!HarnessMatches
IdsDense == L!IdsDense
ApiAlwaysLimited == L!ApiAlwaysLimited
NeverWeakerThanFriend == L!NeverWeakerThanFriend
LimitSound == L!LimitSound
NewFriendLimited == L!NewFriendLimited
LimitChangeIsTold == L!LimitChangeIsTold
MovedIsTold == L!MovedIsTold
MovedInstanceIsTold == L!MovedInstanceIsTold
ServerPicksSignedIn == L!ServerPicksSignedIn
RotateMoves == L!RotateMoves
SetExact == L!SetExact
OnlyItsSession == L!OnlyItsSession
RefusedChangesNothing == L!RefusedChangesNothing

=============================================================================
