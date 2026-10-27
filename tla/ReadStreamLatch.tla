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
(* Every refusal goes through Fail, which is ReadStream::Fail exactly: it  *)
(* records the reason only when none is recorded and the position is not  *)
(* yet poisoned, and it always poisons. The actions are the operations and *)
(* the outside events: a primitive read the wire accepts, a primitive read *)
(* the wire refuses for reason r, the caller's serialize_reject, entering  *)
(* a nested object (serialize_object's entry check), and leaving one with  *)
(* the callback's result true or false (serialize_object's exit check and  *)
(* serialize_latch_failure).                                               *)
(*                                                                         *)
(* OldLatch selects the 2.0 serialize_latch_failure, which poisoned the    *)
(* position and recorded nothing. It is the reversed witness: with it,     *)
(* TLC must find RefusedIffReason violated by a callback returning false   *)
(* on a healthy stream (ReadStreamLatchOld.cfg).                           *)
(*                                                                         *)
(* Re-initialization (ReadStream::Initialize) starts a new stream and a    *)
(* new behaviour of this spec; it is not an action here.                   *)
(*                                                                         *)
(* Checked 2026-10-27 with TLC 2.19 on a bench machine, three wire reasons *)
(* and MaxDepth 3:                                                         *)
(*   ReadStreamLatch.cfg     152 states generated, 21 distinct, depth 6,   *)
(*                           no error: every invariant and property holds. *)
(*   ReadStreamLatchOld.cfg  RefusedIffReason violated in 3 states:        *)
(*                           Init, ObjectEnter, ObjectExit(FALSE) leaves   *)
(*                           refused = TRUE with reason = "none", which is *)
(*                           what the 2.0 header did (its unit test        *)
(*                           asserted REFUSAL_NONE after a callback-only   *)
(*                           refusal).                                     *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS WireReasons,  \* a few of the wire's reasons, e.g. {"past_end", "nonzero_padding"}
          MaxDepth,     \* the deepest nesting of objects explored
          OldLatch      \* TRUE for the 2.0 latch that recorded no reason

NONE     == "none"
REJECTED == "rejected"
PAST_END == "past_end"

ASSUME PAST_END \in WireReasons
ASSUME NONE \notin WireReasons /\ REJECTED \notin WireReasons
ASSUME MaxDepth \in Nat
ASSUME OldLatch \in BOOLEAN

Reasons == WireReasons \cup {REJECTED}

VARIABLES refused, reason, depth, last

vars == <<refused, reason, depth, last>>

TypeOK ==
    /\ refused \in BOOLEAN
    /\ reason \in Reasons \cup {NONE}
    /\ depth \in 0..MaxDepth
    /\ last \in {"start", "ok", "fail"}

Init ==
    /\ refused = FALSE
    /\ reason = NONE
    /\ depth = 0
    /\ last = "start"

\* ReadStream::Fail
Fail(r) ==
    /\ reason' = IF reason = NONE /\ ~refused THEN r ELSE reason
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

\* once refused, the reason never changes: the first reason wins
ReasonNeverChanges == [][(refused => reason' = reason)]_vars

\* once refused, the stream stays refused and every later operation fails
RefusalIsTerminal == [][(refused => (refused' /\ last' = "fail"))]_vars

\* a wire refusal is never relabelled: once a wire reason is recorded, a later
\* reject or failing callback leaves it in place (the first reason wins)
WireReasonKept == [][(reason \in WireReasons => reason' = reason)]_vars

\* a callback returning false on a healthy stream refuses, as rejected
CallbackFalseIsRejected ==
    [][((~refused /\ depth > 0 /\ depth' = depth - 1 /\ last' = "fail") => reason' = REJECTED)]_vars
=============================================================================
