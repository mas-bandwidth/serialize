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
(* a byte count that fits the cursor starts healthy, and a stream over a   *)
(* count that does not starts refused as value_out_of_range (U7). The      *)
(* second is why Fail records whatever the position: such a stream is past *)
(* its end before any read. Which counts fit is SizeSpec, below. The latch *)
(* specification holds the cursor at an empty fitting count and does not   *)
(* change it.                                                              *)
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
(* The cursor's byte count is a separate specification in this module, on  *)
(* a signed machine of width 6, the smallest width at which a positive     *)
(* count still fits under the 8 bits of headroom the code leaves: poison   *)
(* steps one past the end, and rounding the cursor up to a byte adds 7.    *)
(* SizeSpec checks the count before multiplying. SizeSpecOld multiplies    *)
(* first and then checks only the sign, which is the old order. A request  *)
(* is compared by subtracting the cursor from the bit count; both ends of  *)
(* that subtraction stay inside the machine. Re-initialization is again a  *)
(* new behaviour, not an action.                                           *)
(*                                                                         *)
(* The UTF bounds at the end model the length checks in                   *)
(* serialize_string_is_valid_utf8 and serialize_utf.h on the same Width:  *)
(* an index or an output length, a total or a capacity, and a width or a  *)
(* needed count near the top of the machine. The new order checks both    *)
(* operands and then compares by subtraction; the old order adds first    *)
(* and wraps. UtfBounds.cfg must pass and UtfBoundsOld.cfg must report    *)
(* UtfBounds violated.                                                    *)
(*                                                                         *)
(* Checked with TLC 2.19, three wire reasons and MaxDepth 3 for the latch, *)
(* and the two size configurations beside this file. The results are in    *)
(* the commit that last changed this file.                                 *)
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

VARIABLES refused, reason, depth, last
VARIABLES szBytes, szBits, szRead, szRefused, szReason
VARIABLES uI, uLen, uOut, uCap, uNeed, uRefused

vars == <<refused, reason, depth, last, szBytes, szBits, szRead, szRefused, szReason, uI, uLen, uOut, uCap, uNeed, uRefused>>
szvars == <<szBytes, szBits, szRead, szRefused, szReason>>
uvars == <<uI, uLen, uOut, uCap, uNeed, uRefused>>

TypeOK ==
    /\ refused \in BOOLEAN
    /\ reason \in Reasons \cup {NONE}
    /\ depth \in 0..MaxDepth
    /\ last \in {"start", "ok", "fail"}

\* a fitting count, held still while the latch actions run
SizeIdle ==
    /\ szBytes = 0
    /\ szBits = 0
    /\ szRead = 0
    /\ szRefused = FALSE
    /\ szReason = NONE

\* a settled conversion, held still while the latch and size actions run
UtfIdle ==
    /\ uI = 0
    /\ uLen = 0
    /\ uOut = 0
    /\ uCap = 1
    /\ uNeed = 1
    /\ uRefused = FALSE

\* ReadStream( buffer, bytes ) and Initialize, abstracted to the two outcomes
\* SizeSpec distinguishes. The cursor itself stays at SizeIdle here.
Init ==
    /\ depth = 0
    /\ \/ /\ refused = FALSE
          /\ reason = NONE
          /\ last = "start"
       \/ /\ refused = TRUE
          /\ reason = OUT_OF_RANGE
          /\ last = "fail"
    /\ SizeIdle
    /\ UtfIdle

\* ReadStream::Fail
Fail(r) ==
    LET recorded == IF r = NONE /\ ~OldFail THEN REJECTED ELSE r
    IN  /\ reason' = IF reason = NONE THEN recorded ELSE reason
        /\ refused' = TRUE
        /\ last' = "fail"
        /\ UNCHANGED szvars

\* StreamRefusal<ReadStream>::Latch, which serialize_latch_failure calls
Latch ==
    IF OldLatch
    THEN /\ refused' = TRUE                    \* 2.0: poison, record nothing
         /\ UNCHANGED reason
         /\ last' = "fail"
         /\ UNCHANGED szvars
    ELSE Fail(REJECTED)                        \* 2.1: Fail( REFUSAL_REJECTED )

\* A primitive read whose bits the wire accepts. On a poisoned position the
\* past-end check every read performs refuses it, as past_end, through Fail.
ReadOk ==
    /\ IF refused
       THEN Fail(PAST_END)
       ELSE /\ last' = "ok"
            /\ UNCHANGED <<refused, reason>>
    /\ UNCHANGED depth
    /\ UNCHANGED szvars

\* A primitive read the wire refuses, for reason r.
WireRefusal(r) ==
    /\ Fail(r)
    /\ UNCHANGED depth
    /\ UNCHANGED szvars

\* serialize_reject: the caller refuses a well formed value.
Reject ==
    /\ Fail(REJECTED)
    /\ UNCHANGED depth
    /\ UNCHANGED szvars

\* serialize::serialize_fail from the caller, with any reason, none included
CallerFail(r) ==
    /\ Fail(r)
    /\ UNCHANGED depth
    /\ UNCHANGED szvars

\* serialize_object's entry: a failed stream refuses before the callback runs.
ObjectEnter ==
    /\ depth < MaxDepth
    /\ IF refused
       THEN /\ last' = "fail"
            /\ UNCHANGED <<refused, reason, depth>>
       ELSE /\ depth' = depth + 1
            /\ last' = "ok"
            /\ UNCHANGED <<refused, reason>>
    /\ UNCHANGED szvars

\* serialize_object's exit, with the callback's result. A false result, or a
\* true one on a stream that failed inside the callback, latches.
ObjectExit(result) ==
    /\ depth > 0
    /\ depth' = depth - 1
    /\ IF ~result \/ refused
       THEN Latch
       ELSE /\ last' = "ok"
            /\ UNCHANGED <<refused, reason>>
    /\ UNCHANGED szvars

Next ==
    /\ \/ ReadOk
       \/ \E r \in WireReasons : WireRefusal(r)
       \/ Reject
       \/ \E r \in Reasons \cup {NONE} : CallerFail(r)
       \/ ObjectEnter
       \/ \E result \in BOOLEAN : ObjectExit(result)
    /\ UNCHANGED uvars

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

-----------------------------------------------------------------------------
\* The byte count, on a signed machine of Width bits.
\* Headroom is what the cursor checks still add after a request is compared
\* by subtraction: poison stores one past the end, and the byte rounding adds 7.

Width == 6
Headroom == 8
MinInt == -2^(Width - 1)
MaxInt == 2^(Width - 1) - 1
Modulo == 2^Width

ToSigned(u) == IF u > MaxInt THEN u - Modulo ELSE u
Wrap(x) == ToSigned(x % Modulo)

MaxBits == MaxInt - Headroom
FitsBytes(b) == b \in 0 .. MaxInt /\ b * 8 <= MaxBits
MaxReadBytes == CHOOSE b \in 0 .. MaxInt : FitsBytes(b) /\ ~FitsBytes(b + 1)

ASSUME MaxReadBytes = 2
ASSUME MaxBits = 23
ASSUME Wrap(-1 * 8) = -8
ASSUME Wrap(4 * 8) = -32
ASSUME Wrap(MinInt * 8) = 0
ASSUME Wrap(24) = 24

Requests == 0 .. MaxInt

\* the latch variables, held still while the size actions run
LatchIdle ==
    /\ refused = FALSE
    /\ reason = NONE
    /\ depth = 0
    /\ last = "start"

\* the count is checked before the multiply. a count that does not fit is an
\* empty reader, already refused as value_out_of_range, cursor one past the end
SizeInit ==
    \E b \in MinInt .. MaxInt :
        IF b \in 0 .. MaxReadBytes
        THEN /\ szBytes = b
             /\ szBits = b * 8
             /\ szRead = 0
             /\ szRefused = FALSE
             /\ szReason = NONE
        ELSE /\ szBytes = b
             /\ szBits = 0
             /\ szRead = 1
             /\ szRefused = TRUE
             /\ szReason = OUT_OF_RANGE

\* the old order: multiply, wrapping at the machine width, then reject only a
\* negative count. the product is kept, including when it wrapped.
SizeInitOld ==
    \E b \in MinInt .. MaxInt :
        LET product == Wrap(b * 8)
        IN  /\ szBytes = b
            /\ szBits = product
            /\ IF b < 0
               THEN /\ szRefused = TRUE
                    /\ szReason = OUT_OF_RANGE
                    /\ szRead = Wrap(product + 1)
               ELSE /\ szRefused = FALSE
                    /\ szReason = NONE
                    /\ szRead = 0

\* a later request, compared by subtraction. both ends are in range whenever
\* CursorRoom holds. a request that does not fit poisons the cursor one past
\* the end and records past_end, unless a reason is already recorded.
SizeNext ==
    \E n \in Requests :
        LET remaining == szBits - szRead
        IN  /\ remaining \in MinInt .. MaxInt
            /\ IF szRefused \/ n > remaining
               THEN /\ szRefused' = TRUE
                    /\ szReason' = IF szReason = NONE THEN PAST_END ELSE szReason
                    /\ szRead' = szBits + 1
                    /\ UNCHANGED <<szBytes, szBits>>
               ELSE /\ szRead' = szRead + n
                    /\ UNCHANGED <<szBytes, szBits, szRefused, szReason>>

SizeInitFull == SizeInit /\ LatchIdle /\ UtfIdle
SizeInitOldFull == SizeInitOld /\ LatchIdle /\ UtfIdle
SizeNextFull == SizeNext /\ UNCHANGED <<refused, reason, depth, last>> /\ UNCHANGED uvars

SizeSpec == SizeInitFull /\ [][SizeNextFull]_vars
SizeSpecOld == SizeInitOldFull /\ [][SizeNextFull]_vars

\* an invalid count stays an empty reader with its first reason
BadCountIsEmpty ==
    (szBytes < 0 \/ szBytes > MaxReadBytes) =>
        (szBits = 0 /\ szRead = 1 /\ szRefused /\ szReason = OUT_OF_RANGE)

\* the bit count, the cursor, and the subtraction that compares a request
\* all stay inside the machine. a fitting count keeps the product it formed
CursorRoom ==
    /\ szBits \in 0 .. MaxBits
    /\ szRead \in 0 .. (szBits + 1)
    /\ szRead \in MinInt .. MaxInt
    /\ (szBits - szRead) \in MinInt .. MaxInt
    /\ szBytes \in 0 .. MaxReadBytes => szBits = szBytes * 8

\* once the size check has refused, a later request does not relabel the reason
ReasonStays == [][szRefused => szReason' = szReason]_vars

-----------------------------------------------------------------------------
\* The UTF length checks, on the same signed machine of Width bits above.
\* uI is an index into an input of uLen bytes, or an output length in a
\* buffer of uCap units; one step converts a sequence of uNeed units. The
\* values are bounded near the top of the machine, where the old sums wrap.

UtfLens == {0, 1, 3, MaxInt - 1, MaxInt}
UtfCaps == {2, 3, MaxInt - 1, MaxInt}
UtfPoss == {0, 1, MaxInt - 3, MaxInt - 2, MaxInt - 1, MaxInt}

\* the new order: both operands are checked before either subtraction, and
\* the width and the needed count are compared with what remains. A step
\* that does not fit refuses and moves neither index.
UtfInit ==
    \E len \in UtfLens, cap \in UtfCaps, i \in UtfPoss, out \in UtfPoss, need \in 1 .. 4 :
        /\ i <= len
        /\ out <= cap
        /\ uLen = len
        /\ uCap = cap
        /\ uNeed = need
        /\ IF need > len - i \/ need + 1 > cap - out
           THEN /\ uI = i
                /\ uOut = out
                /\ uRefused = TRUE
           ELSE /\ uI = i + need
                /\ uOut = out + need
                /\ uRefused = FALSE

\* the old order: the sums are formed first, wrapping at the machine width,
\* and compared afterwards. A wrapped sum reads as small or negative, so a
\* step that does not fit is accepted and its indices wrap with it.
UtfInitOld ==
    \E len \in UtfLens, cap \in UtfCaps, i \in UtfPoss, out \in UtfPoss, need \in 1 .. 4 :
        /\ i <= len
        /\ out <= cap
        /\ uLen = len
        /\ uCap = cap
        /\ uNeed = need
        /\ IF Wrap(i + need - 1) >= len \/ Wrap(out + need + 1) > cap
           THEN /\ uI = i
                /\ uOut = out
                /\ uRefused = TRUE
           ELSE /\ uI = Wrap(i + need)
                /\ uOut = Wrap(out + need)
                /\ uRefused = FALSE

UtfInitFull == UtfInit /\ LatchIdle /\ SizeIdle
UtfInitOldFull == UtfInitOld /\ LatchIdle /\ SizeIdle

UtfSpec == UtfInitFull /\ [][UNCHANGED vars]_vars
UtfSpecOld == UtfInitOldFull /\ [][UNCHANGED vars]_vars

\* an accepted step stays inside the input and the capacity, and both
\* subtractions the check performs stay inside the machine
UtfBounds ==
    /\ uNeed \in 1 .. 4
    /\ uLen \in 0 .. MaxInt
    /\ uCap \in 0 .. MaxInt
    /\ ~uRefused =>
           /\ uI \in 0 .. uLen
           /\ uOut \in 0 .. uCap
           /\ (uLen - uI) \in MinInt .. MaxInt
           /\ (uCap - uOut) \in MinInt .. MaxInt
=============================================================================
