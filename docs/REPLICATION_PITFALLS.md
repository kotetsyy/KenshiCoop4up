# Replication pitfalls and gate design

> **Purpose.** Hard-won failure modes in KenshiCoop's item/world replication, and
> the testing habits that did or did not catch them. Each entry states a rule, the
> concrete bug that produced it, and the signature to grep for. This is not a
> changelog — per-protocol narrative lives in `resources/PROTOCOL_HISTORY.md`
> (which is untracked, hence this file).
>
> Most entries generalise past items: they are really about drawing conclusions
> from engine reads, and about writing gates that can fail.

---

## 1. Budget in milliseconds, not ticks

`mainLoop_hook` runs every engine tick and Kenshi's loop runs at roughly
**100–125 Hz**. Any budget expressed as "N consecutive reads" is therefore a
tolerance of `N × ~8–10 ms`, which is almost never what the author meant.

`WD_DEAD_READS_MAX = 3` was intended as "let the read agree with itself before
believing it". In a real session it retired a tracked ground item **29 ms after
the drop that created it**, and a second one 32 seconds before the peer actually
picked that item up. The author then had no handle to answer the peer's pickup
with, so its ground copy stayed on the floor next to the item the peer now held —
the duplicate players reported.

**Rule.** Pair every read-count budget with a real-time hold, and log the elapsed
duration in the line that acts on it, so the budget is auditable from a session
log instead of inferred. See `WD_DEAD_HOLD_MS` and the `ground-prune` line, which
now reports `(N consecutive reads over Xms, everLive=1)`.

## 2. Never conclude from a single engine read

The engine streams objects out and back. A cached `RootObject*` goes unreadable
for a frame, `isInInventory` flickers, a spatial query misses. Any of these is a
transient, and code that treats one of them as a verdict makes a permanent
mistake from a momentary one.

Three places did exactly this, all in the pickup path:

- `why=gone` erased a ground track on one unreadable read — the very read that
  `reconcileGroundGear` deliberately refuses to trust on its own.
- `why=untracked` gave up after a single attempt at the site-anchored spatial
  scan. That scan uses `getObjectsWithinSphere`, which this codebase repeatedly
  documents as unreliable **in towns** — so the one-shot fallback was weakest
  exactly where players hit it.
- The drop mirror fabricated an item the moment a top-level search missed (§4).

**Rule.** When an operation cannot be satisfied now, park it and retry against a
deadline rather than answering once. Unsatisfied identified pickups are now
`PendingPickup` entries retried each tick (named track first, site scan second)
until `WD_REHOME_MAX_MS`, then reported as `PICKUP-GAVEUP` rather than vanishing
quietly. Identity-less intents are still refused outright — without an instance
identity, "re-home the oldest same-sid copy" teleports an unrelated object.

## 3. Let the asymmetry of the cost pick the default

Forgetting a track that is still on the ground costs a **permanent duplicate**.
Keeping a track whose object is genuinely gone costs **a stale pointer until the
next read**. Those are not comparable, so the tie should not be broken evenly.

The same asymmetry runs the other way for destruction: the author may only
destroy its ground copy after it has *seen* the item in the target container,
because a wrong guess loses the only instance. Keeping a duplicate beats
destroying the last copy.

**Rule.** Write down which error is recoverable before choosing the threshold.

## 4. Fabrication must prove absence, not failure-to-find

`APPLY-HEALED` mints an item from the drop intent's provenance when the mirror
cannot find its own copy. That is safe only if the item is genuinely absent —
and "my search didn't find it" is a much weaker claim, because **search scope**
is part of the search.

