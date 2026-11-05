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
(* The size. A reader is built over a signed byte count, and numBits is    *)
(* that count times 8 in the machine's signed integer. The model runs at   *)
(* a small machine width (MachineMax, the largest signed value) so the     *)
(* wrap is reachable. The count is validated before any arithmetic: a      *)
(* negative count, or one above MachineMax \div 16, gives an empty reader  *)
(* already refused as value_out_of_range. The headroom invariant says a    *)
(* healthy or poisoned position (numBits + 1) plus the largest request     *)
(* still fits the machine integer. OldSize (ReadStreamLatchSizeOld.cfg)    *)
(* is the earlier order: multiply first and refuse only a negative count,  *)
(* so a large count wraps to a negative bit count on a healthy stream.     *)
(* The model carries the arithmetic (the wrap), not the memory layout:     *)
(* the reader has no ring or compaction, only the product of the count.    *)
(*                                                                         *)
(* Re-initialization (ReadStream::Initialize) starts a new stream and a    *)
(* new behaviour of this spec; it is not an action here.                   *)
(*                                                                         *)
(* Checked with TLC 2.19 on an idle bench machine, three wire reasons and  *)
(* MaxDepth 3; the results are in the PR and in the commit that last       *)
(* changed this file.                                                      *)
(***************************************************************************)
EXTENDS Integers

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

VARIABLES refused, reason, depth, last, numBits

vars == <<refused, reason, depth, last, numBits>>

\* Defaults, overridden by a cfg: the new order, a machine whose largest signed
\* integer is 127, and bit requests of at most 32
OldSize     == FALSE
OldSizeOn   == TRUE
MachineMax  == 127
MaxRequest  == 32

Sizes == (0 - MachineMax - 1) .. MachineMax

\* the product as the machine computes it: two's complement wrap into the signed range
Wrap(x) == ((x + MachineMax + 1) % (2 * (MachineMax + 1))) - MachineMax - 1

\* the largest count the reader accepts: its bit count leaves headroom
MaxBytes == MachineMax \div 16

TypeOK ==
    /\ refused \in BOOLEAN
    /\ reason \in Reasons \cup {NONE}
    /\ depth \in 0..MaxDepth
    /\ last \in {"start", "ok", "fail"}
    /\ numBits \in Sizes

\* ReadStream( buffer, bytes ) and Initialize, for a byte count b: healthy over a
\* count in range, refused as value_out_of_range over any other, with an empty
\* reader. OldSize multiplies first and refuses only a negative count.
Init ==
    /\ depth = 0
    /\ \E b \in Sizes :
         IF OldSize
         THEN /\ numBits = Wrap(b * 8)
              /\ IF b < 0
                 THEN /\ refused = TRUE /\ reason = OUT_OF_RANGE /\ last = "fail"
                 ELSE /\ refused = FALSE /\ reason = NONE /\ last = "start"
         ELSE IF b < 0 \/ b > MaxBytes
              THEN /\ numBits = 0
                   /\ refused = TRUE /\ reason = OUT_OF_RANGE /\ last = "fail"
              ELSE /\ numBits = b * 8
                   /\ refused = FALSE /\ reason = NONE /\ last = "start"

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
  /\ UNCHANGED numBits
  /\ \/ ReadOk
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

\* the bit count is never negative, and the poisoned position (one past it) plus
\* the largest request fits the machine integer: no later comparison can wrap
NumBitsSound ==
    /\ numBits >= 0
    /\ numBits + 1 + MaxRequest <= MachineMax

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
