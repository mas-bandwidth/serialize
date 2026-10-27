--------------------------- MODULE ReadStreamLatch ---------------------------
(***************************************************************************)
(* The read stream's failure latch, as serialize.h implements it in 2.1.   *)
(*                                                                         *)
(* STANDARD.md, "Reader Obligations" (failure is terminal) and "Refusal    *)
(* reasons" (the first reason wins; a refusal the caller decides is        *)
(* `rejected`). The state is what ReadStream owns:                         *)
(*                                                                         *)
(*   refused  the reader's position is poisoned                            *)
(*            (StreamRefusal<ReadStream>::IsFailed)                        *)
(*   reason   ReadStream::m_refusal, NONE or one reason from the list      *)
(*                                                                         *)
(* plus two pieces of bookkeeping the invariants need: how many nested     *)
(* objects are open, and the result of the last operation.                 *)
(*                                                                         *)
(* Every refusal goes through Fail, which is ReadStream::Fail exactly. It  *)
(* maps a refusal with no reason (NONE, or a value outside the closed      *)
(* list, which the model folds into NONE) to REJECTED (IMPLEMENTING.md     *)
(* U14), records the reason only when none is recorded, whatever the       *)
(* position, and always poisons.                                           *)
(*                                                                         *)
(* There are two initial states, because ReadStream has two: a stream over *)
(* a non-negative byte count starts healthy, and a stream over a negative  *)
(* byte count starts refused as value_out_of_range (U7). The second is why *)
(* Fail records whatever the position: such a stream is past its end       *)
(* before any read.                                                        *)
(*                                                                         *)
(* The actions are the operations and the outside events: a primitive read *)
(* the wire accepts, a primitive read the wire refuses for reason r, the   *)
(* caller's serialize_reject, the caller's serialize_fail with any reason  *)
(* including none, entering a nested object (serialize_object's entry      *)
(* check), and leaving one with the callback's result true or false        *)
(* (serialize_object's exit check and serialize_latch_failure).            *)
(*                                                                         *)
(* Two reversed witnesses, each an earlier behaviour TLC must find:       *)
(*   OldLatch: serialize_latch_failure poisoned and recorded nothing, so a *)
(*     callback returning false on a healthy stream left reason none       *)
(*     (ReadStreamLatchOld.cfg).                                           *)
(*   OldFail: an interim 2.1 Fail (c2e089e) recorded none when handed    *)
(*     none (asserted in checked builds, silent in release), so the stream *)
(*     reported none and the next read relabelled it past_end              *)
(*     (ReadStreamLatchOldFail.cfg). 2.0's Fail stayed at none for good.   *)
(*                                                                         *)
(* Re-initialization (ReadStream::Initialize) starts a new stream and a    *)
(* new behaviour of this spec; it is not an action here.                   *)
(*                                                                         *)
(* Checked with TLC 2.19 on an idle bench machine, three wire reasons and  *)
(* MaxDepth 3; the results are in the PR and in the commit that last       *)
(* changed this file.                                                      *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS WireReasons,  \* a few of the wire's reasons, including past_end and value_out_of_range
          MaxDepth,     \* the deepest nesting of objects explored
          OldLatch,     \* TRUE for the 2.0 latch that recorded no reason
          OldFail       \* TRUE for the 2.0 Fail that recorded none when handed none

NONE         == "none"
REJECTED     == "rejected"
PAST_END     == "past_end"
OUT_OF_RANGE == "value_out_of_range"

ASSUME PAST_END \in WireReasons /\ OUT_OF_RANGE \in WireReasons
ASSUME NONE \notin WireReasons /\ REJECTED \notin WireReasons
ASSUME MaxDepth \in Nat
ASSUME OldLatch \in BOOLEAN /\ OldFail \in BOOLEAN

Reasons == WireReasons \cup {REJECTED}

VARIABLES refused, reason, depth, last

vars == <<refused, reason, depth, last>>

TypeOK ==
    /\ refused \in BOOLEAN
    /\ reason \in Reasons \cup {NONE}
    /\ depth \in 0..MaxDepth
    /\ last \in {"start", "ok", "fail"}

\* ReadStream( buffer, bytes ) and Initialize: healthy over bytes >= 0, refused
\* as value_out_of_range over bytes < 0
Init ==
    /\ depth = 0
    /\ \/ /\ refused = FALSE
          /\ reason = NONE
          /\ last = "start"
       \/ /\ refused = TRUE
          /\ reason = OUT_OF_RANGE
          /\ last = "fail"

\* ReadStream::Fail
Fail(r) ==
    LET recorded == IF r = NONE /\ ~OldFail THEN REJECTED ELSE r
    IN  /\ reason' = IF reason = NONE THEN recorded ELSE reason
        /\ refused' = TRUE
        /\ last' = "fail"

\* StreamRefusal<ReadStream>::Latch, which serialize_latch_failure calls
Latch ==
    IF OldLatch
    THEN /\ refused' = TRUE                    \* 2.0: poison, record nothing
         /\ UNCHANGED reason
         /\ last' = "fail"
    ELSE Fail(REJECTED)                        \* 2.1: Fail( REFUSAL_REJECTED )

\* A primitive read whose bits the wire accepts. On a poisoned position the
\* past-end check every read performs refuses it, as past_end, through Fail.
ReadOk ==
    /\ IF refused
       THEN Fail(PAST_END)
       ELSE /\ last' = "ok"
            /\ UNCHANGED <<refused, reason>>
    /\ UNCHANGED depth

\* A primitive read the wire refuses, for reason r.
WireRefusal(r) ==
    /\ Fail(r)
    /\ UNCHANGED depth

\* serialize_reject: the caller refuses a well formed value.
Reject ==
    /\ Fail(REJECTED)
    /\ UNCHANGED depth

\* serialize::serialize_fail from the caller, with any reason, none included
CallerFail(r) ==
    /\ Fail(r)
    /\ UNCHANGED depth

\* serialize_object's entry: a failed stream refuses before the callback runs.
ObjectEnter ==
    /\ depth < MaxDepth
    /\ IF refused
       THEN /\ last' = "fail"
            /\ UNCHANGED <<refused, reason, depth>>
       ELSE /\ depth' = depth + 1
            /\ last' = "ok"
            /\ UNCHANGED <<refused, reason>>

\* serialize_object's exit, with the callback's result. A false result, or a
\* true one on a stream that failed inside the callback, latches.
ObjectExit(result) ==
    /\ depth > 0
    /\ depth' = depth - 1
    /\ IF ~result \/ refused
       THEN Latch
       ELSE /\ last' = "ok"
            /\ UNCHANGED <<refused, reason>>

Next ==
    \/ ReadOk
    \/ \E r \in WireReasons : WireRefusal(r)
    \/ Reject
    \/ \E r \in Reasons \cup {NONE} : CallerFail(r)
    \/ ObjectEnter
    \/ \E result \in BOOLEAN : ObjectExit(result)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
\* Invariants

\* refused if and only if a reason is recorded: no refused stream reports none,
\* and no healthy stream reports a reason
RefusedIffReason == refused <=> reason # NONE

\* no read succeeds after a refusal, objects included
NoSuccessAfterRefusal == refused => last = "fail"

\* Action properties, checked as PROPERTY

\* once refused, the reason never changes: the first reason wins, and a later
\* read never relabels it past_end
ReasonNeverChanges == [][(refused => reason' = reason)]_vars

\* once refused, the stream stays refused and every later operation fails
RefusalIsTerminal == [][(refused => (refused' /\ last' = "fail"))]_vars

\* a wire refusal is never relabelled: once a wire reason is recorded, a later
\* reject, failing callback or caller fail leaves it in place
WireReasonKept == [][(reason \in WireReasons => reason' = reason)]_vars

\* a callback returning false on a healthy stream refuses, as rejected
CallbackFalseIsRejected ==
    [][((~refused /\ depth > 0 /\ depth' = depth - 1 /\ last' = "fail") => reason' = REJECTED)]_vars
=============================================================================