`dropItemFromInventory` reads top level only, on purpose (see the `includeNested`
note in `PROTOCOL_HISTORY.md`: a bagged item belongs to a different `Inventory`
and must not be counted as the character's loose kit). But hoovering
a pile of loot into one character overflows the grid and Kenshi stows the tail in
the worn backpack. The mirror then declared the item missing and fabricated one,
leaving the real item in the bag **and** a minted duplicate on the ground. Four
of these appeared in a single player session, three consecutively.

**Rule.** Before fabricating, exhaust every place the item could legitimately be.
`relocateWeaponToGround` now falls back to `dropItemFromNestedContainer` and logs
`RELOCATE-NESTED` when that reach is what saved the object.

## 5. Engine constraints that shape what a test can even do

Discovered the hard way while trying to build fixtures; each one silently returns
zero rather than failing loudly.

| Constraint | Symptom |
|---|---|
| A character has two weapon slots, and **both its grid and its worn bag refuse a third weapon**. | `[mk] tryAddItem-fail sid='...' type=2` — a scenario trying to mint a burst of weapons adds nothing. Prefer an armour template. |
| A character's loose storage **is** its worn backpack, so a second container cannot be added. | `[mk] tryAddItem-fail ... type=46`. The same-template multi-bag case is unreachable on a character. |
| `getObjectsWithinSphere` is unreliable in towns. | Spatial discovery and spatial recovery both silently miss. Query-free paths (the `dropItem` detour, handle-based liveness) exist for this reason; do not build a sole recovery path on a spatial scan. |
| A town-dropped item often reports its transform as `(0,0,0)` the frame it grounds. | Mirrored copies land at world origin unless the sentinel is filtered. |

## 6. Gate on a conservation ledger, not on presence

"Does the peer have it?" passes on a duplicate, because the peer does have it —
and so does the ground. A count taken only at the end also passes on a
destroy-and-recreate loop.

The invariant that catches both directions at once is a **per-template ledger
evaluated on each side**: every instance dumped must exist exactly once, either
on the ground or in the picker's bag.

```
ground + bag == dumped        # per template, per client
```

A template summing **above** what was dumped is the duplicate; **below** it is the
item that never arrived. `inv_dump_all` prints this as `dist='1+0/1,1+0/1'` so a
failure is readable without opening the logs.

Two traps in measuring it:

- Count nested contents (`includeNested`), or a successful transfer **into a
  backpack** reads as a loss.
- Measure a **delta** against each side's own baseline. Probe templates are
  ordinary kit the receiving character may already carry, and an absolute count
  passes on the save's own contents with nothing having crossed the wire. An
  early version of the nested-bag gate did exactly that (host 7, join 5, PASS).

## 7. "Tolerated but reported" is how a bug survives green runs

`APPLY-HEALED` was deliberately logged-and-allowed, on the reasoning that a heal
means the publish hold was outrun and the backstop covered it. It covered nothing
— it was minting duplicates — and it did so through several passing runs because
no gate would fail on it.

**Rule.** If a signature means the product did something it should not have, fail
on it. If it is genuinely acceptable, say why in the gate, not in a comment. The
oracles now fail on `APPLY-HEALED`, `PICKUP-GAVEUP`, any surviving one-shot
`why=untracked`, and any `ground-prune` that retired a once-live track inside
`WD_DEAD_HOLD_MS`.

## 8. A fault-injection lever must model the *actual* fault

This is the subtlest lesson here, and it cost three A/B attempts.

The goal was a gate that fails before a fix and passes after. Two levers failed
to discriminate:

1. **The plain scenario** passed on both builds — the fault is timing-dependent
   and does not reproduce on demand.
2. **`KENSHICOOP_WD_FORGET_TRACK`** (discard the author's track permanently) also
   passed pre-fix, because a *permanently* lost track is covered by the
   site-anchored recovery, which `inv_regear_forget` already gates. The injected
   fault was **more severe** than the real one, so it exercised a different
   recovery path and said nothing about the bug under test.

What discriminated was modelling the real transient:
`KENSHICOOP_WD_TRANSIENT_DEAD=N` makes the first N pickup-time resolutions report
the object gone and later reads succeed — the engine streaming an object out and
back. Pre-fix, the host ledger reads `2+0/1` (two ground copies of a template
dumped once); post-fix it reads `1+0/1`.

**Rule.** If a lever makes the failure permanent when reality makes it momentary,
a passing A/B proves only that some *other* recovery path works. Match the
severity, not just the shape.

## 9. Log unconditionally for anything a player can see

W1 ground-item diagnostics sit behind `KENSHICOOP_INV_DUMP`, so a real session log
of "I dropped it here and it never appeared there" contained nothing attributable
about the W1 path at all. The W2 path, whose key lines are unconditional, was
diagnosable from the same log in minutes.

**Rule.** A state the player can observe deserves an unconditional line. Keep the
verbose per-item dumps behind the flag; put the verdicts (`DROP-CAP-SKIP`,
`SEND-DEFER`, `PICKUP-GAVEUP`, `APPLY-LOST`) in front of it.

## 10. Gate the workflow, not the unit

Every gear gate was a one-item round trip: drop one thing, pick it up, assert. The
player's actual workflow was to **dump a character's entire inventory and hoover
all of it up with one other character**. That difference is where the bugs lived —
a burst mints many tracks at once, and one receiving grid fills up and overflows
into the backpack. Neither condition existed in any gate, which is why the suite
stayed green across three reported-bug sessions.

**Rule.** Ask how the feature is actually driven, and make at least one gate drive
it that way. `inv_dump_all` exists for this; `inv_dump_all_transient` is its
fault-injected twin (§8).

## 11. Put the camera where the player's camera would be

§10 is about the *action* a gate drives. This is about the *viewpoint* it drives
it from, which turns out to be load-bearing in its own right.

`park()` teleports bodies and does not touch the camera, and nothing else in the
harness moved it either — `cameraCenter()` could only read. So every automated
run watched wherever the save happened to leave the camera. In `split_far`, which
separates the two squads by ~5,200 u, that meant **both** clients stared at the
host's squad for the whole window while the join's characters stood 5,200 u
off-screen. No player has ever played that way, and the anchor count said so:
`anchors=3`, being the two tab leaders plus one camera hovering 167 u above the
host's leader (just outside the 100 u dedupe). There was never an anchor at the
far end.

Adding `cameraFocusOn` and pointing each side at its own tab leader changed the
run substantially. NPC population at the join's own squad, previously 5, ranged
**5–22** across five runs, and two of those runs failed existence-parity with
ghost runs of 7 consecutive samples — the reported "join sees NPCs the host
doesn't" symptom, which the parked-camera configuration never produced:

| popMover | 5 | 5 | 10 | 12 | 22 |
|---|---|---|---|---|---|
| existence-parity | PASS | PASS | PASS | FAIL | FAIL |

The failure tracks **population**, not the camera directly; the camera matters
because it is one of the things that drives population up. Smoothness went from
PASS to FAIL in 5/5 follow-camera runs (`zeroFrac ≈ 0.95`).

Two cautions on reading this:

- Only the **join's** camera was ever varied. The host's sat on the host's squad
  in every run including the parked one, so nothing here isolates the effect of
  the host looking away from its own people.
- One parked-camera sample is not a baseline. "The camera raises population" is
  consistent with the data but not established by it — two follow-camera runs
  also came in at 5.

**Rule.** State the viewpoint a scenario runs from as deliberately as it states
the positions, and re-point the camera after any teleport. A gate that never
renders the region it is making claims about is measuring enumeration only,
while the symptom it is chasing is something a player *sees*.

## 12. A symmetric swap is still a disjoint view

The first phased version of `split_far` ran `own → cross → back`: each side on
its own squad, then **both** cameras swapped to the peer's squad, then back.
That looks like it covers the space, and it does not. Swapping both at once
mirrors the arrangement without ever overlapping it — in all three phases the
two clients are drawing *different* characters.

That matters because a disagreement measured under disjoint views is
unattributable. If the join counts bodies at a spot and the host does not, the
host might have failed to replicate them, or the host might simply not be
looking. Nothing in the run separates those.

For two clients and two squads there are four arrangements, and the swap covers
neither of the interesting ones:

| | host → stay | host → mover |
|---|---|---|
| **join → mover** | `own` (disjoint) | `cross` (disjoint) |
| **join → stay** | **`both_stay`** (overlap) | **`both_mover`** (overlap) |

The scenario now runs `own → both_stay → both_mover → back`. `cross` is dropped:
it is disjoint like `own`, and `both_mover` already answers the question it was
there for (does the host's count rise when the host looks at the mover?) while
also giving overlap. Verified from the `[cam]` telemetry rather than assumed —
under `both_mover` the two cameras report identical centres
(`-50476.1,868.4,-2699.8` on both sides).

**Rule.** When a test compares two observers, check that its phases put them on
the same subject at least once. Otherwise every difference it reports has a
second explanation, and the reassuring result — the counts matching — is the
one most likely to be an artefact of neither side looking.

**Caveat carried forward.** The per-phase table compares *counts*
(`countNpcsNear`), not identities: two clients can hold five bodies each and
disagree about which five. `existence_parity` is the identity check, and it
stays advisory here precisely because this scenario exists to expose ghosts.

## 13. Both harness clients share one save folder, and the host writes to it

The two worlds disagree at PLATOON granularity, not per body. Diff the
`[platoon] first-sight` container ids from the two logs and the sets differ by
7–13 squads; at a settlement that reads as duplicated *roles* — two `Barman`
bodies where there is one bar, six `Ninja Guard` against four. The extra copies
go `drv → ghost → hid` and then run their AI invisibly, and the seconds before
suppression are what players report as "the join sees NPCs the host doesn't".
That much is solid, and it is worth knowing that census truncation, staleness,
zone loading and the mint path were all clean in every run that showed it.

What the harness **cannot** currently tell you is why. Both installs auto-load
from the same `%LOCALAPPDATA%\kenshi\save` folder, and the host's connect-push
bakes its live world *into that same folder* while the join is reading it —
`sync_save.cmd` says in its own header "close both Kenshi instances first so
save files aren't mid-write", which is exactly what the runner then does not
do. So what the join loads is decided by a race against the host's write.

This is easy to mistake for a causal result. Varying `-JoinDelaySec` produces a
clean monotone table — host solo before bake 15s / 68s / 128s giving 7–12 / 2 /
0 join-only platoons — which reads as "bake earlier, diverge more". It is not.
Deferring the bake by 120s *in the plugin*, so the host settles exactly as long
but the join launches at the usual moment, gave **13** join-only platoons, the
worst result measured. The stagger was moving the join's load relative to the
host's write, not changing what a settled world looks like.

**Rule.** Before drawing any conclusion about what the join loaded, establish
*which bytes* it loaded. On one machine with a shared save folder and a host
that writes mid-session, that is a race, and no amount of timing variation
turns it into evidence. Give the two installs separate save folders — or drive
the join's load entirely from `LOAD_GO` after the write has quiesced — before
re-running any experiment of this shape.

## 14. A predicate two clients must agree on has to be published, not derived

The attention gate stops both clients reconciling a region neither is watching:
if no attention centre is within `KENSHICOOP_ATTENTION_RADIUS`, the host omits
those bodies from its census and the join stops counting suppression frames
against them. That only works if the two sides reach the *same* verdict about
the same body. Wherever an input to the predicate is private, they cannot.

Three of the four centres were already common — both clients load the same save,
so both hold both squad tabs, and a tab leader's position is the same number on
each machine. The camera was the exception. `PKT_CAM_HINT` shipped join → host
only, because its original job was widening the host's interest spheres, and for
that one direction is enough. As a *gate* input it is not: with the host's camera
private, the join has to assume the host is always watching, which is the
assumption that keeps the ghost churn alive. The hint is now bidirectional.

The safety property the design rests on is that a squad is never dormant just
because no camera is on it — every tab leader is an interest centre, so bodies
beside a leader are observed whether or not anybody is looking. That is true by
construction in `interestCenters` (leaders fill the first slots; the four-slot
cap can only drop *camera* anchors), and it is measured rather than assumed:
`dormPc` on the `[audit] exist` line counts dormant bodies within the radius of
any player character, `existence_parity` FAILs on a non-zero, and `pcs` reports
the squad size it was measured against so a vacuous zero is visible. Note that
every harness save carries one character per tab, so the case the metric exists
to catch — a squad *member* that walked away from its leader and anchors nothing
— cannot be staged here yet.

**Rule.** Before gating behaviour on a predicate, list its inputs and ask which
of them the peer can see. An input only one side holds does not make the gate
approximate, it makes the two sides run different gates. Publish it, or drop it
from the predicate.

## 15. The engine owns every object's lifetime; a stored pointer expires when its block unloads

A `RootObject*` we minted is not ours. Kenshi destroys ground items when the zone
block they stand in deactivates, and travelling far is exactly what deactivates
blocks — so a map of proxy pointers is a map of pointers whose validity is a
function of where the players have walked since. `worldProxies_` held raw
pointers, and after a long trek every use of one was a use-after-free: the cull
called `GameWorld::destroy` on an object the engine had already destroyed (which
it notices — `Item <name> alredy has destroy reason ...` in `kenshi_info.log`),
the snapshot path called the VIRTUAL `setPositionRotation` through a dangling
vtable, and the claim path read `isInInventory` out of freed memory.

The reads are the subtle half. Freed heap usually still maps, so the SEH guards
around these calls do not fire; they return plausible garbage. A stale
`isInInventory` reading true makes the join tell the host "I picked your item up",
and the host destroys a real item nobody touched. A crash is the loud version of
this bug and duplicate/vanished items are the quiet version.

The fix is not a stronger guard, it is an identity: every proxy records the
engine `hand` it was minted with, verified at mint by resolving it back to the
same pointer, and `liveWorldProxy()` re-resolves before anything touches the
object. A hand is serial-checked, so a destroyed object's hand stops resolving
and a recycled table slot resolves to a *different* pointer — both read as "gone",
and gone means drop the mapping and call nothing. `groundItemLiveness(hand)`
already worked this way for our own tracked items; only the proxies, whose
`Engine.h` comment openly said "we hold only as a pointer", did not.

**Rule.** Never store a bare engine pointer across ticks. Store the hand, resolve
it at the point of use, and treat an unresolvable hand as an already-destroyed
object rather than an error. If a pointer must be cached, the question to answer
is not "can this be null" but "what does the engine do to this object when the
players walk away from it".

## 16. One value with two writers cannot be replicated as a snapshot

Every channel in this codebase publishes state and lets the newest value win,
which is correct when each value has exactly one author. The player's money has
two: Kenshi keeps ONE wallet per save (`Faction::factionOwnerships`), and both
players spend from it. Exchange that as a total and concurrent spends erase each
other — from 1000, one player buying for 200 and the other for 300 send 800 and
700, whichever lands last is the pool, and one purchase was free. The bug is
worst in the case players will actually create, both shopping in the same town.

So the join sends the CHANGE (`PKT_MONEY_DELTA`) and the host — whose wallet *is*
the pool — sends the total. Three things that fall out of that and are easy to
miss:

- **The delta channel must be reliable AND ordered.** A dropped or reordered
  total is self-healing (the next one is still correct); a dropped delta mints or
  burns cats permanently.
- **The publisher needs an ack, or the join's own purchase visibly bounces.**
  The host's total lags the join's local spend by a round trip, so adopting it
  blindly reverts the purchase and then re-applies it. `MoneyPacket::ackSeq`
  names the last delta folded in, and the join re-applies whatever is still
  pending on top.
- **Detection can be a sample, not a hook.** One wallet read per tick against a
  baseline catches every path that moves money — trade UI, sale, loot, bounty,
  hire, bar tab — with no per-path detours. The baseline must be updated on our
  own writes too, or applying the host's total reads back as a fresh local
  change and the pool oscillates.

**Rule.** Before writing a channel, ask how many authors the value has. If the
answer is more than one, replicate the operation rather than the state, and gate
it on CONSERVATION (`money_sync` asserts both clients end at
`base - joinSpend - hostSpend`) — a convergence gate cannot tell a lost update
from agreement, because both clients agreeing on the wrong number passes.

**Corollary on the negative control.** `money_persist` first waited for the
host's pool to move before saving. That is the right assertion in the wrong
place: with `KENSHICOOP_MONEY_SYNC=0` the fold never arrives, so the run stalled
before the save and the A/B proved only that a scenario can fail to set itself
up. Latching whatever the pool reads (recording `moved=0|1`) and saving anyway
carries the unfixed build to the post-load comparison, where the erase is
visible and named. A negative control has to REACH the assertion, so no step of
the script may be gated on a precondition that only the fix satisfies.

## 17. Split authority has to be total, or bodies fall between the halves

Cell authority (protocol 49) hands each side the NPC census for the 4608 u zone
cells its own squad stands in, instead of the host authoring everywhere. Asking
which of the two models is better turns out to be the wrong question: measured
per arm on the same build, host authority took `dual_drive` 4/4 against 1/4 and
`world_parity` 4/4 against 1/4 with no smoothness cost while the squads stood
together, and the same A/B run apart had the join hold a steady 11 NPCs at its
own town under cell authority against 5, 5 and 23 without it. Each model wins
decisively in one regime. So the rule switches on SEPARATION rather than
electing a global winner: while both squads claim the same cell, every cell
resolves to the host (`KENSHICOOP_CELL_COLLAPSE`, default on).

Two ways that went wrong, both instructive:

- **A proximity predicate is not a co-location predicate.** The first version
  collapsed on Chebyshev distance 1, reasoning that adjacent cells are the same
  neighbourhood. Cells are 4608 u; two towns a long walk apart can sit in
  adjacent cells, and `split_far2` — the scenario whose entire premise is that
  the pair is separated — collapsed onto the host and lost the population it
  exists to protect. Same cell or nothing.
- **Collapsing the CLAIMED cells is not collapsing the map.** A pair walking
  together leaves a trail of cells the join claimed a moment ago, and
  `authoritySrc` kept handing those to the join as last occupant
  (`AUTHSRC_VACATE`). But a collapsed join publishes no census at all, so bodies
  standing in that trail were corroborated by nobody, and the host froze them as
  census-absent — the visible symptom being a town going still behind the
  players. While collapsed, `authoritySrc` now returns the host for EVERY cell,
  claimed, vacated or open.

**Rule.** Any scheme that divides authorship must define an author for every
region, including the ones nobody currently occupies. A cell whose nominal
author has stopped publishing is worse than a cell with no author at all,
because the absence reads as a deliberate "this body is gone".

## 18. A marker written on one edge is a record of history, not of state

`KENSHICOOP_DEBUG_MARKERS` pins a colored label to each judged body — green DRV
for host-driven, red HID for suppressed, yellow LOC for a local-sim copy. The
DRV label is written every tick a body is driven, and nothing ever removes it
when the drive stops: the body is still standing there, so the pruner (which
destroys labels only for bodies that have vanished) keeps vouching for it. The
label therefore accumulates into "everything this client has driven at any point
this session", and since both clients accumulate their own, the same NPC ends up
green on BOTH screens. That reads on screen as two clients driving one body —
which is the exact fault the tag exists to let you rule out. It cost a long
detour chasing a dual-drive that the logs plainly denied: the host had refused
zero bodies, frozen one, and was receiving nothing but the join's own PC.

The fix is to re-derive the label from the live set rather than trust the edge:
the prune destroys any green label whose body is absent from `drivenChars_`,
which the drive rebuilds every tick, so a stopped drive loses its tag within the
2 s prune cadence and a resumed one re-creates it.

**Rule.** Liveness of the SUBJECT is not liveness of the CLAIM. A diagnostic
asserting a fact that can stop being true has to be re-asserted on a cadence and
withdrawn when it lapses — otherwise the instrument manufactures the failure it
was built to detect, and it does so most convincingly in exactly the long
sessions where you are least able to check it.

## 19. Reliable transport ordering does not imply ordered application across queues

On a join bootstrap, the host sent 34 files / 3,964,020 bytes, but the join
verified only 3,499,846 bytes (`badCrc=4`, `XFER-ACK ok=0`). Reconnecting
produced `XFER-COMMIT badCrc=0`. The first failure did not retry or issue
`READY`; the host remained paused until the reconnect.

`SAVE_BEGIN`, `SAVE_FILE` and `SAVE_DONE` arrive in order on reliable `CH_BULK`,
but `pumpSaveReceive()` used to drain three separate inbound queues one at a
time. The net thread could append the last FILE chunks after the file drain and
before the DONE drain; the game thread then verified and discarded the staging
folder before applying those chunks. It now snapshots the three queues under
one lock so every DONE is processed with its preceding FILE chunks.

**Rule.** When a completion marker gates a commit, dequeue its data and marker
atomically even if the transport itself guarantees ordering. This fixes the
identified queue race; reproducing the two-machine bootstrap after deploying
the same DLL to both players is still necessary to confirm that it caused this
particular failed transfer.

## 20. Save existence is not save-manager readiness

The UDP handshake completed (`WELCOME`, proto v63, RTT about 58 ms), and the
host baked and announced its world with `LOAD_GO`. The join stayed at the
title screen exchanging traffic without a load decision or a transfer request.

`titleUpdate_hook` gated `driveLoadSync` on `savesReady`, which called
`SaveManager::savesExist()`. With no local saves, this blocked the very NACK
that would request the host's save: the client could not obtain the content
required to pass its gate. `savesReady` now checks the SaveManager singleton,
independently of local content. The unused existence probe was removed.

Verification: a real game client with an isolated empty save folder received
`LOAD_GO`, replied `MISSING -> NACK`, committed 27 files / 3,376,186 bytes with
`badCrc=0`, loaded the world, and emitted `READY->host loadId=1 first-world-live`.
The sender used the actual NetLink / SaveXfer implementations and host save.
This local UDP smoke proves the missing-save path, not remote WAN conditions.

**Rule.** A bootstrap prerequisite must not depend on the content the bootstrap
itself is responsible for fetching.

## 21. A transfer cannot create its source shortfall

Two players taking the last unit must compete for one authoritative stock.
The old transfer apply accepted a request against an empty source by creating
the missing unit (`moved=0 fab=1`). A paired native-inventory regression reproduced
the extra item on v0.1.26.

Transfers now acknowledge only units actually detached from the authoritative
source. The optimistic sender returns rejected units from destination to source
before publishing its inventory. Client loot snapshots no longer overwrite the
host's stock, and an exhausted stock is not a permanent cap on later deposits
or production. `critical_stock` and `critical_npc` check the last-unit race,
returning the winning item, and subsequent replenishment on both game processes.

## 22. A backpack transfer includes its contents at the commit

Adding two items and immediately handing over their backpack used to transfer
an older, empty mirror; the next destination snapshot then erased both items.
The old build failed this paired-game regression.

`PKT_INV_XFER` now carries the complete captured bag contents, including an
explicit empty payload. When the receiver owns the source, its acknowledgement
carries the source's actual contents instead. An acquired foreign bag cannot
be edited until that acknowledgement is applied to the exact local bag instance.
`critical_bag` and `critical_bag_take` assert one destination bag, no source bag,
and both nested items after immediate transfers in either direction.

## 23. Loading a native ground item is not dropping an item

A newly streamed-in save-native item exists independently on both game copies.
Treating its first spatial discovery as a session drop minted a second copy on
the peer. Spatial discovery now seeds native ground stock without announcing a
drop; an actual native inventory-drop edge promotes the item into session stock.
`critical_ground` introduces a native item after the initial census and asserts
one, not two, on both copies.


## 24. A merchant's pockets are not the merchant's stock

The real Squin travel-shop regression found 109 stock units on the host and zero
on the client proxy of the same canonical merchant. The host's ordinary character
inventory contained only seven entries. Native trade aggregates furniture inventories
from the platoon's home building; a minted merchant had no home on the client.

`SpawnInfoPacket` carries the host merchant's shop home and platoon template SID.
Minted and adopted proxies borrow that template and bind the home through native
`Ownerships::setHomeBuilding`, deferring the description until the building loads.
This does not edit shared GameData or generate/replenish goods.

A clean-source repeat exposed a second failure: both client shopkeepers existed
but native `isATrader` returned false. Changing `SQ_ROAMING` to `SQ_RESIDENT`
did not fix it. The native predicate reads `platoon->squadTemplate`'s `is trader`
flag; an arbitrary mint template loses that flag. Preserving the actual host
template made both minted merchants valid and their native trade windows converge:
175 units / `B7D99E13` and 93 units / `7AD9CC7F`. Keep the strict canonical-key
and complete-content checks; a nearby unsynchronized local merchant is not a pass.

Shop shelves are `BCTYPE_USABLE`, not `BCTYPE_STORAGE`. Inventory-bearing usable
furniture now joins the authoritative container census; inventory-less beds and
seats do not. Fixture identities use the same 400-unit radius and 96-row limit
as inventory publication, so those runtime shelves can be resolved on the peer.

`critical_shops_squin` compares the complete native trade view under the same
canonical merchant key on both game processes, including item characteristics.
One measured run converged to 85 units and hash `DE53641B` on both peers; its
physical source inventories also had identical content fingerprints.

**Rule.** Trace the consumer's actual backing inventory, then publish both its
state and its identity. Nonempty but different shops are still a failed regression.

## 25. Native provenance and placement are part of stock reconstruction

Waystation regressions exposed different research blueprints with the same
`BLUEPRINT_ITEM` template. Grouping only by template preserved the wrong research
when quantities happened to match. Native manufacturer/material fields also
carry research and ordinary trade-item data, not only weapon provenance.

Reconcile groups, in-place gear moves and removals now include both native
provenance fields. Reconstruction resolves those records without a weapon-only
category restriction. A blueprint is created from its native research record;
ordinary non-weapon metadata is restored after the correct native constructor.
Unresolved provenance is not replaced with an arbitrary blueprint or material.

A later clean-source smoke disproved the compaction workaround: a Waystation
shop held 84 units on the host but only 81 on the client. Three Water items were
rejected repeatedly; each occupies a 5x5 rectangle in an 18x18 shelf.
Native auto-arrange did not guarantee the host's valid packing.

Protocol 68 carries each loose stack's native section and grid position.
Complete building stock validates all target rectangles before mutation,
reuses matching native items, and places each physical stack at the host's
coordinates through native section callbacks. It does not increase capacity,
invent goods or rearrange player bags. The first paired-game run converged both
Waystation shops completely (152 units / `32AFC767`, 120 units / `D254F474`).

Snapshot relocation runs under the existing `g_invVetoSuspend` guard: native
detach/add callbacks must not capture mirror maintenance as a player's drag.

Native fabrication without supplied provenance preserves factory defaults;
explicit snapshot strings, including empty strings, restore the source fields.
Clearing factory defaults broke stacking of native-produced ore and bag contents.
The full paired-game regression caught this before distribution.

Critical fixtures choose the same item by native string ID on both peers rather
than localized names. Bag transfer fixtures empty the source bag explicitly,
then transfer two freshly added children immediately, without waiting for their
ordinary snapshot. The clean-build eleven-scenario suite passed on nonprimary
DISPLAY1, including Squin, both Waystation shops, simultaneous loot/deposit/
replenishment, native ore production and both backpack transfer directions.

**Rule.** Preserve the native identity and constructor semantics; matching total
counts alone cannot distinguish different blueprints. A valid host inventory can
still fail to fit a peer's fragmented native grid.
